#pragma once

// The contract every venue adapter satisfies.

#include "llt/venue/events.hpp"

namespace llt {

// Feeds are templates on their handler rather than virtual classes. Dispatch then
// costs nothing and the compiler can inline the strategy straight into the decode
// loop, which matters when the whole budget for a tick is a few hundred nanoseconds.
template <typename H>
concept MdHandler =
    requires(H& h, const BookTicker& bt, const DepthSnapshot& d, const Trade& t) {
        h.on_book_ticker(bt);
        h.on_depth(d);
        h.on_trade(t);
    };

}  // namespace llt
