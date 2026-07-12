#include "netbench/common.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <utility>
namespace nb {
std::atomic<bool> stopped{false};
static void stop_handler(int) {
    stopped = 1;
}
void install_signals() {
    struct sigaction a{};
    a.sa_handler = stop_handler;
    sigemptyset(&a.sa_mask);
    syscheck(sigaction(SIGINT, &a, nullptr) == 0, "sigaction");
    syscheck(sigaction(SIGTERM, &a, nullptr) == 0, "sigaction");
}
void syscheck(bool ok, const char *op) {
    if (!ok)
        throw Error(std::string(op) + ": " + std::strerror(errno));
}
Fd::~Fd() {
    if (fd_ >= 0)
        ::close(fd_);
}
Fd::Fd(Fd &&o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
Fd &Fd::operator=(Fd &&o) noexcept {
    if (this != &o) {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
}
uint64_t now_ns() {
    timespec t{};
    syscheck(clock_gettime(CLOCK_MONOTONIC_RAW, &t) == 0, "clock_gettime");
    return static_cast<uint64_t>(t.tv_sec) * 1000000000ULL + static_cast<uint64_t>(t.tv_nsec);
}
double cpu_seconds() {
    rusage r{};
    syscheck(getrusage(RUSAGE_SELF, &r) == 0, "getrusage");
    return static_cast<double>(r.ru_utime.tv_sec + r.ru_stime.tv_sec) +
           static_cast<double>(r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}
static uint32_t number(const std::string &s) {
    uint32_t n{};
    auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (e != std::errc{} || p != s.data() + s.size())
        throw Error("invalid integer: " + s);
    return n;
}
static double real(const std::string &s) {
    size_t end{};
    double x;
    try {
        x = std::stod(s, &end);
    } catch (...) {
        throw Error("invalid number: " + s);
    }
    if (end != s.size() || !std::isfinite(x))
        throw Error("invalid number: " + s);
    return x;
}
Config parse(int argc, char **argv, bool server) {
    Config c;
    if (server)
        c.duration = 30;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (k == "--help") {
            std::cout
                << "Options (all require values): --protocol tcp|udp --host IPv4 --port 0..65535\n"
                   "--payload 64..1048576 --messages 1..1000000 --duration seconds (0.01..60)\n"
                   "--clients 1..32 --iterations 1..100 --rate messages/s (1..100000)\n"
                   "--mbps aggregate request payload cap (0.01..1000) --timeout-ms 10..5000\n"
                   "--sndbuf bytes --rcvbuf bytes (0=system default, max 16777216)\n"
                   "--io blocking|nonblocking --nodelay 0|1 --format json|csv\n"
                   "--write split|coalesced\n"
                   "Stop at the first duration/message limit. Server port 0 chooses a free port.\n";
            std::exit(0);
        }
        if (i + 1 >= argc)
            throw Error("missing value for " + k);
        std::string v = argv[++i];
        if (k == "--protocol")
            c.protocol = v;
        else if (k == "--host")
            c.host = v;
        else if (k == "--io")
            c.io = v;
        else if (k == "--format")
            c.format = v;
        else if (k == "--write")
            c.write = v;
        else if (k == "--port") {
            auto p = number(v);
            if (p > 65535)
                throw Error("invalid port");
            c.port = static_cast<uint16_t>(p);
        } else if (k == "--payload")
            c.payload = number(v);
        else if (k == "--messages")
            c.messages = number(v);
        else if (k == "--clients")
            c.clients = number(v);
        else if (k == "--iterations")
            c.iterations = number(v);
        else if (k == "--rate")
            c.rate = number(v);
        else if (k == "--timeout-ms")
            c.timeout_ms = number(v);
        else if (k == "--sndbuf")
            c.sndbuf = number(v);
        else if (k == "--rcvbuf")
            c.rcvbuf = number(v);
        else if (k == "--duration")
            c.duration = real(v);
        else if (k == "--mbps")
            c.mbps = real(v);
        else if (k == "--nodelay") {
            if (v != "0" && v != "1")
                throw Error("nodelay must be 0 or 1");
            c.nodelay = v == "1";
        } else
            throw Error("unknown option: " + k);
    }
    in_addr addr{};
    if (inet_pton(AF_INET, c.host.c_str(), &addr) != 1)
        throw Error("host must be a numeric IPv4 address");
    if ((c.protocol != "tcp" && c.protocol != "udp") ||
        (c.io != "blocking" && c.io != "nonblocking") ||
        (c.format != "json" && c.format != "csv") || (c.write != "split" && c.write != "coalesced"))
        throw Error("invalid protocol, io, or format");
    if (c.payload < 64 || c.payload > 1048576 || c.messages < 1 || c.messages > 1000000 ||
        c.clients < 1 || c.clients > 32 ||
        static_cast<uint64_t>(c.clients) * c.messages > 1000000 || c.iterations < 1 ||
        c.iterations > 100 || c.rate < 1 || c.rate > 100000 || c.timeout_ms < 10 ||
        c.timeout_ms > 5000 || c.duration < 0.01 || c.duration > 60 || c.mbps < 0.01 ||
        c.mbps > 1000 || c.sndbuf > 16777216 || c.rcvbuf > 16777216 || (!server && c.port == 0))
        throw Error("configuration outside bounded limits");
    return c;
}
std::string config_json(const Config &c) {
    return "{\"protocol\":\"" + c.protocol + "\",\"host\":\"" + c.host +
           "\",\"port\":" + std::to_string(c.port) + ",\"payload\":" + std::to_string(c.payload) +
           ",\"messages\":" + std::to_string(c.messages) +
           ",\"clients\":" + std::to_string(c.clients) +
           ",\"iterations\":" + std::to_string(c.iterations) +
           ",\"duration\":" + std::to_string(c.duration) + ",\"rate\":" + std::to_string(c.rate) +
           ",\"mbps\":" + std::to_string(c.mbps) +
           ",\"timeout_ms\":" + std::to_string(c.timeout_ms) +
           ",\"sndbuf\":" + std::to_string(c.sndbuf) + ",\"rcvbuf\":" + std::to_string(c.rcvbuf) +
           ",\"io\":\"" + c.io + "\",\"nodelay\":" + (c.nodelay ? "true" : "false") + "}";
}
static sockaddr_in address(const Config &c) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(c.port);
    syscheck(inet_pton(AF_INET, c.host.c_str(), &a.sin_addr) == 1, "inet_pton");
    return a;
}
void configure(int fd, const Config &c, bool nonblocking) {
    if (c.sndbuf) {
        int n = static_cast<int>(c.sndbuf);
        syscheck(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &n, sizeof(n)) == 0, "SO_SNDBUF");
    }
    if (c.rcvbuf) {
        int n = static_cast<int>(c.rcvbuf);
        syscheck(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n)) == 0, "SO_RCVBUF");
    }
    // Short kernel waits allow cancellation; the operation's absolute deadline governs timeout.
    timeval t{0, 50000};
    syscheck(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t)) == 0, "SO_RCVTIMEO");
    syscheck(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t)) == 0, "SO_SNDTIMEO");
    if (c.protocol == "tcp") {
        int n = c.nodelay;
        syscheck(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &n, sizeof(n)) == 0, "TCP_NODELAY");
    }
    int flags = fcntl(fd, F_GETFL, 0);
    syscheck(flags >= 0, "F_GETFL");
    syscheck(fcntl(fd, F_SETFL, nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0,
             "F_SETFL");
}
Fd make_socket(const Config &c, bool nonblocking) {
    Fd fd(socket(AF_INET, (c.protocol == "tcp" ? SOCK_STREAM : SOCK_DGRAM) | SOCK_CLOEXEC, 0));
    syscheck(fd.get() >= 0, "socket");
    configure(fd.get(), c, nonblocking);
    return fd;
}
void bind_socket(int fd, const Config &c) {
    int yes = 1;
    syscheck(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0, "SO_REUSEADDR");
    auto a = address(c);
    syscheck(bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0, "bind");
}
void connect_socket(int fd, const Config &c, uint64_t deadline) {
    int flags = fcntl(fd, F_GETFL, 0);
    syscheck(flags >= 0, "F_GETFL");
    syscheck(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0, "nonblocking connect");
    auto a = address(c);
    int r = connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a));
    if (r < 0) {
        if (errno != EINPROGRESS && errno != EINTR)
            syscheck(false, "connect");
        wait_ready(fd, POLLOUT, deadline);
        int err{};
        socklen_t len = sizeof(err);
        syscheck(getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0, "SO_ERROR");
        if (err) {
            errno = err;
            syscheck(false, "connect");
        }
    }
    syscheck(fcntl(fd, F_SETFL, flags) == 0, "restore socket flags");
}
uint16_t local_port(int fd) {
    sockaddr_in a{};
    socklen_t n = sizeof(a);
    syscheck(getsockname(fd, reinterpret_cast<sockaddr *>(&a), &n) == 0, "getsockname");
    return ntohs(a.sin_port);
}
int socket_buffer(int fd, int option) {
    int n{};
    socklen_t len = sizeof(n);
    syscheck(getsockopt(fd, SOL_SOCKET, option, &n, &len) == 0, "getsockopt buffer");
    return n;
}
static void deadline_check(uint64_t deadline) {
    if (stopped)
        throw Error("interrupted");
    if (now_ns() >= deadline)
        throw Timeout("I/O deadline exceeded");
}
void wait_ready(int fd, short events, uint64_t deadline) {
    for (;;) {
        deadline_check(deadline);
        auto now = now_ns();
        if (now >= deadline)
            throw Timeout("I/O deadline exceeded");
        auto left = (deadline - now + 999999) / 1000000;
        pollfd p{fd, events, 0};
        int r = poll(&p, 1, static_cast<int>(std::min<uint64_t>(left, 50)));
        if (r < 0) {
            if (errno == EINTR)
                continue;
            syscheck(false, "poll");
        }
        if (r > 0) {
            if (p.revents & POLLNVAL)
                throw Error("invalid descriptor");
            if (p.revents & (events | POLLHUP | POLLERR))
                return;
        }
    }
}
void send_all(int fd, const uint8_t *data, size_t size, uint64_t deadline) {
    size_t pos = 0;
    while (pos < size) {
        deadline_check(deadline);
        auto n = send(fd, data + pos, size - pos, MSG_NOSIGNAL);
        if (n > 0)
            pos += static_cast<size_t>(n);
        else if (n == 0)
            throw Closed("zero-length send");
        else if (errno == EINTR)
            continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
            wait_ready(fd, POLLOUT, deadline);
        else
            syscheck(false, "send");
    }
}
void recv_exact(int fd, uint8_t *data, size_t size, uint64_t deadline) {
    size_t pos = 0;
    while (pos < size) {
        deadline_check(deadline);
        auto n = recv(fd, data + pos, size - pos, 0);
        if (n > 0)
            pos += static_cast<size_t>(n);
        else if (n == 0)
            throw Closed(pos ? "EOF inside frame" : "peer closed");
        else if (errno == EINTR)
            continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
            wait_ready(fd, POLLIN, deadline);
        else
            syscheck(false, "recv");
    }
}
void send_datagram(int fd, const uint8_t *data, size_t size, uint64_t deadline,
                   const sockaddr *peer, socklen_t peerlen) {
    for (;;) {
        deadline_check(deadline);
        auto n = sendto(fd, data, size, MSG_NOSIGNAL, peer, peerlen);
        if (n >= 0) {
            if (static_cast<size_t>(n) != size)
                throw Error("short UDP send");
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            wait_ready(fd, POLLOUT, deadline);
        else
            syscheck(false, "sendto");
    }
}
size_t recv_datagram(int fd, uint8_t *data, size_t capacity, uint64_t deadline, sockaddr *peer,
                     socklen_t *peerlen) {
    for (;;) {
        deadline_check(deadline);
        auto n = recvfrom(fd, data, capacity, MSG_TRUNC, peer, peerlen);
        if (n >= 0) {
            if (static_cast<size_t>(n) > capacity)
                throw Error("truncated UDP datagram");
            return static_cast<size_t>(n);
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            wait_ready(fd, POLLIN, deadline);
        else
            syscheck(false, "recvfrom");
    }
}
void pace_until(uint64_t deadline) {
    while (!stopped) {
        auto now = now_ns();
        if (now >= deadline)
            return;
        auto ns = std::min<uint64_t>(deadline - now, 10000000);
        timespec t{static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
        if (nanosleep(&t, nullptr) < 0 && errno != EINTR)
            syscheck(false, "nanosleep");
    }
}
} // namespace nb
