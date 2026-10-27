#pragma once

// Non-blocking TCP client socket.

#include <cstdint>

namespace llt::net {

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket();

    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&)            = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    // Resolves and connects, blocking up to timeout_ms. On success the socket is
    // non-blocking with Nagle disabled: a quote that waits for a full segment is a
    // quote that arrives after the market has moved.
    [[nodiscard]] bool connect(const char* host, uint16_t port, int timeout_ms);

    void close() noexcept;

    [[nodiscard]] int  fd() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

private:
    int fd_ = -1;
};

}  // namespace llt::net
