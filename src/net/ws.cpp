#include "llt/net/ws.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <poll.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "llt/core/clock.hpp"

namespace llt::net {
namespace {

constexpr const char* kWsGuid    = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr size_t      kRxInitial = 64 * 1024;
constexpr size_t      kRxMax     = 8 * 1024 * 1024;

constexpr uint8_t kOpContinuation = 0x0;
constexpr uint8_t kOpText         = 0x1;
constexpr uint8_t kOpBinary       = 0x2;
constexpr uint8_t kOpClose        = 0x8;
constexpr uint8_t kOpPing         = 0x9;
constexpr uint8_t kOpPong         = 0xA;

thread_local char t_last_error[256] = {};

void record(const char* what) noexcept {
    std::snprintf(t_last_error, sizeof(t_last_error), "%s", what);
}

size_t base64(const unsigned char* src, size_t len, char* dst, size_t dst_cap) noexcept {
    const int n = ::EVP_EncodeBlock(reinterpret_cast<unsigned char*>(dst), src,
                                    static_cast<int>(len));
    if (n < 0 || static_cast<size_t>(n) >= dst_cap) return 0;
    return static_cast<size_t>(n);
}

// Sec-WebSocket-Accept = base64(sha1(client_key + GUID)).
bool compute_accept(const char* client_key, char* out, size_t out_cap) noexcept {
    char joined[128];
    const int joined_len = std::snprintf(joined, sizeof(joined), "%s%s", client_key, kWsGuid);
    if (joined_len <= 0) return false;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;
    if (::EVP_Digest(joined, static_cast<size_t>(joined_len), digest, &digest_len,
                     ::EVP_sha1(), nullptr) != 1) {
        return false;
    }
    return base64(digest, digest_len, out, out_cap) != 0;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
        const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
        if (ca != cb) return false;
    }
    return true;
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

}  // namespace

void WsClient::close() noexcept {
    tls_.close();
    rx_.clear();
    rx_begin_ = 0;
    rx_end_   = 0;
    frag_.clear();
    frag_opcode_ = 0;
}

const char* WsClient::last_error() noexcept {
    return t_last_error[0] != '\0' ? t_last_error : TlsStream::last_error();
}

void WsClient::compact() {
    if (rx_begin_ == 0) return;
    const size_t remaining = rx_end_ - rx_begin_;
    if (remaining != 0) std::memmove(rx_.data(), rx_.data() + rx_begin_, remaining);
    rx_begin_ = 0;
    rx_end_   = remaining;
}

void WsClient::reserve_more() {
    if (rx_.empty()) {
        rx_.resize(kRxInitial);
        return;
    }
    if (rx_end_ == rx_.size() && rx_.size() < kRxMax) {
        rx_.resize(std::min(rx_.size() * 2, kRxMax));
    }
}

bool WsClient::write_all(const void* data, size_t len) {
    const auto* p    = static_cast<const uint8_t*>(data);
    size_t      sent = 0;

    while (sent < len) {
        size_t         n  = 0;
        const IoStatus st = tls_.write(p + sent, len - sent, n);
        if (st == IoStatus::Ok) {
            sent += n;
            continue;
        }
        if (st == IoStatus::WouldBlock) {
            pollfd pfd{};
            pfd.fd     = tls_.fd();
            pfd.events = POLLOUT;
            if (::poll(&pfd, 1, 5000) != 1) {
                record("websocket write timed out");
                return false;
            }
            continue;
        }
        record("websocket write failed");
        return false;
    }
    return true;
}

bool WsClient::send_frame(uint8_t opcode, const void* data, size_t len) {
    uint8_t hdr[14];
    size_t  h = 0;

    hdr[h++] = static_cast<uint8_t>(0x80U | opcode);

    // Every client-to-server frame must be masked; servers drop unmasked ones.
    if (len < 126) {
        hdr[h++] = static_cast<uint8_t>(0x80U | len);
    } else if (len <= 0xFFFFU) {
        hdr[h++] = static_cast<uint8_t>(0x80U | 126U);
        hdr[h++] = static_cast<uint8_t>((len >> 8) & 0xFFU);
        hdr[h++] = static_cast<uint8_t>(len & 0xFFU);
    } else {
        hdr[h++] = static_cast<uint8_t>(0x80U | 127U);
        for (int shift = 56; shift >= 0; shift -= 8) {
            hdr[h++] = static_cast<uint8_t>((len >> shift) & 0xFFU);
        }
    }

    uint8_t key[4];
    if (::RAND_bytes(key, sizeof(key)) != 1) {
        record("RAND_bytes failed");
        return false;
    }
    std::memcpy(hdr + h, key, sizeof(key));
    h += sizeof(key);

    tx_.resize(h + len);
    std::memcpy(tx_.data(), hdr, h);

    const auto* src = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        tx_[h + i] = static_cast<uint8_t>(src[i] ^ key[i & 3U]);
    }

    return write_all(tx_.data(), tx_.size());
}

bool WsClient::send_text(std::string_view payload) {
    return send_frame(kOpText, payload.data(), payload.size());
}

bool WsClient::send_close() noexcept {
    const uint8_t normal[2] = {0x03, 0xE8};  // status 1000
    return send_frame(kOpClose, normal, sizeof(normal));
}

bool WsClient::read_handshake(const char* expected_accept, int timeout_ms) {
    const uint64_t deadline =
        llt::clk::mono_ns() + static_cast<uint64_t>(timeout_ms) * uint64_t{1'000'000};

    size_t header_end = 0;
    for (;;) {
        const std::string_view buf(reinterpret_cast<const char*>(rx_.data()), rx_end_);
        const size_t           pos = buf.find("\r\n\r\n");
        if (pos != std::string_view::npos) {
            header_end = pos + 4;
            break;
        }
        if (rx_end_ >= 32 * 1024) {
            record("handshake response too large");
            return false;
        }

        reserve_more();
        size_t         n  = 0;
        const IoStatus st = tls_.read(rx_.data() + rx_end_, rx_.size() - rx_end_, n);
        if (st == IoStatus::Ok) {
            rx_end_ += n;
            continue;
        }
        if (st == IoStatus::WouldBlock) {
            const uint64_t now = llt::clk::mono_ns();
            if (now >= deadline) {
                record("handshake timed out");
                return false;
            }
            pollfd pfd{};
            pfd.fd     = tls_.fd();
            pfd.events = POLLIN;
            static_cast<void>(
                ::poll(&pfd, 1, static_cast<int>((deadline - now) / 1'000'000ULL) + 1));
            continue;
        }
        record("connection closed during handshake");
        return false;
    }

    const std::string_view head(reinterpret_cast<const char*>(rx_.data()), header_end);

    const size_t eol = head.find("\r\n");
    if (head.substr(0, eol).find("101") == std::string_view::npos) {
        std::snprintf(t_last_error, sizeof(t_last_error), "handshake rejected: %.*s",
                      static_cast<int>(eol), head.data());
        return false;
    }

    bool   accept_ok = false;
    size_t cursor    = eol + 2;
    while (cursor < head.size()) {
        const size_t line_end = head.find("\r\n", cursor);
        if (line_end == std::string_view::npos || line_end == cursor) break;

        const std::string_view line  = head.substr(cursor, line_end - cursor);
        const size_t           colon = line.find(':');
        if (colon != std::string_view::npos &&
            iequals(trim(line.substr(0, colon)), "sec-websocket-accept")) {
            accept_ok = trim(line.substr(colon + 1)) == expected_accept;
        }
        cursor = line_end + 2;
    }

    if (!accept_ok) {
        record("Sec-WebSocket-Accept mismatch");
        return false;
    }

    // Anything past the header is already frame data and must be kept.
    rx_begin_ = header_end;
    return true;
}

bool WsClient::connect(const char* host, uint16_t port, const char* path, int timeout_ms) {
    close();
    t_last_error[0] = '\0';

    TcpSocket sock;
    if (!sock.connect(host, port, timeout_ms)) {
        record("tcp connect failed");
        return false;
    }
    if (!tls_.connect(std::move(sock), host, timeout_ms)) return false;

    unsigned char nonce[16];
    if (::RAND_bytes(nonce, sizeof(nonce)) != 1) {
        record("RAND_bytes failed");
        return false;
    }

    char key_b64[32] = {};
    if (base64(nonce, sizeof(nonce), key_b64, sizeof(key_b64)) == 0) {
        record("base64 failed");
        return false;
    }

    char expected_accept[64] = {};
    if (!compute_accept(key_b64, expected_accept, sizeof(expected_accept))) {
        record("accept digest failed");
        return false;
    }

    char      request[1024];
    const int len = std::snprintf(request, sizeof(request),
                                  "GET %s HTTP/1.1\r\n"
                                  "Host: %s\r\n"
                                  "Upgrade: websocket\r\n"
                                  "Connection: Upgrade\r\n"
                                  "Sec-WebSocket-Key: %s\r\n"
                                  "Sec-WebSocket-Version: 13\r\n"
                                  "\r\n",
                                  path, host, key_b64);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(request)) {
        record("handshake request too long");
        return false;
    }

    reserve_more();
    if (!write_all(request, static_cast<size_t>(len))) return false;

    return read_handshake(expected_accept, timeout_ms);
}

WsClient::Parse WsClient::parse_frame(Event& ev, std::string_view& payload) {
    const size_t avail = rx_end_ - rx_begin_;
    if (avail < 2) return Parse::NeedMore;

    const uint8_t* p      = rx_.data() + rx_begin_;
    const bool     fin    = (p[0] & 0x80U) != 0;
    const uint8_t  opcode = static_cast<uint8_t>(p[0] & 0x0FU);
    const bool     masked = (p[1] & 0x80U) != 0;

    uint64_t len = p[1] & 0x7FU;
    size_t   hdr = 2;

    if (len == 126) {
        if (avail < hdr + 2) return Parse::NeedMore;
        len = (static_cast<uint64_t>(p[2]) << 8) | p[3];
        hdr += 2;
    } else if (len == 127) {
        if (avail < hdr + 8) return Parse::NeedMore;
        len = 0;
        for (size_t i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
        hdr += 8;
    }

    if (len > kRxMax) {
        record("frame exceeds maximum size");
        return Parse::Protocol;
    }

    const bool is_control = (opcode & 0x08U) != 0;
    if (is_control && (len > 125 || !fin)) {
        record("fragmented or oversized control frame");
        return Parse::Protocol;
    }

    size_t mask_off = 0;
    if (masked) {
        if (avail < hdr + 4) return Parse::NeedMore;
        mask_off = hdr;
        hdr += 4;
    }

    if (avail < hdr + len) return Parse::NeedMore;

    uint8_t* body = rx_.data() + rx_begin_ + hdr;
    if (masked) {
        const uint8_t* key = rx_.data() + rx_begin_ + mask_off;
        for (uint64_t i = 0; i < len; ++i) {
            body[i] = static_cast<uint8_t>(body[i] ^ key[i & 3U]);
        }
    }
    rx_begin_ += hdr + static_cast<size_t>(len);

    const auto as_view = [](const uint8_t* d, uint64_t n) {
        return std::string_view(reinterpret_cast<const char*>(d), static_cast<size_t>(n));
    };

    switch (opcode) {
        case kOpPing:
            if (!send_frame(kOpPong, body, static_cast<size_t>(len))) return Parse::Protocol;
            ev      = Event::Ping;
            payload = as_view(body, len);
            return Parse::Frame;

        case kOpPong:
            ev      = Event::Pong;
            payload = as_view(body, len);
            return Parse::Frame;

        case kOpClose:
            ev      = Event::Close;
            payload = as_view(body, len);
            return Parse::Frame;

        case kOpText:
        case kOpBinary:
            if (fin) {
                ev      = (opcode == kOpText) ? Event::Text : Event::Binary;
                payload = as_view(body, len);
                return Parse::Frame;
            }
            frag_.assign(body, body + len);
            frag_opcode_ = opcode;
            return Parse::NeedMore;

        case kOpContinuation:
            if (frag_opcode_ == 0) {
                record("continuation without an initial frame");
                return Parse::Protocol;
            }
            frag_.insert(frag_.end(), body, body + len);
            if (!fin) return Parse::NeedMore;
            ev           = (frag_opcode_ == kOpText) ? Event::Text : Event::Binary;
            payload      = as_view(frag_.data(), frag_.size());
            frag_opcode_ = 0;
            return Parse::Frame;

        default:
            record("unknown websocket opcode");
            return Parse::Protocol;
    }
}

WsClient::Event WsClient::poll(std::string_view& payload) {
    payload = {};
    if (!tls_.valid()) return Event::Error;

    for (;;) {
        Event ev = Event::None;
        switch (parse_frame(ev, payload)) {
            case Parse::Frame:
                return ev;
            case Parse::Protocol:
                return Event::Error;
            case Parse::NeedMore:
                break;
        }

        compact();
        reserve_more();
        if (rx_end_ == rx_.size()) {
            record("receive buffer exhausted");
            return Event::Error;
        }

        size_t         n  = 0;
        const IoStatus st = tls_.read(rx_.data() + rx_end_, rx_.size() - rx_end_, n);
        switch (st) {
            case IoStatus::Ok:
                if (n == 0) return Event::None;
                last_recv_tsc_ = llt::clk::rdtsc();
                rx_end_ += n;
                break;
            case IoStatus::WouldBlock:
                return Event::None;
            case IoStatus::Closed:
                return Event::Close;
            case IoStatus::Error:
                return Event::Error;
        }
    }
}

}  // namespace llt::net
