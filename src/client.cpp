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
            if (config.protocol == "tcp") {
                const auto header = encode({config.payload, sequence, 0, 1});
                send_all(socket.get(), header.data(), header.size(), deadline);
                send_all(socket.get(), pattern.data(), pattern.size(), deadline);
                std::vector<uint8_t> response(header_size + config.payload);
                recv_exact(socket.get(), response.data(), response.size(), deadline);
                const auto reply = decode(response, false);
                if (reply.payload != config.payload || reply.sequence != sequence ||
                    !std::equal(pattern.begin(), pattern.end(), response.begin() + header_size))
                    throw Error("TCP echo integrity mismatch");
            } else {
                const auto fragments = fragment_count(config.payload);
                SequenceTracker received(fragments);
                std::vector<uint8_t> packet(header_size + udp_chunk), echo(header_size + udp_chunk);
                for (uint32_t fragment = 0; fragment < fragments; ++fragment) {
                    const Header request{config.payload, sequence, fragment, fragments};
                    const auto header = encode(request);
                    const auto size = fragment_size(request);
                    std::copy(header.begin(), header.end(), packet.begin());
                    std::copy_n(pattern.data() + fragment * udp_chunk, size,
                                packet.data() + header_size);
                    send_datagram(socket.get(), packet.data(), header_size + size, deadline);
                    while (!stopped) {
                        const auto length =
                            recv_datagram(socket.get(), echo.data(), echo.size(), deadline);
                        const auto reply = decode(std::span(echo.data(), length), true);
                        if (reply.sequence != sequence)
                            continue;
                        if (reply.payload != config.payload || reply.fragment > fragment ||
                            length != header_size + fragment_size(reply) ||
                            !std::equal(echo.begin() + header_size,
                                        echo.begin() + static_cast<ptrdiff_t>(length),
                                        pattern.begin() + reply.fragment * udp_chunk))
                            throw Error("UDP echo integrity mismatch");
                        received.observe(reply.fragment);
                        if (reply.fragment == fragment)
                            break;
                    }
                    if (fragment + 1 < fragments)
                        pace_until(std::min<uint64_t>(deadline, began + interval * (fragment + 1) /
                                                                            fragments));
                }
                if (received.missing())
                    throw Timeout("missing UDP fragment echo");
            }
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
