#include "netbench/metrics.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>
namespace nb {
static std::string escape(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += static_cast<char>(c);
        } else if (c < 32)
            o += ' ';
        else
            o += static_cast<char>(c);
    }
    return o;
}
struct Summary {
    Stats s;
    double mean = 0, p50 = 0, p95 = 0, p99 = 0;
    explicit Summary(const std::vector<Stats> &all) {
        for (const auto &a : all) {
            s.attempted += a.attempted;
            s.completed += a.completed;
            s.datagrams_sent += a.datagrams_sent;
            s.datagrams_received += a.datagrams_received;
            s.duplicates += a.duplicates;
            s.reordered += a.reordered;
            s.late += a.late;
            s.timeouts += a.timeouts;
            s.errors += a.errors;
            s.rtt_us.insert(s.rtt_us.end(), a.rtt_us.begin(), a.rtt_us.end());
        }
        std::sort(s.rtt_us.begin(), s.rtt_us.end());
        if (!s.rtt_us.empty()) {
            mean = std::accumulate(s.rtt_us.begin(), s.rtt_us.end(), 0.0) /
                   static_cast<double>(s.rtt_us.size());
            auto q = [&](double p) {
                return s.rtt_us[static_cast<size_t>(
                                    std::ceil(p * static_cast<double>(s.rtt_us.size()))) -
                                1];
            };
            p50 = q(.5);
            p95 = q(.95);
            p99 = q(.99);
        }
    }
};
static const char *result_status(const Stats &stats) {
    if (stopped)
        return "interrupted";
    if (stats.errors)
        return "error";
    if (stats.timeouts)
        return "incomplete";
    return stats.completed ? "ok" : "no_samples";
}

std::string result_json(const Config &c, uint32_t it, const std::vector<Stats> &stats, double sec,
                        double cpu) {
    Summary a(stats);
    const auto &s = a.s;
    std::ostringstream o;
    o << std::setprecision(12);
    o << "{\"schema_version\":1,\"iteration\":" << it << ",\"config\":" << config_json(c)
      << ",\"status\":\"" << result_status(s) << "\""
      << ",\"elapsed_s\":" << sec << ",\"client_cpu_s\":" << cpu
      << ",\"client_cpu_percent\":" << 100 * cpu / sec << ",\"attempted_messages\":" << s.attempted
      << ",\"completed_messages\":" << s.completed
      << ",\"goodput_mbps\":" << static_cast<double>(s.completed) * c.payload * 8 / sec / 1e6
      << ",\"rtt_samples\":" << s.rtt_us.size() << ",\"rtt_mean_us\":";
    if (s.rtt_us.empty())
        o << "null,\"rtt_median_us\":null,\"rtt_p95_us\":null,\"rtt_p99_us\":null";
    else
        o << a.mean << ",\"rtt_median_us\":" << a.p50 << ",\"rtt_p95_us\":" << a.p95
          << ",\"rtt_p99_us\":" << a.p99;
    o << ",\"datagrams_sent\":" << s.datagrams_sent
      << ",\"unique_echo_datagrams\":" << s.datagrams_received << ",\"udp_echo_loss_percent\":";
    if (c.protocol != "udp" || !s.datagrams_sent)
        o << "null";
    else
        o << 100.0 * static_cast<double>(s.datagrams_sent - s.datagrams_received) /
                 static_cast<double>(s.datagrams_sent);
    o << ",\"missing_messages\":" << s.attempted - s.completed << ",\"duplicates\":" << s.duplicates
      << ",\"reordered\":" << s.reordered << ",\"late_or_stale\":" << s.late
      << ",\"timeouts\":" << s.timeouts << ",\"errors\":" << s.errors << ",\"connections\":[";
    for (size_t i = 0; i < stats.size(); ++i) {
        if (i)
            o << ',';
        o << "{\"sndbuf_effective\":" << stats[i].sndbuf
          << ",\"rcvbuf_effective\":" << stats[i].rcvbuf << ",\"error\":\""
          << escape(stats[i].error) << "\"}";
    }
    o << "]}";
    return o.str();
}

static std::string csv_quote(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '\"')
            out += '\"';
        out += c;
    }
    return out + '\"';
}

std::string csv_header() {
    return "iteration,protocol,payload,clients,io,nodelay,elapsed_s,client_cpu_percent,attempted_"
           "messages,completed_messages,goodput_mbps,rtt_mean_us,rtt_median_us,rtt_p95_us,rtt_p99_"
           "us,datagrams_sent,unique_echo_datagrams,timeouts,errors,write,buffering,status,udp_"
           "echo_loss_percent,missing_messages,duplicates,reordered,late_or_stale,config_json,"
           "record_json";
}

std::string result_csv(const Config &c, uint32_t it, const std::vector<Stats> &stats, double sec,
                       double cpu) {
    Summary a(stats);
    const auto &s = a.s;
    std::ostringstream o;
    o << std::setprecision(12);
    o << it << ',' << c.protocol << ',' << c.payload << ',' << c.clients << ',' << c.io << ','
      << c.nodelay << ',' << sec << ',' << 100 * cpu / sec << ',' << s.attempted << ','
      << s.completed << ',' << static_cast<double>(s.completed) * c.payload * 8 / sec / 1e6 << ',';
    if (!s.rtt_us.empty())
        o << a.mean << ',' << a.p50 << ',' << a.p95 << ',' << a.p99;
    else
        o << ",,,";
    o << ',' << s.datagrams_sent << ',' << s.datagrams_received << ',' << s.timeouts << ','
      << s.errors << ',' << c.write << ',' << c.buffering << ',' << result_status(s) << ',';
    if (c.protocol == "udp" && s.datagrams_sent)
        o << 100.0 * static_cast<double>(s.datagrams_sent - s.datagrams_received) /
                 static_cast<double>(s.datagrams_sent);
    o << ',' << s.attempted - s.completed << ',' << s.duplicates << ',' << s.reordered << ','
      << s.late << ',' << csv_quote(config_json(c)) << ','
      << csv_quote(result_json(c, it, stats, sec, cpu));
    return o.str();
}
} // namespace nb
