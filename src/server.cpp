#include "netbench/common.hpp"
#include "netbench/protocol.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <thread>
using namespace nb;
namespace {
struct Worker {
    // Stable allocation survives vector moves; thread is destroyed/joined first.
    std::unique_ptr<std::atomic<bool>> done;
    std::jthread thread;
};
void serve_tcp(Fd fd, const Config &c, std::atomic<uint64_t> &frames,
               std::atomic<uint64_t> &errors) {
    bool in_frame = false;
    try {
        std::vector<uint8_t> frame;
        while (!stopped) {
            std::array<uint8_t, header_size> header{};
            recv_exact(fd.get(), header.data(), 1,
                       now_ns() + static_cast<uint64_t>((c.duration + 1) * 1e9));
            in_frame = true;
            const uint64_t deadline = now_ns() + static_cast<uint64_t>(c.timeout_ms) * 1000000;
            recv_exact(fd.get(), header.data() + 1, header.size() - 1, deadline);
            auto h = decode(header, false);
            if (c.buffering == "allocate")
                frame = std::vector<uint8_t>(header_size + h.payload);
            else
                frame.resize(header_size + h.payload);
            std::copy(header.begin(), header.end(), frame.begin());
            recv_exact(fd.get(), frame.data() + header_size, h.payload, deadline);
            send_all(fd.get(), frame.data(), frame.size(), deadline);
            ++frames;
            in_frame = false;
        }
    } catch (const Closed &) {
        if (in_frame && !stopped)
            ++errors;
    } catch (const Timeout &) {
        if (in_frame && !stopped)
            ++errors;
    } catch (const std::exception &) {
        if (!stopped)
            ++errors;
    }
}
} // namespace
int main(int argc, char **argv) {
    try {
        install_signals();
        Config c = parse(argc, argv, true);
        auto start = now_ns();
        double cpu = cpu_seconds();
        auto sock = make_socket(c, c.protocol == "tcp" || c.io == "nonblocking");
        bind_socket(sock.get(), c);
        if (c.protocol == "tcp")
            syscheck(listen(sock.get(), 32) == 0, "listen");
        std::cout << "{\"event\":\"ready\",\"port\":" << local_port(sock.get())
                  << ",\"sndbuf_effective\":" << socket_buffer(sock.get(), SO_SNDBUF)
                  << ",\"rcvbuf_effective\":" << socket_buffer(sock.get(), SO_RCVBUF) << "}\n"
                  << std::flush; // The runner needs readiness before the accept loop blocks.
        std::atomic<uint64_t> frames{0}, errors{0};
        uint64_t accepted = 0;
        size_t peak_workers = 0;
        std::mutex done_mutex;
        std::condition_variable done_cv;
        std::vector<Worker> workers;
        workers.reserve(c.clients);
        struct ShutdownWorkers {
            ~ShutdownWorkers() { stopped = true; }
        } shutdown_workers;
        auto end = start + static_cast<uint64_t>(c.duration * 1e9);
        std::array<uint8_t, header_size + udp_chunk> packet{};
        while (!stopped && now_ns() < end) {
            for (auto i = workers.begin(); i != workers.end();) {
                if (i->done->load()) {
                    i->thread.join();
                    i = workers.erase(i);
                } else
                    ++i;
            }
            if (c.protocol == "tcp" && workers.size() >= c.clients) {
                // Do not reject a reconnect merely because its previous worker has not
                // observed FIN yet. The finite listen backlog holds new connections.
                std::unique_lock lock(done_mutex);
                done_cv.wait_for(lock, std::chrono::milliseconds(50), [&] {
                    return stopped || std::any_of(workers.begin(), workers.end(),
                                                  [](const auto &w) { return w.done->load(); });
                });
                continue;
            }
            try {
                wait_ready(sock.get(), POLLIN, std::min(end, now_ns() + 50000000));
            } catch (const Timeout &) {
                continue;
            } catch (const Error &) {
                if (stopped)
                    break;
                throw;
            }
            if (c.protocol == "tcp") {
                Fd client(accept4(sock.get(), nullptr, nullptr, SOCK_CLOEXEC));
                if (client.get() < 0) {
                    if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED)
                        continue;
                    syscheck(false, "accept");
                }
                configure(client.get(), c, c.io == "nonblocking");
                auto done = std::make_unique<std::atomic<bool>>(false);
                std::jthread thread([fd = std::move(client), &c, &frames, &errors,
                                     done_ptr = done.get(), &done_mutex, &done_cv]() mutable {
                    serve_tcp(std::move(fd), c, frames, errors);
                    {
                        std::lock_guard lock(done_mutex);
                        done_ptr->store(true);
                    }
                    done_cv.notify_one();
                });
                workers.push_back({std::move(done), std::move(thread)});
                peak_workers = std::max(peak_workers, workers.size());
                ++accepted;
            } else {
                sockaddr_in peer{};
                socklen_t len = sizeof(peer);
                try {
                    auto deadline = now_ns() + static_cast<uint64_t>(c.timeout_ms) * 1000000;
                    size_t n = recv_datagram(sock.get(), packet.data(), packet.size(), deadline,
                                             reinterpret_cast<sockaddr *>(&peer), &len);
                    auto h = decode(std::span(packet.data(), n), true);
                    if (n != header_size + fragment_size(h))
                        throw Error("invalid datagram size");
                    send_datagram(sock.get(), packet.data(), n, deadline,
                                  reinterpret_cast<sockaddr *>(&peer), len);
                    ++frames;
                } catch (const std::exception &) {
                    if (!stopped)
                        ++errors;
                }
            }
        }
        stopped = 1;
        for (auto &worker : workers)
            worker.thread.join();
        std::cout << "{\"event\":\"summary\",\"frames_echoed\":" << frames.load()
                  << ",\"errors\":" << errors.load() << ",\"accepted_connections\":" << accepted
                  << ",\"peak_workers\":" << peak_workers << ",\"cpu_s\":" << cpu_seconds() - cpu
                  << ",\"lifetime_s\":" << static_cast<double>(now_ns() - start) / 1e9 << "}"
                  << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "server: " << e.what() << '\n';
        return 1;
    }
}
