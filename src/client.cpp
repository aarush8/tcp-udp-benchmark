#include "netbench/common.hpp"
#include "netbench/metrics.hpp"
#include "netbench/protocol.hpp"
#include <algorithm>
#include <barrier>
#include <iostream>
#include <thread>

using namespace nb;

namespace {
struct MessageBuffers {
    std::vector<uint8_t> request;
    std::vector<uint8_t> response;

    void resize(size_t size) {
        request.resize(size);
        response.resize(size);
    }
};

struct MessageTiming {
    uint64_t start;
    uint64_t deadline;
    uint64_t interval;
};

void exchange_tcp(int fd, const Config &config, uint64_t sequence,
                  const std::vector<uint8_t> &pattern, MessageBuffers &buffers,
                  const MessageTiming &timing) {
    const auto header = encode({config.payload, sequence, 0, 1});
    std::copy(header.begin(), header.end(), buffers.request.begin());

    if (config.write == "coalesced") {
        send_all(fd, buffers.request.data(), buffers.request.size(), timing.deadline);
    } else {
        send_all(fd, buffers.request.data(), header_size, timing.deadline);
        send_all(fd, buffers.request.data() + header_size, config.payload, timing.deadline);
    }

    recv_exact(fd, buffers.response.data(), buffers.response.size(), timing.deadline);
    const auto echo = decode(buffers.response, false);
    if (echo.payload != config.payload || echo.sequence != sequence ||
        !std::equal(pattern.begin(), pattern.end(), buffers.response.begin() + header_size)) {
        throw Error("TCP echo integrity mismatch");
    }
}

void exchange_udp(int fd, const Config &config, uint64_t sequence,
                  const std::vector<uint8_t> &pattern, MessageBuffers &buffers,
                  const MessageTiming &timing, Stats &stats) {
    const uint32_t fragments = fragment_count(config.payload);
    SequenceTracker tracker(fragments);

    for (uint32_t fragment = 0; fragment < fragments; ++fragment) {
        const Header request{config.payload, sequence, fragment, fragments};
        const auto header = encode(request);
        const size_t payload_size = fragment_size(request);
        std::copy(header.begin(), header.end(), buffers.request.begin());
        std::copy_n(pattern.data() + fragment * udp_chunk, payload_size,
                    buffers.request.data() + header_size);
        send_datagram(fd, buffers.request.data(), header_size + payload_size, timing.deadline);
        ++stats.datagrams_sent;

        while (!stopped) {
            const size_t received = recv_datagram(fd, buffers.response.data(),
                                                  buffers.response.size(), timing.deadline);
            const auto echo = decode(std::span(buffers.response.data(), received), true);
            if (echo.sequence != sequence) {
                ++stats.late;
                continue;
            }
            if (echo.fragment > fragment)
                throw Error("echo references unsent fragment");
            if (echo.payload != config.payload || received != header_size + fragment_size(echo) ||
                !std::equal(buffers.response.begin() + header_size,
                            buffers.response.begin() + static_cast<ptrdiff_t>(received),
                            pattern.begin() + echo.fragment * udp_chunk)) {
                throw Error("UDP echo integrity mismatch");
            }
            if (tracker.observe(echo.fragment))
                ++stats.datagrams_received;
            else
                ++stats.duplicates;
            if (echo.fragment == fragment)
                break;
        }

        // Fragment pacing is part of logical RTT and must share its deadline.
        if (fragment + 1 < fragments) {
            const auto next = timing.start + timing.interval * (fragment + 1) / fragments;
            pace_until(std::min(timing.deadline, next));
        }
    }
    stats.reordered += tracker.reordered();
    if (tracker.missing())
        throw Timeout("incomplete UDP logical message");
}

void run_worker(const Config &config, Stats &stats, std::barrier<> &ready, const uint64_t &start) {
    Fd socket;
    try {
        socket = make_socket(config, config.io == "nonblocking");
        connect_socket(socket.get(), config,
                       now_ns() + static_cast<uint64_t>(config.timeout_ms) * 1000000);
        stats.sndbuf = socket_buffer(socket.get(), SO_SNDBUF);
        stats.rcvbuf = socket_buffer(socket.get(), SO_RCVBUF);
    } catch (const std::exception &error) {
        ++stats.errors;
        stats.error = error.what();
    }

    // All connections finish setup before main publishes a common start time.
    ready.arrive_and_wait();
    ready.arrive_and_wait();
    if (stats.errors)
        return;

    try {
        const auto pattern = payload_pattern(config.payload);
        const auto buffer_size =
            header_size + (config.protocol == "tcp" ? config.payload : udp_chunk);
        MessageBuffers reusable;
        if (config.buffering == "reuse") {
            reusable.resize(buffer_size);
            if (config.protocol == "tcp")
                std::copy(pattern.begin(), pattern.end(), reusable.request.begin() + header_size);
        }

        const uint64_t end = start + static_cast<uint64_t>(config.duration * 1e9);
        const double requests_per_second =
            std::min(static_cast<double>(config.rate) / config.clients,
                     config.mbps * 1e6 / 8 / config.payload / config.clients);
        const uint64_t interval = static_cast<uint64_t>(1e9 / requests_per_second);
        uint64_t next = start;

        for (uint64_t sequence = 0; sequence < config.messages && !stopped; ++sequence) {
            if (now_ns() >= end)
                break;
            if (next >= end) {
                pace_until(end);
                break;
            }
            pace_until(next);
            if (stopped || now_ns() >= end)
                break;

            ++stats.attempted;
            const uint64_t began = now_ns();
            const MessageTiming timing{
                began, began + static_cast<uint64_t>(config.timeout_ms) * 1000000, interval};
            try {
                MessageBuffers allocated;
                if (config.buffering == "allocate") {
                    allocated.resize(buffer_size);
                    if (config.protocol == "tcp") {
                        std::copy(pattern.begin(), pattern.end(),
                                  allocated.request.begin() + header_size);
                    }
                }
                auto &buffers = config.buffering == "reuse" ? reusable : allocated;
                if (config.protocol == "tcp")
                    exchange_tcp(socket.get(), config, sequence, pattern, buffers, timing);
                else
                    exchange_udp(socket.get(), config, sequence, pattern, buffers, timing, stats);

                ++stats.completed;
                stats.rtt_us.push_back(static_cast<double>(now_ns() - began) / 1e3);
            } catch (const Timeout &) {
                ++stats.timeouts;
                if (config.protocol == "tcp")
                    break; // A partially consumed stream cannot safely resume at the next frame.
            }
            // Advance from the actual start to avoid catch-up bursts after a stall.
            next = std::max(next + interval, began + interval);
        }
    } catch (const std::exception &error) {
        if (!stopped) {
            ++stats.errors;
            stats.error = error.what();
        }
    }
}
} // namespace

int main(int argc, char **argv) {
    try {
        install_signals();
        const auto config = parse(argc, argv, false);
        if (config.format == "csv")
            std::cout << csv_header() << '\n';
        bool failed = false;
        for (uint32_t iteration = 0; iteration < config.iterations && !stopped; ++iteration) {
            std::vector<Stats> stats(config.clients);
            std::vector<std::thread> threads;
            threads.reserve(config.clients);
            std::barrier ready(static_cast<ptrdiff_t>(config.clients + 1));
            uint64_t start = 0;
            try {
                for (uint32_t worker = 0; worker < config.clients; ++worker) {
                    threads.emplace_back(
                        [&, worker] { run_worker(config, stats[worker], ready, start); });
                }
            } catch (const std::exception &error) {
                // Release existing workers even when the OS refuses another thread.
                for (size_t worker = threads.size(); worker < stats.size(); ++worker) {
                    ++stats[worker].errors;
                    stats[worker].error = error.what();
                    ready.arrive_and_drop();
                }
            }
            ready.arrive_and_wait();
            start = now_ns();
            double cpu = cpu_seconds();
            ready.arrive_and_wait();
            for (auto &thread : threads)
                thread.join();

            const double seconds = static_cast<double>(now_ns() - start) / 1e9;
            cpu = cpu_seconds() - cpu;
            std::cout << (config.format == "json"
                              ? result_json(config, iteration, stats, seconds, cpu)
                              : result_csv(config, iteration, stats, seconds, cpu))
                      << '\n'
                      << std::flush;
            uint64_t completed = 0;
            for (const auto &worker : stats) {
                failed = failed || worker.errors || worker.timeouts;
                completed += worker.completed;
            }
            failed = failed || !completed;
        }
        return stopped ? 130 : failed ? 2 : 0;
    } catch (const std::exception &error) {
        std::cerr << "client: " << error.what() << '\n';
        return 1;
    }
}
