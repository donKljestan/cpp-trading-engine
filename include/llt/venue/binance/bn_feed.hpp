#pragma once

// Binance USDⓈ-M futures market data feed: websocket in, decoded events out.

#include <string>
#include <string_view>
#include <vector>

#include "llt/net/ws.hpp"
#include "llt/venue/adapter.hpp"
#include "llt/venue/binance/bn_parse.hpp"

namespace llt::binance {

inline constexpr const char* kMdHost        = "fstream.binance.com";
inline constexpr const char* kMdHostTestnet = "stream.binancefuture.com";

struct FeedStats {
    uint64_t frames        = 0;
    uint64_t book_tickers  = 0;
    uint64_t depths        = 0;
    uint64_t trades        = 0;
    uint64_t pings         = 0;
    uint64_t ignored       = 0;
    uint64_t decode_errors = 0;
};

// The venue accepts a stream list either in the connect URL or as a message on a live
// connection. The message form is used because the URL overflows once the list grows.
[[nodiscard]] inline std::string subscribe_message(const std::vector<std::string>& streams,
                                                   int id) {
    std::string msg = R"({"method":"SUBSCRIBE","params":[)";
    for (size_t i = 0; i < streams.size(); ++i) {
        if (i != 0) msg += ',';
        msg += '"';
        msg += streams[i];
        msg += '"';
    }
    msg += R"(],"id":)";
    msg += std::to_string(id);
    msg += '}';
    return msg;
}

// Decodes one payload and hands it to the handler.
//
// Shared by the live feed and the replayer on purpose: recorded data must take exactly
// the same path as live data, or a backtest is measuring a different program.
template <MdHandler H>
[[nodiscard]] bool dispatch_payload(std::string_view payload, uint64_t recv_tsc,
                                    FeedStats& stats, H& handler) {
    std::string_view stream;
    std::string_view data = payload;
    std::string_view kind;

    if (unwrap_combined(payload, stream, data)) {
        // Stream names are "<symbol>@<kind>[@<interval>]", or a bare "!kind" for the
        // all-market variants.
        const size_t at = stream.find('@');
        kind = (at == std::string_view::npos) ? stream : stream.substr(at + 1);
        if (!kind.empty() && kind.front() == '!') kind.remove_prefix(1);
    } else {
        std::string_view event_type;
        if (!json::find_field(data, "e", event_type)) {
            // Subscription acknowledgements carry "result" instead of an event type.
            std::string_view ack;
            if (!json::find_field(data, "result", ack)) ++stats.ignored;
            return false;
        }
        kind = json::unquote(event_type);
    }

    if (kind.empty()) {
        ++stats.ignored;
        return false;
    }

    if (kind.starts_with("bookTicker")) {
        BookTicker bt{};
        if (!parse_book_ticker(data, recv_tsc, bt)) {
            ++stats.decode_errors;
            return false;
        }
        ++stats.book_tickers;
        handler.on_book_ticker(bt);
        return true;
    }

    if (kind.starts_with("depth")) {
        DepthSnapshot d{};
        if (!parse_depth(data, recv_tsc, d)) {
            ++stats.decode_errors;
            return false;
        }
        ++stats.depths;
        handler.on_depth(d);
        return true;
    }

    // As of 2026-10 the venue acknowledges an @aggTrade subscription and then never
    // sends a frame, while @trade works. Both are routed here.
    if (kind.starts_with("aggTrade") || kind.starts_with("trade")) {
        Trade t{};
        if (!parse_trade(data, recv_tsc, t)) {
            ++stats.decode_errors;
            return false;
        }
        ++stats.trades;
        handler.on_trade(t);
        return true;
    }

    ++stats.ignored;
    return false;
}

class MdFeed {
public:
    using Stats = FeedStats;

    struct Config {
        std::vector<std::string> streams;  // e.g. "btcusdt@bookTicker"
        std::string              host       = kMdHost;
        uint16_t                 port       = 443;
        int                      timeout_ms = 10'000;
    };

    enum class PollResult { Idle, Delivered, Disconnected, Error };

    [[nodiscard]] bool connect(const Config& cfg) {
        if (cfg.streams.empty()) return false;
        if (!ws_.connect(cfg.host.c_str(), cfg.port, "/ws", cfg.timeout_ms)) return false;
        return ws_.send_text(subscribe_message(cfg.streams, 1));
    }

    void close() noexcept { ws_.close(); }

    [[nodiscard]] bool             connected() const noexcept { return ws_.valid(); }
    [[nodiscard]] int              fd() const noexcept { return ws_.fd(); }
    [[nodiscard]] const FeedStats& stats() const noexcept { return stats_; }

    [[nodiscard]] static const char* last_error() noexcept {
        return net::WsClient::last_error();
    }

    template <MdHandler H>
    [[nodiscard]] PollResult poll(H& handler) {
        std::string_view payload;

        switch (ws_.poll(payload)) {
            case net::WsClient::Event::None:
                return PollResult::Idle;
            case net::WsClient::Event::Ping:
                ++stats_.pings;
                return PollResult::Idle;
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
        return dispatch_payload(payload, ws_.last_recv_tsc(), stats_, handler)
                   ? PollResult::Delivered
                   : PollResult::Idle;
    }

private:
    net::WsClient ws_;
    FeedStats     stats_;
};

}  // namespace llt::binance
