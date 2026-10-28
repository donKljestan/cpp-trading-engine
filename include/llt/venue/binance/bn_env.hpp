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

// Verified 2026-10-28: the old testnet.binancefuture.com now redirects here, and both
// serve the same 743-symbol demo book against production's 936.
inline constexpr Endpoints kDemo{"demo", "demo-fapi.binance.com",
                                 "demo-fstream.binance.com", "demo-ws-fapi.binance.com",
                                 false};

// Selected by LLT_BINANCE_ENV=production; demo otherwise, including when unset.
//
// This is the *trading* environment. Research tools that only read market data pick
// their source explicitly, because recording the demo book for a fortnight by accident
// would waste the fortnight.
[[nodiscard]] const Endpoints& active_env();

// "production" or "demo"; anything else returns demo.
[[nodiscard]] const Endpoints& env_by_name(const char* name);

}  // namespace llt::binance
