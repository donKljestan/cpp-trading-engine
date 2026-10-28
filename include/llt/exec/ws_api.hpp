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

#include "llt/core/clock.hpp"
#include "llt/exec/signer.hpp"
#include "llt/net/ws.hpp"

namespace llt::exec {

// Builds a JSON params object.
//
// Values are inserted without escaping, which is safe for the only things that go in
// one: symbols, enum names and formatted numbers. Nothing here ever carries free text.
class Params {
public:
    Params& s(std::string_view key, std::string_view value) {
        separate();
        buf_ += '"';
        buf_ += key;
        buf_ += "\":\"";
        buf_ += value;
        buf_ += '"';
        return *this;
    }

    Params& n(std::string_view key, long long value) {
        separate();
        buf_ += '"';
        buf_ += key;
        buf_ += "\":";
        buf_ += std::to_string(value);
        return *this;
    }

    [[nodiscard]] std::string finish() const { return "{" + buf_ + "}"; }

    // Authenticated requests still need a timestamp after session.logon: the session
    // proves who is asking, the timestamp stops an old request being replayed.
    [[nodiscard]] std::string finish_stamped() const {
        Params copy = *this;
        copy.n("timestamp", static_cast<long long>(llt::clk::wall_ns() / 1'000'000));
        return copy.finish();
    }

private:
    void separate() {
        if (!buf_.empty()) buf_ += ',';
    }

    std::string buf_;
};

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
