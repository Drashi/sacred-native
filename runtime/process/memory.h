#pragma once
#include "../../platform/guest_arena.h"
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace sacred {
enum Permission : uint8_t { Read = 1, Write = 2, Execute = 4 };
struct GuestFault : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Memory {
public:
    static constexpr uint64_t size = GuestArena::size;
    static constexpr uint32_t page_size = 4096;
    explicit Memory(size_t host_page = 0) : arena_(host_page) {}
    const GuestArena& arena() const { return arena_; }
    void reserve(uint32_t base, uint64_t length, const std::string& owner);
    void commit(uint32_t base, uint64_t length, uint8_t permissions, const std::string& owner);
    void protect(uint32_t base, uint64_t length, uint8_t permissions, const std::string& owner);
    void decommit(uint32_t base, uint64_t length, const std::string& owner);
    void release(uint32_t base, const std::string& owner);
    // Every guest load and store of the generated code is checked; the common single-page case is inline (and, for the
    // generated code, also in x86.h through access_table()) and the slow path (page spans, faults with their
    // diagnostics) is out of line.
    void check(uint32_t address, size_t length, uint8_t permissions) const {
        if (address >= 0x10000 && length && length <= size - address &&
            address / page_size == (uint64_t{address} + length - 1) / page_size &&
            (pages_[address / page_size] & (committed | permissions)) == (committed | permissions)) {
            return;
        }

        check_slow(address, length, permissions);
    }

    // check() without the fault: true when every page of the range is committed and readable.
    bool readable(uint32_t address, uint64_t length) const {
        if (address < 0x10000 || !length || length > size - address) {
            return false;
        }

        for (uint64_t page = address / page_size; page <= (address + length - 1) / page_size; ++page) {
            if ((pages_[page] & (committed | Read)) != (committed | Read)) {
                return false;
            }
        }

        return true;
    }

    void read(uint32_t address, void* destination, size_t length) const {
        check(address, length, Read);
        if (length) {
            std::memcpy(destination, arena_.base() + address, length);
        }
    }

    void write(uint32_t address, const void* source, size_t length) {
        check(address, length, Write);
        if (length) {
            std::memcpy(arena_.base() + address, source, length);
        }
    }

    uint32_t load32(uint32_t address) const;
    void store32(uint32_t address, uint32_t value);
    uint8_t permissions(uint32_t address) const;
    uint64_t reserved_below(uint32_t limit) const;
    // One byte per guest page: `committed` | permissions, 0 for an uncommitted page. A requested permission set is
    // granted exactly when (byte & (committed | set)) == (committed | set), which is the check above; the generated
    // code's inline fast path reads the same bytes (g_page_access in x86.h) and falls back to the checked call for
    // anything else.
    static constexpr uint8_t committed = 0x80;
    const uint8_t* access_table() const { return pages_.data(); }
    uintptr_t host_address(uint32_t address) const { return reinterpret_cast<uintptr_t>(arena_.base()) + address; }

private:
    struct Allocation {
        uint64_t length;
        std::string owner;
    };
    void check_slow(uint32_t address, size_t length, uint8_t permissions) const;
    GuestArena arena_;
    std::vector<uint8_t> pages_ = std::vector<uint8_t>(size / page_size);
    std::map<uint32_t, Allocation> allocations_;
    void owned(uint32_t base, uint64_t length, const std::string& owner) const;
};

namespace layout {
constexpr uint32_t exe = 0x00400000, granny = 0x10000000, tincat = 0x11000000;
constexpr uint32_t image_end = 0x20000000;
constexpr uint32_t heap = 0x20000000, heap_end = 0x40000000;
// Overflow window of the guest heap: free space between the huge stack slots and the TLS area.
constexpr uint32_t heap_overflow = 0x48000000, heap_overflow_end = 0x60000000;
constexpr uint32_t tls = 0x60000000, tls_end = 0x61000000, tls_stride = 0x10000;
constexpr uint32_t stacks = 0x70000000, stacks_end = 0x78000000, stack_stride = 0x100000;
// Threads whose requested stack exceeds stack_stride use 4 MiB slots (ids < 16).
constexpr uint32_t big_stacks = 0x72000000, big_stack_stride = 0x400000;
// Threads whose requested stack exceeds big_stack_stride use 8 MiB slots (ids < 16).
constexpr uint32_t huge_stacks = 0x40000000, huge_stack_stride = 0x800000;
constexpr uint32_t tebs = 0x7f000000;
constexpr uint32_t hle = 0xf0000000, hle_end = 0xf1000000, token_stride = 16;
constexpr uint32_t returns = 0xf1000000;
}

class Heap {
public:
    explicit Heap(Memory& memory) : memory_(memory) {}
    ~Heap();
    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;
    // Page-granular block: freed memory becomes inaccessible (runtime-owned objects rely on that).
    uint32_t allocate(uint32_t bytes);
    // The guest's own heap APIs: requests up to small_limit come from 64 KiB slabs of fixed-size slots
    // (16-byte aligned, zero-filled on every allocation, no per-request page rounding); larger requests
    // are page-granular. A freed slot stays mapped, like a Windows heap.
    uint32_t allocate_small(uint32_t bytes);
    void free(uint32_t address);
    static constexpr uint32_t small_limit = 2048;

private:
    struct Slab {
        uint32_t length, slot;
        std::vector<bool> live;
    };
    uint32_t allocate_pages(uint32_t bytes);
    Memory& memory_;
    std::map<uint32_t, uint64_t> blocks_;
    std::map<uint32_t, Slab> slabs_;                // by base address
    std::vector<std::vector<uint32_t>> free_slots_; // per size class, addresses ready for reuse
};

struct TlsTemplate {
    uint32_t index;
    std::vector<uint8_t> bytes;
    uint32_t zero_fill;
};

class Thread {
public:
    // stack_bytes is the requested capacity; 0 selects the default stack.
    Thread(Memory& memory, uint32_t id, const std::vector<TlsTemplate>& templates, uint32_t stack_bytes = 0);
    ~Thread();
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    uint32_t teb, stack_top, stack_limit, tls_vector;
    uint32_t fs_base() const { return teb; }
    // Only the next guard page grows a stack. Skipped pages and foreign reservations remain faults; original
    // __chkstk probes every guest page.
    void probe_stack(uint32_t address, size_t length) {
        if (!length || address >= stack_limit || length > stack_top - address ||
            address / Memory::page_size != (stack_limit - Memory::page_size) / Memory::page_size) {
            return;
        }

        grow_stack();
    }

    // The host MMU path (x86.h RECOMP_MMU_GUEST): a host fault at `address` in this thread's stack reservation below
    // the committed stack commits down to it, in whole host pages, so the host mapping and the guest page table
    // agree. False (nothing done) for any other address. Called from the fault handler: throws nothing.
    bool grow_to(uint32_t address);

private:
    void grow_stack();
    Memory& memory_;
    std::string owner_;
    std::vector<uint32_t> allocations_;
    uint32_t stack_bottom_;
};
}
