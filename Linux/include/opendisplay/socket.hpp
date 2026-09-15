#pragma once

#include "opendisplay/types.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace od {

class Socket {
public:
    explicit Socket(int fd = -1);
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] int fd() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }
    int release();
    /// Wakes every thread blocked in readExact or writeAll on this socket; they return false.
    /// Keeps the descriptor open, so callers can still use it until they have joined.
    void shutdown();
    void close();
    bool readExact(std::span<char> destination);
    /// Sends every byte within a total deadline of `timeout`.
    bool writeAll(std::string_view bytes, std::chrono::milliseconds timeout);

private:
    int fd_ = -1;
};

Socket connectTcp(const std::string& host, std::uint16_t port);
Socket connectUsb(int deviceHandle, std::uint16_t port);

}  // namespace od
