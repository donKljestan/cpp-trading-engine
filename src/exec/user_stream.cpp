#include "llt/exec/user_stream.hpp"

#include <cstdio>

#include "llt/venue/json_scan.hpp"

namespace llt::exec {
namespace {

thread_local char t_last_error[256] = {};

void record(const char* what) noexcept {
    std::snprintf(t_last_error, sizeof(t_last_error), "%s", what);
}

}  // namespace

const char* UserStream::last_error() noexcept {
    return t_last_error[0] != '\0' ? t_last_error : net::WsClient::last_error();
}

bool UserStream::start(WsApiClient& api, const char* stream_host, uint64_t now_ns,
                       int timeout_ms) {
    close();

    if (!api.authenticated()) {
        record("websocket api session is not authenticated");
        return false;
    }

    WsApiResponse resp;
    if (!api.request("userDataStream.start", Params{}.finish_stamped(), timeout_ms, resp)) {
        record(WsApiClient::last_error());
        return false;
    }
    if (!resp.ok()) {
        std::snprintf(t_last_error, sizeof(t_last_error),
                      "userDataStream.start rejected: %d %s", resp.error_code,
                      resp.error_msg.c_str());
        return false;
    }

    std::string_view key;
    if (!json::find_field(resp.result, "listenKey", key)) {
        record("no listenKey in the response");
        return false;
    }
    listen_key_ = std::string(json::unquote(key));
    if (listen_key_.empty()) {
        record("empty listenKey");
        return false;
    }

    const std::string path = "/ws/" + listen_key_;
    if (!ws_.connect(stream_host, 443, path.c_str(), timeout_ms)) {
        record("could not connect the user stream");
        return false;
    }

    last_keepalive_ns_ = now_ns;
    started_           = true;
    return true;
}

bool UserStream::keepalive(WsApiClient& api, uint64_t now_ns, int timeout_ms) {
    if (!started_) return false;

    WsApiResponse resp;
    if (!api.request("userDataStream.ping", Params{}.finish_stamped(), timeout_ms, resp) ||
        !resp.ok()) {
        std::snprintf(t_last_error, sizeof(t_last_error),
                      "userDataStream.ping failed: %d %s", resp.error_code,
                      resp.error_msg.c_str());
        return false;
    }

    last_keepalive_ns_ = now_ns;
    ++stats_.keepalives;
    return true;
}

void UserStream::close() noexcept {
    ws_.close();
    listen_key_.clear();
    started_ = false;
}

}  // namespace llt::exec
