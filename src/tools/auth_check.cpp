// Proves a credential works before anything is built on top of it.
//
// Sends one signed, read-only request and reports what came back. No order is placed,
// and nothing is written anywhere.
//
//   auth_check            against the demo environment
//   LLT_BINANCE_ENV=production auth_check
//
// Requires:
//   LLT_BINANCE_API_KEY      the key id from the exchange
//   LLT_BINANCE_PRIVATE_KEY  path to the Ed25519 PEM, not the key itself

#include <cstdio>
#include <string>

#include "llt/core/clock.hpp"
#include "llt/exec/signer.hpp"
#include "llt/exec/ws_api.hpp"
#include "llt/net/http.hpp"
#include "llt/venue/binance/bn_env.hpp"
#include "llt/venue/json_scan.hpp"

namespace {

// Percent-encodes everything outside the unreserved set. A base64 signature contains
// '+', '/' and '=', all of which change meaning in a query string.
std::string url_encode(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string           out;
    out.reserve(s.size() * 3);

    for (const char ch : s) {
        const auto c          = static_cast<unsigned char>(ch);
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

bool signed_get(llt::net::HttpClient& http, const llt::exec::Credentials& cred,
                const std::string& endpoint, const std::string& params,
                llt::net::HttpResponse& out) {
    std::string query = params;
    if (!query.empty()) query += '&';
    query += "timestamp=" + std::to_string(llt::clk::wall_ns() / 1'000'000);
    query += "&recvWindow=5000";

    std::string signature;
    if (!cred.signer.sign(query, signature)) {
        std::fprintf(stderr, "signing failed: %s\n",
                     llt::exec::Ed25519Signer::last_error());
        return false;
    }

    const std::string path =
        endpoint + "?" + query + "&signature=" + url_encode(signature);
    const std::string header = "X-MBX-APIKEY: " + cred.api_key + "\r\n";

    return http.get(path.c_str(), 10'000, out, header.c_str());
}

}  // namespace

int main() {
    llt::clk::clock_init();

    const auto& env = llt::binance::active_env();
    std::printf("environment  %s (%s)\n", env.name, env.rest);

    llt::exec::Credentials cred;
    std::string            error;
    if (!llt::exec::Credentials::from_env(cred, error)) {
        std::fprintf(stderr, "\ncredentials not usable: %s\n", error.c_str());
        std::fprintf(stderr, "\n  export LLT_BINANCE_API_KEY=<key id>\n");
        std::fprintf(stderr, "  export LLT_BINANCE_PRIVATE_KEY=$HOME/path/to/private.pem\n");
        return 1;
    }

    std::string fingerprint;
    static_cast<void>(cred.signer.public_key_hex(fingerprint));
    std::printf("api key      %.8s... (%zu chars)\n", cred.api_key.c_str(),
                cred.api_key.size());
    std::printf("public key   %s\n\n", fingerprint.c_str());

    llt::net::HttpClient http;
    if (!http.connect(env.rest, 443, 10'000)) {
        std::fprintf(stderr, "connect failed: %s\n", llt::net::HttpClient::last_error());
        return 1;
    }

    // Unsigned first: separates "the network is broken" from "the key is rejected".
    llt::net::HttpResponse resp;
    if (!http.get("/fapi/v1/time", 10'000, resp) || resp.status != 200) {
        std::fprintf(stderr, "public endpoint failed (HTTP %d): %s\n", resp.status,
                     resp.body.c_str());
        return 1;
    }
    std::printf("public endpoint   ok\n");

    // Newer deployments serve v3; older ones only v2.
    bool ok = false;
    for (const char* endpoint : {"/fapi/v3/balance", "/fapi/v2/balance"}) {
        if (!signed_get(http, cred, endpoint, "", resp)) return 1;
        if (resp.status == 200) {
            std::printf("signed endpoint   ok  (%s)\n\n", endpoint);
            ok = true;
            break;
        }
        if (resp.status != 404) break;
    }

    if (!ok) {
        std::fprintf(stderr, "\nsigned request rejected, HTTP %d\n", resp.status);
        std::fprintf(stderr, "%s\n\n", resp.body.c_str());
        std::fprintf(stderr, "common causes:\n");
        std::fprintf(stderr, "  -2014  the api key id does not match this environment\n");
        std::fprintf(stderr, "  -1022  the public key uploaded is not this private key\n");
        std::fprintf(stderr, "  -1021  local clock is outside recvWindow; check chrony\n");
        std::fprintf(stderr, "  -2015  key is IP-restricted and this host is not allowed\n");
        return 1;
    }

    // Only the funded assets, since a demo account lists many at zero.
    llt::json::ArrayScanner entries(resp.body);
    std::string_view        entry;
    int                     shown = 0;

    std::printf("%-10s %18s %18s\n", "asset", "balance", "available");
    while (entries.next(entry)) {
        std::string_view asset;
        std::string_view balance;
        std::string_view available;
        static_cast<void>(llt::json::find_field(entry, "asset", asset));
        if (!llt::json::find_field(entry, "balance", balance)) {
            static_cast<void>(llt::json::find_field(entry, "walletBalance", balance));
        }
        static_cast<void>(llt::json::find_field(entry, "availableBalance", available));

        llt::Fixed amount;
        if (!llt::json::parse_decimal(balance, amount) || amount.raw == 0) continue;

        std::printf("%-10.*s %18.*s %18.*s\n",
                    static_cast<int>(llt::json::unquote(asset).size()),
                    llt::json::unquote(asset).data(),
                    static_cast<int>(llt::json::unquote(balance).size()),
                    llt::json::unquote(balance).data(),
                    static_cast<int>(llt::json::unquote(available).size()),
                    llt::json::unquote(available).data());
        ++shown;
    }

    if (shown == 0) {
        std::printf("  (no funded assets - the demo account may need topping up in the UI)\n");
    }

    // The order path uses the WebSocket API, not REST, so a working REST credential is
    // only half the answer.
    std::printf("\n-- websocket api (%s) --\n", env.trade_ws);

    llt::exec::WsApiClient ws;
    if (!ws.connect(env.trade_ws)) {
        std::fprintf(stderr, "connect failed: %s\n", llt::exec::WsApiClient::last_error());
        return 1;
    }
    std::printf("connected         ok\n");

    if (!ws.logon(cred)) {
        std::fprintf(stderr, "session.logon failed: %s\n",
                     llt::exec::WsApiClient::last_error());
        return 1;
    }
    std::printf("session.logon     ok\n");

    // Proves the session is genuinely authenticated: this call carries no signature.
    llt::exec::WsApiResponse ws_resp;
    if (ws.request("v2/account.balance", "{}", 10'000, ws_resp) && ws_resp.ok()) {
        std::printf("unsigned query    ok  (session is authenticated)\n");
    } else if (ws.request("account.balance", "{}", 10'000, ws_resp) && ws_resp.ok()) {
        std::printf("unsigned query    ok  (session is authenticated)\n");
    } else {
        std::fprintf(stderr, "post-logon query failed: %d %s\n", ws_resp.error_code,
                     ws_resp.error_msg.c_str());
        return 1;
    }

    ws.close();

    std::printf("\ncredentials work against %s, over both REST and the websocket api.\n",
                env.name);
    return 0;
}
