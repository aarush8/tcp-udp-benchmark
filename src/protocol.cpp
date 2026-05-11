#include "netbench/protocol.hpp"
#include <algorithm>
namespace nb {
static void put(uint8_t *p, uint64_t n, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i)
        p[i] = static_cast<uint8_t>(n >> (8 * (bytes - 1 - i)));
}
static uint64_t get(const uint8_t *p, size_t bytes) {
    uint64_t n = 0;
    for (size_t i = 0; i < bytes; ++i)
        n = (n << 8) | p[i];
    return n;
}
std::array<uint8_t, header_size> encode(Header h) {
    std::array<uint8_t, header_size> b{};
    put(b.data(), 0x4e423031, 4);
    put(b.data() + 4, h.payload, 4);
    put(b.data() + 8, h.sequence, 8);
    put(b.data() + 16, h.fragment, 4);
    put(b.data() + 20, h.fragments, 4);
    return b;
}
uint32_t fragment_count(uint32_t n) {
    return (n + udp_chunk - 1) / udp_chunk;
}
Header decode(std::span<const uint8_t> b, bool udp) {
    if (b.size() < header_size || get(b.data(), 4) != 0x4e423031)
        throw Error("invalid wire magic/header");
    Header h{static_cast<uint32_t>(get(b.data() + 4, 4)), get(b.data() + 8, 8),
             static_cast<uint32_t>(get(b.data() + 16, 4)),
             static_cast<uint32_t>(get(b.data() + 20, 4))};
    if (h.payload < 64 || h.payload > max_payload ||
        h.fragments != (udp ? fragment_count(h.payload) : 1) || h.fragment >= h.fragments)
        throw Error("invalid frame bounds");
    return h;
}
size_t fragment_size(Header h) {
    return std::min(udp_chunk, h.payload - h.fragment * udp_chunk);
}
std::vector<uint8_t> payload_pattern(uint32_t size) {
    std::vector<uint8_t> p(size);
    for (size_t i = 0; i < p.size(); ++i)
        p[i] = static_cast<uint8_t>((i * 131 + 17) % 251);
    return p;
}
bool SequenceTracker::observe(size_t s) {
    if (s >= seen_.size())
        throw Error("sequence outside tracking window");
    if (seen_[s]) {
        ++duplicates_;
        return false;
    }
    if (received_ && s < high_)
        ++reordered_;
    high_ = std::max(high_, s);
    seen_[s] = true;
    ++received_;
    return true;
}
} // namespace nb
