#include "llt/net/tcp.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <utility>

namespace llt::net {
namespace {

bool make_nonblocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool finish_connect(int fd, int timeout_ms) noexcept {
    pollfd pfd{};
    pfd.fd     = fd;
    pfd.events = POLLOUT;

    if (::poll(&pfd, 1, timeout_ms) != 1) return false;

    int       err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) return false;
    return err == 0;
}

}  // namespace

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void TcpSocket::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool TcpSocket::connect(const char* host, uint16_t port, int timeout_ms) {
    close();

    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(port));

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    if (::getaddrinfo(host, port_str, &hints, &res) != 0) return false;

    for (const addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        if (!make_nonblocking(fd)) {
            ::close(fd);
            continue;
        }

        bool connected = ::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0;
        if (!connected && errno == EINPROGRESS) connected = finish_connect(fd, timeout_ms);

        if (!connected) {
            ::close(fd);
            continue;
        }

        const int one = 1;
        static_cast<void>(::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)));

        fd_ = fd;
        break;
    }

    ::freeaddrinfo(res);
    return fd_ >= 0;
}

}  // namespace llt::net
