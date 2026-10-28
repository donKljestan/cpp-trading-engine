#include "llt/venue/binance/bn_env.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace llt::binance {

const Endpoints& env_by_name(const char* name) {
    return (name != nullptr && std::strcmp(name, "production") == 0) ? kProduction : kDemo;
}

const Endpoints& active_env() {
    return env_by_name(std::getenv("LLT_BINANCE_ENV"));
}

}  // namespace llt::binance
