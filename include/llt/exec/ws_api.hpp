#pragma once

// Binance WebSocket API client: request/response over a persistent connection.
//
// Why not REST. After session.logon the connection is authenticated, so an order
// carries no signature and no apiKey header. That removes a signing step and a set of
// headers from every order, and keeps one warm TLS session instead of paying for a
// handshake. From Tokyo that is the difference between a few hundred microseconds and
// a few milliseconds per order.

#include <cstdint>
#include <string>
#include <string_view>

#include "llt/exec/signer.hpp"
#include "llt/net/ws.hpp"

namespace llt::exec {

struct WsApiResponse {
    std::string id;
    int         status = 0;
    std::string result;  // raw JSON, object or array
    int         error_code = 0;
    std::string error_msg;

    [[nodiscard]] bool ok() const noexcept { return status == 200; }
};

class WsApiClient {
public:
    WsApiClient() = default;

    WsApiClient(const WsApiClient&)            = delete;
    WsApiClient& operator=(const WsApiClient&) = delete;

    [[nodiscard]] bool connect(const char* host, int timeout_ms = 10'000);
    void               close() noexcept;

    // Authenticates the session. Every later request on this connection is implicitly
    // signed by having survived this one.
    [[nodiscard]] bool logon(const Credentials& cred, int timeout_ms = 10'000);

    // Blocking round trip. The order path will use an async form; this is for setup,
    // queries and reconciliation, where simplicity matters more than latency.
    [[nodiscard]] bool request(std::string_view method, std::string_view params_json,
                               int timeout_ms, WsApiResponse& out);

    [[nodiscard]] bool connected() const noexcept { return ws_.valid(); }
    [[nodiscard]] bool authenticated() const noexcept { return authenticated_; }

    [[nodiscard]] static const char* last_error() noexcept;

private:
    [[nodiscard]] bool wait_readable(int timeout_ms) noexcept;

    net::WsClient ws_;
    uint64_t      next_id_       = 1;
    bool          authenticated_ = false;
};

}  // namespace llt::exec
