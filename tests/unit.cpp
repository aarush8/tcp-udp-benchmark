#include "netbench/common.hpp"
#include "netbench/metrics.hpp"
#include "netbench/protocol.hpp"
#include <fcntl.h>
#include <gtest/gtest.h>
#include <pthread.h>
#include <thread>
#include <unistd.h>
using namespace nb;
TEST(Protocol, WireRoundTripAndBounds) {
    auto bytes = encode({1048576, 0x123456789abcdef0ULL, 873, 874});
    auto h = decode(bytes, true);
    EXPECT_EQ(h.sequence, 0x123456789abcdef0ULL);
    EXPECT_EQ(fragment_size(h), 976u);
    EXPECT_THROW(decode(bytes, false), Error);
    bytes[0] = 0;
    EXPECT_THROW(decode(bytes, true), Error);
    EXPECT_THROW(decode(std::span(bytes.data(), 3), true), Error);
    EXPECT_THROW(decode(encode({63, 0, 0, 1}), false), Error);
}
TEST(Sequence, GapsDuplicatesAndReordering) {
    SequenceTracker s(5);
    EXPECT_TRUE(s.observe(3));
    EXPECT_TRUE(s.observe(1));
    EXPECT_FALSE(s.observe(3));
    EXPECT_EQ(s.missing(), 3u);
    EXPECT_EQ(s.reordered(), 1u);
    EXPECT_EQ(s.duplicates(), 1u);
    EXPECT_THROW(s.observe(5), Error);
}
struct Pair {
    Fd a, b;
    Pair() {
        int fd[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd) != 0)
            throw Error("socketpair");
        a = Fd(fd[0]);
        b = Fd(fd[1]);
    }
};
TEST(IO, FragmentedAndCoalescedTcpFrames) {
    Pair p;
    auto header = encode({64, 7, 0, 1});
    auto payload = payload_pattern(64);
    std::thread writer([&] {
        for (auto byte : header)
            send_all(p.a.get(), &byte, 1, now_ns() + 1000000000);
        send_all(p.a.get(), payload.data(), payload.size(), now_ns() + 1000000000);
        send_all(p.a.get(), header.data(), header.size(), now_ns() + 1000000000);
        send_all(p.a.get(), payload.data(), payload.size(), now_ns() + 1000000000);
    });
    for (int i = 0; i < 2; ++i) {
        std::array<uint8_t, header_size> got{};
        recv_exact(p.b.get(), got.data(), got.size(), now_ns() + 1000000000);
        EXPECT_EQ(decode(got, false).sequence, 7u);
        std::vector<uint8_t> body(64);
        recv_exact(p.b.get(), body.data(), body.size(), now_ns() + 1000000000);
        EXPECT_EQ(body, payload);
    }
    writer.join();
}
TEST(IO, PartialWritesBackpressure) {
    Pair p;
    Config c;
    c.protocol = "udp";
    c.sndbuf = 1024;
    configure(p.a.get(), c, true);
    auto data = payload_pattern(1048576);
    std::vector<uint8_t> got(data.size());
    std::thread reader(
        [&] { recv_exact(p.b.get(), got.data(), got.size(), now_ns() + 3000000000); });
    send_all(p.a.get(), data.data(), data.size(), now_ns() + 3000000000);
    reader.join();
    EXPECT_EQ(data, got);
}
TEST(IO, TimeoutsAndPartialEof) {
    Pair p;
    Config c;
    c.protocol = "udp";
    configure(p.b.get(), c, true);
    uint8_t byte = 42;
    EXPECT_THROW(recv_exact(p.b.get(), &byte, 1, now_ns() + 20000000), Timeout);
    send_all(p.a.get(), &byte, 1, now_ns() + 100000000);
    shutdown(p.a.get(), SHUT_WR);
    uint8_t buf[2];
    EXPECT_THROW(recv_exact(p.b.get(), buf, 2, now_ns() + 100000000), Closed);
}
TEST(IO, SendTimeoutAndClosedPeer) {
    Pair p;
    Config c;
    c.protocol = "udp";
    c.sndbuf = 1024;
    configure(p.a.get(), c, true);
    auto bytes = payload_pattern(1048576);
    EXPECT_THROW(send_all(p.a.get(), bytes.data(), bytes.size(), now_ns() + 20000000), Timeout);
    p.b = Fd();
    EXPECT_THROW(send_all(p.a.get(), bytes.data(), 1, now_ns() + 100000000), Error);
}
TEST(Resource, MoveOwnershipClosesOnce) {
    int raw;
    {
        Pair p;
        raw = p.a.get();
        Fd owned(std::move(p.a));
        EXPECT_EQ(p.a.get(), -1);
        EXPECT_NE(fcntl(raw, F_GETFD), -1);
    }
    EXPECT_EQ(fcntl(raw, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}
TEST(Metrics, MissingSamplesAreNull) {
    Config c;
    Stats s;
    s.errors = 1;
    auto json = result_json(c, 0, {s}, 1, .1);
    EXPECT_NE(json.find("\"rtt_mean_us\":null"), std::string::npos);
    EXPECT_NE(json.find("\"udp_echo_loss_percent\":null"), std::string::npos);
}
TEST(Metrics, EmptyRunIsNotSuccessful) {
    Config config;
    Stats empty;
    const auto json = result_json(config, 0, {empty}, 1, .1);
    EXPECT_NE(json.find("\"status\":\"no_samples\""), std::string::npos);
    EXPECT_NE(result_csv(config, 0, {empty}, 1, .1).find(",no_samples,"), std::string::npos);
}

namespace {
std::atomic<unsigned> interruptions{0};
void interrupt_wait(int) {
    interruptions.fetch_add(1, std::memory_order_relaxed);
}
} // namespace
TEST(IO, InterruptedRecvResumesWithoutLosingBytes) {
    Pair p;
    struct sigaction action{}, previous{};
    action.sa_handler = interrupt_wait;
    sigemptyset(&action.sa_mask);
    ASSERT_EQ(sigaction(SIGUSR1, &action, &previous), 0);
    uint8_t value = 0;
    std::exception_ptr failure;
    std::thread reader([&] {
        try {
            recv_exact(p.b.get(), &value, 1, now_ns() + 1000000000);
        } catch (...) {
            failure = std::current_exception();
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(pthread_kill(reader.native_handle(), SIGUSR1), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    uint8_t sent = 91;
    send_all(p.a.get(), &sent, 1, now_ns() + 1000000000);
    reader.join();
    EXPECT_FALSE(failure);
    EXPECT_EQ(value, sent);
    EXPECT_GE(interruptions.load(), 1u);
    EXPECT_EQ(sigaction(SIGUSR1, &previous, nullptr), 0);
}
TEST(IO, OversizedUdpDatagramIsRejected) {
    Config c;
    c.protocol = "udp";
    c.port = 0;
    auto receiver = make_socket(c, true);
    bind_socket(receiver.get(), c);
    c.port = local_port(receiver.get());
    auto sender = make_socket(c, true);
    connect_socket(sender.get(), c, now_ns() + 1000000000);
    std::vector<uint8_t> bytes(2048, 42);
    send_datagram(sender.get(), bytes.data(), bytes.size(), now_ns() + 1000000000);
    uint8_t small[16];
    EXPECT_THROW(recv_datagram(receiver.get(), small, sizeof(small), now_ns() + 1000000000), Error);
}
TEST(Metrics, NearestRankQuantilesAndCorrectGoodputUnits) {
    Config c;
    c.payload = 1000;
    Stats s;
    s.rtt_us = {1, 2, 3, 4, 100};
    s.completed = 5;
    s.attempted = 5;
    auto json = result_json(c, 0, {s}, 2, .5);
    EXPECT_NE(json.find("\"goodput_mbps\":0.02"), std::string::npos);
    EXPECT_NE(json.find("\"rtt_mean_us\":22"), std::string::npos);
    EXPECT_NE(json.find("\"rtt_median_us\":3"), std::string::npos);
    EXPECT_NE(json.find("\"rtt_p95_us\":100"), std::string::npos);
    EXPECT_NE(json.find("\"client_cpu_percent\":25"), std::string::npos);
}
