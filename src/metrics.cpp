#include "netbench/metrics.hpp"
#include <sstream>
namespace nb {
std::string result_json(const Config &, uint32_t iteration, const std::vector<Stats> &all, double,
                        double) {
    uint64_t completed = 0, errors = 0, timeouts = 0;
    for (const auto &stats : all) {
        completed += stats.completed;
        errors += stats.errors;
        timeouts += stats.timeouts;
    }
    std::ostringstream output;
    output << "{\"iteration\":" << iteration << ",\"completed_messages\":" << completed
           << ",\"errors\":" << errors << ",\"timeouts\":" << timeouts << ",\"status\":\""
           << (errors     ? "error"
               : timeouts ? "incomplete"
                          : "ok")
           << "\"}";
    return output.str();
}
std::string csv_header() {
    return "iteration,completed_messages";
}
std::string result_csv(const Config &, uint32_t iteration, const std::vector<Stats> &all, double,
                       double) {
    uint64_t completed = 0;
    for (const auto &stats : all)
        completed += stats.completed;
    return std::to_string(iteration) + "," + std::to_string(completed);
}
} // namespace nb
