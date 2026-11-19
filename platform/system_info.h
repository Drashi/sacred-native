#pragma once
#include <cstdint>

namespace sacred {
struct MemoryTotals {
    uint64_t physical_total, physical_free, swap_total, swap_free;
};
MemoryTotals host_memory();
}
