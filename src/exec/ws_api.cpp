#include "llt/exec/ws_api.hpp"

#include <poll.h>

#include <cstdio>

#include "llt/core/clock.hpp"
#include "llt/venue/json_scan.hpp"

namespace llt::exec {
namespace {

thread_local char t_last_error[512] = {};

void record(const char* what) noexcept {
    std::snprintf(t_last_error, sizeof(t_last_error), "%s", what);
}

}  // namespace

const char* WsApiClient::last_error() noexcept {
    return t_last_error[0] != '\0' ? t_last_error : net::WsClient::last_error();
}

bool WsApiClient::connect(const char* host, int timeout_ms) {
    close();
    t_last_error[0] = '\0';
    return ws_.connect(host, 443, "/ws-fapi/v1", timeout_ms);
}

void WsApiClient::close() noexcept {
    ws_.close();
    authenticated_ = false;
}

bool WsApiClient::wait_readable(int timeout_ms) noexcept {
    pollfd pfd{};
    pfd.fd     = ws_.fd();
    pfd.events = POLLIN;
    return ::poll(&pfd, 1, timeout_ms) == 1;
}

bool WsApiClient::request(std::string_view method, std::string_view params_json,
                          int timeout_ms, WsApiResponse& out) {
    if (!ws_.valid()) {
        record("not connected");
        return false;
    }

    const std::string id = std::to_string(next_id_++);

    std::string req;
    req.reserve(params_json.size() + 64);
    req += R"({"id":")";
    req += id;
    req += R"(","method":")";
    req += method;
    req += '"';
    if (!params_json.empty()) {
        req += R"(,"params":)";
        req += params_json;
    }
    req += '}';

    if (!ws_.send_text(req)) {
        record("send failed");
        return false;
    }

    const uint64_t deadline =
        llt::clk::mono_ns() + static_cast<uint64_t>(timeout_ms) * uint64_t{1'000'000};

    for (;;) {
        std::string_view payload;
        const auto       ev = ws_.poll(payload);

        if (ev == net::WsClient::Event::Close) {
            record("connection closed while awaiting a response");
            close();
            return false;
        }
        if (ev == net::WsClient::Event::Error) {
            record("websocket error while awaiting a response");
            return false;
        }
        if (ev != net::WsClient::Event::Text) {
            const uint64_t now = llt::clk::mono_ns();
            if (now >= deadline) {
                record("request timed out");
                return false;
            }
            static_cast<void>(
                wait_readable(static_cast<int>((deadline - now) / 1'000'000U) + 1));
            continue;
        }

        // Responses carry the id of the request they answer; anything else on this
        // connection is not ours to consume.
        std::string_view value;
        if (!llt::json::find_field(payload, "id", value)) continue;
        if (llt::json::unquote(value) != id) continue;

        out = WsApiResponse{};
        out.id = id;

        uint64_t status = 0;
        if (llt::json::find_field(payload, "status", value) &&
            llt::json::parse_uint(value, status)) {
            out.status = static_cast<int>(status);
        }
        if (llt::json::find_field(payload, "result", value)) out.result = value;

        std::string_view error;
        if (llt::json::find_field(payload, "error", error)) {
            std::string_view code;
            if (llt::json::find_field(error, "code", code)) {
                out.error_code = static_cast<int>(std::strtol(std::string(code).c_str(),
                                                              nullptr, 10));
            }
            std::string_view msg;
            if (llt::json::find_field(error, "msg", msg)) {
                out.error_msg = std::string(llt::json::unquote(msg));
            }
        }
        return true;
    }
}

bool WsApiClient::logon(const Credentials& cred, int timeout_ms) {
    if (!ws_.valid()) {
        record("not connected");
        return false;
    }

    const std::string timestamp = std::to_string(llt::clk::wall_ns() / 1'000'000);

    // The venue signs the parameters sorted by name, joined as a query string. For
    // session.logon that is apiKey then timestamp.
    const std::string payload = "apiKey=" + cred.api_key + "&timestamp=" + timestamp;

    std::string signature;
    if (!cred.signer.sign(payload, signature)) {
        record(Ed25519Signer::last_error());
        return false;
    }

    // Base64 is JSON-safe as-is; unlike a query string, nothing here needs escaping.
    std::string params = R"({"apiKey":")" + cred.api_key + R"(","timestamp":)" +
                         timestamp + R"(,"signature":")" + signature + R"("})";

    WsApiResponse resp;
    if (!request("session.logon", params, timeout_ms, resp)) return false;

    if (!resp.ok()) {
        std::snprintf(t_last_error, sizeof(t_last_error), "session.logon rejected: %d %s",
                      resp.error_code, resp.error_msg.c_str());
        return false;
    }

    authenticated_ = true;
    return true;
}

}  // namespace llt::exec
