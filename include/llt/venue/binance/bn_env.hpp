#pragma once

// Which exchange environment the process talks to.
//
// One place defines every hostname, so demo and production cannot be mixed by
// accident - pointing market data at demo while orders go to production would be a
// quiet and expensive mistake.
//
// The default is demo. Production has to be asked for by name.

namespace llt::binance {

struct Endpoints {
    const char* name;
    const char* rest;      // REST API
    const char* md_ws;     // market data streams
    const char* trade_ws;  // WebSocket API for order entry
    bool        is_production;
};

inline constexpr Endpoints kProduction{"production", "fapi.binance.com",
                                       "fstream.binance.com", "ws-fapi.binance.com", true};

// Verified 2026-10-28 by protocol probe, not by DNS: *.binance.com is a wildcard, so a
// hostname resolving proves nothing. demo-ws-fapi.binance.com looked plausible and
// redirects to the website; the demo WebSocket API is still on the old testnet host,
// which also serves the same 743-symbol book as demo-fapi against production's 936.
inline constexpr Endpoints kDemo{"demo", "demo-fapi.binance.com",
                                 "demo-fstream.binance.com",
                                 "testnet.binancefuture.com", false};

// Selected by LLT_BINANCE_ENV=production; demo otherwise, including when unset.
//
// This is the *trading* environment. Research tools that only read market data pick
// their source explicitly, because recording the demo book for a fortnight by accident
// would waste the fortnight.
[[nodiscard]] const Endpoints& active_env();

// "production" or "demo"; anything else returns demo.
[[nodiscard]] const Endpoints& env_by_name(const char* name);

}  // namespace llt::binance
