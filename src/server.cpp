#include "netbench/common.hpp"
#include "netbench/protocol.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
using namespace nb;

int main(int argc, char **argv) {
    try {
        install_signals();
        const auto config = parse(argc, argv, true);
        if (config.protocol != "tcp")
            throw Error("UDP is not available in this version");
        const auto end = now_ns() + static_cast<uint64_t>(config.duration * 1e9);
        auto socket = make_socket(config, true);
        bind_socket(socket.get(), config);
        if (config.protocol == "tcp")
            syscheck(listen(socket.get(), 8) == 0, "listen");
        std::cout << "{\"event\":\"ready\",\"port\":" << local_port(socket.get()) << "}\n"
                  << std::flush;
        uint64_t frames = 0;
        while (!stopped && now_ns() < end) {
            try {
                wait_ready(socket.get(), POLLIN, std::min(end, now_ns() + 50000000));
            } catch (const Timeout &) {
                continue;
            }
            if (config.protocol == "tcp") {
                Fd client(accept4(socket.get(), nullptr, nullptr, SOCK_CLOEXEC));
                if (client.get() < 0) {
                    if (errno == EINTR || errno == EAGAIN)
                        continue;
                    syscheck(false, "accept");
                }
                configure(client.get(), config, false);
                while (!stopped && now_ns() < end) {
                    const auto deadline =
                        std::min(end, now_ns() + config.timeout_ms * uint64_t{1000000});
                    std::array<uint8_t, header_size> header{};
                    try {
                        recv_exact(client.get(), header.data(), header.size(), deadline);
                        const auto message = decode(header, false);
                        std::vector<uint8_t> frame(header_size + message.payload);
                        std::copy(header.begin(), header.end(), frame.begin());
                        recv_exact(client.get(), frame.data() + header_size, message.payload,
                                   deadline);
                        send_all(client.get(), frame.data(), frame.size(), deadline);
                        ++frames;
                    } catch (const Closed &) {
                        break;
                    } catch (const Timeout &) {
                        break;
                    }
                }
            }
        }
        std::cout << "{\"event\":\"summary\",\"frames_echoed\":" << frames << "}\n";
        return 0;
    } catch (const std::exception &error) {
        if (stopped)
            return 0;
        std::cerr << "server: " << error.what() << '\n';
        return 1;
    }
}
