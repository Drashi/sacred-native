#include "system_info.h"
#include "../runtime/process/memory.h"

#if defined(__linux__)
#include <sys/sysinfo.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#error "host_memory needs a port for this operating system"
#endif

namespace sacred {
MemoryTotals host_memory() {
#if defined(__linux__)
    struct sysinfo info;
    if (sysinfo(&info)) {
        throw GuestFault("host memory totals are unavailable");
    }

    const uint64_t unit = info.mem_unit ? info.mem_unit : 1;
    return {info.totalram * unit, info.freeram * unit, info.totalswap * unit, info.freeswap * unit};
#else
    uint64_t total = 0;
    size_t length = sizeof total;
    if (sysctlbyname("hw.memsize", &total, &length, nullptr, 0)) {
        throw GuestFault("host memory totals are unavailable");
    }

    // Free memory as the kernel reports it: free + inactive (reclaimable) + speculative pages.
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS ||
        host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) !=
            KERN_SUCCESS) {
        throw GuestFault("host memory statistics are unavailable");
    }

    const uint64_t free = (uint64_t(vm.free_count) + vm.inactive_count + vm.speculative_count) * page;
    // macOS swap is dynamic; vm.swapusage reports the current swap files (zero when none exist yet).
    xsw_usage swap{};
    length = sizeof swap;
    if (sysctlbyname("vm.swapusage", &swap, &length, nullptr, 0)) {
        swap = {};
    }

    return {total, free < total ? free : total, swap.xsu_total, swap.xsu_avail};
#endif
}
}
