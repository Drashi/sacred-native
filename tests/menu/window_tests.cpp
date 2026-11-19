#include "runtime/win32/menu.h"
#include "runtime/ddraw/ddraw.h"
#include "platform/window_host.h"
#include <SDL.h>
#include <chrono>
#include <cstdio>
#include <exception>
#include <functional>
#include <thread>

using namespace sacred;
static std::vector<uint32_t> messages;
static uint32_t default_procedure;
static bool adjust_position;
static unsigned modes, devices, depth_formats;
static void depth_callback(X86* cpu) {
    auto& control = active_control();
    uint32_t format = control.arg(0);
    if (control.arg(1) != 0x9abc || control.memory.load32(format) != 32 || control.memory.load32(format + 4) != 0x400 ||
        control.memory.load32(format + 12) != 16 || control.memory.load32(format + 20) != 0xffff) {
        throw GuestFault("Z-buffer callback layout mismatch");
    }

    ++depth_formats;
    cpu->r[R_EAX] = 0;
    cpu->eip = control.memory.load32(cpu->r[R_ESP]);
    cpu->r[R_ESP] += 12;
}

static void mode_callback(X86* cpu) {
    auto& control = active_control();
    uint32_t desc = control.arg(0);
    if (control.arg(1) != 0x1234 || control.memory.load32(desc) != 124 || control.memory.load32(desc + 8) != 768 ||
        control.memory.load32(desc + 12) != 1024 || control.memory.load32(desc + 84) != 32) {
        throw GuestFault("logical mode callback layout mismatch");
    }

    ++modes;
    cpu->r[R_EAX] = 0;
    cpu->eip = control.memory.load32(cpu->r[R_ESP]);
    cpu->r[R_ESP] += 12;
}

static void device_callback(X86* cpu) {
    auto& control = active_control();
    uint32_t desc = control.arg(2);
    if (control.arg(3) != 0x5678 || control.memory.load32(desc + 4) != 56 || control.memory.load32(desc + 60) != 56 ||
        control.memory.load32(desc + 132) < 1024 || control.memory.load32(desc + 184) != 0x00020002 ||
        control.memory.load32(desc + 196) != 0xa4665c60 || (control.memory.load32(desc) & 0x90000)) {
        throw GuestFault("software D3D device callback layout mismatch");
    }

    ++devices;
    cpu->r[R_EAX] = 0;
    cpu->eip = control.memory.load32(cpu->r[R_ESP]);
    cpu->r[R_ESP] += 20;
}

static void procedure(X86* cpu) {
    auto& control = active_control();
    std::vector<uint64_t> args;
    for (unsigned i = 0; i < 4; ++i) {
        args.push_back(control.arg(i));
    }

    messages.push_back(uint32_t(args[1]));
    if (args[1] == 0x46 && adjust_position) {
        control.memory.store32(uint32_t(args[3]) + 8, control.memory.load32(uint32_t(args[3]) + 8) + 1);
    }

    auto result =
        control.invoke(default_procedure, {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}}, ""}, args);
    cpu->r[R_EAX] = uint32_t(result.integer);
    cpu->eip = control.memory.load32(cpu->r[R_ESP]);
    cpu->r[R_ESP] += 20;
}

static void qualify(const std::string& assets, WindowHost& host) {
    Memory memory;
    Loader loader(memory);
    const char* names[] = {"Sacred.exe", "granny.dll", "tincat2.dll"};
    for (size_t i = 0; i < sacred_modules.size(); ++i) {
        loader.map_file(assets + "/" + names[i], sacred_modules[i]);
    }

    loader.enable_native_catalog();
    loader.bind();
    Thread thread(memory, 0, {});
    Control control(memory, thread);
    Menu menu(control, loader, {}, &host);
    menu.bind();
    DirectDraw draw(control, loader, &menu, &host);
    draw.bind();
    control.entry(0x812bf0, procedure, CallEntry, "native window callback ABI fixture");
    default_procedure = loader.resolve("user32.dll", "DefWindowProcA");
    uint32_t scratch = control.heap.allocate(4096);
    const char name[] = "callback fixture";
    memory.write(scratch + 128, name, sizeof name);
    auto brush = control
                     .invoke(loader.resolve("gdi32.dll", "GetStockObject"),
                             {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""}, {4})
                     .integer;
    const uint32_t fields[] = {48, 0xb, 0x812bf0, 0, 0, layout::exe, 0, 0, uint32_t(brush), 0, scratch + 128, 0};
    for (unsigned i = 0; i < 12; ++i) {
        memory.store32(scratch + i * 4, fields[i]);
    }

    auto atom = control
                    .invoke(loader.resolve("user32.dll", "RegisterClassExA"),
                            {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""}, {scratch})
                    .integer;
    if (!atom) {
        throw GuestFault("fixture class registration failed");
    }

    auto window = control
                      .invoke(loader.resolve("user32.dll", "CreateWindowExA"),
                              {Convention::Stdcall, ReturnChannel::Eax, std::vector<Argument>(12), ""},
                              {0, atom, scratch + 128, 0x90000000, 0, 0, 160, 120, 0, 0, layout::exe, 0})
                      .integer;
    if (!window || messages != std::vector<uint32_t>{0x81, 0x83, 1, 5, 3, 0x18}) {
        throw GuestFault("window creation callback order or return mismatch");
    }

    if (!control
             .invoke(loader.resolve("user32.dll", "GetClientRect"),
                     {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""}, {window, scratch + 256})
             .integer ||
        memory.load32(scratch + 256) || memory.load32(scratch + 260) || memory.load32(scratch + 264) != 160 ||
        memory.load32(scratch + 268) != 120) {
        throw GuestFault("client rectangle does not match the native popup");
    }

    auto show = [&](uint32_t mode) {
        return control
            .invoke(loader.resolve("user32.dll", "ShowWindow"),
                    {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""}, {window, mode})
            .integer;
    };
    if (show(0) != 1 || show(5) != 0 || show(10) != 1) {
        throw GuestFault("window visibility transition return mismatch");
    }

    if (control
            .invoke(loader.resolve("user32.dll", "IsIconic"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
                    {window})
            .integer) {
        throw GuestFault("ordinary window reported minimized");
    }

    if (!control
             .invoke(loader.resolve("user32.dll", "UpdateWindow"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
                     {window})
             .integer) {
        throw GuestFault("synchronous default painting failed");
    }

    control.entry(0x643f20, mode_callback, CallEntry, "logical display mode callback ABI fixture");
    control.entry(0x6443e0, device_callback, CallEntry, "D3D7 device callback ABI fixture");
    auto method = [&](uint32_t object, unsigned slot, std::vector<uint64_t> args) {
        args.insert(args.begin(), object);
        return control
            .invoke(memory.load32(memory.load32(object) + slot * 4),
                    {Convention::Stdcall, ReturnChannel::Eax, std::vector<Argument>(args.size()), ""}, args)
            .integer;
    };
    if (control
            .invoke(loader.resolve("ddraw.dll", "DirectDrawCreateEx"),
                    {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}}, ""},
                    {0, scratch + 200, 0x8994d8, 0})
            .integer) {
        throw GuestFault("DirectDraw7 creation failed");
    }

    uint32_t dd = memory.load32(scratch + 200);
    if (method(dd, 20, {window, 0xc08})) {
        throw GuestFault("windowed cooperative level failed");
    }

    if (method(dd, 19, {})) {
        throw GuestFault("RestoreDisplayMode must succeed as a no-op for a windowed adapter");
    }

    memory.store32(scratch + 1024, 380);
    memory.store32(scratch + 1536, 380);
    if (method(dd, 11, {scratch + 1024, scratch + 1536}) || memory.load32(scratch + 1028) != 1 ||
        memory.load32(scratch + 1540) != 1) {
        throw GuestFault("DDCAPS advertised an unimplemented operation");
    }

    if (method(dd, 8, {0, 0, 0x1234, 0x643f20}) || modes != 1) {
        throw GuestFault("logical mode enumeration failed");
    }

    const uint8_t iid[] = {0x77, 0x9e, 4, 0xf5, 0x61, 0x48, 0xd2, 0x11, 0xa4, 7, 0, 0xa0, 0xc9, 6, 0x29, 0xa8};
    memory.write(scratch + 256, iid, sizeof iid);
    if (method(dd, 0, {scratch + 256, scratch + 200})) {
        throw GuestFault("Direct3D7 query failed");
    }

    uint32_t d3d = memory.load32(scratch + 200);
    if (method(d3d, 3, {0x6443e0, 0x5678}) || devices != 1) {
        throw GuestFault("D3D7 device enumeration failed");
    }

    control.entry(0x644e60, depth_callback, CallEntry, "Z-buffer callback ABI fixture");
    const uint8_t rgb[] = {0x60, 0x5c, 0x66, 0xa4, 0x73, 0x26, 0xcf, 0x11, 0xa3, 0x1a, 0, 0xaa, 0, 0xb9, 0x33, 0x56};
    memory.write(scratch + 256, rgb, sizeof rgb);
    if (method(d3d, 6, {scratch + 256, 0x644e60, 0x9abc}) || depth_formats != 1) {
        throw GuestFault("Z-buffer format enumeration failed");
    }

    if (method(d3d, 2, {}) != 1 || method(dd, 2, {}) != 0) {
        throw GuestFault("enumerated adapter lifetime mismatch");
    }

    bool visible = false;
    host.render(menu.native_window(uint32_t(window)),
                [&](Renderer&) { visible = SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE; });
    auto hide = [&] {
        return control
            .invoke(loader.resolve("user32.dll", "SetCursor"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
                    {0})
            .integer;
    };
    if (hide() != (visible ? 0xe3000001u : 0u) || hide() != 0) {
        throw GuestFault("SetCursor did not return previous native selection");
    }

    host.render(menu.native_window(uint32_t(window)),
                [&](Renderer&) { visible = SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE; });
    if (visible) {
        throw GuestFault("SetCursor left the native cursor visible");
    }

    auto position = [&](uint32_t x) {
        return control
            .invoke(loader.resolve("user32.dll", "SetWindowPos"),
                    {Convention::Stdcall, ReturnChannel::Eax, std::vector<Argument>(7), ""},
                    {window, 0, x, 30, 192, 144, 0x40})
            .integer;
    };
    messages.clear();
    adjust_position = true;
    if (position(20) != 1 || messages != std::vector<uint32_t>{0x46, 0x83, 0x47, 3, 5}) {
        throw GuestFault("synchronous window-position callback order mismatch");
    }

    adjust_position = false;
    auto state = host.window_state(menu.native_window(uint32_t(window)));
    if (state.x != 21 || state.y != 30 || state.width != 192 || state.height != 144) {
        throw GuestFault("window-position callback mutation was not applied to SDL");
    }

    memory.store32(scratch + 256, uint32_t(-5));
    memory.store32(scratch + 260, 8);
    if (!control
             .invoke(loader.resolve("user32.dll", "ClientToScreen"),
                     {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""}, {window, scratch + 256})
             .integer ||
        memory.load32(scratch + 256) != 16 || memory.load32(scratch + 260) != 38) {
        throw GuestFault("client-to-screen point did not use current native position");
    }

    messages.clear();
    if (position(21) != 1 || messages != std::vector<uint32_t>{0x46, 0x47}) {
        throw GuestFault("unchanged window position produced client move/size messages");
    }

    const uint32_t native = menu.native_window(uint32_t(window));
    for (int i = 0; i < 100; ++i) {
        host.inject_mouse(native, i, 7);
        host.inject_key(native, "A", i % 2 == 0);
    }

    host.inject_button(native, 5, 5, 1, true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!(host.input_state().buttons & 1)) {
        if (std::chrono::steady_clock::now() > deadline) {
            throw GuestFault("injected input never reached the host queue");
        }

        std::this_thread::yield();
    }

    unsigned moves = 0, keydowns = 0, keyups = 0, clicks = 0;
    uint32_t last_x = 0, last_y = 0;
    while (control
               .invoke(loader.resolve("user32.dll", "PeekMessageA"),
                       {Convention::Stdcall, ReturnChannel::Eax, std::vector<Argument>(5), ""},
                       {scratch + 2048, 0, 0, 0, 1})
               .integer) {
        const uint32_t message = memory.load32(scratch + 2048 + 4);
        if (message == 0x200) {
            ++moves;
            last_x = memory.load32(scratch + 2048 + 12) & 0xffff;
            last_y = memory.load32(scratch + 2048 + 12) >> 16;
        } else if (message == 0x100) {
            ++keydowns;
        } else if (message == 0x101) {
            ++keyups;
        } else if (message == 0x201) {
            ++clicks;
        }
    }

    if (moves != 1 || last_x != 99 || last_y != 7 || keydowns != 50 || keyups != 50 || clicks != 1) {
        throw GuestFault(
            "queued mouse motion was not coalesced to the newest position: moves=" + std::to_string(moves) +
            " x=" + std::to_string(last_x) + " keydowns=" + std::to_string(keydowns));
    }

    // Sacred reads Esc from WM_CHAR 0x1B outside text entry, so TranslateMessage must queue it behind the WM_KEYDOWN.
    auto wait_for = [&](const std::function<bool()>& ready, const char* what) {
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ready()) {
            if (std::chrono::steady_clock::now() > limit) {
                throw GuestFault(std::string("host never reached: ") + what);
            }

            std::this_thread::yield();
        }
    };
    struct Seen {
        uint32_t message, wparam, lparam;
    };
    auto take = [&]() -> std::vector<Seen> {
        std::vector<Seen> seen;
        while (control
                   .invoke(loader.resolve("user32.dll", "PeekMessageA"),
                           {Convention::Stdcall, ReturnChannel::Eax, std::vector<Argument>(5), ""},
                           {scratch + 2048, 0, 0, 0, 1})
                   .integer) {
            seen.push_back({memory.load32(scratch + 2048 + 4), memory.load32(scratch + 2048 + 8),
                            memory.load32(scratch + 2048 + 12)});
        }

        return seen;
    };
    auto translate = [&] {
        return control
            .invoke(loader.resolve("user32.dll", "TranslateMessage"),
                    {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""}, {scratch + 2048})
            .integer;
    };
    auto async_state = [&](uint32_t vk) {
        return control
            .invoke(loader.resolve("user32.dll", "GetAsyncKeyState"),
                    {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""}, {vk})
            .integer;
    };
    const struct {
        const char* name;
        uint32_t vk, character;
    } controls[] = {{"Escape", 27, 0x1b}, {"Tab", 9, 9}, {"Backspace", 8, 8}, {"Return", 13, 13}};
    for (const auto& key : controls) {
        host.inject_key(native, key.name, true);
        wait_for([&] { return host.input_state().keys.test(key.vk); }, "key down state");
        if ((async_state(key.vk) & 0x8000) != 0x8000) {
            throw GuestFault(std::string("GetAsyncKeyState misses ") + key.name);
        }

        auto down = take();
        if (down.size() != 1 || down[0].message != 0x100 || down[0].wparam != key.vk) {
            throw GuestFault(std::string("unexpected messages for ") + key.name + " key down");
        }

        if (translate() != 1) {
            throw GuestFault(std::string("TranslateMessage skipped ") + key.name);
        }

        auto chars = take();
        if (chars.size() != 1 || chars[0].message != 0x102 || chars[0].wparam != key.character ||
            chars[0].lparam != down[0].lparam) {
            throw GuestFault(std::string("TranslateMessage did not queue the WM_CHAR for ") + key.name);
        }

        host.inject_key(native, key.name, false);
        wait_for([&] { return !host.input_state().keys.test(key.vk); }, "key up state");
        if (async_state(key.vk) & 0x8000) {
            throw GuestFault(std::string("GetAsyncKeyState sticks on ") + key.name);
        }

        auto up = take();
        if (up.size() != 1 || up[0].message != 0x101) {
            throw GuestFault("key up message missing");
        }
    }

    host.inject_key(native, "A", true);
    wait_for([&] { return host.input_state().keys.test('A'); }, "letter down state");
    take();
    if (translate() != 1 || !take().empty()) {
        throw GuestFault("TranslateMessage duplicated a printable character the host text input already delivers");
    }

    host.inject_key(native, "A", false);
    wait_for([&] { return !host.input_state().keys.test('A'); }, "letter up state");
    take();

    // Button flags belong to the event, a release outside every window still reaches the guest, and a focus loss
    // that makes the host forget a held button releases it for the guest as well.
    host.inject_button(native, 6, 6, 1, true);
    host.inject_button(native, 6, 6, 1, false);
    wait_for([&] { return host.input_state().buttons == 0 && host.input_state().x == 6; }, "click state");
    auto click = take();
    if (click.size() != 2 || click[0].message != 0x201 || click[0].wparam != 1 || click[1].message != 0x202 ||
        click[1].wparam != 0) {
        throw GuestFault("button message flags were not taken from the event");
    }

    host.inject_button(native, 7, 7, 1, true);
    wait_for([&] { return host.input_state().buttons & 1; }, "second press");
    host.inject_button(0, 7, 7, 1, false);
    wait_for([&] { return host.input_state().buttons == 0; }, "release outside windows");
    auto outside = take();
    if (outside.size() != 2 || outside[0].message != 0x201 || outside[1].message != 0x202) {
        throw GuestFault("a button release without a native window was dropped");
    }

    host.inject_button(native, 8, 8, 1, true);
    wait_for([&] { return host.input_state().buttons & 1; }, "third press");
    SDL_Event lost{};
    lost.type = SDL_WINDOWEVENT;
    lost.window.windowID = native;
    lost.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    if (SDL_PushEvent(&lost) < 0) {
        throw GuestFault("cannot inject focus loss");
    }

    wait_for([&] { return host.input_state().buttons == 0; }, "focus loss reset");
    auto focus = take();
    if (focus.size() != 4 || focus[0].message != 0x201 || focus[1].message != 0x202 || focus[2].message != 6 ||
        focus[3].message != 8) {
        throw GuestFault("focus loss did not release the held button before deactivating");
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }

    WindowHost host;
    std::exception_ptr failure;
    std::thread guest([&] {
        try {
            qualify(argv[1], host);
        } catch (...) {
            failure = std::current_exception();
        }

        host.finish();
    });
    host.loop();
    guest.join();
    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "WINDOW_FAILURE %s\n", error.what());
        }

        return 1;
    }

    std::puts("WINDOW_CALLBACKS_PASS real_SDL_window=1 creation_callbacks=6 painting=1 worker_joined=1");
    return 0;
}
