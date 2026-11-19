#include "winsock.h"
#include "../../platform/net_socket.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace sacred {
namespace {
constexpr uint32_t invalid_socket = 0xffffffffu, socket_error = 0xffffffffu;
constexpr int wsaeintr = 10004, wsaebadf = 10009, wsaeacces = 10013, wsaefault = 10014, wsaeinval = 10022,
              wsaewouldblock = 10035, wsaealready = 10037, wsaenotsock = 10038, wsaemsgsize = 10040,
              wsaenoprotoopt = 10042, wsaeafnosupport = 10047, wsaeaddrinuse = 10048, wsaeaddrnotavail = 10049,
              wsaenetunreach = 10051, wsaeconnaborted = 10053, wsaeconnreset = 10054, wsaeisconn = 10056,
              wsaenotconn = 10057, wsaetimedout = 10060, wsaeconnrefused = 10061, wsaehostunreach = 10065,
              wsahost_not_found = 11001;
thread_local int last_error = 0;

int wsa_error(int e) {
    switch (e) {
    case EINTR:
        return wsaeintr;
    case EBADF:
        return wsaebadf;
    case EACCES:
    case EPERM:
        return wsaeacces;
    case EFAULT:
        return wsaefault;
    case EAGAIN:
    case EINPROGRESS:
        return wsaewouldblock;
    case EALREADY:
        return wsaealready;
    case ENOTSOCK:
        return wsaenotsock;
    case EMSGSIZE:
        return wsaemsgsize;
    case ENOPROTOOPT:
        return wsaenoprotoopt;
    case EAFNOSUPPORT:
        return wsaeafnosupport;
    case EADDRINUSE:
        return wsaeaddrinuse;
    case EADDRNOTAVAIL:
        return wsaeaddrnotavail;
    case ENETUNREACH:
        return wsaenetunreach;
    case ECONNABORTED:
        return wsaeconnaborted;
    case ECONNRESET:
    case EPIPE:
        return wsaeconnreset;
    case EISCONN:
        return wsaeisconn;
    case ENOTCONN:
        return wsaenotconn;
    case ETIMEDOUT:
        return wsaetimedout;
    case ECONNREFUSED:
        return wsaeconnrefused;
    case EHOSTUNREACH:
        return wsaehostunreach;
    default:
        return wsaeinval;
    }
}

sockaddr_in read_addr(Memory& m, uint32_t p) {
    uint8_t raw[16];
    m.read(p, raw, sizeof raw);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    std::memcpy(&a.sin_port, raw + 2, 2);
    std::memcpy(&a.sin_addr, raw + 4, 4);
    return a;
}

void write_addr(Memory& m, uint32_t p, uint32_t len_p, const sockaddr_in& a) {
    if (!p) {
        return;
    }

    uint8_t raw[16]{};
    raw[0] = AF_INET;
    std::memcpy(raw + 2, &a.sin_port, 2);
    std::memcpy(raw + 4, &a.sin_addr, 4);
    m.write(p, raw, sizeof raw);
    if (len_p) {
        m.store32(len_p, 16);
    }
}

std::string cstring(Memory& m, uint32_t p) {
    std::string s;
    for (char c; s.size() < 1024; s.push_back(c)) {
        m.read(p + uint32_t(s.size()), &c, 1);
        if (!c) {
            return s;
        }
    }

    throw GuestFault("unterminated winsock string");
}

bool sol_option(uint32_t name, int& level, int& option) {
    level = SOL_SOCKET;
    switch (name) {
    case 0x0004:
        option = SO_REUSEADDR;
        return true;
    case 0x0008:
        option = SO_KEEPALIVE;
        return true;
    case 0x0020:
        option = SO_BROADCAST;
        return true;
    case 0x1001:
        option = SO_SNDBUF;
        return true;
    case 0x1002:
        option = SO_RCVBUF;
        return true;
    case 0x1007:
        option = SO_ERROR;
        return true;
    case 0x1008:
        option = SO_TYPE;
        return true;
    default:
        return false;
    }
}
}

int Winsock::host(uint32_t socket) {
    std::lock_guard<std::mutex> lock(sockets_mutex_);
    auto it = sockets_.find(socket);
    return it == sockets_.end() ? -1 : it->second;
}

uint32_t Winsock::fail(int error) {
    last_error = error;
    return socket_error;
}

template <class F> auto Winsock::blocking(F f) -> decltype(f()) {
    auto& scheduler = control_.scheduler();
    const bool owned = scheduler.owns();
    if (owned) {
        scheduler.release();
    }

    auto result = f();
    const int saved = errno;
    if (owned) {
        scheduler.acquire();
    }

    errno = saved;
    return result;
}

void Winsock::bind() {
    // WS2_32 and WSOCK32 share the ordinal numbering except 10..12: WS2_32 has ioctlsocket, inet_addr, inet_ntoa,
    // WSOCK32 (Winsock 1.1, imported by tincat2.dll) has inet_addr, inet_ntoa, ioctlsocket. tincat2 calls #12 with
    // (s, FIONBIO 0x8004667e, &arg) at 0x1000d027.
    auto both = [this](int ordinal, int args, HostFunction fn) {
        std::vector<Argument> spec(size_t(args), Argument{});
        for (const char* dll : {"ws2_32.dll", "wsock32.dll"}) {
            const bool wsock = std::string(dll) == "wsock32.dll";
            const int number = !wsock || ordinal < 10 || ordinal > 12 ? ordinal : ordinal == 10 ? 12 : ordinal - 1;
            const std::string symbol = "#" + std::to_string(number);
            const bool imported =
                std::any_of(loader_.hle_slots().begin(), loader_.hle_slots().end(),
                            [&](const auto& slot) { return slot.dll == dll && slot.symbol == symbol; });
            if (!imported) {
                continue;
            }

            control_.host(loader_.resolve(dll, symbol), {Convention::Stdcall, ReturnChannel::Eax, spec, ""}, fn,
                          std::string(dll) + "!" + symbol);
        }
    };
    auto& m = control_.memory;
    auto sock = [this](uint64_t v) {
        return host(uint32_t(v));
    };

    both(115, 2, [&m](Control&, const std::vector<uint64_t>& a) { // WSAStartup
        if (!a[1]) {
            return Result{uint32_t(wsaefault), {}};
        }

        std::vector<uint8_t> data(400, 0);
        data[0] = 2;
        data[1] = 2;
        data[2] = 2;
        data[3] = 2;
        std::memcpy(&data[4], "WinSock 2.0", 11);
        std::memcpy(&data[261], "Running", 7);
        data[390] = 0xff;
        data[391] = 0x7f;
        data[392] = 0xe4;
        data[393] = 0xff;
        m.write(uint32_t(a[1]), data.data(), data.size());
        return Result{0, {}};
    });
    both(116, 0, [](Control&, const std::vector<uint64_t>&) { return Result{0, {}}; }); // WSACleanup
    both(111, 0, [](Control&, const std::vector<uint64_t>&) { return Result{uint32_t(last_error), {}}; });
    both(8, 1, [](Control&, const std::vector<uint64_t>& a) { return Result{htonl(uint32_t(a[0])), {}}; });
    both(14, 1, [](Control&, const std::vector<uint64_t>& a) { return Result{ntohl(uint32_t(a[0])), {}}; });
    both(9, 1, [](Control&, const std::vector<uint64_t>& a) { return Result{htons(uint16_t(a[0])), {}}; });
    both(15, 1, [](Control&, const std::vector<uint64_t>& a) { return Result{ntohs(uint16_t(a[0])), {}}; });

    both(23, 3, [this](Control&, const std::vector<uint64_t>& a) { // socket
        if (a[0] != AF_INET) {
            last_error = wsaeafnosupport;
            return Result{invalid_socket, {}};
        }

        const int type = a[1] == 1 ? SOCK_STREAM : a[1] == 2 ? SOCK_DGRAM : -1;
        if (type < 0) {
            last_error = wsaeinval;
            return Result{invalid_socket, {}};
        }

        const int fd = socket_open(AF_INET, type);
        if (fd < 0) {
            last_error = wsa_error(errno);
            return Result{invalid_socket, {}};
        }

        if (type == SOCK_DGRAM) {
            // Two instances on one host bind the same fixed UDP port; broadcasts reach both.
            const int on = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
        }

        std::lock_guard<std::mutex> lock(sockets_mutex_);
        const uint32_t handle = next_socket_;
        next_socket_ += 4;
        sockets_[handle] = fd;
        return Result{handle, {}};
    });
    both(3, 1, [this](Control&, const std::vector<uint64_t>& a) { // closesocket
        int fd;
        {
            std::lock_guard<std::mutex> lock(sockets_mutex_);
            auto it = sockets_.find(uint32_t(a[0]));
            if (it == sockets_.end()) {
                return Result{fail(wsaenotsock), {}};
            }

            fd = it->second;
            sockets_.erase(it);
        }

        ::close(fd);
        return Result{0, {}};
    });
    both(22, 2, [this, sock](Control&, const std::vector<uint64_t>& a) { // shutdown
        const int fd = sock(a[0]);
        if (fd < 0) {
            return Result{fail(wsaenotsock), {}};
        }

        return Result{::shutdown(fd, int(a[1])) ? fail(wsa_error(errno)) : 0u, {}};
    });
    both(2, 3, [this, &m, sock](Control&, const std::vector<uint64_t>& a) { // bind
        const int fd = sock(a[0]);
        if (fd < 0) {
            return Result{fail(wsaenotsock), {}};
        }

        if (a[2] < 16) {
            return Result{fail(wsaefault), {}};
        }

        sockaddr_in addr = read_addr(m, uint32_t(a[1]));
        return Result{::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) ? fail(wsa_error(errno)) : 0u, {}};
    });
    both(13, 2, [this, sock](Control&, const std::vector<uint64_t>& a) { // listen
        const int fd = sock(a[0]);
        if (fd < 0) {
            return Result{fail(wsaenotsock), {}};
        }

        return Result{::listen(fd, int(a[1])) ? fail(wsa_error(errno)) : 0u, {}};
    });
    both(1, 3, [this, &m, sock](Control&, const std::vector<uint64_t>& a) { // accept
        const int fd = sock(a[0]);
        if (fd < 0) {
            last_error = wsaenotsock;
            return Result{invalid_socket, {}};
        }

        sockaddr_in peer{};
        socklen_t len = sizeof peer;
        const int child = blocking([&] { return socket_accept(fd, reinterpret_cast<sockaddr*>(&peer), &len); });
        if (child < 0) {
            last_error = wsa_error(errno);
            return Result{invalid_socket, {}};
        }

        write_addr(m, uint32_t(a[1]), uint32_t(a[2]), peer);
        std::lock_guard<std::mutex> lock(sockets_mutex_);
        const uint32_t handle = next_socket_;
        next_socket_ += 4;
        sockets_[handle] = child;
        return Result{handle, {}};
    });
    both(4, 3, [this, &m, sock](Control&, const std::vector<uint64_t>& a) { // connect
        const int fd = sock(a[0]);
        if (fd < 0) {
            return Result{fail(wsaenotsock), {}};
        }

        sockaddr_in addr = read_addr(m, uint32_t(a[1]));
        const int r = blocking([&] { return ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr); });
        return Result{r ? fail(wsa_error(errno)) : 0u, {}};
    });
    both(6, 3, [this, &m, sock](Control&, const std::vector<uint64_t>& a) { // getsockname
        const int fd = sock(a[0]);
        if (fd < 0) {
            return Result{fail(wsaenotsock), {}};
        }

        sockaddr_in addr{};
        socklen_t len = sizeof addr;
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len)) {
            return Result{fail(wsa_error(errno)), {}};
        }

        write_addr(m, uint32_t(a[1]), uint32_t(a[2]), addr);
        return Result{0, {}};
    });
    both(21, 5, [this, &m, sock](Control&, const std::vector<uint64_t>& a) { // setsockopt
        const int fd = sock(a[0]);
        if (fd < 0) {
            return Result{fail(wsaenotsock), {}};
        }

        int level, option;
        if (a[1] == 6 && a[2] == 1) {
            level = IPPROTO_TCP;
            option = TCP_NODELAY;
        } else if (a[1] != 0xffff || !sol_option(uint32_t(a[2]), level, option)) {
            return Result{fail(wsaenoprotoopt), {}};
        }

        if (a[4] < 4) {
            return Result{fail(wsaefault), {}};
        }

        const int value = int(m.load32(uint32_t(a[3])));
        return Result{::setsockopt(fd, level, option, &value, sizeof value) ? fail(wsa_error(errno)) : 0u, {}};
    });
    both(7, 5, [this, &m, sock](Control&, const std::vector<uint64_t>& a) { // getsockopt
        const int fd = sock(a[0]);
        if (fd < 0) {
            return Result{fail(wsaenotsock), {}};
        }

        int level, option;
        if (a[1] != 0xffff || !sol_option(uint32_t(a[2]), level, option)) {
            return Result{fail(wsaenoprotoopt), {}};
        }

        int value = 0;
        socklen_t len = sizeof value;
        if (::getsockopt(fd, level, option, &value, &len)) {
            return Result{fail(wsa_error(errno)), {}};
        }

        if (option == SO_ERROR && value) {
            value = wsa_error(value);
        }

        if (option == SO_TYPE) {
            value = value == SOCK_STREAM ? 1 : 2;
        }

        m.store32(uint32_t(a[3]), uint32_t(value));
        if (a[4]) {
            m.store32(uint32_t(a[4]), 4);
        }

        return Result{0, {}};
    });
    both(10, 3, [this, &m, sock](Control&, const std::vector<uint64_t>& a) { // ioctlsocket
        const int fd = sock(a[0]);
        if (fd < 0) {
            return Result{fail(wsaenotsock), {}};
        }

        if (a[1] == 0x8004667eu) { // FIONBIO
            const int flags = ::fcntl(fd, F_GETFL);
            const bool on = m.load32(uint32_t(a[2])) != 0;
            ::fcntl(fd, F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
            return Result{0, {}};
        }

        if (a[1] == 0x4004667fu) { // FIONREAD
            int n = 0;
            if (::ioctl(fd, FIONREAD, &n)) {
                return Result{fail(wsa_error(errno)), {}};
            }

            m.store32(uint32_t(a[2]), uint32_t(n));
            return Result{0, {}};
        }

        return Result{fail(wsaeinval), {}};
    });
    both(19, 4, [this, &m, sock](Control&, const std::vector<uint64_t>& a) { // send
        const int fd = sock(a[0]);
        if (fd < 0) {
            return Result{fail(wsaenotsock), {}};
        }

        std::vector<uint8_t> buf(static_cast<size_t>(uint32_t(a[2])));
        if (!buf.empty()) {
            m.read(uint32_t(a[1]), buf.data(), buf.size());
        }

        const ssize_t n = blocking([&] { return socket_send(fd, buf.data(), buf.size()); });
        return Result{n < 0 ? fail(wsa_error(errno)) : uint32_t(n), {}};
    });
    both(20, 6, [this, &m, sock](Control&, const std::vector<uint64_t>& a) { // sendto
        const int fd = sock(a[0]);
        if (fd < 0) {
            return Result{fail(wsaenotsock), {}};
        }

        std::vector<uint8_t> buf(static_cast<size_t>(uint32_t(a[2])));
        if (!buf.empty()) {
            m.read(uint32_t(a[1]), buf.data(), buf.size());
        }

        sockaddr_in to = read_addr(m, uint32_t(a[4]));
        const ssize_t n = blocking(
            [&] { return socket_sendto(fd, buf.data(), buf.size(), reinterpret_cast<sockaddr*>(&to), sizeof to); });
        return Result{n < 0 ? fail(wsa_error(errno)) : uint32_t(n), {}};
    });
    both(16, 4, [this, &m, sock](Control&, const std::vector<uint64_t>& a) { // recv
        const int fd = sock(a[0]);
        if (fd < 0) {
            return Result{fail(wsaenotsock), {}};
        }

        std::vector<uint8_t> buf(static_cast<size_t>(uint32_t(a[2])));
        const ssize_t n = blocking([&] { return ::recv(fd, buf.data(), buf.size(), 0); });
        if (n < 0) {
            return Result{fail(wsa_error(errno)), {}};
        }

        if (n) {
            m.write(uint32_t(a[1]), buf.data(), size_t(n));
        }

        return Result{uint32_t(n), {}};
    });
    both(17, 6, [this, &m, sock](Control&, const std::vector<uint64_t>& a) { // recvfrom
        const int fd = sock(a[0]);
        if (fd < 0) {
            return Result{fail(wsaenotsock), {}};
        }

        std::vector<uint8_t> buf(static_cast<size_t>(uint32_t(a[2])));
        sockaddr_in from{};
        socklen_t len = sizeof from;
        const ssize_t n = blocking(
            [&] { return ::recvfrom(fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &len); });
        if (n < 0) {
            return Result{fail(wsa_error(errno)), {}};
        }

        if (n) {
            m.write(uint32_t(a[1]), buf.data(), size_t(n));
        }

        write_addr(m, uint32_t(a[4]), uint32_t(a[5]), from);
        return Result{uint32_t(n), {}};
    });
    both(18, 5, [this, &m](Control&, const std::vector<uint64_t>& a) { // select
        struct Set {
            uint32_t guest;
            std::vector<uint32_t> handles;
            fd_set host;
        };
        Set sets[3];
        int top = -1;
        for (int i = 0; i < 3; ++i) {
            sets[i].guest = uint32_t(a[size_t(i) + 1]);
            FD_ZERO(&sets[i].host);
            if (!sets[i].guest) {
                continue;
            }

            const uint32_t count = std::min<uint32_t>(m.load32(sets[i].guest), 64);
            for (uint32_t k = 0; k < count; ++k) {
                const uint32_t handle = m.load32(sets[i].guest + 4 + 4 * k);
                const int fd = host(handle);
                if (fd < 0) {
                    return Result{fail(wsaenotsock), {}};
                }

                sets[i].handles.push_back(handle);
                FD_SET(fd, &sets[i].host);
                top = std::max(top, fd);
            }
        }

        timeval tv{}, *timeout = nullptr;
        if (a[4]) {
            tv.tv_sec = int32_t(m.load32(uint32_t(a[4])));
            tv.tv_usec = int32_t(m.load32(uint32_t(a[4]) + 4));
            timeout = &tv;
        }

        const int n = blocking([&] { return ::select(top + 1, &sets[0].host, &sets[1].host, &sets[2].host, timeout); });
        if (n < 0) {
            return Result{fail(wsa_error(errno)), {}};
        }

        for (auto& set : sets) {
            if (!set.guest) {
                continue;
            }

            uint32_t kept = 0;
            for (uint32_t handle : set.handles) {
                if (FD_ISSET(host(handle), &set.host)) {
                    m.store32(set.guest + 4 + 4 * kept++, handle);
                }
            }

            m.store32(set.guest, kept);
        }

        return Result{uint32_t(n), {}};
    });
    both(151, 2, [&m](Control&, const std::vector<uint64_t>& a) { // __WSAFDIsSet
        const uint32_t count = std::min<uint32_t>(m.load32(uint32_t(a[1])), 64);
        for (uint32_t k = 0; k < count; ++k) {
            if (m.load32(uint32_t(a[1]) + 4 + 4 * k) == uint32_t(a[0])) {
                return Result{1, {}};
            }
        }

        return Result{0, {}};
    });
    both(11, 1, [&m](Control&, const std::vector<uint64_t>& a) { // inet_addr
        in_addr addr{};
        if (!a[0] || !::inet_aton(cstring(m, uint32_t(a[0])).c_str(), &addr)) {
            return Result{0xffffffffu, {}};
        }

        uint32_t value;
        std::memcpy(&value, &addr, 4);
        return Result{value, {}};
    });
    both(12, 1, [this, &m](Control&, const std::vector<uint64_t>& a) { // inet_ntoa
        if (!ntoa_) {
            ntoa_ = control_.heap.allocate(16);
        }

        in_addr addr;
        const uint32_t value = uint32_t(a[0]);
        std::memcpy(&addr, &value, 4);
        char text[16]{};
        ::inet_ntop(AF_INET, &addr, text, sizeof text);
        m.write(ntoa_, text, sizeof text);
        return Result{ntoa_, {}};
    });
    auto hostent = [this, &m](const std::string& name, const std::vector<uint32_t>& addrs) -> uint32_t {
        // Guest hostent: h_name, h_aliases, h_addrtype i16, h_length i16, h_addr_list; then the lists and data.
        if (!hostent_) {
            hostent_ = control_.heap.allocate(512);
        }

        const uint32_t base = hostent_, aliases = base + 16, list = base + 20, data = base + 64, text = base + 128;
        const size_t n = std::min<size_t>(addrs.size(), 8);
        m.store32(base, text);
        m.store32(base + 4, aliases);
        m.store32(base + 8, 2 | (4u << 16));
        m.store32(base + 12, list);
        m.store32(aliases, 0);
        for (size_t i = 0; i < n; ++i) {
            m.store32(list + uint32_t(4 * i), data + uint32_t(4 * i));
            m.write(data + uint32_t(4 * i), &addrs[i], 4);
        }

        m.store32(list + uint32_t(4 * n), 0);
        const std::string clipped = name.substr(0, 255);
        m.write(text, clipped.c_str(), clipped.size() + 1);
        return base;
    };
    both(52, 1, [hostent, &m](Control&, const std::vector<uint64_t>& a) { // gethostbyname
        char local[256] = {};
        std::string name = a[0] ? cstring(m, uint32_t(a[0])) : std::string();
        if (name.empty()) {
            ::gethostname(local, sizeof local - 1);
            name = local;
        }

        addrinfo hints{}, *found = nullptr;
        hints.ai_family = AF_INET;
        if (::getaddrinfo(name.c_str(), nullptr, &hints, &found) || !found) {
            last_error = wsahost_not_found;
            return Result{0, {}};
        }

        std::vector<uint32_t> addrs;
        for (auto* p = found; p; p = p->ai_next) {
            uint32_t v;
            std::memcpy(&v, &reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr, 4);
            if (std::find(addrs.begin(), addrs.end(), v) == addrs.end()) {
                addrs.push_back(v);
            }
        }

        ::freeaddrinfo(found);
        return Result{hostent(name, addrs), {}};
    });
    both(51, 3, [hostent, &m](Control&, const std::vector<uint64_t>& a) { // gethostbyaddr
        if (a[1] != 4 || a[2] != AF_INET) {
            last_error = wsahost_not_found;
            return Result{0, {}};
        }

        uint32_t v;
        m.read(uint32_t(a[0]), &v, 4);
        char text[16]{};
        ::inet_ntop(AF_INET, &v, text, sizeof text);
        return Result{hostent(text, {v}), {}};
    });
}
}
