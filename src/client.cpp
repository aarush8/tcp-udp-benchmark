#include "netbench/common.hpp"
#include "netbench/protocol.hpp"
#include <algorithm>
#include <iostream>
using namespace nb;

int main(int argc, char **argv) {
    try {
        install_signals();
        const auto config = parse(argc, argv, false);
        if (config.clients != 1 || config.iterations != 1)
            throw Error("this version supports one client and one iteration");
        if (config.protocol != "tcp")
            throw Error("UDP is not available in this version");
        auto socket = make_socket(config, false);
        connect_socket(socket.get(), config, now_ns() + config.timeout_ms * 1000000ULL);
        const auto pattern = payload_pattern(config.payload);
        const auto end = now_ns() + static_cast<uint64_t>(config.duration * 1e9);
        const auto interval =
            static_cast<uint64_t>(1e9 / std::min(static_cast<double>(config.rate),
                                                 config.mbps * 1e6 / 8 / config.payload));
        uint64_t completed = 0, next = now_ns();
        for (uint64_t sequence = 0; sequence < config.messages && !stopped; ++sequence) {
            if (now_ns() >= end || next >= end)
                break;
            pace_until(next);
            const auto began = now_ns();
            const auto deadline = began + config.timeout_ms * 1000000ULL;
            const auto header = encode({config.payload, sequence, 0, 1});
            send_all(socket.get(), header.data(), header.size(), deadline);
            send_all(socket.get(), pattern.data(), pattern.size(), deadline);
            std::vector<uint8_t> response(header_size + config.payload);
            recv_exact(socket.get(), response.data(), response.size(), deadline);
            const auto reply = decode(response, false);
            if (reply.payload != config.payload || reply.sequence != sequence ||
                !std::equal(pattern.begin(), pattern.end(), response.begin() + header_size))
                throw Error("TCP echo integrity mismatch");
            ++completed;
            next = began + interval;
        }
        std::cout << "{\"completed_messages\":" << completed << "}\n";
        return stopped ? 130 : 0;
    } catch (const std::exception &error) {
        std::cerr << "client: " << error.what() << '\n';
        return stopped ? 130 : 2;
    }
}
