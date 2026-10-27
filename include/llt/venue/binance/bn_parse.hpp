#pragma once

// Binance USDⓈ-M futures stream decoding.
//
// Header-only and single-pass, because this sits directly on the hot path: every
// microsecond here is a microsecond the quote is stale.

#include <string_view>

#include "llt/venue/events.hpp"
#include "llt/venue/json_scan.hpp"

namespace llt::binance {

// Splits {"stream":"btcusdt@bookTicker","data":{...}} into its two parts.
// Returns false for a raw (non-combined) payload, which the caller can decode directly.
[[nodiscard]] inline bool unwrap_combined(std::string_view json, std::string_view& stream,
                                          std::string_view& data) noexcept {
    json::ObjectScanner sc(json);
    std::string_view    key;
    std::string_view    value;

    bool have_stream = false;
    bool have_data   = false;

    while (sc.next(key, value)) {
        if (key == "stream") {
            stream      = json::unquote(value);
            have_stream = true;
        } else if (key == "data") {
            data      = value;
            have_data = true;
        }
    }
    return have_stream && have_data;
}

// {"e":"bookTicker","u":..,"s":"BTCUSDT","b":"..","B":"..","a":"..","A":"..","T":..,"E":..}
[[nodiscard]] inline bool parse_book_ticker(std::string_view json, uint64_t recv_tsc,
                                            BookTicker& out) noexcept {
    json::ObjectScanner sc(json);
    std::string_view    key;
    std::string_view    value;

    enum : uint32_t {
        kSym   = 1U << 0,
        kBidPx = 1U << 1,
        kBidQ  = 1U << 2,
        kAskPx = 1U << 3,
        kAskQ  = 1U << 4,
    };
    constexpr uint32_t kRequired = kSym | kBidPx | kBidQ | kAskPx | kAskQ;

    uint32_t seen = 0;
    while (sc.next(key, value)) {
        if (key.size() != 1) continue;
        switch (key[0]) {
            case 's':
                out.symbol = Symbol(json::unquote(value));
                seen |= kSym;
                break;
            case 'b':
                if (json::parse_decimal(value, out.bid_px)) seen |= kBidPx;
                break;
            case 'B':
                if (json::parse_decimal(value, out.bid_qty)) seen |= kBidQ;
                break;
            case 'a':
                if (json::parse_decimal(value, out.ask_px)) seen |= kAskPx;
                break;
            case 'A':
                if (json::parse_decimal(value, out.ask_qty)) seen |= kAskQ;
                break;
            case 'u':
                static_cast<void>(json::parse_uint(value, out.update_id));
                break;
            case 'E':
                static_cast<void>(json::parse_uint(value, out.time.exchange_ms));
                break;
            case 'T':
                static_cast<void>(json::parse_uint(value, out.time.transact_ms));
                break;
            default:
                break;
        }
    }

    out.time.recv_tsc = recv_tsc;
    return seen == kRequired;
}

// Partial depth stream, e.g. btcusdt@depth20@100ms.
[[nodiscard]] inline bool parse_depth(std::string_view json, uint64_t recv_tsc,
                                      DepthSnapshot& out) noexcept {
    json::ObjectScanner sc(json);
    std::string_view    key;
    std::string_view    value;

    bool have_symbol = false;
    bool have_bids   = false;
    bool have_asks   = false;

    out.n_bids = 0;
    out.n_asks = 0;

    const auto fill = [](std::string_view arr, PriceLevel* dst, uint8_t& count) {
        json::PairArrayScanner levels(arr);
        size_t                 n = 0;
        Fixed                  px;
        Fixed                  qty;
        while (n < DepthSnapshot::kMaxLevels && levels.next(px, qty)) {
            dst[n].px  = px;
            dst[n].qty = qty;
            ++n;
        }
        count = static_cast<uint8_t>(n);
    };

    while (sc.next(key, value)) {
        if (key == "s") {
            out.symbol  = Symbol(json::unquote(value));
            have_symbol = true;
        } else if (key == "b") {
            fill(value, out.bids, out.n_bids);
            have_bids = true;
        } else if (key == "a") {
            fill(value, out.asks, out.n_asks);
            have_asks = true;
        } else if (key == "U") {
            static_cast<void>(json::parse_uint(value, out.first_update_id));
        } else if (key == "u") {
            static_cast<void>(json::parse_uint(value, out.final_update_id));
        } else if (key == "pu") {
            static_cast<void>(json::parse_uint(value, out.prev_final_id));
        } else if (key == "E") {
            static_cast<void>(json::parse_uint(value, out.time.exchange_ms));
        } else if (key == "T") {
            static_cast<void>(json::parse_uint(value, out.time.transact_ms));
        }
    }

    out.time.recv_tsc = recv_tsc;
    return have_symbol && have_bids && have_asks;
}

// Handles both shapes, which differ only in the id field:
//   @trade     {"e":"trade","t":<tradeId>,"s":..,"p":..,"q":..,"T":..,"m":..}
//   @aggTrade  {"e":"aggTrade","a":<aggId>,"s":..,"p":..,"q":..,"T":..,"m":..}
[[nodiscard]] inline bool parse_trade(std::string_view json, uint64_t recv_tsc,
                                      Trade& out) noexcept {
    json::ObjectScanner sc(json);
    std::string_view    key;
    std::string_view    value;

    enum : uint32_t { kSym = 1U << 0, kPx = 1U << 1, kQty = 1U << 2, kMaker = 1U << 3 };
    constexpr uint32_t kRequired = kSym | kPx | kQty | kMaker;

    uint32_t seen = 0;
    while (sc.next(key, value)) {
        if (key.size() != 1) continue;
        switch (key[0]) {
            case 's':
                out.symbol = Symbol(json::unquote(value));
                seen |= kSym;
                break;
            case 'p':
                if (json::parse_decimal(value, out.px)) seen |= kPx;
                break;
            case 'q':
                if (json::parse_decimal(value, out.qty)) seen |= kQty;
                break;
            case 'm':
                if (json::parse_bool(value, out.buyer_is_maker)) seen |= kMaker;
                break;
            case 'a':
            case 't':
                static_cast<void>(json::parse_uint(value, out.id));
                break;
            case 'E':
                static_cast<void>(json::parse_uint(value, out.time.exchange_ms));
                break;
            case 'T':
                static_cast<void>(json::parse_uint(value, out.time.transact_ms));
                break;
            default:
                break;
        }
    }

    out.time.recv_tsc = recv_tsc;
    return seen == kRequired;
}

}  // namespace llt::binance
