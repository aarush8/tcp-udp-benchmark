#include "netbench/common.hpp"
#include "netbench/metrics.hpp"
#include "netbench/protocol.hpp"
#include <algorithm>
#include <barrier>
#include <iostream>
#include <thread>
using namespace nb;
namespace {
void run(const Config &c, Stats &s, std::barrier<> &ready, uint64_t &start) {
    Fd sock;
    try {
        sock = make_socket(c, c.io == "nonblocking");
        connect_socket(sock.get(), c, now_ns() + static_cast<uint64_t>(c.timeout_ms) * 1000000);
        s.sndbuf = socket_buffer(sock.get(), SO_SNDBUF);
        s.rcvbuf = socket_buffer(sock.get(), SO_RCVBUF);
    } catch (const std::exception &e) {
        ++s.errors;
        s.error = e.what();
    }
    ready.arrive_and_wait(); // Main publishes one common start after all connects finish.
    ready.arrive_and_wait();
    if (s.errors)
        return;
    try {
        auto pattern = payload_pattern(c.payload);
        std::vector<uint8_t> reusable_send, reusable_echo;
        if (c.buffering == "reuse") {
            reusable_send.resize(header_size + c.payload);
            reusable_echo.resize(header_size + c.payload);
            std::copy(pattern.begin(), pattern.end(), reusable_send.begin() + header_size);
        }
        uint64_t end = start + static_cast<uint64_t>(c.duration * 1e9);
        double per_second = std::min(static_cast<double>(c.rate) / c.clients,
                                     c.mbps * 1e6 / 8 / c.payload / c.clients);
        uint64_t interval = static_cast<uint64_t>(1e9 / per_second), next = start;
        for (uint64_t seq = 0; seq < c.messages && !stopped; ++seq) {
            if (now_ns() >= end)
                break;
            if (next >= end) {
                pace_until(end);
                break;
            }
            pace_until(next);
            if (stopped || now_ns() >= end)
                break;
            ++s.attempted;
            uint64_t began = now_ns();
            uint64_t deadline = began + static_cast<uint64_t>(c.timeout_ms) * 1000000;
            try {
                if (c.protocol == "tcp") {
                    auto header = encode({c.payload, seq, 0, 1});
                    std::vector<uint8_t> allocated_send, allocated_echo;
                    if (c.buffering == "allocate") {
                        allocated_send.resize(header_size + c.payload);
                        allocated_echo.resize(header_size + c.payload);
                        std::copy(pattern.begin(), pattern.end(),
                                  allocated_send.begin() + header_size);
                    }
                    auto &packet = c.buffering == "reuse" ? reusable_send : allocated_send;
                    auto &echo = c.buffering == "reuse" ? reusable_echo : allocated_echo;
                    std::copy(header.begin(), header.end(), packet.begin());
                    if (c.write == "coalesced")
                        send_all(sock.get(), packet.data(), packet.size(), deadline);
                    else {
                        send_all(sock.get(), packet.data(), header_size, deadline);
                        send_all(sock.get(), packet.data() + header_size, c.payload, deadline);
                    }
                    recv_exact(sock.get(), echo.data(), echo.size(), deadline);
                    auto h = decode(echo, false);
                    if (h.payload != c.payload || h.sequence != seq ||
                        !std::equal(pattern.begin(), pattern.end(), echo.begin() + header_size))
                        throw Error("TCP echo integrity mismatch");
                } else {
                    uint32_t fragments = fragment_count(c.payload);
                    SequenceTracker tracker(fragments);
                    std::vector<uint8_t> allocated_send, allocated_echo;
                    if (c.buffering == "allocate") {
                        allocated_send.resize(header_size + udp_chunk);
                        allocated_echo.resize(header_size + udp_chunk);
                    }
                    auto &packet = c.buffering == "reuse" ? reusable_send : allocated_send;
                    auto &echo = c.buffering == "reuse" ? reusable_echo : allocated_echo;
                    for (uint32_t f = 0; f < fragments; ++f) {
                        Header h{c.payload, seq, f, fragments};
                        auto header = encode(h);
                        size_t n = fragment_size(h);
                        std::copy(header.begin(), header.end(), packet.begin());
                        std::copy_n(pattern.data() + f * udp_chunk, n, packet.data() + header_size);
                        send_datagram(sock.get(), packet.data(), header_size + n, deadline);
                        ++s.datagrams_sent;
                        while (!stopped) {
                            size_t size =
                                recv_datagram(sock.get(), echo.data(), echo.size(), deadline);
                            auto r = decode(std::span(echo.data(), size), true);
                            if (r.sequence != seq) {
                                ++s.late;
                                continue;
                            }
                            if (r.fragment > f)
                                throw Error("echo references unsent fragment");
                            if (r.payload != c.payload || size != header_size + fragment_size(r) ||
                                !std::equal(echo.begin() + header_size,
                                            echo.begin() + static_cast<ptrdiff_t>(size),
                                            pattern.begin() + r.fragment * udp_chunk))
                                throw Error("UDP echo integrity mismatch");
                            if (tracker.observe(r.fragment))
                                ++s.datagrams_received;
                            else
                                ++s.duplicates;
                            if (r.fragment == f)
                                break;
                        }
                        // Charge pacing to the logical RTT: UDP fragments are sent in stop-and-wait
                        // order.
                        if (f + 1 < fragments)
                            pace_until(began + interval * (f + 1) / fragments);
                    }
                    s.reordered += tracker.reordered();
                    if (tracker.missing())
                        throw Timeout("incomplete UDP logical message");
                }
                ++s.completed;
                s.rtt_us.push_back(static_cast<double>(now_ns() - began) / 1e3);
            } catch (const Timeout &) {
                ++s.timeouts;
                if (c.protocol == "tcp")
                    break; // Cannot resume a stream with unknown framing position.
            }
            // No catch-up bursts after a stall; sleep time is excluded from successful RTTs.
            next = std::max(next + interval, began + interval);
        }
    } catch (const std::exception &e) {
        if (!stopped) {
            ++s.errors;
            s.error = e.what();
        }
    }
}
} // namespace
int main(int argc, char **argv) {
    try {
        install_signals();
        auto c = parse(argc, argv, false);
        if (c.format == "csv")
            std::cout << csv_header() << '\n';
        bool failed = false;
        for (uint32_t it = 0; it < c.iterations && !stopped; ++it) {
            std::vector<Stats> stats(c.clients);
            std::vector<std::thread> threads;
            std::barrier ready(static_cast<ptrdiff_t>(c.clients + 1));
            uint64_t start = 0;
            for (uint32_t n = 0; n < c.clients; ++n)
                threads.emplace_back([&, n] { run(c, stats[n], ready, start); });
            ready.arrive_and_wait();
            start = now_ns();
            double cpu = cpu_seconds();
            ready.arrive_and_wait();
            for (auto &t : threads)
                t.join();
            double sec = static_cast<double>(now_ns() - start) / 1e9;
            cpu = cpu_seconds() - cpu;
            std::cout << (c.format == "json" ? result_json(c, it, stats, sec, cpu)
                                             : result_csv(c, it, stats, sec, cpu))
                      << std::endl;
            for (const auto &s : stats)
                failed = failed || s.errors || s.timeouts;
        }
        return stopped ? 130 : failed ? 2 : 0;
    } catch (const std::exception &e) {
        std::cerr << "client: " << e.what() << '\n';
        return 1;
    }
}
