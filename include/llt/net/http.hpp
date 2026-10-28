#pragma once

// Minimal HTTPS/1.1 client with keep-alive.
//
// Not a general-purpose library: it exists so repeated REST calls reuse one TLS
// session. A fresh connection per request costs four round trips, which from Belgrade
// is over a second and makes any latency figure meaningless.

#include <cstdint>
#include <string>

#include "llt/net/tls.hpp"

namespace llt::net {

struct HttpResponse {
    int         status = 0;
    std::string body;
};

class HttpClient {
public:
    HttpClient() = default;

    HttpClient(const HttpClient&)            = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    [[nodiscard]] bool connect(const char* host, uint16_t port, int timeout_ms);
    void               close() noexcept;

    [[nodiscard]] bool valid() const noexcept { return tls_.valid(); }

    // Reconnects once and retries if the server dropped a kept-alive connection.
    // `extra_headers`, when given, must already be CRLF-terminated lines.
    [[nodiscard]] bool get(const char* path, int timeout_ms, HttpResponse& out,
                           const char* extra_headers = nullptr);

    [[nodiscard]] static const char* last_error() noexcept;

private:
    [[nodiscard]] bool send_request(const char* path, const char* extra_headers);
    [[nodiscard]] bool read_response(int timeout_ms, HttpResponse& out);
    [[nodiscard]] bool fill_until(uint64_t deadline_ns);

    TlsStream   tls_;
    std::string host_;
    uint16_t    port_    = 443;
    int         timeout_ = 10'000;
    std::string rx_;
};

}  // namespace llt::net
