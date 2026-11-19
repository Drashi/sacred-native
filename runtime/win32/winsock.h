#pragma once
#include "../process/control.h"
#include "../process/loader.h"
#include <map>
#include <mutex>

namespace sacred {
// WS2_32 (Sacred.exe) and WSOCK32 (tincat2.dll) ordinals over POSIX sockets. Guest SOCKET values map to host
// descriptors; blocking calls release the execution baton while they wait.
class Winsock {
public:
    Winsock(Control& control, const Loader& loader) : control_(control), loader_(loader) {}
    void bind();

private:
    Control& control_;
    const Loader& loader_;
    std::mutex sockets_mutex_;
    std::map<uint32_t, int> sockets_;
    uint32_t next_socket_ = 0x100;
    uint32_t hostent_ = 0, ntoa_ = 0;
    int host(uint32_t socket);
    uint32_t fail(int error);
    template <class F> auto blocking(F f) -> decltype(f());
};
}
