#include "opendisplay/socket.hpp"

#include <usbmuxd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <thread>

namespace od {
namespace {

using Clock = std::chrono::steady_clock;

/// Waits for `events`, or forever without a deadline. A shutdown ends the wait with
/// POLLHUP, which reports false to the caller.
bool waitForSocket(const int fd, const short events, const std::optional<Clock::time_point> deadline) {
    pollfd descriptor{.fd = fd, .events = events, .revents = 0};
    for (;;) {
        int timeoutMs = -1;
        if (deadline) {
            const auto now = Clock::now();
            if (now >= *deadline) {
                return false;
            }
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(*deadline - now);
            timeoutMs = static_cast<int>(std::max<std::chrono::milliseconds::rep>(0, remaining.count()));
        }
        const int result = ::poll(&descriptor, 1, timeoutMs);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return result > 0 && (descriptor.revents & events) != 0;
    }
}

}  // namespace

Socket::Socket(const int fd) : fd_(fd) {}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : fd_(other.release()) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.release();
    }
    return *this;
}

int Socket::release() {
    const int result = fd_;
    fd_ = -1;
    return result;
}

void Socket::shutdown() {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
    }
}

void Socket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool Socket::readExact(const std::span<char> destination) {
    std::size_t offset = 0;
    while (offset < destination.size()) {
        const auto count = ::recv(fd_, destination.data() + offset, destination.size() - offset,
                                  MSG_DONTWAIT);
        if (count == 0) {
            return false;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (waitForSocket(fd_, POLLIN, std::nullopt)) {
                    continue;
                }
            }
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool Socket::writeAll(const std::string_view bytes, const std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        const auto count = ::send(fd_, bytes.data() + offset, bytes.size() - offset,
                                  MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (waitForSocket(fd_, POLLOUT, deadline)) {
                    continue;
                }
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

Socket connectTcp(const std::string& host, const std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    const int result = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (result != 0) {
        throw std::runtime_error("cannot resolve " + host + ": " + gai_strerror(result));
    }

    Socket connected;
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        Socket candidate(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
        if (!candidate.valid()) {
            continue;
        }
        const int enabled = 1;
        setsockopt(candidate.fd(), IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
        if (::connect(candidate.fd(), address->ai_addr, address->ai_addrlen) == 0) {
            connected = std::move(candidate);
            break;
        }
    }
    freeaddrinfo(addresses);
    if (!connected.valid()) {
        throw std::runtime_error("cannot connect to " + host + ":" + service);
    }
    return connected;
}

Socket connectUsb(const int deviceHandle, const std::uint16_t port) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    int result = -ENODEV;
    do {
        result = usbmuxd_connect(static_cast<std::uint32_t>(deviceHandle), port);
        if (result >= 0) {
            return Socket(result);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    } while (std::chrono::steady_clock::now() < deadline);

    std::string detail = "unknown libusbmuxd error";
    if (result < 0 && result >= -4095) {
        detail = std::strerror(-result);
    }
    throw std::runtime_error("usbmuxd could not connect to device port "
                             + std::to_string(port) + ": " + detail + " (error "
                             + std::to_string(result)
                             + "). Keep the receiver app open and verify `idevicepair validate`");
}

}  // namespace od
