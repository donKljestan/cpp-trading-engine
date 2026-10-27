#include "llt/net/tls.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>

#include <cerrno>
#include <cstdio>
#include <utility>

#include "llt/core/clock.hpp"

namespace llt::net {
namespace {

thread_local char t_last_error[256] = {};

void record(const char* what) noexcept {
    const unsigned long code = ::ERR_get_error();
    if (code != 0) {
        char detail[192];
        ::ERR_error_string_n(code, detail, sizeof(detail));
        std::snprintf(t_last_error, sizeof(t_last_error), "%s: %s", what, detail);
    } else {
        std::snprintf(t_last_error, sizeof(t_last_error), "%s", what);
    }
}

SSL_CTX* shared_ctx() noexcept {
    static SSL_CTX* ctx = [] () -> SSL_CTX* {
        SSL_CTX* c = ::SSL_CTX_new(::TLS_client_method());
        if (c == nullptr) return nullptr;

        static_cast<void>(::SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION));
        ::SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
        static_cast<void>(::SSL_CTX_set_default_verify_paths(c));
        static_cast<void>(::SSL_CTX_set_mode(c, SSL_MODE_ENABLE_PARTIAL_WRITE |
                                                    SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER));
        return c;
    }();
    return ctx;
}

// Waits for the condition OpenSSL asked for. Returns false on timeout or poll error.
bool wait_for(int fd, int ssl_err, uint64_t deadline_ns) noexcept {
    const uint64_t now = llt::clk::mono_ns();
    if (now >= deadline_ns) return false;

    const uint64_t remaining_ms = (deadline_ns - now) / 1'000'000ULL;

    pollfd pfd{};
    pfd.fd     = fd;
    pfd.events = (ssl_err == SSL_ERROR_WANT_WRITE) ? POLLOUT : POLLIN;

    return ::poll(&pfd, 1, static_cast<int>(remaining_ms) + 1) == 1;
}

}  // namespace

TlsStream::~TlsStream() { close(); }

TlsStream::TlsStream(TlsStream&& other) noexcept
    : sock_(std::move(other.sock_)), ssl_(std::exchange(other.ssl_, nullptr)) {}

TlsStream& TlsStream::operator=(TlsStream&& other) noexcept {
    if (this != &other) {
        close();
        sock_ = std::move(other.sock_);
        ssl_  = std::exchange(other.ssl_, nullptr);
    }
    return *this;
}

void TlsStream::close() noexcept {
    if (ssl_ != nullptr) {
        ::SSL_free(ssl_);
        ssl_ = nullptr;
    }
    sock_.close();
}

bool TlsStream::connect(TcpSocket&& sock, const char* host, int timeout_ms) {
    close();

    SSL_CTX* ctx = shared_ctx();
    if (ctx == nullptr) {
        record("SSL_CTX_new");
        return false;
    }

    ssl_ = ::SSL_new(ctx);
    if (ssl_ == nullptr) {
        record("SSL_new");
        return false;
    }

    sock_ = std::move(sock);
    if (::SSL_set_fd(ssl_, sock_.fd()) != 1) {
        record("SSL_set_fd");
        close();
        return false;
    }

    // SNI tells the server which cert to send; set1_host makes OpenSSL actually check
    // that the cert it sent back belongs to this hostname. Without the second call a
    // valid certificate for any domain would pass.
    //
    // The SNI macro expands to a C-style cast inside OpenSSL's own header.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    const long sni_rc = ::SSL_set_tlsext_host_name(ssl_, host);
#pragma GCC diagnostic pop
    if (sni_rc != 1) {
        record("SSL_set_tlsext_host_name");
        close();
        return false;
    }
    ::SSL_set_hostflags(ssl_, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (::SSL_set1_host(ssl_, host) != 1) {
        record("SSL_set1_host");
        close();
        return false;
    }

    const uint64_t deadline =
        llt::clk::mono_ns() + static_cast<uint64_t>(timeout_ms) * uint64_t{1'000'000};

    for (;;) {
        ::ERR_clear_error();
        const int rc = ::SSL_connect(ssl_);
        if (rc == 1) break;

        const int err = ::SSL_get_error(ssl_, rc);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            if (!wait_for(sock_.fd(), err, deadline)) {
                record("TLS handshake timed out");
                close();
                return false;
            }
            continue;
        }

        record("SSL_connect");
        close();
        return false;
    }

    if (::SSL_get_verify_result(ssl_) != X509_V_OK) {
        record("peer certificate verification failed");
        close();
        return false;
    }

    return true;
}

IoStatus TlsStream::read(void* buf, size_t cap, size_t& out) noexcept {
    out = 0;
    if (ssl_ == nullptr) return IoStatus::Error;

    ::ERR_clear_error();
    size_t    n  = 0;
    const int rc = ::SSL_read_ex(ssl_, buf, cap, &n);
    if (rc == 1) {
        out = n;
        return IoStatus::Ok;
    }

    switch (::SSL_get_error(ssl_, rc)) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return IoStatus::WouldBlock;
        case SSL_ERROR_ZERO_RETURN:
            return IoStatus::Closed;
        case SSL_ERROR_SYSCALL:
            // errno 0 here means the peer vanished without a close_notify.
            if (errno == 0 || errno == ECONNRESET) return IoStatus::Closed;
            record("SSL_read_ex");
            return IoStatus::Error;
        default:
            record("SSL_read_ex");
            return IoStatus::Error;
    }
}

IoStatus TlsStream::write(const void* buf, size_t len, size_t& out) noexcept {
    out = 0;
    if (ssl_ == nullptr) return IoStatus::Error;

    ::ERR_clear_error();
    size_t    n  = 0;
    const int rc = ::SSL_write_ex(ssl_, buf, len, &n);
    if (rc == 1) {
        out = n;
        return IoStatus::Ok;
    }

    switch (::SSL_get_error(ssl_, rc)) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return IoStatus::WouldBlock;
        case SSL_ERROR_ZERO_RETURN:
            return IoStatus::Closed;
        default:
            record("SSL_write_ex");
            return IoStatus::Error;
    }
}

const char* TlsStream::last_error() noexcept { return t_last_error; }

}  // namespace llt::net
