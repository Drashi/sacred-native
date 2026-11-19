#include "startup.h"

namespace sacred {
namespace {
constexpr uint32_t page_noaccess = 1, page_readonly = 2, page_readwrite = 4, page_execute = 0x10,
                   page_execute_read = 0x20, page_execute_readwrite = 0x40, page_nocache = 0x200;
uint32_t windows_protection(uint8_t permissions) {
    switch (permissions) {
    case 0:
        return page_noaccess;
    case Read:
        return page_readonly;
    case Read | Write:
        return page_readwrite;
    case Execute:
        return page_execute;
    case Read | Execute:
        return page_execute_read;
    default:
        return page_execute_readwrite;
    }
}

uint8_t permissions_of(uint32_t protection) {
    switch (protection & ~page_nocache) {
    case page_noaccess:
        return 0;
    case page_readonly:
        return Read;
    case page_readwrite:
        return Read | Write;
    case page_execute:
        return Execute;
    case page_execute_read:
        return Read | Execute;
    case page_execute_readwrite:
        return Read | Write | Execute;
    default:
        throw GuestFault("unqualified VirtualProtect protection " + std::to_string(protection));
    }
}
}

void Startup::bind_virtual() {
    control_.host(
        loader_.resolve("kernel32.dll", "HeapDestroy"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control& current, const std::vector<uint64_t>& a) {
            const uint32_t heap = uint32_t(a[0]);
            if (!heaps_.count(heap)) {
                return Result{error(6), {}};
            }

            for (auto block = blocks_.begin(); block != blocks_.end();) {
                if (block->second.heap != heap) {
                    ++block;
                    continue;
                }

                current.heap.free(block->first);
                block = blocks_.erase(block);
            }

            heaps_.erase(heap);
            return Result{1, {}};
        },
        "kernel32.dll!HeapDestroy");
    control_.host(
        loader_.resolve("kernel32.dll", "VirtualProtect"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4, PointerDirection::Out}}, ""},
        [this](Control& current, const std::vector<uint64_t>& a) {
            const uint32_t size = uint32_t(a[1]), page = Memory::page_size;
            if (!a[0] || !size || !a[3] || uint64_t(a[0]) + size > UINT32_MAX) {
                return Result{error(87), {}};
            }

            const uint32_t first = uint32_t(a[0]) & ~(page - 1);
            const uint64_t end = (uint64_t(a[0]) + size + page - 1) & ~uint64_t(page - 1);
            const uint8_t permissions = permissions_of(uint32_t(a[2]));
            auto found = page_protection_.find(first);
            const uint32_t previous =
                found != page_protection_.end() ? found->second : windows_protection(current.memory.permissions(first));
            current.memory.protect(first, end - first, permissions, "heap");
            for (uint64_t at = first; at < end; at += page) {
                page_protection_[uint32_t(at)] = uint32_t(a[2]);
            }

            current.memory.store32(uint32_t(a[3]), previous);
            return Result{1, {}};
        },
        "kernel32.dll!VirtualProtect");
}
}
