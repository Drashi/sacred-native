#include "control.h"
#include "boundary.h"
#include "seh.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>
#include <pthread.h>
#include <csignal>
#include <unistd.h>
#include <unistd.h>

uint8_t* g_mem;
// Direct calls of the generated Sacred.exe code (seh.h RECOMP_DIRECT_*): off until enable_direct_calls().
extern "C" {
int recomp_direct_enabled = 0;
ptrdiff_t recomp_transfer_offset = 0;
}

static bool direct_calls_blocked = false;
// The inline page check of the generated code (x86.h); all zero (every access checked) without a guest control.
static uint8_t no_guest_pages[sacred::Memory::size / sacred::Memory::page_size];
const uint8_t* g_page_access = no_guest_pages;
uint32_t g_watch_base, g_watch_len, g_dirty_count;
RecompDirty g_dirty[RECOMP_DIRTY_SLOTS];
const int recomp_resumable_stacks = 1;

namespace sacred {
static thread_local Control* current;
#ifdef RECOMP_MMU_GUEST
// The generated code accesses guest memory unchecked (x86.h RECOMP_MMU_GUEST): a host fault inside the arena is
// either the growth of a guest stack, done here, or the fatal guest fault the checked path would have raised.
static struct sigaction previous_segv, previous_bus;
}

// Debugging aid: a hook that may take a fault in the arena first (true: handled, the access is retried).
bool (*sacred_guest_fault_hook)(uint32_t address, uint32_t source_pc) = nullptr;
namespace sacred {
static void on_guest_fault(int signal, siginfo_t* info, void*) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(info->si_addr), base = reinterpret_cast<uintptr_t>(g_mem);
    if (g_mem && at >= base && at - base < Memory::size && sacred_guest_fault_hook &&
        sacred_guest_fault_hook(uint32_t(at - base), current ? current->source_pc : 0)) {
        return;
    }

    if (g_mem && at >= base && at - base < Memory::size) {
        if (current && current->grow_stack_to(uint32_t(at - base))) {
            return;
        }

        char message[96];
        const int n = std::snprintf(message, sizeof message, "SACRED_GUEST_FAULT address=%08x source_pc=%08x\n",
                                    unsigned(at - base), current ? unsigned(current->source_pc) : 0u);
        if (n > 0) {
            (void)!write(2, message, size_t(n));
        }
    }

    sigaction(signal, signal == SIGBUS ? &previous_bus : &previous_segv, nullptr); // the fault repeats there
}

static void install_fault_handler() {
    static const bool installed = [] {
        struct sigaction action{};
        action.sa_sigaction = on_guest_fault;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, &previous_segv);
        sigaction(SIGBUS, &action, &previous_bus);
        return true;
    }();
    (void)installed;
}
#endif
std::array<ProfiledCpu, 64> profiled_cpus;
Control& active_control() {
    if (!current) {
        throw GuestFault("no active guest control");
    }

    return *current;
}

static int argument_register(const Signature& signature, size_t index) {
    if (signature.convention == Convention::Thiscall && index == 0) {
        return R_ECX;
    }

    if (signature.convention == Convention::Fastcall) {
        size_t used = 0;
        for (size_t i = 0; i <= index; ++i) {
            if (signature.arguments[i].bytes == 4 && used++ < 2) {
                if (i == index) {
                    return used == 1 ? R_ECX : R_EDX;
                }
            }
        }
    }

    return -1;
}

uint32_t Signature::stack_bytes() const {
    uint32_t bytes = 0;
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i].bytes != 4 && arguments[i].bytes != 8) {
            throw GuestFault("ABI arguments must occupy four or eight guest bytes");
        }

        if (convention == Convention::Thiscall && i == 0 && arguments[i].bytes != 4) {
            throw GuestFault("thiscall requires a guest-width this pointer");
        }

        if (argument_register(*this, i) < 0) {
            bytes += arguments[i].bytes;
        }
    }

    return bytes;
}

Control::Control(Memory& mem, Thread& thread) : Control(std::make_shared<Shared>(mem), thread) {
    root_ = true;
    shared_->scheduler.acquire();
}

Control::Control(Control& process, Thread& thread) : Control(process.shared_, thread) {
    observe_entry = process.observe_entry;
    observe_host = process.observe_host;
}

Control::Control(std::shared_ptr<Shared> shared, Thread& thread)
    : memory(shared->memory), heap(shared->heap), unhandled_filter(shared->unhandled_filter), source_pc(cpu.source_pc),
      shared_(std::move(shared)), thread_(thread), images_(shared_->images), entries_(shared_->entries),
      hosts_(shared_->hosts), unimplemented_(shared_->unimplemented), candidates_(shared_->candidates),
      generation_(shared_->generation) {
    if (current) {
        throw GuestFault("guest control ownership collision");
    }

    current = this;
    const std::ptrdiff_t offset = reinterpret_cast<char*>(&cpu) - reinterpret_cast<char*>(this);
    if (cpu_offset_ && cpu_offset_ != offset) {
        throw GuestFault("inconsistent guest CPU context layout");
    }

    cpu_offset_ = offset;
    recomp_transfer_offset = reinterpret_cast<char*>(&transfer_owner_) - reinterpret_cast<char*>(&cpu);
    g_mem = reinterpret_cast<uint8_t*>(memory.host_address(0));
    g_page_access = memory.access_table();
#ifdef RECOMP_MMU_GUEST
    install_fault_handler();
#endif
    cpu.fs_base = thread.teb;
    cpu.r[R_ESP] = thread.stack_top;
    cpu.cs = 0x1b;
    cpu.ds = 0x23;
    cpu.mxcsr = 0x1f80;
    x87_finit(&cpu);
#ifdef __APPLE__
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
#else
    const uint64_t tid = uint64_t(gettid());
#endif
    for (auto& slot : profiled_cpus) {
        X86* empty = nullptr;
        if (slot.cpu.compare_exchange_strong(empty, &cpu)) {
            slot.thread = tid;
            break;
        }
    }
}

Control::~Control() {
    for (auto& slot : profiled_cpus) {
        if (slot.cpu.load() == &cpu) {
            slot.thread = 0;
            slot.cpu = nullptr;
        }
    }

    recomp_seh_reset(&cpu);
    current = nullptr;
    if (root_ && shared_->scheduler.owns()) {
        shared_->scheduler.release();
    }

    if (shared_.use_count() == 1) {
        g_mem = nullptr;
        g_page_access = no_guest_pages;
    }
}

void Control::module(std::string name, uint32_t base, uint32_t size) {
    if (!size || uint64_t(base) + size > Memory::size) {
        throw GuestFault("invalid dispatch module extent");
    }

    for (const auto& image : images_) {
        if (uint64_t(base) < uint64_t(image.base) + image.size && uint64_t(image.base) < uint64_t(base) + size) {
            throw GuestFault("dispatch module collision");
        }
    }

    images_.push_back({std::move(name), base, size});
}

void Control::catalog(const std::vector<uint8_t>& bytes) {
    if (bytes.empty() || bytes.size() % 4 || !candidates_.empty()) {
        throw GuestFault("invalid or duplicate dispatch boundary catalog");
    }

    candidates_.resize(bytes.size() / 4);
    std::memcpy(candidates_.data(), bytes.data(), bytes.size());
    if (std::adjacent_find(candidates_.begin(), candidates_.end(), [](uint32_t a, uint32_t b) { return a >= b; }) !=
        candidates_.end()) {
        candidates_.clear();
        throw GuestFault("dispatch boundary catalog is not strictly sorted");
    }
}

void Control::entry(uint32_t address, void (*function)(X86*), uint8_t kinds, std::string provenance) {
    memory.check(address, 1, Execute);
    if (!function || !(kinds & (CallEntry | BlockEntry)) || provenance.empty()) {
        throw GuestFault("invalid or duplicate compiled dispatch entry");
    }

    entries_.add(Entry{address, kinds, function});
}

Entry* EntryTable::find(uint32_t address) {
    if (sorted_ != items_.size()) {
        auto by_address = [](const Entry& a, const Entry& b) {
            return a.address < b.address;
        };
        std::sort(items_.begin() + std::ptrdiff_t(sorted_), items_.end(), by_address);
        std::inplace_merge(items_.begin(), items_.begin() + std::ptrdiff_t(sorted_), items_.end(), by_address);
        if (std::adjacent_find(items_.begin(), items_.end(),
                               [](const Entry& a, const Entry& b) { return a.address == b.address; }) != items_.end()) {
            throw GuestFault("invalid or duplicate compiled dispatch entry");
        }

        sorted_ = items_.size();
    }

    auto it = std::lower_bound(items_.begin(), items_.end(), address,
                               [](const Entry& e, uint32_t value) { return e.address < value; });
    return it != items_.end() && it->address == address ? &*it : nullptr;
}

void (*Control::replace_entry(uint32_t address, void (*function)(X86*)))(X86*) {
    Entry* found = entries_.find(address);
    if (!function || !found || !(found->kinds & CallEntry)) {
        throw GuestFault("no compiled call entry to replace");
    }

    // Generated Sacred.exe code calls its callees' regions directly, past this table.
    if (address < 0x10000000u) {
        direct_calls_blocked = true;
        recomp_direct_enabled = 0;
    }

    return std::exchange(found->function, function);
}

void Control::enable_direct_calls(bool on) {
    recomp_direct_enabled = on && !direct_calls_blocked;
}

bool Control::direct_calls() {
    return recomp_direct_enabled != 0;
}

bool Control::run_call() {
    const uint32_t slot = cpu.r[R_ESP], address = memory.load32(slot);
    if (drivers_.size() == Memory::page_size / layout::token_stride) {
        throw GuestFault("callback sentinel range exhausted");
    }

    const uint64_t id = ++generation_;
    const uint32_t sentinel = layout::returns + uint32_t(drivers_.size()) * layout::token_stride;
    drivers_.push_back({id, sentinel});
    memory.store32(slot, sentinel);
    auto leave = [&] {
        recomp_seh_driver_leave(&cpu, id);
        for (auto it = jumps_.begin(); it != jumps_.end();) {
            if (it->second.owner == id) {
                it = jumps_.erase(it);
            } else {
                ++it;
            }
        }

        drivers_.pop_back();
    };
    int completed = 0;
    try {
        completed = sacred_drive(&cpu, id, sentinel);
    } catch (...) {
        leave();
        throw;
    }

    leave();
    if (completed) {
        cpu.eip = address;
    }

    return completed != 0;
}

void Control::host(uint32_t token, Signature signature, HostFunction function, std::string name) {
    signature.stack_bytes();
    if (token < layout::hle || token >= layout::hle_end || (token - layout::hle) % layout::token_stride || !function ||
        !hosts_.emplace(token, Host{std::move(signature), std::move(function), std::move(name)}).second) {
        throw GuestFault("invalid or duplicate host callable token");
    }
}

uint32_t Control::arg(uint32_t index) const {
    return memory.load32(cpu.r[R_ESP] + 4 + index * 4);
}

[[noreturn]] void Control::missing(uint32_t target, const char* reason) const {
    std::ostringstream out;
    out << reason << " target=" << std::hex << target << " source_pc=" << source_pc << " eip=" << cpu.eip;
    out << " boundary="
        << (std::binary_search(candidates_.begin(), candidates_.end(), target) ? "exported_candidate" : "not_admitted");
    for (const auto& image : images_) {
        if (target >= image.base && uint64_t(target) < uint64_t(image.base) + image.size) {
            out << " module=" << image.name << " rva=" << target - image.base;
        }
    }

    for (unsigned i = 0; i < 8; ++i) {
        out << " r" << i << "=" << cpu.r[i];
    }

    out << " x87_cw=" << cpu.fpu_cw << " x87_sw=" << cpu.fpu_sw << " x87_top=" << cpu.fpu_top
        << " x87_tag=" << cpu.fpu_tag << " mxcsr=" << cpu.mxcsr;
    for (unsigned i = 0; i < 8; ++i) {
        out << " st" << i << "=" << cpu.st[i].signExp << ":" << cpu.st[i].signif;
        out << " xmm" << i << "=";
        for (unsigned lane = 0; lane < 4; ++lane) {
            out << cpu.xmm[i][lane] << ",";
        }
    }

    out << " flags=" << x86_get_eflags(&cpu) << " fs=" << cpu.fs_base << " stack=";
    for (uint32_t i = 0; i < 8; ++i) {
        try {
            out << memory.load32(cpu.r[R_ESP] + i * 4) << ",";
        } catch (const GuestFault&) {
            out << "<unreadable>";
            break;
        }
    }

    out << " source_bytes=";
    for (uint32_t i = 0; i < 16; ++i) {
        uint8_t byte;
        try {
            memory.read(source_pc + i, &byte, 1);
        } catch (const GuestFault&) {
            out << "<unreadable>";
            break;
        }

        out << std::setw(2) << std::setfill('0') << unsigned(byte);
    }

    throw GuestFault(out.str());
}

void Control::call_host(const Host& host) {
#ifdef SACRED_QA
    if (isolated) {
        throw GuestFault("host call in isolated invoke: " + host.name);
    }
#endif
    if (host_depth_ == host_arguments_.size()) {
        host_arguments_.emplace_back();
    }

    std::vector<uint64_t>& args = host_arguments_[host_depth_];
    args.clear();
    struct Level {
        size_t& depth;
        Level(size_t& value) : depth(value) { ++depth; }
        ~Level() { --depth; }
    } level{host_depth_};
    uint32_t at = cpu.r[R_ESP] + 4;
    for (size_t i = 0; i < host.signature.arguments.size(); ++i) {
        int reg = argument_register(host.signature, i);
        uint64_t value = reg < 0 ? memory.load32(at) : cpu.r[reg];
        if (reg < 0) {
            if (host.signature.arguments[i].bytes == 8) {
                value |= uint64_t(memory.load32(at + 4)) << 32;
            }

            at += host.signature.arguments[i].bytes;
        }

        args.push_back(value);
    }

    Result result;
    uint32_t call_source = source_pc;
    try {
        result = host.function(*this, args);
    } catch (const GuestFault& error) {
        std::ostringstream diagnostic;
        diagnostic << host.name << " source_pc=" << std::hex << source_pc << " args=";
        for (uint64_t argument : args) {
            diagnostic << argument << ",";
        }

        diagnostic << " " << error.what();
        throw GuestFault(diagnostic.str());
    }

    if (transferring()) {
        return; // Native locals unwind normally before the C boundary.
    }

    if (observe_host) {
        observe_host(host.name, call_source, args, host.signature.result, result);
    }

    switch (host.signature.result) {
    case ReturnChannel::Eax:
        cpu.r[R_EAX] = uint32_t(result.integer);
        break;
    case ReturnChannel::EdxEax:
        cpu.r[R_EAX] = uint32_t(result.integer);
        cpu.r[R_EDX] = uint32_t(result.integer >> 32);
        break;
    case ReturnChannel::St0:
        x87_push(&cpu, result.floating);
        break;
    case ReturnChannel::OutBuffer:
    case ReturnChannel::Void:
        break;
    }

    cpu.eip = memory.load32(cpu.r[R_ESP]);
    cpu.r[R_ESP] += 4 + (host.signature.convention == Convention::Cdecl ? 0 : host.signature.stack_bytes());
}

void Control::dispatch(uint32_t target, uint8_t kind) {
    DispatchSlot& slot = dispatch_cache_[(target ^ (target >> 13)) & (dispatch_slots - 1)];
    if (slot.target == target) {
        if (slot.host) {
            if (RECOMP_UNLIKELY(trace_hook_)) {
                trace_hook_(*this, target, 0, kind);
            }

            call_host(*slot.host);
            return;
        }

        if (slot.kinds & kind) {
            cpu.eip = target;
            if (RECOMP_UNLIKELY(observe_entry)) {
                observe(target);
            }

            if (RECOMP_UNLIKELY(trace_hook_)) {
                trace_hook_(*this, target, slot.kinds, kind);
            }

            try {
                slot.function(&cpu);
            } catch (const GuestFault& error) {
                missing(cpu.eip, error.what());
            }

            return;
        }
    }

    // Return tokens are recognized only by their live owner in the C driver.
    if (target >= layout::returns && target < layout::returns + Memory::page_size) {
        missing(target, "unowned callback return sentinel");
    }

    if (auto found = hosts_.find(target); found != hosts_.end()) {
        slot = DispatchSlot{target, 0, nullptr, &found->second};
        if (RECOMP_UNLIKELY(trace_hook_)) {
            trace_hook_(*this, target, 0, kind);
        }

        call_host(found->second);
        return;
    }

    Entry* found = entries_.find(target);
    if (auto unknown = unimplemented_.find(target); unknown != unimplemented_.end()) {
        missing(target, ("unimplemented import " + unknown->second).c_str());
    }

    if (!found || !(found->kinds & kind)) {
        missing(target, "missing compiled guest target");
    }

    slot = DispatchSlot{target, found->kinds, found->function, nullptr};
    cpu.eip = target;
    if (RECOMP_UNLIKELY(observe_entry)) {
        observe(target);
    }

    if (RECOMP_UNLIKELY(trace_hook_)) {
        trace_hook_(*this, target, found->kinds, kind);
    }

    try {
        slot.function(&cpu);
    } catch (const GuestFault& error) {
        missing(cpu.eip, error.what());
    }
}

const std::string* Control::host_name(uint32_t token) const {
    auto found = hosts_.find(token);
    return found == hosts_.end() ? nullptr : &found->second.name;
}

void Control::observe(uint32_t target) {
    observe_entry(target);
    if (drop_observer_) {
        drop_observer_ = false;
        observe_entry = nullptr;
    }
}

void (*Control::direct_target(uint32_t target))(X86*) {
    DispatchSlot& slot = dispatch_cache_[(target ^ (target >> 13)) & (dispatch_slots - 1)];
    if (slot.target != target) {
        if ((target >= layout::returns && target < layout::returns + Memory::page_size) || hosts_.count(target) ||
            unimplemented_.count(target)) {
            return nullptr;
        }

        const Entry* found = entries_.find(target);
        if (!found || (found->kinds & (CallEntry | BlockEntry)) != (CallEntry | BlockEntry)) {
            return nullptr;
        }

        slot = DispatchSlot{target, found->kinds, found->function, nullptr};
    }

    if (slot.host || (slot.kinds & (CallEntry | BlockEntry)) != (CallEntry | BlockEntry)) {
        return nullptr;
    }

    return slot.function;
}

uint64_t Control::owner() const {
    if (drivers_.empty()) {
        throw GuestFault("guest transfer without a live C driver");
    }

    return drivers_.back().id;
}

void Control::defer(uint64_t destination, uint32_t target) {
    if (std::none_of(drivers_.begin(), drivers_.end(),
                     [destination](const Driver& d) { return d.id == destination; })) {
        missing(target, "nonlocal target has no live driver");
    }

    transfer_owner_ = destination;
    cpu.eip = target;
}

int Control::step(uint64_t id, uint32_t sentinel) {
    if (transferring()) {
        if (transfer_owner_ != id) {
            return 0;
        }

        transfer_owner_ = 0;
        ++transfers;
        return 2;
    }

    bool first = drivers_.back().first;
    drivers_.back().first = false;
    if (!first && cpu.eip == sentinel) {
        return 1;
    }

    dispatch(cpu.eip, first ? CallEntry : BlockEntry);
    if (transferring()) {
        if (transfer_owner_ != id) {
            return 0;
        }

        transfer_owner_ = 0;
        ++transfers;
    }

    return cpu.eip == sentinel ? 1 : 2;
}

Result Control::invoke(uint32_t target, const Signature& signature, const std::vector<uint64_t>& arguments) {
    if (arguments.size() != signature.arguments.size()) {
        throw GuestFault("callback argument count");
    }

    uint32_t stack_bytes = signature.stack_bytes();
    if (drivers_.size() == Memory::page_size / layout::token_stride) {
        throw GuestFault("callback sentinel range exhausted");
    }

    X86 saved = cpu;
    if (drivers_.empty()) {
        source_pc = cpu.eip;
    }

    uint32_t saved_source = source_pc;
    uint32_t saved_chain = memory.load32(cpu.fs_base);
    if (uint64_t(stack_bytes) + 4 > cpu.r[R_ESP]) {
        throw GuestFault("callback stack address overflow");
    }

    memory.check(cpu.r[R_ESP] - stack_bytes - 4, stack_bytes + 4, Write);
    uint64_t id = ++generation_;
    uint32_t sentinel = layout::returns + uint32_t(drivers_.size()) * layout::token_stride;
    drivers_.push_back({id, sentinel});
    cpu.r[R_ESP] -= stack_bytes + 4;
    uint32_t at = cpu.r[R_ESP] + 4;
    memory.store32(cpu.r[R_ESP], sentinel);
    for (size_t i = 0; i < arguments.size(); ++i) {
        int reg = argument_register(signature, i);
        if (reg >= 0) {
            cpu.r[reg] = uint32_t(arguments[i]);
        } else {
            memory.store32(at, uint32_t(arguments[i]));
            if (signature.arguments[i].bytes == 8) {
                memory.store32(at + 4, uint32_t(arguments[i] >> 32));
            }

            at += signature.arguments[i].bytes;
        }
    }

    cpu.eip = target;
    Result result;
    try {
        int completed = sacred_drive(&cpu, id, sentinel);
        if (completed) {
            uint32_t expected = saved.r[R_ESP] - (signature.convention == Convention::Cdecl ? stack_bytes : 0);
            if (cpu.r[R_ESP] != expected) {
                throw GuestFault("callback stack cleanup mismatch");
            }

            for (int reg : {R_EBX, R_ESI, R_EDI, R_EBP}) {
                if (cpu.r[reg] != saved.r[reg]) {
                    throw GuestFault("callback corrupted nonvolatile register r" + std::to_string(reg) +
                                     " at guest entry " + std::to_string(target));
                }
            }

            result.integer = cpu.r[R_EAX];
            if (signature.result == ReturnChannel::EdxEax) {
                result.integer |= uint64_t(cpu.r[R_EDX]) << 32;
            }

            if (signature.result == ReturnChannel::St0) {
                result.floating = X87_ST(&cpu, 0);
            }

            if (cpu.fs_base != saved.fs_base || memory.load32(cpu.fs_base) != saved_chain) {
                throw GuestFault("callback changed FS chain");
            }

            cpu = saved;
            source_pc = saved_source;
        }
    } catch (...) {
        recomp_seh_driver_leave(&cpu, id);
        for (auto it = jumps_.begin(); it != jumps_.end();) {
            if (it->second.owner == id) {
                it = jumps_.erase(it);
            } else {
                ++it;
            }
        }

        drivers_.pop_back();
        cpu = saved;
        memory.store32(saved.fs_base, saved_chain);
        throw;
    }

    recomp_seh_driver_leave(&cpu, id);
    for (auto it = jumps_.begin(); it != jumps_.end();) {
        if (it->second.owner == id) {
            it = jumps_.erase(it);
        } else {
            ++it;
        }
    }

    drivers_.pop_back();
    return result;
}

void Control::guest_setjmp() {
    uint32_t buffer = arg(0);
    memory.check(buffer, 64, Write);
    const int regs[] = {R_EBP, R_EBX, R_EDI, R_ESI};
    for (unsigned i = 0; i < 4; ++i) {
        memory.store32(buffer + i * 4, cpu.r[regs[i]]);
    }

    memory.store32(buffer + 16, cpu.r[R_ESP] + 4);
    memory.store32(buffer + 20, memory.load32(cpu.r[R_ESP]));
    memory.store32(buffer + 24, memory.load32(cpu.fs_base));
    jumps_[buffer] = {owner(), cpu.r[R_ESP] + 4};
    cpu.r[R_EAX] = 0;
    cpu.eip = memory.load32(cpu.r[R_ESP]);
    cpu.r[R_ESP] += 4;
}

void Control::guest_longjmp() {
    uint32_t buffer = arg(0), value = arg(1);
    auto jump = jumps_.find(buffer);
    if (jump == jumps_.end()) {
        missing(buffer, "unowned guest jmp_buf");
    }

    if (memory.load32(buffer + 24) != memory.load32(cpu.fs_base)) {
        uint32_t chain = memory.load32(buffer + 24);
        recomp_seh_unwind(&cpu, chain == 0xffffffffu ? 0 : chain, 0, 0, value);
    }

    const int regs[] = {R_EBP, R_EBX, R_EDI, R_ESI};
    for (unsigned i = 0; i < 4; ++i) {
        cpu.r[regs[i]] = memory.load32(buffer + i * 4);
    }

    cpu.r[R_ESP] = memory.load32(buffer + 16);
    cpu.r[R_EAX] = value ? value : 1;
    if (recomp_seh_pending_target()) {
        recomp_seh_intercept(&cpu, memory.load32(buffer + 20));
    } else {
        defer(jump->second.owner, memory.load32(buffer + 20));
    }
}

void Control::bind_exception_imports(const Loader& loader) {
    for (const auto& slot : loader.hle_slots()) {
        unimplemented_[slot.token] = slot.dll + "!" + slot.symbol;
        if (slot.dll != "kernel32.dll") {
            continue;
        }

        if (slot.symbol == "RaiseException") {
            host(
                slot.token, {Convention::Stdcall, ReturnChannel::Void, {{4}, {4}, {4}, {4}}, ""},
                [](Control& control, const std::vector<uint64_t>& a) {
                    if (!recomp_seh_raise(&control.cpu, a[0], a[1], a[2], a[3])) {
                        control.missing(control.cpu.eip, "unhandled guest exception");
                    }

                    return Result{};
                },
                slot.symbol);
        } else if (slot.symbol == "RtlUnwind") {
            host(
                slot.token, {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}}, ""},
                [](Control& control, const std::vector<uint64_t>& a) {
                    recomp_seh_unwind(&control.cpu, a[0], a[1], a[2], a[3]);
                    return Result{a[3], {}};
                },
                slot.symbol);
        } else if (slot.symbol == "SetUnhandledExceptionFilter") {
            host(
                slot.token, {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
                [](Control& c, const std::vector<uint64_t>& a) {
                    uint32_t previous = c.unhandled_filter;
                    c.unhandled_filter = a[0];
                    return Result{previous, {}};
                },
                slot.symbol);
        } else if (slot.symbol == "UnhandledExceptionFilter") {
            host(
                slot.token, {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
                [](Control& c, const std::vector<uint64_t>& a) {
                    if (!c.unhandled_filter) {
                        return Result{};
                    }

                    return c.invoke(c.unhandled_filter,
                                    {Convention::Stdcall, ReturnChannel::Eax, {{4}}, "top-level filter"}, a);
                },
                slot.symbol);
        }
    }
}
}

extern "C" {
void sacred_guest_read(uint32_t a, void* out, size_t bytes) {
    auto& control = sacred::active_control();
    control.probe_stack(a, bytes);
    control.memory.read(a, out, bytes);
}

void sacred_guest_write(uint32_t a, const void* in, size_t bytes) {
    auto& control = sacred::active_control();
    control.probe_stack(a, bytes);
    control.memory.write(a, in, bytes);
}

int sacred_boundary_step(X86*, uint64_t owner, uint32_t sentinel) {
    return sacred::active_control().step(owner, sentinel);
}

uint64_t sacred_driver_owner(X86*) {
    return sacred::active_control().owner();
}

int sacred_transfer_pending() {
    return sacred::active_control().transferring();
}

void recomp_yield(X86* c) {
    auto& control = sacred::Control::of(c);
    c->budget = 0;
#ifdef SACRED_QA
    if (control.isolated) {
        return;
    }
#endif
    control.scheduler().yield();
}

void sacred_defer_transfer(X86*, uint64_t owner, uint32_t target) {
    sacred::active_control().defer(owner, target);
}

void recomp_guest_setjmp(X86*) {
    sacred::active_control().guest_setjmp();
}

void recomp_guest_longjmp(X86*) {
    sacred::active_control().guest_longjmp();
}

void (*recomp_direct_target(X86* c, uint32_t target))(X86*) {
    return sacred::Control::of(c).direct_target(target);
}

void recomp_call(X86*, uint32_t target) {
    sacred::active_control().dispatch(target, sacred::CallEntry);
}

void recomp_jump(X86* c, uint32_t target) {
    if (recomp_seh_pending_target()) {
        recomp_seh_intercept(c, target);
        return;
    }

    sacred::active_control().dispatch(target, sacred::BlockEntry);
}

void recomp_run(X86*, uint32_t target) {
    auto& control = sacred::active_control();
    auto result = control.invoke(target, {sacred::Convention::Stdcall, sacred::ReturnChannel::Eax, {}, ""}, {});
    if (!control.transferring()) {
        control.cpu.r[R_EAX] = uint32_t(result.integer);
    }
}

void recomp_unknown_call(X86*, uint32_t target) {
    sacred::active_control().missing(target, "unknown call");
}

void recomp_unknown_jump(X86*, uint32_t target) {
    sacred::active_control().missing(target, "unknown jump");
}

void recomp_unmodelled(X86*, uint32_t target) {
    sacred::active_control().missing(target, "unmodelled instruction");
}

void recomp_breakpoint(X86*, uint32_t target) {
    sacred::active_control().missing(target, "unexpected guest breakpoint");
}

void recomp_int(X86*, uint32_t vector) {
    sacred::active_control().missing(vector, "unexpected guest interrupt");
}

void recomp_fpu_error(X86* c) {
    if (!recomp_seh_raise(c, 0xc0000090u, 0, 0, 0)) {
        sacred::active_control().missing(c->eip, "unhandled guest floating-point exception");
    }
}

void recomp_alignment_error(X86* c, uint32_t address) {
    sacred::active_control().missing(address, "guest alignment exception");
    (void)c;
}

void recomp_div_error(X86* c, uint32_t address) {
    if (!recomp_seh_raise(c, 0xc0000094u, 0, 0, 0)) {
        sacred::active_control().missing(address, "unhandled guest divide exception");
    }
}

void recomp_watch_hit(uint32_t, uint32_t, uint64_t) {
    throw sacred::GuestFault("unadmitted memory watch");
}

uint32_t recomp_callback_depth() {
    return sacred::active_control().depth();
}

void recomp_callback_reset(X86*) {
    if (sacred::active_control().depth()) {
        throw sacred::GuestFault("cannot reset a CPU with live C callback drivers");
    }
}
}
