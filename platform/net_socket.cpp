#include "net_socket.h"
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace sacred {
namespace {
#if defined(__linux__)
constexpr int send_flags = MSG_NOSIGNAL;
#elif defined(__APPLE__)
constexpr int send_flags = 0;
// Sockets returned by socket()/accept() on macOS inherit neither flag; set both before the handle is published.
int prepare(int fd) {
    if (fd < 0) {
        return fd;
    }

    const int on = 1;
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on) != 0) {
        const int saved = errno;
        ::close(fd);
        errno = saved;
        return -1;
    }

    return fd;
}
#else
#error "net_socket needs a port for this operating system"
#endif
}

int socket_open(int domain, int type) {
#if defined(__linux__)
    return ::socket(domain, type | SOCK_CLOEXEC, 0);
#else
    return prepare(::socket(domain, type, 0));
#endif
}

int socket_accept(int fd, sockaddr* peer, socklen_t* length) {
#if defined(__linux__)
    return ::accept4(fd, peer, length, SOCK_CLOEXEC);
#else
    return prepare(::accept(fd, peer, length));
#endif
}

ssize_t socket_send(int fd, const void* data, size_t size) {
    return ::send(fd, data, size, send_flags);
}

ssize_t socket_sendto(int fd, const void* data, size_t size, const sockaddr* to, socklen_t length) {
    return ::sendto(fd, data, size, send_flags, to, length);
}
}
