#pragma once

// Minimal RFC 6455 WebSocket client, enough for Binance market data and order entry.
//
// Scope is deliberately narrow: client role only, no extensions, no permessage-deflate.
// Compression would add a variable-latency decompress step to the hot path, which is a
// bad trade when the payloads are a few hundred bytes of JSON.

#include <cstdint>
#include <string_view>
#include <vector>

#include "llt/net/tls.hpp"

namespace llt::net {

class WsClient {
public:
    enum class Event {
        None,  // nothing ready yet; caller should poll the fd and come back
        Text,
        Binary,
        Ping,
        Pong,
        Close,
        Error,
    };

    WsClient() = default;

    WsClient(const WsClient&)            = delete;
    WsClient& operator=(const WsClient&) = delete;

    [[nodiscard]] bool connect(const char* host, uint16_t port, const char* path,
                               int timeout_ms);
    void close() noexcept;

    // Returns at most one frame per call. `payload` points into an internal buffer and
    // is invalidated by the next call. Ping frames are answered before being surfaced,
    // so the caller may ignore them.
    [[nodiscard]] Event poll(std::string_view& payload);

    [[nodiscard]] bool send_text(std::string_view payload);
    [[nodiscard]] bool send_close() noexcept;

    [[nodiscard]] bool valid() const noexcept { return tls_.valid(); }
    [[nodiscard]] int  fd() const noexcept { return tls_.fd(); }

    [[nodiscard]] static const char* last_error() noexcept;

private:
    enum class Parse { NeedMore, Frame, Protocol };

    Parse                     parse_frame(Event& ev, std::string_view& payload);
    [[nodiscard]] bool        fill_once(bool& closed);
    void                      compact();
    void                      reserve_more();
    [[nodiscard]] bool        send_frame(uint8_t opcode, const void* data, size_t len);
    [[nodiscard]] bool        write_all(const void* data, size_t len);
    [[nodiscard]] bool        read_handshake(const char* expected_accept, int timeout_ms);

    TlsStream            tls_;
    std::vector<uint8_t> rx_;
    size_t               rx_begin_ = 0;
    size_t               rx_end_   = 0;

    std::vector<uint8_t> frag_;
    uint8_t              frag_opcode_ = 0;

    std::vector<uint8_t> tx_;
};

}  // namespace llt::net
