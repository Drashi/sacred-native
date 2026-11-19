#include "../../platform/qa.h"
#include "menu.h"
#include "../../platform/window_host.h"
#include "../../platform/font.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace sacred {
Menu::Menu(Control& control, const Loader& loader, std::string profile, WindowHost* host, Startup* files)
    : control_(control), loader_(loader), profile_(std::move(profile)), host_(host), files_(files) {
    if (!profile_.empty()) {
        std::ifstream setting(profile_ + "/foreground-timeout");
        if (setting && !(setting >> foreground_timeout_)) {
            throw GuestFault("invalid foreground timeout profile");
        }
    }
}

Menu::~Menu() {
    if (host_) {
        host_->set_event_notifier({});
    }

    for (const auto& dc : contexts_) {
        if (dc.second.stock_data) {
            control_.heap.free(dc.second.stock_data);
        }
    }

    for (const auto& bitmap : bitmaps_) {
        control_.heap.free(bitmap.second.data);
    }
}

uint32_t Menu::native_window(uint32_t window) const {
    if (!window && !windows_.empty()) {
        return windows_.begin()->second.native;
    }

    auto found = windows_.find(window);
    if (found == windows_.end()) {
        throw GuestFault("unknown DirectDraw guest window");
    }

    return found->second.native;
}

std::string Menu::ansi(uint32_t pointer) const {
    std::string text;
    for (unsigned i = 0; i < 32768; ++i) {
        uint8_t c;
        control_.memory.read(pointer + i, &c, 1);
        if (!c) {
            return text;
        }

        text += char(c);
    }

    throw GuestFault("unterminated menu ANSI string");
}

uint16_t Menu::word(uint32_t address) const {
    uint8_t bytes[2];
    control_.memory.read(address, bytes, 2);
    return uint16_t(bytes[0]) | uint16_t(bytes[1]) << 8;
}

uint32_t Menu::resource(uint32_t instance, uint32_t name, uint32_t type) const {
    const Module* module = nullptr;
    for (const auto& candidate : loader_.modules()) {
        if (candidate.base == (instance ? instance : layout::exe)) {
            module = &candidate;
        }
    }

    if (!module) {
        throw GuestFault("unqualified resource module");
    }

    auto directory = module->directories[2];
    if (!directory.first) {
        return 0;
    }

    uint32_t root = module->address(directory.first, directory.second), node = root;
    auto lower = [](std::string text) {
        for (char& ch : text) {
            if (ch >= 'A' && ch <= 'Z') {
                ch += 'a' - 'A';
            }
        }

        return text;
    };
    const uint32_t keys[] = {type, name};
    for (unsigned level = 0; level < 3; ++level) {
        unsigned count = word(node + 12) + word(node + 14);
        bool found = false;
        for (unsigned i = 0; i < count; ++i) {
            uint32_t entry = node + 16 + i * 8, id = control_.memory.load32(entry);
            if (level < 2) {
                uint32_t key = keys[level];
                if (key <= 0xffff) {
                    if (id != key) {
                        continue;
                    }
                } else {
                    if (!(id & 0x80000000)) {
                        continue;
                    }

                    uint32_t text = root + (id & 0x7fffffff);
                    std::string actual;
                    for (unsigned j = 0; j < word(text); ++j) {
                        uint16_t ch = word(text + 2 + j * 2);
                        if (ch > 127) {
                            throw GuestFault("unqualified resource name encoding");
                        }

                        actual += char(ch);
                    }

                    if (lower(actual) != lower(ansi(key))) {
                        continue;
                    }
                }
            }

            uint32_t next = control_.memory.load32(entry + 4);
            if (bool(next & 0x80000000) != (level < 2)) {
                throw GuestFault("invalid resource directory level");
            }

            node = root + (next & 0x7fffffff);
            found = true;
            break;
        }

        if (!found) {
            return 0;
        }
    }

    return node;
}

std::pair<uint32_t, uint32_t> Menu::version(const std::string& path) const {
    std::string name = path.substr(path.find_last_of("\\/") + 1);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c; });
    for (const auto& module : loader_.modules()) {
        if (module.name == name) {
            auto directory = module.directories[2];
            if (!directory.first) {
                return {};
            }

            uint32_t root = module.address(directory.first, directory.second), node = root;
            for (unsigned level = 0; level < 3; ++level) {
                unsigned named = word(node + 12), count = word(node + 14);
                bool found = false;
                for (unsigned i = 0; i < count; ++i) {
                    uint32_t entry = node + 16 + (named + i) * 8;
                    uint32_t id = control_.memory.load32(entry);
                    if ((level == 0 && id != 16) || (level == 1 && id != 1)) {
                        continue;
                    }

                    uint32_t next = control_.memory.load32(entry + 4);
                    if (bool(next & 0x80000000) != (level < 2)) {
                        throw GuestFault("invalid version resource directory level");
                    }

                    node = root + (next & 0x7fffffff);
                    found = true;
                    break;
                }

                if (!found) {
                    return {};
                }
            }

            uint32_t rva = control_.memory.load32(node), size = control_.memory.load32(node + 4);
            return {module.address(rva, size), size};
        }
    }

    throw GuestFault("version query for unqualified file: " + path);
}

uint32_t Menu::load_icon(uint32_t instance, uint32_t name) {
    auto key = std::make_pair(instance, name <= 0xffff ? "#" + std::to_string(name) : ansi(name));
    if (auto previous = shared_icons_.find(key); previous != shared_icons_.end()) {
        return previous->second;
    }

    if (!instance) {
        throw GuestFault("unqualified system icon");
    }

    uint32_t group = resource(instance, name, 14);
    if (!group) {
        control_.memory.store32(active_control().cpu.fs_base + 0x34, 1814);
        return 0;
    }

    auto& m = control_.memory;
    uint32_t at = instance + m.load32(group);
    if (word(at) || word(at + 2) != 1 || !word(at + 4)) {
        throw GuestFault("invalid icon group");
    }

    uint32_t entry = at + 6;
    // LoadIcon's shared class icon is 32x32, not the largest available image.
    for (unsigned i = 0; i < word(at + 4); ++i) {
        uint8_t dimensions[2];
        m.read(at + 6 + i * 14, dimensions, 2);
        if (dimensions[0] == 32 && dimensions[1] == 32) {
            entry = at + 6 + i * 14;
            break;
        }
    }

    uint32_t image = resource(instance, word(entry + 12), 3);
    if (!image) {
        throw GuestFault("missing icon image");
    }

    uint32_t data = instance + m.load32(image), bytes = m.load32(image + 4);
    uint32_t width = m.load32(data + 4), doubled_height = m.load32(data + 8), bpp = word(data + 14);
    if (m.load32(data) != 40 || !width || width > 256 || !doubled_height || doubled_height > 512 ||
        doubled_height % 2 || word(data + 12) != 1 || m.load32(data + 16) ||
        (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)) {
        throw GuestFault("unqualified icon bitmap format bpp=" + std::to_string(bpp));
    }

    uint32_t height = doubled_height / 2, colors = bpp <= 8 ? m.load32(data + 32) : 0;
    if (bpp <= 8 && !colors) {
        colors = 1u << bpp;
    }

    uint32_t pixels = 40 + colors * 4, pitch = ((width * bpp + 31) / 32) * 4;
    uint32_t mask = pixels + pitch * height, mask_pitch = ((width + 31) / 32) * 4;
    if (mask + mask_pitch * height > bytes) {
        throw GuestFault("truncated icon bitmap");
    }

    std::vector<uint8_t> source(bytes);
    m.read(data, source.data(), source.size());
    Icon icon{width, height, std::vector<uint8_t>(width * height * 4)};
    bool alpha = false;
    if (bpp == 32) {
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                alpha |= source[pixels + y * pitch + x * 4 + 3] != 0;
            }
        }
    }

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t row = height - 1 - y, color;
            if (bpp <= 8) {
                uint32_t bit = x * bpp;
                uint8_t index = (source[pixels + row * pitch + bit / 8] >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
                if (index >= colors) {
                    throw GuestFault("invalid icon palette index");
                }

                color = 40 + index * 4;
            } else {
                color = pixels + row * pitch + x * (bpp / 8);
            }

            uint32_t out = (y * width + x) * 4;
            icon.rgba[out] = source[color + 2];
            icon.rgba[out + 1] = source[color + 1];
            icon.rgba[out + 2] = source[color];
            icon.rgba[out + 3] = alpha                                                           ? source[color + 3]
                                 : (source[mask + row * mask_pitch + x / 8] & (0x80 >> (x % 8))) ? 0
                                                                                                 : 255;
        }
    }

    uint32_t handle = 0xe2000000 + uint32_t(icons_.size()) + 1;
    icons_.emplace(handle, std::move(icon));
    shared_icons_.emplace(std::move(key), handle);
    return handle;
}

Result Menu::window_message(uint32_t window, uint32_t message, uint32_t wparam, uint32_t lparam) {
    auto found = windows_.find(window);
    if (found == windows_.end()) {
        throw GuestFault("message for unknown guest window");
    }

    return active_control().invoke(found->second.procedure,
                                   {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}}, ""},
                                   {window, message, wparam, lparam});
}

void Menu::bind() {
    bind_input();
    bind_gdi();
    control_.host(
        loader_.resolve("user32.dll", "OffsetRect"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::InOut}, {4}, {4}}, ""},
        [](Control& c, const std::vector<uint64_t>& a) {
            c.memory.check(uint32_t(a[0]), 16, Read | Write);
            for (unsigned i = 0; i < 4; ++i) {
                uint32_t address = uint32_t(a[0]) + i * 4;
                c.memory.store32(address, c.memory.load32(address) + uint32_t(a[1 + (i & 1)]));
            }

            return Result{1, {}};
        },
        "user32.dll!OffsetRect");
    control_.host(
        loader_.resolve("user32.dll", "SetRect"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::Out}, {4}, {4}, {4}, {4}}, ""},
        [](Control& c, const std::vector<uint64_t>& a) {
            const uint32_t rect[] = {uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3]), uint32_t(a[4])};
            c.memory.write(uint32_t(a[0]), rect, sizeof rect);
            return Result{1, {}};
        },
        "user32.dll!SetRect");
    control_.host(
        loader_.resolve("user32.dll", "ClientToScreen"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, PointerDirection::InOut}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto found = windows_.find(uint32_t(a[0]));
            if (found == windows_.end()) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1400);
                return Result{};
            }

            auto state = host_->window_state(found->second.native);
            c.memory.store32(uint32_t(a[1]), c.memory.load32(uint32_t(a[1])) + uint32_t(state.x));
            c.memory.store32(uint32_t(a[1]) + 4, c.memory.load32(uint32_t(a[1]) + 4) + uint32_t(state.y));
            return Result{1, {}};
        },
        "user32.dll!ClientToScreen");
    control_.host(
        loader_.resolve("user32.dll", "GetClientRect"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, PointerDirection::Out}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto found = windows_.find(uint32_t(a[0]));
            if (found == windows_.end()) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1400);
                return Result{};
            }

            auto state = host_->window_state(found->second.native);
            const uint32_t rect[] = {0, 0, uint32_t(state.width), uint32_t(state.height)};
            c.memory.write(uint32_t(a[1]), rect, sizeof rect);
            return Result{1, {}};
        },
        "user32.dll!GetClientRect");
    control_.host(
        loader_.resolve("user32.dll", "GetWindowRect"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, PointerDirection::Out}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto found = windows_.find(uint32_t(a[0]));
            if (found == windows_.end()) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1400);
                return Result{};
            }

            // The native window is borderless: its window rectangle is its client area in screen coordinates.
            auto state = host_->window_state(found->second.native);
            const uint32_t rect[] = {uint32_t(state.x), uint32_t(state.y), uint32_t(state.x + state.width),
                                     uint32_t(state.y + state.height)};
            c.memory.write(uint32_t(a[1]), rect, sizeof rect);
            return Result{1, {}};
        },
        "user32.dll!GetWindowRect");
    control_.host(
        loader_.resolve("user32.dll", "SetWindowPos"),
        {Convention::Stdcall, ReturnChannel::Eax, std::vector<Argument>(7), ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto found = windows_.find(uint32_t(a[0]));
            if (found == windows_.end()) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1400);
                return Result{};
            }

            if (a[1] || a[6] != 0x40 || !host_) {
                throw GuestFault("unqualified window positioning flags");
            }

            uint32_t data = c.heap.allocate(80);
            const uint32_t fields[] = {uint32_t(a[0]), 0,   uint32_t(a[2]), uint32_t(a[3]), uint32_t(a[4]),
                                       uint32_t(a[5]), 0x40};
            c.memory.write(data, fields, sizeof fields);
            auto& window = found->second;
            try {
                window_message(uint32_t(a[0]), 0x46, 0, data);
                if (c.transferring()) {
                    c.heap.free(data);
                    return Result{};
                }

                if (c.memory.load32(data) != a[0] || c.memory.load32(data + 4) || c.memory.load32(data + 24) != 0x40) {
                    throw GuestFault("unqualified WINDOWPOS callback mutation");
                }

                int32_t x = c.memory.load32(data + 8), y = c.memory.load32(data + 12);
                int32_t width = c.memory.load32(data + 16), height = c.memory.load32(data + 20);
                if (width <= 0 || height <= 0) {
                    throw GuestFault("unqualified nonpositive window size");
                }

                bool moved = x != window.x || y != window.y;
                bool resized = width != window.width || height != window.height;
                if (resized) {
                    const uint32_t rectangles[] = {uint32_t(x),
                                                   uint32_t(y),
                                                   uint32_t(x) + uint32_t(width),
                                                   uint32_t(y) + uint32_t(height),
                                                   uint32_t(window.x),
                                                   uint32_t(window.y),
                                                   uint32_t(window.x) + uint32_t(window.width),
                                                   uint32_t(window.y) + uint32_t(window.height),
                                                   uint32_t(window.x),
                                                   uint32_t(window.y),
                                                   uint32_t(window.x) + uint32_t(window.width),
                                                   uint32_t(window.y) + uint32_t(window.height),
                                                   data};
                    c.memory.write(data + 28, rectangles, sizeof rectangles);
                    auto result = window_message(uint32_t(a[0]), 0x83, 1, data + 28);
                    if (c.transferring()) {
                        c.heap.free(data);
                        return Result{};
                    }

                    if (result.integer) {
                        throw GuestFault("unqualified nonclient valid-rectangle flags");
                    }

                    for (unsigned i = 0; i < 4; ++i) {
                        if (c.memory.load32(data + 28 + i * 4) != rectangles[i]) {
                            throw GuestFault("unqualified custom client rectangle");
                        }
                    }
                }

                host_->position_window(window.native, x, y, width, height);
                window.x = x;
                window.y = y;
                window.width = width;
                window.height = height;
                window.style |= 0x10000000;
                if (resized) {
                    window.invalid = window.erase = true;
                }

                uint32_t flags = 0x40 | (moved ? 0 : 0x1000) | (resized ? 0 : 0x800);
                c.memory.store32(data + 24, flags);
                window_message(uint32_t(a[0]), 0x47, 0, data);
            } catch (...) {
                c.heap.free(data);
                throw;
            }

            c.heap.free(data);
            return Result{1, {}};
        },
        "user32.dll!SetWindowPos");
    control_.host(
        loader_.resolve("user32.dll", "SetCursor"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (a[0]) {
                throw GuestFault("unqualified non-null cursor selection");
            }

            if (!host_) {
                throw GuestFault("SetCursor requires the SDL host");
            }

            const bool was_visible = host_->hide_cursor();
            if (qa_env("SACRED_QA_INPUT_LOG")) {
                std::fprintf(stderr, "QA_INPUT SetCursor(NULL) native cursor was_visible=%d now_hidden=1\n",
                             was_visible);
            }

            return Result{was_visible ? 0xe3000001u : 0u, {}};
        },
        "user32.dll!SetCursor");
    control_.host(
        loader_.resolve("user32.dll", "UpdateWindow"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto found = windows_.find(uint32_t(a[0]));
            if (found == windows_.end()) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1400);
                return Result{};
            }

            if (found->second.invalid) {
                window_message(a[0], 0xf, 0, 0);
                if (!c.transferring()) {
                    found->second.invalid = false;
                }
            }

            return Result{1, {}};
        },
        "user32.dll!UpdateWindow");
    for (const char* symbol : {"GetForegroundWindow", "GetFocus"}) {
        bool thread_focus = std::string(symbol) == "GetFocus";
        control_.host(
            loader_.resolve("user32.dll", symbol), {Convention::Stdcall, ReturnChannel::Eax, {}, ""},
            [this, thread_focus](Control& c, const std::vector<uint64_t>&) {
                for (const auto& window : windows_) {
                    if ((!thread_focus || window.second.owner_thread == c.memory.load32(c.cpu.fs_base + 0x24)) &&
                        host_->window_state(window.second.native).focused) {
                        foreground_claim_ = 0;
                        return Result{window.first, {}};
                    }
                }

                if (auto claim = windows_.find(foreground_claim_);
                    foreground_claim_ && claim != windows_.end() &&
                    (!thread_focus || claim->second.owner_thread == c.memory.load32(c.cpu.fs_base + 0x24))) {
                    return Result{foreground_claim_, {}};
                }

                return Result{};
            },
            std::string("user32.dll!") + symbol);
    }

    // Foreground activation is the host compositor's decision (focus-stealing prevention, an asynchronous focus
    // event), so the guest can find its freshly shown window not yet focused. Ask the host to raise it and report the
    // success Windows gives a process that already owns the foreground. GetForegroundWindow then names that window
    // until one of ours really gets the focus: Sacred's startup loops on SetForegroundWindow until its window is the
    // foreground one, and macOS refuses to activate an application while the user works in another one, so a start
    // in the background hung there.
    control_.host(
        loader_.resolve("user32.dll", "SetForegroundWindow"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto found = windows_.find(uint32_t(a[0]));
            if (found == windows_.end()) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1400);
                return Result{};
            }

            if (!host_) {
                throw GuestFault("SetForegroundWindow requires the SDL host");
            }

            host_->raise_window(found->second.native);
            if (!host_->window_state(found->second.native).focused) {
                foreground_claim_ = uint32_t(a[0]);
            }

            return Result{1, {}};
        },
        "user32.dll!SetForegroundWindow");
    // Reached only when the window is not focused yet (a start in the background, see SetForegroundWindow): asks
    // the host for the focus, claims it like SetForegroundWindow (Sacred's 0x811440 loops on GetFocus as well) and
    // returns the window that had it, as Windows does.
    control_.host(
        loader_.resolve("user32.dll", "SetFocus"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (!host_) {
                throw GuestFault("SetFocus requires the SDL host");
            }

            uint32_t previous = 0;
            for (const auto& window : windows_) {
                if (window.second.owner_thread == c.memory.load32(c.cpu.fs_base + 0x24) &&
                    host_->window_state(window.second.native).focused) {
                    previous = window.first;
                }
            }

            if (!a[0]) {
                return Result{previous, {}};
            }

            auto found = windows_.find(uint32_t(a[0]));
            if (found == windows_.end()) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1400);
                return Result{};
            }

            if (!previous) {
                previous = foreground_claim_;
                host_->raise_window(found->second.native);
                if (!host_->window_state(found->second.native).focused) {
                    foreground_claim_ = uint32_t(a[0]);
                }
            }

            return Result{previous, {}};
        },
        "user32.dll!SetFocus");
    control_.host(
        loader_.resolve("user32.dll", "IsIconic"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto found = windows_.find(uint32_t(a[0]));
            if (found == windows_.end()) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1400);
                return Result{};
            }

            return Result{host_->window_state(found->second.native).minimized ? 1u : 0u, {}};
        },
        "user32.dll!IsIconic");
    control_.host(
        loader_.resolve("user32.dll", "ShowWindow"), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto found = windows_.find(uint32_t(a[0]));
            if (found == windows_.end()) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1400);
                return Result{};
            }

            if (a[1] != 0 && a[1] != 1 && a[1] != 5 && a[1] != 10) {
                throw GuestFault("unqualified window show mode");
            }

            bool was_visible = found->second.style & 0x10000000;
            bool visible = a[1] != 0;
            if (visible) {
                found->second.style |= 0x10000000;
            } else {
                found->second.style &= ~0x10000000u;
            }

            if (was_visible != visible) {
                window_message(a[0], 0x18, visible, 0);
            }

            if (!c.transferring()) {
                host_->show_window(found->second.native, visible);
            }

            return Result{was_visible ? 1u : 0u, {}};
        },
        "user32.dll!ShowWindow");
    control_.host(
        loader_.resolve("user32.dll", "DefWindowProcA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto found = windows_.find(uint32_t(a[0]));
            if (found == windows_.end()) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1400);
                return Result{};
            }

            switch (a[1]) {
            case 0x10: // WM_CLOSE: the default action destroys the window.
                destroy_window(uint32_t(a[0]));
                return Result{};
            case 0xf: // Default WM_PAINT validates and erases the initial update region.
                if (found->second.invalid && found->second.erase && found->second.brush) {
                    host_->erase_window(found->second.native, brushes_.at(found->second.brush));
                }

                found->second.invalid = false;
                found->second.erase = false;
                return Result{};
            case 0x81: // WM_NCCREATE: the window metadata was installed before reentry.
                c.memory.check(a[3], 48, Read);
                return Result{1, {}};
            case 0x83: // WM_NCCALCSIZE, without a nonclient border.
                if (a[2] > 1 || (found->second.style & 0x00c40000)) {
                    throw GuestFault("unqualified nonclient sizing");
                }

                c.memory.check(a[3], a[2] ? 52 : 16, Write);
                return Result{};
            case 0x46: // Popup windows have no default min/max frame constraint.
                c.memory.check(a[3], 28, Read | Write);
                if (!(found->second.style & 0x80000000) || (found->second.style & 0x00c40000)) {
                    throw GuestFault("unqualified default window-position constraint");
                }

                return Result{};
            case 0x47: {
                c.memory.check(a[3], 28, Read);
                uint32_t flags = c.memory.load32(a[3] + 24);
                auto& window = found->second;
                if (!(flags & 0x1000)) {
                    window_message(uint32_t(a[0]), 3, 0, (uint32_t(window.x) & 0xffff) | (uint32_t(window.y) << 16));
                    if (c.transferring()) {
                        return Result{};
                    }
                }

                if (!(flags & 0x800)) {
                    window_message(uint32_t(a[0]), 5, 0, uint32_t(window.width) | (uint32_t(window.height) << 16));
                }

                return Result{};
            }
            case 0:
            case 1:
            case 2:
            case 3:
            case 5:
            case 6:
            case 7:
            case 8:
            case 0x18:
            case 0x1c:
            case 0x82:
            case 0x100:
            case 0x101:
            case 0x102: // WM_KEYDOWN, WM_KEYUP and WM_CHAR have no default action.
            // WM_SYSKEYDOWN, WM_SYSKEYUP and WM_SYSCHAR (any key with Alt held, Alt alone): the game window has no
            // system menu to activate, so there is nothing to do by default.
            case 0x104:
            case 0x105:
            case 0x106:
            case 0x200:
            case 0x201:
            case 0x202:
            case 0x204:
            case 0x205:
            case 0x207:
            case 0x208:
            case 0x20a:
                // These notifications require no default action. Guest handlers
                // have already observed them through the original WNDPROC.
                return Result{};
            default:
                throw GuestFault("unimplemented default window message " + std::to_string(a[1]));
            }
        },
        "user32.dll!DefWindowProcA");
    control_.host(
        loader_.resolve("imm32.dll", "ImmReleaseContext"), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (!windows_.count(uint32_t(a[0]))) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1400);
                return Result{};
            }

            if (a[1]) {
                throw GuestFault("unqualified non-US input context release");
            }

            // A NULL query acquired no context storage. The original accepts
            // its paired release; there is no native IME object to retain.
            return Result{1, {}};
        },
        "imm32.dll!ImmReleaseContext");
    control_.host(
        loader_.resolve("imm32.dll", "ImmGetContext"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (!windows_.count(uint32_t(a[0]))) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1400);
            }

            // The admitted US keyboard profile has no Windows IME. A NULL HIMC
            // is availability failure, not an empty successful composition.
            // Ordinary committed keyboard text is delivered separately by SDL.
            return Result{};
        },
        "imm32.dll!ImmGetContext");
    control_.host(
        loader_.resolve("imm32.dll", "ImmGetProperty"), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [](Control& c, const std::vector<uint64_t>& a) {
            if (a[0] != 0x04090409 || a[1] != 4) {
                throw GuestFault("unqualified IME property query");
            }

            // The captured US layout has no IME property bits. Wine also clears
            // last error for this non-IME availability query.
            c.memory.store32(c.cpu.fs_base + 0x34, 0);
            return Result{};
        },
        "imm32.dll!ImmGetProperty");
    control_.host(
        loader_.resolve("user32.dll", "GetKeyboardLayout"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [](Control& c, const std::vector<uint64_t>& a) {
            if (a[0] && a[0] != c.memory.load32(c.cpu.fs_base + 0x24)) {
                throw GuestFault("unqualified keyboard-layout thread");
            }

            // Frozen US guest keyboard profile, measured in the original probe.
            return Result{0x04090409, {}};
        },
        "user32.dll!GetKeyboardLayout");
    control_.host(
        loader_.resolve("user32.dll", "CreateWindowExA"),
        {Convention::Stdcall,
         ReturnChannel::Eax,
         {{4},
          {4, PointerDirection::In},
          {4, PointerDirection::In},
          {4},
          {4},
          {4},
          {4},
          {4},
          {4},
          {4},
          {4},
          {4, PointerDirection::InOut}},
         "stdcall LRESULT(HWND,UINT,WPARAM,LPARAM)"},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (!host_) {
                throw GuestFault("window creation requires SDL host");
            }

            const WindowClass* klass = nullptr;
            if (a[1] <= 0xffff) {
                for (const auto& entry : classes_) {
                    if (entry.second.atom == a[1]) {
                        klass = &entry.second;
                    }
                }
            } else {
                auto found = classes_.find(ansi(a[1]));
                if (found != classes_.end()) {
                    klass = &found->second;
                }
            }

            if (!klass) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1407);
                return Result{};
            }

            if (a[8] || a[9] || (a[3] & 0x40000000)) {
                throw GuestFault("unqualified child window or supplied menu");
            }

            if (klass->menu && resource(klass->instance, klass->menu, 4)) {
                throw GuestFault("unimplemented class menu resource");
            }

            int32_t width = int32_t(a[6]), height = int32_t(a[7]);
            if (width <= 0 || height <= 0) {
                throw GuestFault("unqualified window dimensions");
            }

            const Icon empty{};
            const Icon& icon = klass->icon ? icons_.at(klass->icon) : empty;
            uint32_t native = host_->create_window(a[2] ? ansi(a[2]) : "", int32_t(a[4]), int32_t(a[5]), width, height,
                                                   (a[3] & 0x80000000) != 0, icon.rgba, icon.width, icon.height);
            uint32_t handle = 0xe1000000 + uint32_t(windows_.size()) + 1;
            Window window{native,
                          klass->procedure,
                          uint32_t(a[3]),
                          uint32_t(a[0]),
                          uint32_t(a[10]),
                          uint32_t(a[8]),
                          0,
                          0,
                          c.memory.load32(c.cpu.fs_base + 0x24),
                          klass->brush,
                          int32_t(a[4]),
                          int32_t(a[5]),
                          width,
                          height,
                          std::vector<uint8_t>(klass->window_extra)};
            windows_.emplace(handle, std::move(window));
            uint32_t creation = c.heap.allocate(48);
            const uint32_t fields[] = {uint32_t(a[11]), uint32_t(a[10]), uint32_t(a[9]), uint32_t(a[8]),
                                       uint32_t(a[7]),  uint32_t(a[6]),  uint32_t(a[5]), uint32_t(a[4]),
                                       uint32_t(a[3]),  uint32_t(a[2]),  uint32_t(a[1]), uint32_t(a[0])};
            for (unsigned i = 0; i < 12; ++i) {
                c.memory.store32(creation + i * 4, fields[i]);
            }

            auto result = window_message(handle, 0x81, 0, creation); // WM_NCCREATE.
            if (c.transferring()) {
                c.heap.free(creation);
                return Result{};
            }

            bool accepted = result.integer != 0;
            if (accepted) {
                uint32_t rect = c.heap.allocate(16);
                c.memory.store32(rect, uint32_t(a[4]));
                c.memory.store32(rect + 4, uint32_t(a[5]));
                c.memory.store32(rect + 8, uint32_t(a[4]) + width);
                c.memory.store32(rect + 12, uint32_t(a[5]) + height);
                window_message(handle, 0x83, 0, rect); // WM_NCCALCSIZE.
                c.heap.free(rect);
                if (!c.transferring()) {
                    accepted = uint32_t(window_message(handle, 1, 0, creation).integer) != UINT32_MAX;
                }
            }

            c.heap.free(creation);
            if (c.transferring()) {
                return Result{};
            }

            if (!accepted) {
                host_->destroy_window(native);
                windows_.erase(handle);
                return Result{};
            }

            window_message(handle, 5, 0, uint32_t(width) | uint32_t(height) << 16);
            if (c.transferring()) {
                return Result{};
            }

            window_message(handle, 3, 0, (uint32_t(a[4]) & 0xffffu) | uint32_t(a[5]) << 16);
            if (c.transferring()) {
                return Result{};
            }

            if (a[3] & 0x10000000) {
                window_message(handle, 0x18, 1, 0);
                if (c.transferring()) {
                    return Result{};
                }

                host_->show_window(native, true);
            }

            return Result{handle, {}};
        },
        "user32.dll!CreateWindowExA");
    control_.host(
        loader_.resolve("user32.dll", "GetSystemMetrics"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (a[0] > 1) {
                throw GuestFault("unqualified system metric");
            }

            if (!host_) {
                throw GuestFault("display metric requires SDL host");
            }

            auto size = host_->display_size();
            return Result{uint32_t(a[0] ? size.second : size.first), {}};
        },
        "user32.dll!GetSystemMetrics");
    control_.host(
        loader_.resolve("user32.dll", "RegisterClassExA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::In}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto& m = c.memory;
            uint32_t p = a[0];
            m.check(p, 48, Read);
            if (m.load32(p) != 48) {
                m.store32(c.cpu.fs_base + 0x34, 87);
                return Result{};
            }

            if (m.load32(p + 12) > 65536 || m.load32(p + 16) > 65536) {
                throw GuestFault("unqualified class extra-byte extent");
            }

            std::string name = ansi(m.load32(p + 40));
            if (classes_.count(name)) {
                m.store32(c.cpu.fs_base + 0x34, 1410);
                return Result{};
            }

            WindowClass window{0xc000 + uint32_t(classes_.size()),
                               m.load32(p + 4),
                               m.load32(p + 8),
                               m.load32(p + 20),
                               m.load32(p + 24),
                               m.load32(p + 28),
                               m.load32(p + 32),
                               m.load32(p + 44),
                               m.load32(p + 16),
                               m.load32(p + 36),
                               std::vector<uint8_t>(m.load32(p + 12))};
            if ((window.icon && !icons_.count(window.icon)) ||
                (window.small_icon && !icons_.count(window.small_icon)) ||
                (window.cursor && window.cursor != 0xe3000001) || (window.brush && !brushes_.count(window.brush))) {
                throw GuestFault("invalid window-class drawing object");
            }

            m.check(window.procedure, 1, Execute);
            classes_.emplace(name, window);
            std::fprintf(stderr, "REGISTER_CLASS name=%s atom=%x wndproc=%x class_extra=%zu window_extra=%u menu=%x\n",
                         name.c_str(), window.atom, window.procedure, window.extra.size(), window.window_extra,
                         window.menu);
            return Result{window.atom, {}};
        },
        "user32.dll!RegisterClassExA");
    control_.host(
        loader_.resolve("user32.dll", "FindWindowA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::In}, {4, PointerDirection::In}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            // The LAN init (0x007d87e6) looks for a running GameServer.exe window to close. Only this process's
            // windows exist here, so a class this process never registered has no window: NULL, ERROR_SUCCESS.
            if (!a[0] || a[0] < 0x10000 || a[1]) {
                throw GuestFault("unqualified FindWindowA query");
            }

            if (classes_.count(ansi(uint32_t(a[0])))) {
                throw GuestFault("FindWindowA on a registered class");
            }

            c.memory.store32(c.cpu.fs_base + 0x34, 0);
            return Result{0, {}};
        },
        "user32.dll!FindWindowA");
    control_.host(
        loader_.resolve("gdi32.dll", "GetStockObject"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (a[0] != 4 || !brushes_.count(0xe4000004)) {
                throw GuestFault("unqualified stock GDI object");
            }

            return Result{0xe4000004, {}};
        },
        "gdi32.dll!GetStockObject");
    control_.host(
        loader_.resolve("user32.dll", "LoadCursorA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, PointerDirection::In}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (a[0] || a[1] != 0x7f00) {
                throw GuestFault("unqualified cursor resource");
            }

            if (!host_) {
                throw GuestFault("cursor creation requires SDL host");
            }

            host_->load_arrow_cursor();
            return Result{0xe3000001, {}};
        },
        "user32.dll!LoadCursorA");
    control_.host(
        loader_.resolve("user32.dll", "LoadIconA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, PointerDirection::In}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) { return Result{load_icon(a[0], a[1]), {}}; },
        "user32.dll!LoadIconA");
    control_.host(
        loader_.resolve("user32.dll", "SystemParametersInfoA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4, PointerDirection::InOut}, {4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if ((a[0] == 0x10 || a[0] == 0x54) && !a[1] && a[3] == 2) {
                if (!host_) {
                    throw GuestFault("screen saver query requires SDL host");
                }

                c.memory.store32(a[2], host_->screen_saver());
            } else if ((a[0] == 0x11 || a[0] == 0x56) && a[1] <= 1 && !a[2] && (a[3] == 2 || a[3] == 3)) {
                // SPIF_UPDATEINIFILE persists nothing: the SDL setting is scoped to this process.
                if (!host_) {
                    throw GuestFault("screen saver update requires SDL host");
                }

                host_->screen_saver(a[1] != 0);
            } else if ((a[0] == 0x3a || a[0] == 0x34 || a[0] == 0x32) && (a[3] == 0 || a[3] == 2)) {
                uint32_t size = a[0] == 0x32 ? 24 : 8;
                if (!a[2] || c.memory.load32(a[2]) != size) {
                    return Result{};
                }

                // SDL does not expose these Windows accessibility features.
                // Report unavailable, matching the original Wine buffer probe.
                std::vector<uint8_t> data(size);
                c.memory.write(a[2], data.data(), data.size());
                c.memory.store32(a[2], size);
            } else if ((a[0] == 0x3b || a[0] == 0x35 || a[0] == 0x33) && a[3] <= 3) {
                // The original Wine implementation deliberately rejects these
                // setters too. Preserve that tested optional-feature error path.
                c.memory.store32(c.cpu.fs_base + 0x34, 1439);
                return Result{};
            } else if (a[0] == 0x2000 && !a[1] && !a[3]) {
                // Initial value measured in the original private Wine profile.
                c.memory.store32(a[2], foreground_timeout_);
            } else if (a[0] == 0x2001 && !a[1] && a[3] == 3) {
                if (profile_.empty()) {
                    throw GuestFault("system setting requires private profile");
                }

                foreground_timeout_ = uint32_t(a[2]);
                std::filesystem::create_directories(profile_);
                std::ofstream setting(profile_ + "/foreground-timeout");
                if (!(setting << foreground_timeout_ << '\n')) {
                    throw GuestFault("cannot persist foreground timeout");
                }

                // No windows exist at this reached startup boundary, hence no
                // WM_SETTINGCHANGE recipients. Never change the shared desktop.
            } else {
                throw GuestFault("unqualified system-parameters action " + std::to_string(a[0]));
            }

            return Result{1, {}};
        },
        "user32.dll!SystemParametersInfoA");
    control_.host(
        loader_.resolve("kernel32.dll", "FindResourceA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4, PointerDirection::In}, {4, PointerDirection::In}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            uint32_t handle = resource(a[0], a[1], a[2]);
            if (!handle) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1814);
                return Result{};
            }

            uint32_t rva = c.memory.load32(handle), size = c.memory.load32(handle + 4);
            uint32_t base = a[0] ? uint32_t(a[0]) : layout::exe;
            c.memory.check(base + rva, size, Read);
            resources_.insert_or_assign(handle, std::make_pair(base + rva, size));
            return Result{handle, {}};
        },
        "kernel32.dll!FindResourceA");
    control_.host(
        loader_.resolve("kernel32.dll", "LoadResource"), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto resource = resources_.find(uint32_t(a[1]));
            if (resource == resources_.end()) {
                c.memory.store32(c.cpu.fs_base + 0x34, 6);
                return Result{};
            }

            return Result{resource->second.first, {}};
        },
        "kernel32.dll!LoadResource");
    control_.host(
        loader_.resolve("kernel32.dll", "SizeofResource"), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto resource = resources_.find(uint32_t(a[1]));
            if (resource == resources_.end()) {
                c.memory.store32(c.cpu.fs_base + 0x34, 6);
                return Result{};
            }

            return Result{resource->second.second, {}};
        },
        "kernel32.dll!SizeofResource");
    control_.host(
        loader_.resolve("kernel32.dll", "LockResource"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            for (const auto& entry : resources_) {
                if (entry.second.first == a[0]) {
                    return Result{a[0], {}};
                }
            }

            c.memory.store32(c.cpu.fs_base + 0x34, 6);
            return Result{};
        },
        "kernel32.dll!LockResource");
    control_.host(
        loader_.resolve("kernel32.dll", "FreeResource"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [](Control&, const std::vector<uint64_t>&) {
            // Win32 retains module resources for the module lifetime. This
            // obsolete compatibility API returns FALSE; it does not free data.
            return Result{};
        },
        "kernel32.dll!FreeResource");
    control_.host(
        loader_.resolve("version.dll", "GetFileVersionInfoSizeA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::In}, {4, PointerDirection::Out}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto resource = version(ansi(a[0]));
            if (a[1]) {
                c.memory.store32(a[1], 0);
            }

            if (!resource.second) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1813);
            }

            // Extra space belongs to VerQueryValueA's ANSI string conversions.
            return Result{resource.second ? resource.second * 2u + 4u : 0u, {}};
        },
        "version.dll!GetFileVersionInfoSizeA");
    control_.host(
        loader_.resolve("version.dll", "GetFileVersionInfoA"),
        {Convention::Stdcall,
         ReturnChannel::Eax,
         {{4, PointerDirection::In}, {4}, {4}, {4, PointerDirection::Out}},
         ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto resource = version(ansi(a[0]));
            if (!resource.second) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1813);
                return Result{};
            }

            if (!a[2]) {
                c.memory.store32(c.cpu.fs_base + 0x34, 122);
                return Result{};
            }

            std::vector<uint8_t> bytes(std::min(uint32_t(a[2]), resource.second));
            c.memory.read(resource.first, bytes.data(), bytes.size());
            c.memory.write(a[3], bytes.data(), bytes.size());
            version_buffers_.insert_or_assign(uint32_t(a[3]),
                                              VersionBuffer{uint32_t(a[2]), (resource.second + 3u) & ~3u, {}});
            return Result{1, {}};
        },
        "version.dll!GetFileVersionInfoA");
    control_.host(
        loader_.resolve("version.dll", "VerQueryValueA"),
        {Convention::Stdcall,
         ReturnChannel::Eax,
         {{4, PointerDirection::In}, {4, PointerDirection::In}, {4, PointerDirection::Out}, {4, PointerDirection::Out}},
         ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto query = ansi(a[1]);
            uint32_t node = a[0];
            auto value = [&](uint32_t n) {
                uint32_t end = n + word(n), at = n + 6;
                while (word(at)) {
                    at += 2;
                    if (at + 2 > end) {
                        throw GuestFault("invalid version resource key");
                    }
                }

                return (at + 2 + 3) & ~3u;
            };
            auto find = [&](const std::string& key) {
                uint32_t end = node + word(node);
                uint32_t child = (value(node) + word(node + 2) * (word(node + 4) ? 2 : 1) + 3) & ~3u;
                while (child + 6 < end) {
                    uint32_t length = word(child);
                    if (!length || child + length > end) {
                        throw GuestFault("invalid version child extent");
                    }

                    std::string actual;
                    for (uint32_t at = child + 6; at + 2 <= child + length; at += 2) {
                        uint16_t ch = word(at);
                        if (!ch) {
                            break;
                        }

                        if (ch > 127) {
                            throw GuestFault("unqualified version key encoding");
                        }

                        actual += char(ch);
                    }

                    if (actual == key) {
                        node = child;
                        return true;
                    }

                    child = (child + length + 3) & ~3u;
                }

                return false;
            };
            if (query.empty() || query[0] != '\\') {
                throw GuestFault("invalid version query path");
            }

            for (size_t start = 1; start < query.size();) {
                size_t end = query.find('\\', start);
                if (!find(query.substr(start, end - start))) {
                    c.memory.store32(a[3], 0);
                    return Result{};
                }

                if (end == std::string::npos) {
                    break;
                }

                start = end + 1;
            }

            uint32_t at = value(node), length = word(node + 2);
            if (word(node + 4) && query != "\\") {
                auto found = version_buffers_.find(uint32_t(a[0]));
                if (found == version_buffers_.end()) {
                    throw GuestFault("unknown version string buffer");
                }

                auto& buffer = found->second;
                auto previous = buffer.strings.find(query);
                if (previous != buffer.strings.end()) {
                    at = previous->second;
                } else {
                    if (length > buffer.size || buffer.next > buffer.size - length) {
                        throw GuestFault("version ANSI expansion buffer exhausted");
                    }

                    std::vector<uint8_t> converted;
                    for (uint32_t i = 0; i < length; ++i) {
                        uint16_t ch = word(at + i * 2);
                        if (ch > 127) {
                            throw GuestFault("unqualified version string codepage");
                        }

                        converted.push_back(uint8_t(ch));
                    }

                    at = uint32_t(a[0]) + buffer.next;
                    c.memory.write(at, converted.data(), converted.size());
                    buffer.strings.emplace(query, at);
                    buffer.next += length;
                }
            } else {
                c.memory.check(at, length, Read);
            }

            c.memory.store32(a[2], at);
            c.memory.store32(a[3], length);
            return Result{1, {}};
        },
        "version.dll!VerQueryValueA");
    control_.host(
        loader_.resolve("user32.dll", "wsprintfA"),
        {Convention::Cdecl, ReturnChannel::Eax, {{4, PointerDirection::Out}, {4, PointerDirection::In}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            std::string format = ansi(a[1]), output;
            uint32_t argument = 2;
            for (size_t i = 0; i < format.size(); ++i) {
                if (format[i] != '%') {
                    output += format[i];
                    continue;
                }

                if (++i == format.size()) {
                    throw GuestFault("incomplete wsprintf format");
                }

                if (format[i] == '%') {
                    output += '%';
                    continue;
                }

                std::string spec = "%";
                while (i < format.size() && (format[i] == '0' || (format[i] >= '1' && format[i] <= '9'))) {
                    spec += format[i++];
                }

                if (i == format.size()) {
                    throw GuestFault("incomplete wsprintf width");
                }

                uint32_t value = c.arg(argument++);
                char buffer[1024];
                spec += format[i];
                int length;
                switch (format[i]) {
                case 'x':
                case 'X':
                case 'u':
                    length = snprintf(buffer, sizeof buffer, spec.c_str(), value);
                    break;
                case 'd':
                case 'i':
                    length = snprintf(buffer, sizeof buffer, spec.c_str(), int32_t(value));
                    break;
                case 's':
                    length = snprintf(buffer, sizeof buffer, spec.c_str(), ansi(value).c_str());
                    break;
                default:
                    throw GuestFault("unqualified wsprintf format: " + format);
                }

                if (length < 0 || length >= int(sizeof buffer)) {
                    throw GuestFault("wsprintf field overflow");
                }

                output.append(buffer, size_t(length));
            }

            if (output.size() > 1023) {
                throw GuestFault("wsprintf output exceeds documented limit");
            }

            c.memory.write(a[0], output.c_str(), output.size() + 1);
            return Result{output.size(), {}};
        },
        "user32.dll!wsprintfA");
}
}
