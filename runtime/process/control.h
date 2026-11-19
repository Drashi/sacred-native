#pragma once
#include "loader.h"
#include "x86.h"
#include "scheduler.h"
#include <deque>
#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

namespace sacred {
enum class Convention { Cdecl, Stdcall, Thiscall, Fastcall };
enum class ReturnChannel { Eax, EdxEax, St0, OutBuffer, Void };
enum class PointerDirection { Value, In, Out, InOut };
struct Argument {
    uint8_t bytes = 4;
    PointerDirection direction = PointerDirection::Value;
};
struct Signature {
    Convention convention = Convention::Stdcall;
    ReturnChannel result = ReturnChannel::Eax;
    std::vector<Argument> arguments;
    std::string callback_signature;
    uint32_t stack_bytes() const;
};
struct Result {
    uint64_t integer = 0;
    X87Value floating{};
};
enum EntryKind : uint8_t { CallEntry = 1, BlockEntry = 2 };
class Control;
// Guest CPU contexts by host thread id (pthread_threadid_np / gettid), for the sampling profiler
// (runtime/modules/profiler.cpp), which reads them while that thread is suspended.
struct ProfiledCpu {
    std::atomic<uint64_t> thread{0};
    std::atomic<X86*> cpu{nullptr};
};
extern std::array<ProfiledCpu, 64> profiled_cpus;
using HostFunction = std::function<Result(Control&, const std::vector<uint64_t>&)>;
struct Entry {
    uint32_t address;
    uint8_t kinds;
    void (*function)(X86*);
};
// Compiled dispatch entries by guest address (~1.5 million of them for Sacred.exe and the DLLs): a flat array,
// sorted again on the first lookup after additions. A std::map node per entry held ~110 MB.
class EntryTable {
public:
    void add(const Entry& entry) { items_.push_back(entry); }
    Entry* find(uint32_t address);

private:
    std::vector<Entry> items_;
    size_t sorted_ = 0;
};

// CPU/drivers belong to one guest thread. Process memory, allocation and
// dispatch registrations are shared; guest execution remains serialized.
class Control {
    struct Image {
        std::string name;
        uint32_t base, size;
    };
    struct Host {
        Signature signature;
        HostFunction function;
        std::string name;
    };
    struct Shared {
        explicit Shared(Memory& value) : memory(value), heap(value) {}
        Memory& memory;
        Heap heap;
        std::vector<Image> images;
        EntryTable entries;
        std::map<uint32_t, Host> hosts;
        std::map<uint32_t, std::string> unimplemented;
        std::vector<uint32_t> candidates;
        uint64_t generation = 0;
        uint32_t unhandled_filter = 0;
        Scheduler scheduler;
        std::function<uint64_t()> clock;
    };
    Control(std::shared_ptr<Shared> shared, Thread& thread);

public:
    Control(Memory& memory, Thread& thread);
    Control(Control& process, Thread& thread);
    ~Control();
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;
    X86 cpu{};
    // The Control that owns a guest CPU context. Generated code only ever passes Control::cpu (saved copies are
    // restored, never executed), so this replaces the thread_local lookup in per-instruction callbacks.
    static Control& of(X86* context) {
        return *reinterpret_cast<Control*>(reinterpret_cast<char*>(context) - cpu_offset_);
    }

    Memory& memory;
    Heap& heap;
    void module(std::string name, uint32_t base, uint32_t size);
    void catalog(const std::vector<uint8_t>& little_endian_boundaries);
    void entry(uint32_t address, void (*function)(X86*), uint8_t kinds, std::string provenance);
    // Puts a native stand-in for a guest function in place of the generated code of its entry and returns that
    // code. Only before guest code runs: dispatch caches are not revisited.
    void (*replace_entry(uint32_t address, void (*function)(X86*)))(X86*);
    // Runs the guest function at cpu.eip, just entered by a CALL (its return address is on the stack), until it
    // returns there. False when a non-local transfer left it instead.
    bool run_call();
    void host(uint32_t token, Signature signature, HostFunction function, std::string name);
    void bind_exception_imports(const Loader& loader);
    Result invoke(uint32_t target, const Signature& signature, const std::vector<uint64_t>& arguments);
    void dispatch(uint32_t target, uint8_t kind);
    [[noreturn]] void missing(uint32_t target, const char* reason) const;
    uint64_t owner() const;
    uint32_t depth() const { return static_cast<uint32_t>(drivers_.size()); }
    void defer(uint64_t owner, uint32_t target);
    bool transferring() const { return transfer_owner_ != 0; }
    int step(uint64_t owner, uint32_t sentinel);
    uint32_t arg(uint32_t index) const;
    void guest_setjmp();
    void guest_longjmp();
    void probe_stack(uint32_t address, size_t length) { thread_.probe_stack(address, length); }
    bool grow_stack_to(uint32_t address) { return thread_.grow_to(address); }
    uint32_t& unhandled_filter;
    Scheduler& scheduler() { return shared_->scheduler; }
    void set_clock(std::function<uint64_t()> clock) { shared_->clock = std::move(clock); }
    uint64_t timestamp() const {
        if (!shared_->clock) {
            throw GuestFault("no guest timestamp source");
        }

        return shared_->clock();
    }

    uint32_t transfers = 0;
    // The generated code's block source of this thread (seh.h recomp_block).
    uint32_t& source_pc;
    // Debug observation of guest entries, after compiled-target admission.
    std::function<void(uint32_t)> observe_entry;
    // Called from inside observe_entry: removes it once it returns (and so from threads created later). Every
    // dispatch pays for an installed observer, and the game makes millions of guest calls per second.
    void stop_observing_entries() { drop_observer_ = true; }
    // Lets the generated code call its callees directly (seh.h RECOMP_DIRECT_*) instead of through dispatch(), which
    // bypasses observe_entry and replace_entry: never while an entry observer must see every call, and never once code
    // of Sacred.exe was replaced. Process-wide.
    static void enable_direct_calls(bool on);
    static bool direct_calls();
    // The compiled function a direct indirect call or jump of the generated code may run for target (seh.h
    // recomp_direct_target): the entry dispatch() would run, or null where dispatch() would do anything else.
    void (*direct_target(uint32_t target))(X86*);
    std::function<void(const std::string&, uint32_t, const std::vector<uint64_t>&, ReturnChannel, const Result&)>
        observe_host;
    // QA call trace (runtime/modules/call_trace.cpp): while set, called for every dispatch of a compiled target or a
    // host function, before it runs. `kinds` are the EntryKind bits of the target (0 for a host function), `kind`
    // is what the dispatch asked for (CallEntry only for the first dispatch of a C driver). Process-wide; it sees
    // every guest call only while direct calls are off.
    using TraceHook = void (*)(Control&, uint32_t target, uint8_t kinds, uint8_t kind);
    static void set_trace(TraceHook hook) { trace_hook_ = hook; }
    // The registered name of a host function, null for anything else.
    const std::string* host_name(uint32_t token) const;
#ifdef SACRED_QA
    // Oracle isolate child (fork): guest memory only. Host calls fail and the baton is never yielded.
    bool isolated = false;
#endif
private:
    void observe(uint32_t target);
    bool drop_observer_ = false;
    static inline TraceHook trace_hook_ = nullptr;
    static inline std::ptrdiff_t cpu_offset_ = 0;
    std::shared_ptr<Shared> shared_;
    Thread& thread_;
    struct Driver {
        uint64_t id;
        uint32_t sentinel;
        bool first = true;
    };
    struct Jump {
        uint64_t owner;
        uint32_t esp;
    };
    std::vector<Image>& images_;
    EntryTable& entries_;
    // Admitted compiled targets, direct-mapped by address: every indirect call and jump of the generated code goes
    // through dispatch(), and the ordered-map lookups were its largest cost. Only successful lookups are cached;
    // entries are never removed, so a cached function stays valid.
    // A slot holds a compiled target or (host set) a host function; hosts are never removed either.
    struct DispatchSlot {
        uint32_t target = 0;
        uint8_t kinds = 0;
        void (*function)(X86*) = nullptr;
        const Host* host = nullptr;
    };
    static constexpr size_t dispatch_slots = 8192;
    std::array<DispatchSlot, dispatch_slots> dispatch_cache_{};
    std::map<uint32_t, Host>& hosts_;
    std::map<uint32_t, std::string>& unimplemented_;
    std::vector<uint32_t>& candidates_;
    std::vector<Driver> drivers_;
    // Argument lists of the host calls in progress, one per nesting level, reused from call to call: the game
    // makes thousands of host calls per frame and each list was a few heap allocations.
    std::deque<std::vector<uint64_t>> host_arguments_;
    size_t host_depth_ = 0;
    void call_host(const Host& host);
    std::map<uint32_t, Jump> jumps_;
    uint64_t& generation_;
    uint64_t transfer_owner_ = 0;
    bool root_ = false;
};
Control& active_control();
}
