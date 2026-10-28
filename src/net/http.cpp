#include "llt/net/http.hpp"

#include <poll.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "llt/core/clock.hpp"

namespace llt::net {
namespace {

thread_local char t_last_error[256] = {};

void record(const char* what) noexcept {
    std::snprintf(t_last_error, sizeof(t_last_error), "%s", what);
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
        const char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
        if (ca != cb) return false;
    }
    return true;
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

}  // namespace

const char* HttpClient::last_error() noexcept {
    return t_last_error[0] != '\0' ? t_last_error : TlsStream::last_error();
}

void HttpClient::close() noexcept {
    tls_.close();
    rx_.clear();
}

bool HttpClient::connect(const char* host, uint16_t port, int timeout_ms) {
    close();
    host_    = host;
    port_    = port;
    timeout_ = timeout_ms;

    TcpSocket sock;
    if (!sock.connect(host, port, timeout_ms)) {
        record("tcp connect failed");
        return false;
    }
    return tls_.connect(std::move(sock), host, timeout_ms);
}

bool HttpClient::send_request(const char* path, const char* extra_headers) {
    std::string req;
    req.reserve(512);
    req += "GET ";
    req += path;
    req += " HTTP/1.1\r\nHost: ";
    req += host_;
    req +=
        "\r\nConnection: keep-alive\r\nAccept: application/json\r\nUser-Agent: llt/0.1\r\n";
    if (extra_headers != nullptr) req += extra_headers;
    req += "\r\n";

    const size_t len = req.size();
    size_t       sent = 0;
    while (sent < len) {
        size_t         n  = 0;
        const IoStatus st = tls_.write(req.data() + sent, len - sent, n);
        if (st == IoStatus::Ok) {
            sent += n;
            continue;
        }
        if (st == IoStatus::WouldBlock) {
            pollfd pfd{};
            pfd.fd     = tls_.fd();
            pfd.events = POLLOUT;
            if (::poll(&pfd, 1, timeout_) != 1) {
                record("request write timed out");
                return false;
            }
            continue;
        }
        record("request write failed");
        return false;
    }
    return true;
}

bool HttpClient::fill_until(uint64_t deadline_ns) {
    char           buf[16 * 1024];
    size_t         n  = 0;
    const IoStatus st = tls_.read(buf, sizeof(buf), n);

    if (st == IoStatus::Ok) {
        rx_.append(buf, n);
        return true;
    }
    if (st == IoStatus::WouldBlock) {
        const uint64_t now = llt::clk::mono_ns();
        if (now >= deadline_ns) {
            record("response timed out");
            return false;
        }
        pollfd pfd{};
        pfd.fd     = tls_.fd();
        pfd.events = POLLIN;
        static_cast<void>(
            ::poll(&pfd, 1, static_cast<int>((deadline_ns - now) / 1'000'000U) + 1));
        return true;
    }

    record(st == IoStatus::Closed ? "connection closed by peer" : "read failed");
    return false;
}

bool HttpClient::read_response(int timeout_ms, HttpResponse& out) {
    const uint64_t deadline =
        llt::clk::mono_ns() + static_cast<uint64_t>(timeout_ms) * uint64_t{1'000'000};

    size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        header_end = rx_.find("\r\n\r\n");
        if (header_end != std::string::npos) break;
        if (!fill_until(deadline)) return false;
    }
    header_end += 4;

    const std::string_view head(rx_.data(), header_end);

    const size_t           first_eol = head.find("\r\n");
    const std::string_view status_line = head.substr(0, first_eol);
    const size_t           sp          = status_line.find(' ');
    if (sp == std::string_view::npos) {
        record("malformed status line");
        return false;
    }
    out.status = std::atoi(status_line.data() + sp + 1);

    size_t content_length = std::string::npos;
    bool   chunked        = false;
    bool   close_after    = false;

    size_t cursor = first_eol + 2;
    while (cursor < head.size()) {
        const size_t line_end = head.find("\r\n", cursor);
        if (line_end == std::string_view::npos || line_end == cursor) break;

        const std::string_view line  = head.substr(cursor, line_end - cursor);
        const size_t           colon = line.find(':');
        if (colon != std::string_view::npos) {
            const std::string_view name  = trim(line.substr(0, colon));
            const std::string_view value = trim(line.substr(colon + 1));

            if (iequals(name, "content-length")) {
                content_length = static_cast<size_t>(std::strtoul(std::string(value).c_str(),
                                                                  nullptr, 10));
            } else if (iequals(name, "transfer-encoding") && iequals(value, "chunked")) {
                chunked = true;
            } else if (iequals(name, "connection") && iequals(value, "close")) {
                close_after = true;
            }
        }
        cursor = line_end + 2;
    }

    out.body.clear();

    if (chunked) {
        size_t pos = header_end;
        for (;;) {
            size_t line_end = rx_.find("\r\n", pos);
            while (line_end == std::string::npos) {
                if (!fill_until(deadline)) return false;
                line_end = rx_.find("\r\n", pos);
            }
            const size_t chunk_size =
                static_cast<size_t>(std::strtoul(rx_.substr(pos, line_end - pos).c_str(),
                                                 nullptr, 16));
            const size_t data_start = line_end + 2;

            while (rx_.size() < data_start + chunk_size + 2) {
                if (!fill_until(deadline)) return false;
            }
            if (chunk_size == 0) {
                pos = data_start + 2;
                break;
            }
            out.body.append(rx_, data_start, chunk_size);
            pos = data_start + chunk_size + 2;
        }
        rx_.erase(0, pos);
    } else {
        if (content_length == std::string::npos) {
            record("response has neither Content-Length nor chunked encoding");
            return false;
        }
        while (rx_.size() < header_end + content_length) {
            if (!fill_until(deadline)) return false;
        }
        out.body.assign(rx_, header_end, content_length);
        rx_.erase(0, header_end + content_length);
    }

    if (close_after) close();
    return true;
}

bool HttpClient::get(const char* path, int timeout_ms, HttpResponse& out,
                     const char* extra_headers) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!tls_.valid() && !connect(host_.c_str(), port_, timeout_)) return false;

        if (send_request(path, extra_headers) && read_response(timeout_ms, out)) return true;

        // A kept-alive connection can be closed by the server at any time; that is not
        // an error until a fresh connection fails the same way.
        close();
    }
    return false;
}

}  // namespace llt::net
