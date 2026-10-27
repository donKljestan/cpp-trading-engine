#include "llt/venue/binance/bn_rest.hpp"

#include <cstdio>

#include "llt/venue/json_scan.hpp"

namespace llt::binance {
namespace {

thread_local char t_last_error[256] = {};

void record(const char* what) noexcept {
    std::snprintf(t_last_error, sizeof(t_last_error), "%s", what);
}

void apply_filter(std::string_view filter, Instrument& inst) {
    std::string_view type;
    if (!json::find_field(filter, "filterType", type)) return;
    type = json::unquote(type);

    std::string_view value;

    if (type == "PRICE_FILTER") {
        if (json::find_field(filter, "tickSize", value)) {
            static_cast<void>(json::parse_decimal(value, inst.tick_size));
        }
    } else if (type == "LOT_SIZE") {
        if (json::find_field(filter, "stepSize", value)) {
            static_cast<void>(json::parse_decimal(value, inst.step_size));
        }
        if (json::find_field(filter, "minQty", value)) {
            static_cast<void>(json::parse_decimal(value, inst.min_qty));
        }
    } else if (type == "MIN_NOTIONAL") {
        if (json::find_field(filter, "notional", value)) {
            static_cast<void>(json::parse_decimal(value, inst.min_notional));
        }
    }
}

}  // namespace

bool parse_exchange_info(std::string_view json, std::vector<Instrument>& out) {
    std::string_view symbols;
    if (!json::find_field(json, "symbols", symbols)) {
        record("exchangeInfo has no symbols array");
        return false;
    }

    json::ArrayScanner entries(symbols);
    std::string_view   entry;

    while (entries.next(entry)) {
        Instrument       inst{};
        std::string_view status;
        std::string_view contract_type;
        std::string_view quote_asset;
        std::string_view filters;

        json::ObjectScanner fields(entry);
        std::string_view    key;
        std::string_view    value;
        while (fields.next(key, value)) {
            if (key == "symbol") {
                inst.symbol = Symbol(json::unquote(value));
            } else if (key == "status") {
                status = json::unquote(value);
            } else if (key == "contractType") {
                contract_type = json::unquote(value);
            } else if (key == "quoteAsset") {
                quote_asset = json::unquote(value);
            } else if (key == "filters") {
                filters = value;
            }
        }

        // Dated futures settle and disappear; only perpetuals are worth quoting.
        if (contract_type != "PERPETUAL" || quote_asset != "USDT") continue;

        json::ArrayScanner filter_list(filters);
        std::string_view   filter;
        while (filter_list.next(filter)) apply_filter(filter, inst);

        if (inst.symbol.empty() || inst.tick_size.is_zero() || inst.step_size.is_zero()) {
            continue;
        }

        inst.tradable = (status == "TRADING");
        out.push_back(inst);
    }

    if (out.empty()) {
        record("exchangeInfo contained no usable perpetuals");
        return false;
    }
    return true;
}

bool RestClient::connect(const char* host, uint16_t port, int timeout_ms) {
    return http_.connect(host, port, timeout_ms);
}

bool RestClient::fetch_instruments(std::vector<Instrument>& out, int timeout_ms) {
    net::HttpResponse resp;
    if (!http_.get("/fapi/v1/exchangeInfo", timeout_ms, resp)) {
        record(net::HttpClient::last_error());
        return false;
    }
    if (resp.status != 200) {
        std::snprintf(t_last_error, sizeof(t_last_error), "exchangeInfo returned HTTP %d",
                      resp.status);
        return false;
    }
    return parse_exchange_info(resp.body, out);
}

bool RestClient::server_time_ms(uint64_t& out, int timeout_ms) {
    net::HttpResponse resp;
    if (!http_.get("/fapi/v1/time", timeout_ms, resp)) {
        record(net::HttpClient::last_error());
        return false;
    }
    if (resp.status != 200) {
        std::snprintf(t_last_error, sizeof(t_last_error), "time returned HTTP %d",
                      resp.status);
        return false;
    }

    std::string_view value;
    if (!json::find_field(resp.body, "serverTime", value)) {
        record("no serverTime in response");
        return false;
    }
    return json::parse_uint(value, out);
}

const char* RestClient::last_error() noexcept {
    return t_last_error[0] != '\0' ? t_last_error : net::HttpClient::last_error();
}

}  // namespace llt::binance
