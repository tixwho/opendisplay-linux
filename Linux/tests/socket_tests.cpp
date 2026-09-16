#include "opendisplay/socket.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>

namespace {

void makeNonblocking(const int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    assert(flags >= 0);
    assert(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

void readsDelayedDataFromNonblockingSocket() {
    int pair[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    makeNonblocking(pair[0]);
    od::Socket reader(pair[0]);
    std::thread writer([fd = pair[1]] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(::send(fd, "hello", 5, 0) == 5);
        ::close(fd);
    });

    std::array<char, 5> bytes{};
    assert(reader.readExact(bytes));
    assert(std::string(bytes.data(), bytes.size()) == "hello");
    writer.join();
}

void writesThroughNonblockingBackpressure() {
    int pair[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    makeNonblocking(pair[0]);
    const int sendBuffer = 4096;
    assert(::setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &sendBuffer,
                        sizeof(sendBuffer)) == 0);
    od::Socket writer(pair[0]);
    const std::string payload(1024 * 1024, 'x');
    std::thread reader([fd = pair[1], expected = payload.size()] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::array<char, 16 * 1024> buffer{};
        std::size_t received = 0;
        while (received < expected) {
            const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
            assert(count > 0);
            received += static_cast<std::size_t>(count);
        }
        ::close(fd);
    });

    assert(writer.writeAll(payload, std::chrono::seconds(5)));
    reader.join();
}

using Clock = std::chrono::steady_clock;

od::Socket stalledWriter(int pair[2]) {
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const int sendBuffer = 4096;
    assert(::setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &sendBuffer,
                        sizeof(sendBuffer)) == 0);
    return od::Socket(pair[0]);
}

void writeGivesUpAtDeadlineWhenPeerStopsReading() {
    int pair[2]{};
    od::Socket writer = stalledWriter(pair);
    const std::string payload(1024 * 1024, 'x');

    const auto started = Clock::now();
    assert(!writer.writeAll(payload, std::chrono::milliseconds(100)));
    const auto elapsed = Clock::now() - started;
    assert(elapsed >= std::chrono::milliseconds(100));
    assert(elapsed < std::chrono::seconds(1));
    ::close(pair[1]);
}

/// A peer that drains slowly keeps every send short and positive. The deadline
/// is total: progress must not restart it.
void writeGivesUpAtDeadlineWhilePeerDrainsSlowly() {
    int pair[2]{};
    od::Socket writer = stalledWriter(pair);
    const std::string payload(1024 * 1024, 'x');
    std::atomic<bool> done = false;
    std::size_t received = 0;
    std::thread reader([&, fd = pair[1]] {
        std::array<char, 1024> buffer{};
        while (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const auto count = ::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
            received += count > 0 ? static_cast<std::size_t>(count) : 0;
        }
    });

    const auto started = Clock::now();
    assert(!writer.writeAll(payload, std::chrono::milliseconds(100)));
    const auto elapsed = Clock::now() - started;
    done = true;
    reader.join();
    assert(elapsed >= std::chrono::milliseconds(100));
    assert(elapsed < std::chrono::seconds(1));
    assert(received > 0);
    assert(received < payload.size());
    ::close(pair[1]);
}

std::atomic<int> interrupts = 0;

void countInterrupt(int) { interrupts.fetch_add(1); }

/// Signals interrupt the wait with EINTR every few milliseconds. The wait must
/// resume with the remaining time, not the full timeout.
void writeGivesUpAtDeadlineAcrossInterruptedWaits() {
    struct sigaction action{};
    action.sa_handler = countInterrupt;
    assert(::sigaction(SIGUSR1, &action, nullptr) == 0);
    int pair[2]{};
    od::Socket writer = stalledWriter(pair);
    const std::string payload(1024 * 1024, 'x');
    std::atomic<bool> done = false;
    std::thread interrupter([&, target = ::pthread_self()] {
        for (int round = 0; round < 400 && !done; ++round) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ::pthread_kill(target, SIGUSR1);
        }
    });

    const auto started = Clock::now();
    assert(!writer.writeAll(payload, std::chrono::milliseconds(100)));
    const auto elapsed = Clock::now() - started;
    done = true;
    interrupter.join();
    assert(interrupts > 0);
    assert(elapsed >= std::chrono::milliseconds(100));
    assert(elapsed < std::chrono::seconds(1));
    ::close(pair[1]);
}

void zeroTimeoutSendsNothing() {
    int pair[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    od::Socket writer(pair[0]);
    assert(!writer.writeAll("data", std::chrono::milliseconds(0)));
    char byte = 0;
    assert(::recv(pair[1], &byte, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN);
    ::close(pair[1]);
}

void shutdownWakesBlockedWrite() {
    int pair[2]{};
    od::Socket writer = stalledWriter(pair);
    const std::string payload(1024 * 1024, 'x');
    bool written = true;
    std::thread sender([&] { written = writer.writeAll(payload, std::chrono::seconds(10)); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto started = Clock::now();
    writer.shutdown();
    sender.join();
    assert(!written);
    assert(Clock::now() - started < std::chrono::seconds(1));
    assert(writer.valid());
    ::close(pair[1]);
}

void shutdownWakesBlockedRead() {
    int pair[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    od::Socket reader(pair[0]);
    bool read = true;
    std::thread receiver([&] {
        std::array<char, 4> header{};
        read = reader.readExact(header);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto started = Clock::now();
    reader.shutdown();
    receiver.join();
    assert(!read);
    assert(Clock::now() - started < std::chrono::seconds(1));
    assert(reader.valid());
    ::close(pair[1]);
}

}  // namespace

int main() {
    readsDelayedDataFromNonblockingSocket();
    writesThroughNonblockingBackpressure();
    writeGivesUpAtDeadlineWhenPeerStopsReading();
    writeGivesUpAtDeadlineWhilePeerDrainsSlowly();
    writeGivesUpAtDeadlineAcrossInterruptedWaits();
    zeroTimeoutSendsNothing();
    shutdownWakesBlockedWrite();
    shutdownWakesBlockedRead();
}
