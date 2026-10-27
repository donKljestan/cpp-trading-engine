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

class MdFeed {
public:
    struct Config {
        std::vector<std::string> streams;  // e.g. "btcusdt@bookTicker"
        std::string              host       = kMdHost;
        uint16_t                 port       = 443;
        int                      timeout_ms = 10'000;
    };

    struct Stats {
        uint64_t frames        = 0;
        uint64_t book_tickers  = 0;
        uint64_t depths        = 0;
        uint64_t trades        = 0;
        uint64_t pings         = 0;
        uint64_t ignored       = 0;
        uint64_t decode_errors = 0;
    };

    enum class PollResult { Idle, Delivered, Disconnected, Error };

    // Subscribes with a SUBSCRIBE message rather than a /stream?streams= URL. The URL
    // form breaks down once the list is long, and this is the venue's documented path
    // for changing subscriptions on a live connection.
    [[nodiscard]] bool connect(const Config& cfg) {
        if (cfg.streams.empty()) return false;
        if (!ws_.connect(cfg.host.c_str(), cfg.port, "/ws", cfg.timeout_ms)) return false;

        std::string msg = R"({"method":"SUBSCRIBE","params":[)";
        for (size_t i = 0; i < cfg.streams.size(); ++i) {
            if (i != 0) msg += ',';
            msg += '"';
            msg += cfg.streams[i];
            msg += '"';
        }
        msg += R"(],"id":1})";

        return ws_.send_text(msg);
    }

    void close() noexcept { ws_.close(); }

    [[nodiscard]] bool         connected() const noexcept { return ws_.valid(); }
    [[nodiscard]] int          fd() const noexcept { return ws_.fd(); }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    [[nodiscard]] static const char* last_error() noexcept {
        return net::WsClient::last_error();
    }

    // Decodes at most one frame per call and hands it to the handler.
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
        return dispatch(payload, handler) ? PollResult::Delivered : PollResult::Idle;
    }

private:
    template <MdHandler H>
    bool dispatch(std::string_view payload, H& handler) {
        std::string_view stream;
        std::string_view data = payload;
        std::string_view kind;

        if (unwrap_combined(payload, stream, data)) {
            // Stream names are "<symbol>@<kind>[@<interval>]", or a bare "!kind" for
            // the all-market variants.
            const size_t at = stream.find('@');
            kind = (at == std::string_view::npos) ? stream : stream.substr(at + 1);
            if (!kind.empty() && kind.front() == '!') kind.remove_prefix(1);
        } else {
            std::string_view event_type;
            if (!json::find_field(data, "e", event_type)) {
                // Subscription acknowledgements carry "result" instead of an event type.
                std::string_view ignored;
                if (!json::find_field(data, "result", ignored)) ++stats_.ignored;
                return false;
            }
            kind = json::unquote(event_type);
        }

        if (kind.empty()) {
            ++stats_.ignored;
            return false;
        }
        const uint64_t tsc = ws_.last_recv_tsc();

        if (kind.starts_with("bookTicker")) {
            BookTicker bt{};
            if (!parse_book_ticker(data, tsc, bt)) {
                ++stats_.decode_errors;
                return false;
            }
            ++stats_.book_tickers;
            handler.on_book_ticker(bt);
            return true;
        }

        if (kind.starts_with("depth")) {
            DepthSnapshot d{};
            if (!parse_depth(data, tsc, d)) {
                ++stats_.decode_errors;
                return false;
            }
            ++stats_.depths;
            handler.on_depth(d);
            return true;
        }

        // As of 2026-10 the exchange acknowledges an @aggTrade subscription and then
        // never sends a frame, while @trade works. Both are routed here; @trade is
        // also the better input for trade-flow signals since prints are not merged.
        if (kind.starts_with("aggTrade") || kind.starts_with("trade")) {
            Trade t{};
            if (!parse_trade(data, tsc, t)) {
                ++stats_.decode_errors;
                return false;
            }
            ++stats_.trades;
            handler.on_trade(t);
            return true;
        }

        ++stats_.ignored;
        return false;
    }

    net::WsClient ws_;
    Stats         stats_;
};

}  // namespace llt::binance
