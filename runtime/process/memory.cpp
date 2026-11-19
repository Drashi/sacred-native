#include "memory.h"
#include <algorithm>
#include <cstring>
#include <sstream>

namespace sacred {
static void page_range(uint32_t base, uint64_t length) {
    if (base < 0x10000 || base % Memory::page_size || !length || length % Memory::page_size ||
        length > Memory::size - base) {
        throw GuestFault("Invalid guest page range");
    }
}

void Memory::reserve(uint32_t base, uint64_t length, const std::string& owner) {
    page_range(base, length);
    auto next = allocations_.lower_bound(base);
    if ((next != allocations_.end() && uint64_t{base} + length > next->first) ||
        (next != allocations_.begin() && uint64_t{std::prev(next)->first} + std::prev(next)->second.length > base)) {
        throw GuestFault("Guest allocation collision: " + owner);
    }

    allocations_.emplace(base, Allocation{length, owner});
}

void Memory::owned(uint32_t base, uint64_t length, const std::string& owner) const {
    page_range(base, length);
    auto it = allocations_.upper_bound(base);
    if (it == allocations_.begin()) {
        throw GuestFault("Unreserved guest range");
    }

    --it;
    if (it->second.owner != owner || uint64_t{base} + length > uint64_t{it->first} + it->second.length) {
        throw GuestFault("Guest allocation ownership mismatch: " + owner);
    }
}

void Memory::commit(uint32_t base, uint64_t length, uint8_t permissions, const std::string& owner) {
    owned(base, length, owner);
    if (permissions & ~7u) {
        throw GuestFault("Invalid guest permissions");
    }

    arena_.commit(base, length);
    for (uint64_t p = base; p < uint64_t{base} + length; p += page_size) {
        auto& page = pages_[p / page_size];
        if (!(page & committed)) {
            std::memset(arena_.base() + p, 0, page_size);
        }

        page = uint8_t(committed | permissions);
    }
}

void Memory::protect(uint32_t base, uint64_t length, uint8_t permissions, const std::string& owner) {
    owned(base, length, owner);
    if (permissions & ~7u) {
        throw GuestFault("Invalid guest permissions");
    }

    for (uint64_t p = base; p < uint64_t{base} + length; p += page_size) {
        if (!(pages_[p / page_size] & committed)) {
            throw GuestFault("Protecting uncommitted guest page");
        }
    }

    for (uint64_t p = base; p < uint64_t{base} + length; p += page_size) {
        pages_[p / page_size] = uint8_t(committed | permissions);
    }
}

void Memory::decommit(uint32_t base, uint64_t length, const std::string& owner) {
    owned(base, length, owner);
    for (uint64_t p = base; p < uint64_t{base} + length; p += page_size) {
        auto& page = pages_[p / page_size];
        if (page & committed) {
            std::memset(arena_.base() + p, 0, page_size);
        }

        page = 0;
    }
}

void Memory::release(uint32_t base, const std::string& owner) {
    auto it = allocations_.find(base);
    if (it == allocations_.end() || it->second.owner != owner) {
        throw GuestFault("Releasing foreign guest allocation");
    }

    decommit(base, it->second.length, owner);
    allocations_.erase(it);
}

void Memory::check_slow(uint32_t address, size_t length, uint8_t permissions) const {
    if (address < 0x10000 || length > size - address) {
        throw GuestFault("Guest access outside IA-32 address range");
    }

    if (!length) {
        return;
    }

    const uint64_t last = (uint64_t{address} + length - 1) / page_size;
    for (uint64_t p = address / page_size; p <= last; ++p) {
        if ((pages_[p] & (committed | permissions)) != (committed | permissions)) {
            std::ostringstream message;
            message << "Guest access to uncommitted or protected page address=" << std::hex << address
                    << " length=" << length << " requested=" << unsigned(permissions) << " page=" << p * page_size
                    << " actual=" << unsigned(pages_[p] & ~committed & 0xffu);
            throw GuestFault(message.str());
        }
    }
}

uint32_t Memory::load32(uint32_t address) const {
    uint8_t b[4];
    read(address, b, sizeof b);
    return uint32_t{b[0]} | uint32_t{b[1]} << 8 | uint32_t{b[2]} << 16 | uint32_t{b[3]} << 24;
}

void Memory::store32(uint32_t address, uint32_t value) {
    const uint8_t b[] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
    write(address, b, sizeof b);
}

uint8_t Memory::permissions(uint32_t address) const {
    return uint8_t(pages_[address / page_size] & ~committed);
}

Heap::~Heap() {
    for (const auto& block : blocks_) {
        memory_.release(block.first, "heap");
    }
}

namespace {
constexpr uint32_t slab_bytes = 65536;
constexpr uint32_t size_classes[] = {16, 32, 48, 64, 80, 96, 128, 160, 192, 256, 320, 384, 512, 768, 1024, 1536, 2048};
constexpr size_t class_count = sizeof size_classes / sizeof size_classes[0];
size_t class_of(uint32_t bytes) {
    size_t index = 0;
    while (size_classes[index] < bytes) {
        ++index;
    }

    return index;
}
}

uint32_t Heap::allocate(uint32_t bytes) {
    if (!bytes) {
        throw GuestFault("Zero-size guest heap request");
    }

    return allocate_pages(bytes);
}

uint32_t Heap::allocate_small(uint32_t bytes) {
    if (!bytes) {
        throw GuestFault("Zero-size guest heap request");
    }

    if (bytes > small_limit) {
        return allocate_pages(bytes);
    }

    if (free_slots_.empty()) {
        free_slots_.resize(class_count);
    }

    const size_t index = class_of(bytes);
    auto& free_list = free_slots_[index];
    if (free_list.empty()) {
        const uint32_t slot = size_classes[index], base = allocate_pages(slab_bytes);
        slabs_.emplace(base, Slab{slab_bytes, slot, std::vector<bool>(slab_bytes / slot, false)});
        for (uint32_t at = slab_bytes / slot; at-- > 0;) {
            free_list.push_back(base + at * slot);
        }
    }

    const uint32_t address = free_list.back();
    free_list.pop_back();
    auto slab = std::prev(slabs_.upper_bound(address));
    slab->second.live[(address - slab->first) / slab->second.slot] = true;
    const std::vector<uint8_t> zeros(slab->second.slot, 0);
    memory_.write(address, zeros.data(), zeros.size());
    return address;
}

uint32_t Heap::allocate_pages(uint32_t bytes) {
    const uint64_t length = (uint64_t{bytes} + Memory::page_size - 1) & ~uint64_t{Memory::page_size - 1};
    // Two windows below 2 GiB (the guest is not large-address-aware): first fit in the primary window, then the
    // overflow window.
    const std::pair<uint32_t, uint32_t> windows[] = {{layout::heap, layout::heap_end},
                                                     {layout::heap_overflow, layout::heap_overflow_end}};
    for (const auto& [low, high] : windows) {
        uint64_t base = low;
        for (auto block = blocks_.lower_bound(low); block != blocks_.end() && block->first < high; ++block) {
            if (base + length <= block->first) {
                break;
            }

            base = uint64_t{block->first} + block->second;
        }

        if (base + length > high) {
            continue;
        }

        memory_.reserve(base, length, "heap");
        memory_.commit(base, length, Read | Write, "heap");
        blocks_.emplace(base, length);
        return base;
    }

    uint64_t used = 0;
    for (const auto& block : blocks_) {
        used += block.second;
    }

    throw GuestFault("Guest heap exhausted (requested " + std::to_string(length) + " bytes, " + std::to_string(used) +
                     " in " + std::to_string(blocks_.size()) + " blocks)");
}

void Heap::free(uint32_t address) {
    auto slab = slabs_.upper_bound(address);
    if (slab != slabs_.begin()) {
        --slab;
        if (address < slab->first + slab->second.length) {
            const uint32_t offset = address - slab->first;
            if (offset % slab->second.slot || !slab->second.live[offset / slab->second.slot]) {
                throw GuestFault("Invalid guest heap allocation");
            }

            slab->second.live[offset / slab->second.slot] = false;
            free_slots_[class_of(slab->second.slot)].push_back(address);
            return;
        }
    }

    auto it = blocks_.find(address);
    if (it == blocks_.end()) {
        throw GuestFault("Invalid guest heap allocation");
    }

    memory_.release(address, "heap");
    blocks_.erase(it);
}

uint64_t Memory::reserved_below(uint32_t limit) const {
    uint64_t bytes = 0;
    for (const auto& allocation : allocations_) {
        if (allocation.first >= limit) {
            break;
        }

        bytes += std::min<uint64_t>(allocation.second.length, uint64_t{limit} - allocation.first);
    }

    return bytes;
}

Thread::Thread(Memory& memory, uint32_t id, const std::vector<TlsTemplate>& templates, uint32_t stack_bytes)
    : memory_(memory), owner_("thread:" + std::to_string(id)) {
    const bool big = stack_bytes > layout::stack_stride - 0x10000;
    const bool huge = stack_bytes > layout::big_stack_stride - 0x10000;
    if (id >= (big ? 16u : (layout::big_stacks - layout::stacks) / layout::stack_stride) ||
        id >= (layout::tls_end - layout::tls) / layout::tls_stride) {
        throw GuestFault("Guest thread range exhausted");
    }

    if (stack_bytes > layout::huge_stack_stride - 0x10000) {
        throw GuestFault("Guest thread stack request exceeds reservation slot");
    }

    const uint32_t stride = huge ? layout::huge_stack_stride : big ? layout::big_stack_stride : layout::stack_stride;
    const uint32_t stack = (huge ? layout::huge_stacks : big ? layout::big_stacks : layout::stacks) + id * stride;
    stack_bottom_ = stack;
    stack_top = stack + stride;
    stack_limit = stack_top - 0x10000;
    teb = layout::tebs + id * Memory::page_size;
    tls_vector = layout::tls + id * layout::tls_stride;
    auto reserve = [&](uint32_t base, uint64_t length) {
        memory_.reserve(base, length, owner_);
        allocations_.push_back(base);
    };
    try {
        reserve(stack, stride);
        memory_.commit(stack_limit, 0x10000, Read | Write, owner_);
        reserve(teb, Memory::page_size);
        memory_.commit(teb, Memory::page_size, Read | Write, owner_);
        reserve(tls_vector, layout::tls_stride);
        memory_.commit(tls_vector, layout::tls_stride, Read | Write, owner_);
        memory_.store32(teb, 0xffffffff); // Empty FS registration chain, not SEH execution.
        memory_.store32(teb + 4, stack_top);
        memory_.store32(teb + 8, stack_limit);
        memory_.store32(teb + 0x18, teb);
        memory_.store32(teb + 0x2c, tls_vector);
        memory_.store32(teb + 0x20, 1); // Guest process identity.
        memory_.store32(teb + 0x24, id + 1);
        uint64_t offset = templates.size() * 4;
        for (const auto& t : templates) {
            offset = (offset + 15) & ~uint64_t{15};
            const uint64_t length = t.bytes.size() + uint64_t{t.zero_fill};
            if (t.index >= templates.size() || offset + length > layout::tls_stride) {
                throw GuestFault("Guest static TLS exceeds thread allocation");
            }

            const uint32_t block = tls_vector + offset;
            memory_.store32(tls_vector + t.index * 4, block);
            memory_.write(block, t.bytes.data(), t.bytes.size());
            offset += length;
        }
    } catch (...) {
        for (uint32_t base : allocations_) {
            memory_.release(base, owner_);
        }

        throw;
    }
}

Thread::~Thread() {
    for (uint32_t base : allocations_) {
        memory_.release(base, owner_);
    }
}

bool Thread::grow_to(uint32_t address) {
    if (address >= stack_limit || address < stack_bottom_ + Memory::page_size) {
        return false;
    }

    const uint32_t host_page = uint32_t(memory_.arena().host_page());
    uint32_t target = address / host_page * host_page;
    if (target < stack_bottom_ + Memory::page_size) {
        target = stack_bottom_ + Memory::page_size;
    }

    try {
        memory_.commit(target, stack_limit - target, Read | Write, owner_);
    } catch (...) {
        return false;
    }

    stack_limit = target;
    memory_.store32(teb + 8, stack_limit);
    return true;
}

void Thread::grow_stack() {
    uint32_t page = stack_limit - Memory::page_size;
    if (page == stack_bottom_) {
        throw GuestFault("guest stack reservation exhausted");
    }

    memory_.commit(page, Memory::page_size, Read | Write, owner_);
    stack_limit = page;
    memory_.store32(teb + 8, stack_limit);
}
}
