#pragma once

// The private event stream.
//
// The listen key is obtained over the already-authenticated WebSocket API rather than
// a signed REST call, so there is one credential path instead of two. The stream
// itself is a separate connection, because the venue delivers it that way.
//
// A listen key expires after sixty minutes of not being renewed. Losing it silently
// means the engine keeps quoting while believing its orders never fill.

#include <cstdint>
#include <string>
#include <string_view>

#include "llt/exec/ws_api.hpp"
#include "llt/net/ws.hpp"
#include "llt/venue/binance/bn_user.hpp"

namespace llt::exec {

template <typename H>
concept UserStreamHandler =
    requires(H& h, const binance::ExecReport& e, const binance::AccountUpdate& a) {
        h.on_exec_report(e);
        h.on_account_update(a);
    };

struct UserStreamStats {
    uint64_t frames         = 0;
    uint64_t exec_reports   = 0;
    uint64_t account_events = 0;
    uint64_t ignored        = 0;
    uint64_t decode_errors  = 0;
    uint64_t keepalives     = 0;
};

class UserStream {
public:
    enum class PollResult { Idle, Delivered, Disconnected, Error };

    // Renewed well inside the sixty-minute expiry, because a missed renewal is
    // indistinguishable from a quiet market until an order fills unnoticed.
    static constexpr uint64_t kKeepaliveIntervalNs = 20ULL * 60 * 1'000'000'000;

    [[nodiscard]] bool start(WsApiClient& api, const char* stream_host, uint64_t now_ns,
                             int timeout_ms = 10'000);
    void               close() noexcept;

    [[nodiscard]] bool due_for_keepalive(uint64_t now_ns) const noexcept {
        return started_ && now_ns - last_keepalive_ns_ >= kKeepaliveIntervalNs;
    }

    [[nodiscard]] bool keepalive(WsApiClient& api, uint64_t now_ns,
                                 int timeout_ms = 10'000);

    [[nodiscard]] bool connected() const noexcept { return ws_.valid(); }
    [[nodiscard]] const UserStreamStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const std::string&     listen_key() const noexcept { return listen_key_; }

    [[nodiscard]] static const char* last_error() noexcept;

    template <UserStreamHandler H>
    [[nodiscard]] PollResult poll(H& handler) {
        std::string_view payload;

        switch (ws_.poll(payload)) {
            case net::WsClient::Event::None:
            case net::WsClient::Event::Ping:
            case net::WsClient::Event::Pong:
                return PollResult::Idle;
            case net::WsClient::Event::Binary:
                ++stats_.ignored;
                return PollResult::Idle;
            case net::WsClient::Event::Close:
                return PollResult::Disconnected;
            case net::WsClient::Event::Error:
                return PollResult::Error;
            case net::WsClient::Event::Text:
                break;
        }

        ++stats_.frames;

        std::string_view type;
        if (!json::find_field(payload, "e", type)) {
            ++stats_.ignored;
            return PollResult::Idle;
        }
        const std::string_view event = json::unquote(type);

        if (event == "ORDER_TRADE_UPDATE") {
            binance::ExecReport report;
            if (!binance::parse_order_update(payload, report)) {
                ++stats_.decode_errors;
                return PollResult::Idle;
            }
            ++stats_.exec_reports;
            handler.on_exec_report(report);
            return PollResult::Delivered;
        }

        if (event == "ACCOUNT_UPDATE") {
            binance::AccountUpdate update;
            if (!binance::parse_account_update(payload, update)) {
                ++stats_.decode_errors;
                return PollResult::Idle;
            }
            ++stats_.account_events;
            handler.on_account_update(update);
            return PollResult::Delivered;
        }

        // listenKeyExpired, MARGIN_CALL and friends: not order flow, but worth seeing
        // in the counters rather than vanishing.
        ++stats_.ignored;
        return PollResult::Idle;
    }

private:
    net::WsClient   ws_;
    std::string     listen_key_;
    UserStreamStats stats_;
    uint64_t        last_keepalive_ns_ = 0;
    bool            started_           = false;
};

}  // namespace llt::exec
