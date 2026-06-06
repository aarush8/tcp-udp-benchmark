#pragma once
#include "netbench/common.hpp"
namespace nb {
struct Stats {
    uint64_t attempted = 0, completed = 0, datagrams_sent = 0, datagrams_received = 0;
    uint64_t duplicates = 0, reordered = 0, late = 0, timeouts = 0, errors = 0;
    int sndbuf = 0, rcvbuf = 0;
    std::vector<double> rtt_us;
    std::string error;
};
std::string result_json(const Config &c, uint32_t iteration, const std::vector<Stats> &stats,
                        double seconds, double cpu);
std::string csv_header();
std::string result_csv(const Config &c, uint32_t iteration, const std::vector<Stats> &stats,
                       double seconds, double cpu);
} // namespace nb
