#include "guest_arena.h"
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace sacred {
GuestArena::GuestArena(size_t host_page) {
    static_assert(sizeof(size_t) >= 8, "The guest arena needs a 64-bit host");
    if (!host_page) {
        const long page = sysconf(_SC_PAGESIZE);
        if (page <= 0) {
            throw std::runtime_error("Cannot determine host page size");
        }

        host_page = static_cast<size_t>(page);
    }

    if (host_page < 4096 || (host_page & (host_page - 1))) {
        throw std::runtime_error("Host page size must be a power of two of at least 4 KiB");
    }

    host_page_ = host_page;
    // Over-reserve so the arena base is aligned to the (possibly larger) host page.
    void* p = mmap(nullptr, size + host_page_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        throw std::runtime_error("Cannot reserve sparse 4 GiB guest arena");
    }

    reservation_ = static_cast<uint8_t*>(p);
    const uintptr_t raw = reinterpret_cast<uintptr_t>(reservation_);
    base_ = reservation_ + ((raw + host_page_ - 1) / host_page_ * host_page_ - raw);
}

GuestArena::~GuestArena() {
    munmap(reservation_, size + host_page_);
}

void GuestArena::commit(uint32_t address, uint64_t length) {
    const uint64_t begin = address / host_page_ * host_page_;
    const uint64_t end = (uint64_t{address} + length + host_page_ - 1) / host_page_ * host_page_;
    last_begin_ = begin;
    last_end_ = end;
    // Never grant host execute permission, even for guest .text.
    if (mprotect(base_ + begin, end - begin, PROT_READ | PROT_WRITE) != 0) {
        throw std::runtime_error("Cannot commit guest backing storage");
    }
}
}
