#include "runtime/process/control.h"
#include "seh.h"
#include "fixtures.h"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <exception>

using namespace sacred;
extern "C" {
struct FixtureEntry {
    uint32_t address;
    void (*function)(X86*);
    const char* name;
    uint8_t kinds;
};
extern const FixtureEntry control_fixtures[];
extern const unsigned control_fixture_count;
}

static unsigned checks;
#define CHECK(value)                                                                                                   \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(value)) {                                                                                                \
            throw GuestFault(std::string("CHECK ") + #value + " at " + std::to_string(__LINE__));                      \
        }                                                                                                              \
    } while (0)

static Signature signature(Convention convention, size_t arguments = 0, ReturnChannel result = ReturnChannel::Eax) {
    return {convention, result, std::vector<Argument>(arguments), "qualification"};
}

static void fault(const std::function<void()>& run, const std::string& expected) {
    try {
        run();
    } catch (const GuestFault& error) {
        CHECK(std::string(error.what()).find(expected) != std::string::npos);
        return;
    }

    throw GuestFault("negative fixture did not fail: " + expected);
}

static void map_fixture(Memory& memory, uint32_t at, const std::vector<uint8_t>& bytes, const std::string& name) {
    uint64_t length = (bytes.size() + 4095) & ~uint64_t(4095);
    memory.reserve(at, length, name);
    memory.commit(at, length, Read | Write, name);
    memory.write(at, bytes.data(), bytes.size());
    memory.protect(at, length, Read | Execute, name);
}

static void qualify(const std::string& assets, const std::string& build) {
    // Given: real mapped images/IAT, a real guest thread, and separately mapped
    // synthetic x86 fixtures translated by the adopted emitter.
    Memory memory;
    Loader loader(memory);
    const char* files[] = {"Sacred.exe", "granny.dll", "tincat2.dll"};
    for (size_t i = 0; i < sacred_modules.size(); ++i) {
        loader.map_file(assets + "/" + files[i], sacred_modules[i]);
    }

    loader.bind();
    Thread thread(memory, 0, loader.tls_templates());
    Control control(memory, thread);
    for (const auto& module : loader.modules()) {
        control.module(module.name, module.base, module.image_size);
    }

    control.catalog(read_image(build + "/boundaries.bin"));
    auto bytes = read_image(build + "/fixtures.bin");
    map_fixture(memory, 0x03000000, bytes, "control-fixture");
    control.module("control-fixture", 0x03000000, (bytes.size() + 4095) & ~4095u);
    auto mismatch = std::vector<uint8_t>(4096, 0xcc);
    const uint8_t prologue[] = {0x55, 0x68, 0x80, 0x10, 0x00, 0x04, 0x64, 0xff, 0x35, 0,    0,
                                0,    0,    0x64, 0x89, 0x25, 0,    0,    0,    0,    0xff, 0xd2,
                                0x5a, 0x59, 0x5d, 0x64, 0xa3, 0,    0,    0,    0,    0xc3};
    std::copy(std::begin(prologue), std::end(prologue), mismatch.begin());
    map_fixture(memory, 0x04001000, mismatch, "mismatch-fixture");
    control.module("mismatch-fixture", 0x04001000, 4096);
    for (unsigned i = 0; i < control_fixture_count; ++i) {
        control.entry(control_fixtures[i].address, control_fixtures[i].function, control_fixtures[i].kinds,
                      control_fixtures[i].name);
    }

    control.bind_exception_imports(loader);
    uint32_t scratch = control.heap.allocate(4096);
    CHECK(scratch == 0x20000000);
    memory.store32(scratch + 0x40, loader.resolve("kernel32.dll", "RaiseException"));
    memory.store32(scratch + 0x44, loader.resolve("kernel32.dll", "RtlUnwind"));
    memory.store32(scratch + 0x54, fixture_setjmp_intrinsic);
    memory.store32(scratch + 0x58, fixture_longjmp_intrinsic);

    auto none = signature(Convention::Stdcall);
    auto two = signature(Convention::Stdcall, 2);
    X86 interrupted = control.cpu;
    control.cpu.r[R_EBX] = 0x1357;
    control.cpu.r[R_ESI] = 0x2468;
    control.cpu.eflags_cf = 1;
    interrupted = control.cpu;

    // When: ordinary guest entry, computed calls/jumps, ABI return channels and
    // native->guest nested callbacks all use the same dispatcher.
    CHECK(control.invoke(fixture_cdecl_sum, signature(Convention::Cdecl, 2), {17, 29}).integer == 46);
    CHECK(control.invoke(fixture_stdcall_sum, two, {17, 29}).integer == 46);
    CHECK(control.invoke(fixture_thiscall_sum, signature(Convention::Thiscall, 2), {100, 7}).integer == 107);
    CHECK(control.invoke(fixture_fastcall_sum, signature(Convention::Fastcall, 3), {10, 20, 30}).integer == 60);
    auto wide = signature(Convention::Stdcall, 2, ReturnChannel::EdxEax);
    wide.arguments[1].bytes = 8;
    CHECK(control.invoke(fixture_wide_result, wide, {1, 0x12345678ffffffffull}).integer == 0x1234567900000000ull);
    X87Value value{0xc000000000000000ull, 0x3fff};
    memory.write(scratch + 0x300, &value.signif, 8);
    memory.write(scratch + 0x308, &value.signExp, 2);
    auto floating =
        control.invoke(fixture_float_result, signature(Convention::Stdcall, 1, ReturnChannel::St0), {scratch + 0x300})
            .floating;
    CHECK(floating.signif == value.signif && floating.signExp == value.signExp);
    auto output = signature(Convention::Stdcall, 1, ReturnChannel::OutBuffer);
    output.arguments[0].direction = PointerDirection::Out;
    control.invoke(fixture_out_result, output, {scratch + 0x310});
    CHECK(memory.load32(scratch + 0x310) == 0x12345678);
    CHECK(control.invoke(fixture_indirect_call, none, {}).integer == 46);
    CHECK(control.invoke(fixture_tail_jump, two, {17, 29}).integer == 46);
    CHECK(control.invoke(fixture_stack_probe, none, {}).integer == 85);
    CHECK(thread.stack_limit == thread.stack_top - 0x3e000);
    CHECK(memory.load32(thread.teb + 8) == thread.stack_limit);
    CHECK(memory.load32(thread.stack_limit) == 0);
    uint32_t guard = thread.stack_limit - Memory::page_size;
    control.probe_stack(guard - Memory::page_size, 4);
    fault([&] { memory.load32(guard - Memory::page_size); }, "uncommitted");
    CHECK(thread.stack_limit == guard + Memory::page_size);
    memory.store32(scratch + 0x50, scratch + 0x60);
    memory.store32(scratch + 0x60, fixture_adjust_this);
    CHECK(control.invoke(fixture_vtable_call, none, {}).integer == 103);
    constexpr uint32_t nested = 0xf0ff0000, escape = nested + 16;
    control.host(
        nested, two,
        [](Control& c, const std::vector<uint64_t>& args) {
            CHECK(c.depth() == 1);
            auto result = c.invoke(fixture_stdcall_sum, signature(Convention::Stdcall, 2), args);
            CHECK(c.depth() == 1);
            return result;
        },
        "qualification synchronous enumeration callback");
    memory.store32(scratch + 0x48, nested);
    CHECK(control.invoke(fixture_nested_callback, none, {}).integer == 25);
    // Guest-visible state; the block counters behind it are host scheduling bookkeeping.
    CHECK(std::memcmp(&control.cpu, &interrupted, offsetof(X86, source_pc)) == 0);

    // Then: an original Sacred C++ throw executes its actual byte-derived helper,
    // matches guest RTTI, unwinds an inner registration/destructor, runs finally,
    // and catches across guest frames without any host C++ throw.
    memory.store32(scratch + 0x84, fixture_guest_destructor); // ThrowInfo.pmfnUnwind
    memory.store32(scratch + 0x8c, scratch + 0xc0);           // pCatchableTypeArray
    memory.store32(scratch + 0xc0, 1);
    memory.store32(scratch + 0xc4, scratch + 0xe0);
    memory.store32(scratch + 0xe4, scratch + 0x100); // TypeDescriptor
    memory.store32(scratch, 0);
    CHECK(control.invoke(fixture_cpp_outer, none, {}).integer == 0xabcdef);
    CHECK(memory.load32(scratch) == 123);
    CHECK(memory.load32(scratch + 0x120) == 1);
    CHECK(memory.load32(thread.teb) == 0xffffffffu);
    CHECK(recomp_seh_driver_records(&control.cpu) == 0);

    // Given an exception escaping a nested callback, native resources must be
    // destroyed by ordinary returns before the outer C checkpoint is reached.
    unsigned released = 0;
    struct Resource {
        unsigned& released;
        ~Resource() { ++released; }
    };
    control.host(
        escape, none,
        [&released](Control& c, const std::vector<uint64_t>&) {
            Resource resource{released};
            c.invoke(fixture_cpp_middle, signature(Convention::Stdcall), {});
            CHECK(c.transferring());
            return Result{};
        },
        "qualification nested callback escape");
    memory.store32(scratch + 0x4c, escape);
    memory.store32(scratch, 0);
    CHECK(control.invoke(fixture_escaping_callback, none, {}).integer == 0xabcdef);
    CHECK(released == 1 && memory.load32(scratch) == 123);
    CHECK(recomp_seh_driver_records(&control.cpu) == 0 && control.depth() == 0);

    // A continuable SEH handler changes the guest CONTEXT, not the host registers.
    memory.store32(scratch, 0);
    CHECK(control.invoke(fixture_continuable, none, {}).integer == 85);
    CHECK(memory.load32(scratch) == 2);
    CHECK(control.invoke(fixture_setjmp_caller, none, {}).integer == 1);
    CHECK(recomp_seh_driver_records(&control.cpu) == 0);

    // The permanent mismatch reproducer leaves its generated frame before raise.
    // Its guest registration remains valid because the owning C driver is live.
    control.cpu.r[R_EDX] = fixture_mismatch_callee;
    CHECK(control.invoke(fixture_mismatch, none, {}).integer == 85);
    CHECK(recomp_seh_driver_records(&control.cpu) == 0);
    CHECK(control.transfers >= 5);
    auto one = signature(Convention::Stdcall, 1);
    uint32_t set_filter = loader.resolve("kernel32.dll", "SetUnhandledExceptionFilter");
    CHECK(control.invoke(set_filter, one, {fixture_continue_filter}).integer == 0);
    CHECK(control.invoke(fixture_top_raise, none, {}).integer == 77);
    CHECK(control.invoke(set_filter, one, {fixture_search_filter}).integer == fixture_continue_filter);
    memory.store32(scratch + 0x320, scratch + 0x340);
    memory.store32(scratch + 0x324, scratch + 0x380);
    CHECK(control.invoke(loader.resolve("kernel32.dll", "UnhandledExceptionFilter"), one, {scratch + 0x320}).integer ==
          0);
    CHECK(memory.load32(scratch + 0x10) == 1);

    // Strict negatives stop at the real dispatcher/bridge surface.
    fault([&] { control.invoke(0x03000001, none, {}); }, "module=control-fixture rva=1");
    fault([&] { control.invoke(fixture_bad_return, none, {}); }, "source_bytes=");
    fault([&] { control.invoke(0x00746573, none, {}); }, "boundary=exported_candidate");
    fault([&] { control.invoke(fixture_stdcall_sum, signature(Convention::Cdecl, 2), {1, 2}); },
          "stack cleanup mismatch");
    fault([&] { control.invoke(loader.resolve("kernel32.dll", "GetTickCount"), none, {}); },
          "unimplemented import kernel32.dll!GetTickCount");
    control.host(
        nested + 32, none,
        [](Control& c, const std::vector<uint64_t>&) {
            return c.invoke(fixture_foreign_return, signature(Convention::Stdcall), {});
        },
        "qualification foreign sentinel");
    memory.store32(scratch + 0x4c, nested + 32);
    fault([&] { control.invoke(fixture_escaping_callback, none, {}); }, "unowned callback return sentinel");
    CHECK(control.depth() == 0 && recomp_seh_driver_records(&control.cpu) == 0);

    // Removing RTTI agreement must not turn a guest throw into fabricated success.
    memory.store32(scratch + 0xe4, scratch + 0x104);
    fault([&] { control.invoke(fixture_cpp_outer, none, {}); }, "unhandled guest exception");
    CHECK(memory.load32(scratch + 0x10) == 2);
    CHECK(control.depth() == 0 && recomp_seh_driver_records(&control.cpu) == 0);
    memory.store32(scratch + 0xe4, scratch + 0x100);
    CHECK(control.invoke(set_filter, one, {0}).integer == fixture_search_filter);
    fault([&] { control.invoke(fixture_noncontinuable, none, {}); }, "noncontinuable exception");
    {
        Thread other(memory, 1, loader.tls_templates());
        uint32_t allocated = 0, root_fs = control.cpu.fs_base, root_sp = control.cpu.r[R_ESP];
        uint32_t root_source = control.source_pc, root_flags = x86_get_eflags(&control.cpu);
        uint16_t root_cw = control.cpu.fpu_cw;
        std::exception_ptr child_failure;
        control.host(
            nested + 48, one,
            [](Control& current, const std::vector<uint64_t>& a) {
                CHECK(&active_control() == &current);
                uint32_t block = current.heap.allocate(4);
                current.memory.store32(block, uint32_t(a[0]));
                return Result{block, {}};
            },
            "process shared allocator qualification");
        std::thread child([&] {
            try {
                Control current(control, other);
                CHECK(current.cpu.fs_base == other.teb && &current.heap == &control.heap);
                current.cpu.r[R_EBX] = 0xdeadbeef;
                current.cpu.fpu_cw = 0x0b7f;
                CHECK(current.invoke(fixture_stdcall_sum, two, {17, 29}).integer == 46);
                allocated = uint32_t(current.invoke(nested + 48, one, {0x12345678}).integer);
                CHECK(current.invoke(set_filter, one, {fixture_continue_filter}).integer == 0);
                CHECK(current.invoke(fixture_top_raise, none, {}).integer == 77);
                CHECK(current.depth() == 0 && recomp_seh_driver_records(&current.cpu) == 0);
                CHECK(current.cpu.r[R_EBX] == 0xdeadbeef && current.cpu.fpu_cw == 0x0b7f);
                current.host(
                    nested + 64, none, [](Control&, const std::vector<uint64_t>&) { return Result{0x4242, {}}; },
                    "worker registered process qualification");
            } catch (...) {
                child_failure = std::current_exception();
            }
        });
        child.join();
        if (child_failure) {
            std::rethrow_exception(child_failure);
        }

        CHECK(g_mem != nullptr && &active_control() == &control);
        CHECK(control.cpu.fs_base == root_fs && control.cpu.r[R_ESP] == root_sp && control.cpu.fpu_cw == root_cw &&
              x86_get_eflags(&control.cpu) == root_flags && control.source_pc == root_source);
        CHECK(allocated != scratch && memory.load32(allocated) == 0x12345678);
        CHECK(control.unhandled_filter == fixture_continue_filter);
        CHECK(control.invoke(nested + 64, none, {}).integer == 0x4242);
        CHECK(control.invoke(set_filter, one, {0}).integer == fixture_continue_filter);
        control.heap.free(allocated);
    }

    control.heap.free(scratch);
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: control_tests ASSETS FIXTURE_BUILD\n");
        return 2;
    }

    try {
        qualify(argv[1], argv[2]);
        std::printf("control qualification: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
