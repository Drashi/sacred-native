#pragma once
// Sacred adapter for the admitted recomp-kit SEH core, not a replacement walker.
#include "control.h"
#include "boundary.h"
#include <cstdio>

#define LOGV(...)                                                                                                      \
    do {                                                                                                               \
        if (log_level() > 1)                                                                                           \
            std::fprintf(stderr, __VA_ARGS__);                                                                         \
    } while (0)
#define LOGW(...)                                                                                                      \
    do {                                                                                                               \
        std::fprintf(stderr, __VA_ARGS__);                                                                             \
        std::fputc('\n', stderr);                                                                                      \
    } while (0)
inline int log_level() {
    return 0;
}

inline bool gm_valid(uint32_t address, uint64_t bytes) {
    try {
        sacred::active_control().memory.check(address, bytes, sacred::Read);
        return true;
    } catch (const sacred::GuestFault&) {
        return false;
    }
}

inline uint32_t heap_alloc(uint32_t bytes, bool) {
    return sacred::active_control().heap.allocate(bytes);
}

inline void heap_free(uint32_t address) {
    sacred::active_control().heap.free(address);
}

inline uint32_t recomp_profile_depth() {
    return 0;
}

inline uint32_t guest_call(X86*, uint32_t target, const uint32_t* args, int count) {
    sacred::Signature signature{sacred::Convention::Cdecl, sacred::ReturnChannel::Eax, {}, "SEH"};
    signature.arguments.resize(count);
    std::vector<uint64_t> values(args, args + count);
    return sacred::active_control().invoke(target, signature, values).integer;
}
