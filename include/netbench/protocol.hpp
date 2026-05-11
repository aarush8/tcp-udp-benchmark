#pragma once
#include "netbench/common.hpp"
#include <array>
#include <span>
namespace nb {
constexpr size_t header_size = 24;
constexpr uint32_t max_payload = 1048576, udp_chunk = 1200;
struct Header {
    uint32_t payload;
    uint64_t sequence;
    uint32_t fragment;
    uint32_t fragments;
};
std::array<uint8_t, header_size> encode(Header h);
Header decode(std::span<const uint8_t> bytes, bool udp);
uint32_t fragment_count(uint32_t payload);
size_t fragment_size(Header h);
std::vector<uint8_t> payload_pattern(uint32_t size);
// Tracks unique arrivals without mistaking reordering or duplicates for loss.
class SequenceTracker {
    std::vector<bool> seen_;
    size_t received_ = 0, duplicates_ = 0, reordered_ = 0, high_ = 0;

  public:
    explicit SequenceTracker(size_t count) : seen_(count) {}
    bool observe(size_t sequence);
    size_t received() const { return received_; }
    size_t missing() const { return seen_.size() - received_; }
    size_t duplicates() const { return duplicates_; }
    size_t reordered() const { return reordered_; }
};
} // namespace nb
