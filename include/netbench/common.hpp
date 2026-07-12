#pragma once
#include <atomic>
#include <csignal>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <vector>
namespace nb {
static_assert(std::atomic<bool>::is_always_lock_free);
extern std::atomic<bool> stopped;
void install_signals();
struct Error : std::runtime_error {
    using runtime_error::runtime_error;
};
struct Timeout : Error {
    using Error::Error;
};
struct Closed : Error {
    using Error::Error;
};
void syscheck(bool ok, const char *operation);
class Fd {
    int fd_ = -1;

  public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd();
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) noexcept;
    Fd &operator=(Fd &&other) noexcept;
    int get() const { return fd_; }
};
uint64_t now_ns();
double cpu_seconds();
struct Config {
    std::string protocol = "tcp", host = "127.0.0.1", io = "blocking", format = "json";
    std::string write = "split";
    uint32_t payload = 1024, messages = 1000, clients = 1, iterations = 1;
    uint32_t timeout_ms = 500, sndbuf = 0, rcvbuf = 0, rate = 10000;
    uint16_t port = 9000;
    double duration = 1.0, mbps = 100.0;
    bool nodelay = false;
};
Config parse(int argc, char **argv, bool server);
std::string config_json(const Config &c);
Fd make_socket(const Config &c, bool nonblocking);
void configure(int fd, const Config &c, bool nonblocking);
void bind_socket(int fd, const Config &c);
void connect_socket(int fd, const Config &c, uint64_t deadline);
uint16_t local_port(int fd);
int socket_buffer(int fd, int option);
// Absolute RAW-clock deadlines survive EINTR and partial progress.
void wait_ready(int fd, short events, uint64_t deadline);
void send_all(int fd, const uint8_t *data, size_t size, uint64_t deadline);
void recv_exact(int fd, uint8_t *data, size_t size, uint64_t deadline);
void send_datagram(int fd, const uint8_t *data, size_t size, uint64_t deadline,
                   const sockaddr *peer = nullptr, socklen_t peerlen = 0);
size_t recv_datagram(int fd, uint8_t *data, size_t capacity, uint64_t deadline,
                     sockaddr *peer = nullptr, socklen_t *peerlen = nullptr);
void pace_until(uint64_t deadline);
} // namespace nb
