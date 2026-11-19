#pragma once
#include <cstddef>
#include <sys/socket.h>
#include <sys/types.h>

namespace sacred {
// Host socket primitives whose spelling differs between Linux and macOS. Every socket is close-on-exec and
// never raises SIGPIPE: Linux passes MSG_NOSIGNAL per call, macOS sets SO_NOSIGPIPE once per socket.
int socket_open(int domain, int type);
int socket_accept(int fd, sockaddr* peer, socklen_t* length);
ssize_t socket_send(int fd, const void* data, size_t size);
ssize_t socket_sendto(int fd, const void* data, size_t size, const sockaddr* to, socklen_t length);
}
