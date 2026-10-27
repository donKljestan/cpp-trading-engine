#pragma once

// TLS client stream over a connected TcpSocket.
//
// Certificate chain and hostname verification are always on and never configurable.
// This connection eventually carries signed order requests; a downgrade switch here
// would be the single most dangerous line in the codebase.

#include <cstddef>

#include "llt/net/tcp.hpp"

using SSL = struct ssl_st;

namespace llt::net {

enum class IoStatus {
    Ok,
    WouldBlock,
    Closed,
    Error,
};

class TlsStream {
public:
    TlsStream() = default;
    ~TlsStream();

    TlsStream(TlsStream&& other) noexcept;
    TlsStream& operator=(TlsStream&& other) noexcept;
    TlsStream(const TlsStream&)            = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    // Takes ownership of the socket and drives the handshake to completion,
    // blocking up to timeout_ms.
    [[nodiscard]] bool connect(TcpSocket&& sock, const char* host, int timeout_ms);

    void close() noexcept;

    [[nodiscard]] IoStatus read(void* buf, size_t cap, size_t& out) noexcept;
    [[nodiscard]] IoStatus write(const void* buf, size_t len, size_t& out) noexcept;

    [[nodiscard]] bool valid() const noexcept { return ssl_ != nullptr; }
    [[nodiscard]] int  fd() const noexcept { return sock_.fd(); }

    // Most recent failure on this thread.
    [[nodiscard]] static const char* last_error() noexcept;

private:
    TcpSocket sock_;
    SSL*      ssl_ = nullptr;
};

}  // namespace llt::net
