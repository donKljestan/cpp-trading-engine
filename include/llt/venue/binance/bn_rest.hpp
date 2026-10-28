#pragma once

// Binance USDⓈ-M futures REST endpoints.
//
// Only the public, unauthenticated calls live here. Signed requests belong to the
// execution plane and deliberately do not share this class.

#include <cstdint>
#include <string_view>
#include <vector>

#include "llt/net/http.hpp"
#include "llt/venue/binance/bn_env.hpp"
#include "llt/venue/types.hpp"

namespace llt::binance {

// Extracts tradable perpetuals from an exchangeInfo payload. Split out from the
// client so it can be tested against a fixture without a network.
[[nodiscard]] bool parse_exchange_info(std::string_view json, std::vector<Instrument>& out);

// Rolling 24h statistics, used to rank symbols rather than to price anything.
struct Ticker24h {
    Symbol   symbol;
    Fixed    last_price;
    double   quote_volume = 0.0;  // a double because 24h turnover overflows 1e-8 fixed point
    uint64_t trade_count  = 0;
};

[[nodiscard]] bool parse_24h_tickers(std::string_view json, std::vector<Ticker24h>& out);

class RestClient {
public:
    [[nodiscard]] bool connect(const char* host = nullptr, uint16_t port = 443,
                               int timeout_ms = 10'000);
    void               close() noexcept { http_.close(); }

    [[nodiscard]] bool fetch_instruments(std::vector<Instrument>& out,
                                         int                      timeout_ms = 20'000);
    [[nodiscard]] bool fetch_24h_tickers(std::vector<Ticker24h>& out,
                                         int                     timeout_ms = 20'000);
    [[nodiscard]] bool server_time_ms(uint64_t& out, int timeout_ms = 10'000);

    [[nodiscard]] static const char* last_error() noexcept;

private:
    net::HttpClient http_;
};

}  // namespace llt::binance
