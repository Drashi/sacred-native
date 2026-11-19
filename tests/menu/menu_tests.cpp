#include "runtime/win32/menu.h"
#include "runtime/ddraw/ddraw.h"
#include "runtime/ole/com.h"
#include <cstdio>
#include <cstring>

using namespace sacred;
static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x))                                                                                                      \
            throw GuestFault(std::string("CHECK ") + #x);                                                              \
    } while (0)
static unsigned enumerations;
static void enumerate_adapter(X86* cpu) {
    auto& control = active_control();
    CHECK(control.arg(0) == 0 && control.arg(3) == 0x12345678 && control.arg(4) == 0);
    CHECK(control.memory.load32(control.arg(1)) != 0);
    CHECK(control.memory.load32(control.arg(2)) != 0);
    ++enumerations;
    cpu->r[R_EAX] = 0; // Caller cancels enumeration.
    cpu->eip = control.memory.load32(cpu->r[R_ESP]);
    cpu->r[R_ESP] += 24;
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw GuestFault("assets argument required");
        }

        Memory memory;
        Loader loader(memory);
        const char* names[] = {"Sacred.exe", "granny.dll", "tincat2.dll"};
        for (size_t i = 0; i < sacred_modules.size(); ++i) {
            loader.map_file(std::string(argv[1]) + "/" + names[i], sacred_modules[i]);
        }

        loader.enable_native_catalog();
        loader.bind();
        Thread thread(memory, 0, {}), second(memory, 1, {});
        Control control(memory, thread);
        Menu menu(control, loader, "build/tests/menu/menu-profile");
        menu.bind();
        DirectDraw draw(control, loader);
        draw.bind();
        Com com(control, loader);
        com.bind();
        uint32_t scratch = control.heap.allocate(8192);
        auto text = [&](uint32_t offset, const char* value) {
            memory.write(scratch + offset, value, strlen(value) + 1);
            return scratch + offset;
        };
        auto call = [&](const char* dll, const char* name, unsigned count, std::vector<uint64_t> args,
                        Convention convention = Convention::Stdcall) {
            return control
                .invoke(loader.resolve(dll, name), {convention, ReturnChannel::Eax, std::vector<Argument>(count), ""},
                        args)
                .integer;
        };
        CHECK(call("user32.dll", "SetRect", 5, {scratch, uint32_t(-5), 7, 1024, 768}) == 1);
        CHECK(memory.load32(scratch) == uint32_t(-5) && memory.load32(scratch + 4) == 7 &&
              memory.load32(scratch + 8) == 1024 && memory.load32(scratch + 12) == 768);
        CHECK(call("user32.dll", "OffsetRect", 3, {scratch, 9, uint32_t(-8)}) == 1);
        CHECK(memory.load32(scratch) == 4 && memory.load32(scratch + 4) == UINT32_MAX &&
              memory.load32(scratch + 8) == 1033 && memory.load32(scratch + 12) == 760);
        uint32_t filename = text(4096, "Z:\\games\\Sacred Gold\\Sacred.exe");
        CHECK(call("version.dll", "GetFileVersionInfoSizeA", 2, {filename, scratch + 5000}) == 0x724);
        CHECK(memory.load32(scratch + 5000) == 0);
        CHECK(call("version.dll", "GetFileVersionInfoA", 4, {filename, 0, 0x724, scratch}) == 1);
        CHECK(call("version.dll", "VerQueryValueA", 4, {scratch, text(4200, "\\"), scratch + 5000, scratch + 5004}) ==
              1);
        CHECK(memory.load32(scratch + 5004) == 52);
        const uint32_t original[] = {0xfeef04bd, 0x10000, 0x20000, 0x2001c, 0x20000, 0x2001c, 0x3f,
                                     0,          0x40004, 1,       0,       0,       0};
        uint32_t fixed = memory.load32(scratch + 5000);
        for (unsigned i = 0; i < 13; ++i) {
            CHECK(memory.load32(fixed + i * 4) == original[i]);
        }

        uint32_t resource = call("kernel32.dll", "FindResourceA", 3, {layout::exe, 1, 16});
        CHECK(resource != 0);
        uint32_t loaded = call("kernel32.dll", "LoadResource", 2, {layout::exe, resource});
        CHECK(loaded != 0 && memory.load32(loaded + (fixed - scratch)) == original[0]);
        CHECK(call("kernel32.dll", "SizeofResource", 2, {layout::exe, resource}) == 0x390);
        CHECK(call("kernel32.dll", "LockResource", 1, {loaded}) == loaded);
        CHECK(call("kernel32.dll", "FreeResource", 1, {loaded}) == 0);
        CHECK(memory.load32(loaded + (fixed - scratch)) == original[0]);
        CHECK(call("kernel32.dll", "FindResourceA", 3, {layout::exe, 0xffff, 16}) == 0);
        CHECK(call("version.dll", "VerQueryValueA", 4,
                   {scratch, text(4200, "\\VarFileInfo\\Translation"), scratch + 5000, scratch + 5004}) == 1);
        CHECK(memory.load32(scratch + 5004) == 4);
        uint32_t translation = memory.load32(memory.load32(scratch + 5000));
        CHECK(call("user32.dll", "wsprintfA", 4,
                   {scratch + 6000, text(4200, "%04x%04x"), translation & 0xffff, translation >> 16},
                   Convention::Cdecl) == 8);
        char formatted[9];
        memory.read(scratch + 6000, formatted, sizeof formatted);
        CHECK(std::strcmp(formatted, "040704b0") == 0);
        uint32_t icon = call("user32.dll", "LoadIconA", 2, {layout::exe, 101});
        CHECK(icon != 0 && call("user32.dll", "LoadIconA", 2, {layout::exe, 101}) == icon);
        CHECK(call("user32.dll", "LoadIconA", 2, {layout::exe, 0xffff}) == 0);
        CHECK(call("gdi32.dll", "GetStockObject", 1, {4}) == 0xe4000004);
        uint32_t window_class = scratch + 7000;
        const uint32_t class_fields[] = {
            48, 0xb, 0x812bf0, 8, 12, layout::exe, icon, 0, 0xe4000004, 0, text(7200, "fixture class"), 0};
        for (unsigned i = 0; i < 12; ++i) {
            memory.store32(window_class + i * 4, class_fields[i]);
        }

        CHECK(call("user32.dll", "RegisterClassExA", 1, {window_class}) == 0xc000);
        CHECK(call("user32.dll", "RegisterClassExA", 1, {window_class}) == 0);
        CHECK(memory.load32(control.cpu.fs_base + 0x34) == 1410);
        CHECK(call("user32.dll", "GetKeyboardLayout", 1, {0}) == 0x04090409);
        memory.store32(control.cpu.fs_base + 0x34, 0x1234);
        CHECK(call("imm32.dll", "ImmGetProperty", 2, {0x04090409, 4}) == 0);
        CHECK(memory.load32(control.cpu.fs_base + 0x34) == 0);
        CHECK(call("user32.dll", "SystemParametersInfoA", 4, {0x2001, 0, 123, 3}) == 1);
        CHECK(call("user32.dll", "SystemParametersInfoA", 4, {0x2000, 0, scratch + 5000, 0}) == 1);
        CHECK(memory.load32(scratch + 5000) == 123);
        CHECK(call("user32.dll", "SystemParametersInfoA", 4, {0x2001, 0, 0, 3}) == 1);
        for (uint32_t action : {0x3au, 0x34u, 0x32u}) {
            uint32_t size = action == 0x32 ? 24 : 8;
            memory.store32(scratch + 5000, size);
            memory.store32(scratch + 5004, 0xffffffff);
            CHECK(call("user32.dll", "SystemParametersInfoA", 4, {action, 0, scratch + 5000, 2}) == 1);
            CHECK(memory.load32(scratch + 5000) == size && memory.load32(scratch + 5004) == 0);
            CHECK(call("user32.dll", "SystemParametersInfoA", 4, {action + 1, 0, scratch + 5000, 2}) == 0);
            CHECK(memory.load32(control.cpu.fs_base + 0x34) == 1439);
        }

        control.entry(0x6442d0, enumerate_adapter, CallEntry, "DirectDraw enumeration callback ABI fixture");
        CHECK(call("ddraw.dll", "DirectDrawEnumerateExA", 3, {0x6442d0, 0x12345678, 7}) == 0);
        CHECK(enumerations == 1);
        CHECK(call("ddraw.dll", "DirectDrawCreateEx", 4, {0, scratch + 6020, 0x8994d8, 0}) == 0);
        uint32_t dd = memory.load32(scratch + 6020);
        CHECK(dd != 0);
        auto method = [&](uint32_t object, unsigned slot, std::vector<uint64_t> args) {
            args.insert(args.begin(), object);
            return control
                .invoke(memory.load32(memory.load32(object) + slot * 4),
                        {Convention::Stdcall, ReturnChannel::Eax, std::vector<Argument>(args.size()), ""}, args)
                .integer;
        };
        CHECK(method(dd, 1, {}) == 2);
        const uint8_t d3d_iid[] = {0x77, 0x9e, 4, 0xf5, 0x61, 0x48, 0xd2, 0x11, 0xa4, 7, 0, 0xa0, 0xc9, 6, 0x29, 0xa8};
        memory.write(scratch + 6000, d3d_iid, sizeof d3d_iid);
        CHECK(method(dd, 0, {scratch + 6000, scratch + 6020}) == 0);
        uint32_t d3d = memory.load32(scratch + 6020);
        CHECK(d3d != 0 && d3d != dd);
        const uint8_t unknown_iid[] = {0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
        memory.write(scratch + 6000, unknown_iid, sizeof unknown_iid);
        CHECK(method(d3d, 0, {scratch + 6000, scratch + 6020}) == 0);
        CHECK(memory.load32(scratch + 6020) == dd);
        CHECK(method(dd, 2, {}) == 3);
        CHECK(method(d3d, 2, {}) == 2);
        CHECK(method(dd, 2, {}) == 1);
        CHECK(method(dd, 2, {}) == 0);
        CHECK(memory.permissions(dd) == 0 && memory.permissions(d3d) == 0);
        CHECK(call("ole32.dll", "CoInitialize", 1, {0}) == 0);
        CHECK(call("ole32.dll", "CoInitialize", 1, {0}) == 1);
        control.cpu.fs_base = second.teb;
        CHECK(call("ole32.dll", "CoInitialize", 1, {0}) == 0);
        control.cpu.fs_base = thread.teb;
        CHECK(call("ole32.dll", "CoInitialize", 1, {0}) == 1);
        memory.store32(scratch + 6000, 0xdeadbeef);
        CHECK(call("ole32.dll", "CoCreateInstance", 5, {0x89be60, 0, 1, 0x89be70, scratch + 6000}) == 0x80040154);
        CHECK(memory.load32(scratch + 6000) == 0);
        const uint32_t key_state = scratch + 6200, key_out = scratch + 6500;
        auto ascii = [&](uint32_t vk, std::initializer_list<unsigned> held, uint32_t scan = 0) {
            uint8_t keys[256] = {};
            for (unsigned held_key : held) {
                keys[held_key] = held_key == 0x14 || held_key == 0x90 ? 1 : 0x80;
            }

            memory.write(key_state, keys, sizeof keys);
            memory.store32(key_out, 0);
            const uint32_t count = uint32_t(call("user32.dll", "ToAscii", 5, {vk, scan, key_state, key_out, 0}));
            return std::make_pair(count, count ? memory.load32(key_out) : 0xffffffffu);
        };
        CHECK(ascii('A', {}) == std::make_pair(1u, uint32_t('a')));
        CHECK(ascii('A', {0x10}) == std::make_pair(1u, uint32_t('A')));
        CHECK(ascii('A', {0x14}) == std::make_pair(1u, uint32_t('A')));
        CHECK(ascii('A', {0x14, 0xa0}) == std::make_pair(1u, uint32_t('a')));
        CHECK(ascii('C', {0x11}) == std::make_pair(1u, 3u));
        CHECK(ascii('A', {0x12}).first == 0);
        CHECK(ascii('A', {}, 0x8000).first == 0);
        CHECK(ascii('2', {}) == std::make_pair(1u, uint32_t('2')));
        CHECK(ascii('2', {0x10}) == std::make_pair(1u, uint32_t('@')));
        CHECK(ascii('9', {0x10}) == std::make_pair(1u, uint32_t('(')));
        CHECK(ascii('1', {0x14}) == std::make_pair(1u, uint32_t('1')));
        CHECK(ascii(189, {0x10}) == std::make_pair(1u, uint32_t('_')));
        CHECK(ascii(222, {}) == std::make_pair(1u, uint32_t('\'')));
        CHECK(ascii(222, {0x10}) == std::make_pair(1u, uint32_t('"')));
        CHECK(ascii(32, {}) == std::make_pair(1u, 32u) && ascii(13, {}) == std::make_pair(1u, 13u));
        CHECK(ascii(8, {}) == std::make_pair(1u, 8u) && ascii(9, {}) == std::make_pair(1u, 9u));
        CHECK(ascii(37, {}).first == 0 && ascii(112, {}).first == 0 && ascii(16, {}).first == 0);
        CHECK(ascii(97, {0x90}) == std::make_pair(1u, uint32_t('1')) && ascii(97, {}).first == 0);
        CHECK(ascii(107, {}) == std::make_pair(1u, uint32_t('+')));
        memory.store32(key_out, 0xffffffff);
        memory.store32(key_out + 4, 0xffffffff);
        memory.write(key_state, std::vector<uint8_t>(256, 0).data(), 256);
        CHECK(call("user32.dll", "ToUnicode", 6, {'Q', 0, key_state, key_out, 2, 0}) == 1);
        CHECK(memory.load32(key_out) == 0xffff0071u);
        CHECK(call("user32.dll", "ToUnicode", 6, {'Q', 0, key_state, key_out, 0, 0}) == 0);
        std::printf("MENU_BOUNDARY_PASS checks=%u\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "MENU_BOUNDARY_FAILURE %s\n", error.what());
        return 1;
    }
}
