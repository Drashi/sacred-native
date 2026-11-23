#include "../../platform/qa.h"
#include "menu.h"
#include "../../platform/window_host.h"
#include "keyboard_layout.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sacred {
namespace {
constexpr uint32_t wm_paint = 0xf, wm_close = 0x10, wm_quit = 0x12, wm_activate = 6, wm_setfocus = 7, wm_killfocus = 8,
                   wm_keydown = 0x100, wm_keyup = 0x101, wm_char = 0x102, wm_syskeydown = 0x104, wm_syskeyup = 0x105,
                   wm_mousemove = 0x200, wm_lbuttondown = 0x201, wm_rbuttondown = 0x204, wm_mbuttondown = 0x207,
                   wm_mousewheel = 0x20a;
uint32_t tick() {
    return uint32_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Decodes UTF-8 host text into guest ANSI (CP1252) bytes. Malformed or unmappable code points are dropped,
// because host input must never fault the guest.
std::string utf8_to_cp1252(const std::string& text) {
    static const uint16_t high[32] = {0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021,
                                      0x2c6,  0x2030, 0x160,  0x2039, 0x152,  0x8d,   0x17d,  0x8f,
                                      0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
                                      0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178};
    std::string out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = text[i];
        uint32_t code = lead;
        size_t extra = 0;
        if (lead >= 0xf0 && lead < 0xf8) {
            code = lead & 7;
            extra = 3;
        } else if (lead >= 0xe0) {
            code = lead & 15;
            extra = 2;
        } else if (lead >= 0xc0) {
            code = lead & 31;
            extra = 1;
        } else if (lead >= 0x80) {
            ++i;
            continue;
        }

        if (lead >= 0xf8 || i + extra >= text.size() + (extra ? 0 : 1)) {
            ++i;
            continue;
        }

        bool valid = true;
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned char next = text[i + k];
            if ((next & 0xc0) != 0x80) {
                valid = false;
                break;
            }

            code = code << 6 | (next & 0x3f);
        }

        i += valid ? extra + 1 : 1;
        if (!valid) {
            continue;
        }

        if (code < 0x80 || (code >= 0xa0 && code <= 0xff)) {
            out.push_back(char(code));
            continue;
        }

        for (unsigned k = 0; k < 32; ++k) {
            if (high[k] == code) {
                out.push_back(char(0x80 + k));
                break;
            }
        }
    }

    return out;
}

bool input_log_enabled() {
    static const bool enabled = qa_env("SACRED_QA_INPUT_LOG");
    return enabled;
}

uint32_t thread_of(Control& control) {
    return control.memory.load32(control.cpu.fs_base + 0x24);
}

uint32_t queue_flags(uint32_t message) {
    if (message >= 0x100 && message <= 0x109) {
        return 1;
    }

    if (message == wm_mousemove) {
        return 2;
    }

    if (message >= 0x201 && message <= 0x209) {
        return 4;
    }

    if (message == wm_paint) {
        return 0x20;
    }

    return 8;
}
}

void Menu::translate_events() {
    HostEvent event;
    while (host_->take_event(event)) {
        uint32_t window = 0;
        for (const auto& candidate : windows_) {
            if (candidate.second.native == event.window) {
                window = candidate.first;
            }
        }

        const bool pointer_or_key = event.kind == HostEventKind::MouseMove || event.kind == HostEventKind::MouseDown ||
                                    event.kind == HostEventKind::MouseUp || event.kind == HostEventKind::Wheel ||
                                    event.kind == HostEventKind::KeyDown || event.kind == HostEventKind::KeyUp ||
                                    event.kind == HostEventKind::Text;
        if (!window && pointer_or_key) {
            // SDL reports window 0 for a button release that ends outside every window of ours; dropping it would leave
            // the guest believing the button is still down. Deliver it to the window that last saw input.
            const uint32_t last = host_->input_state().window;
            for (const auto& candidate : windows_) {
                if (candidate.second.native == last) {
                    window = candidate.first;
                }
            }

            if (!window && !windows_.empty()) {
                window = windows_.begin()->first;
            }

            if (input_log_enabled()) {
                std::fprintf(stderr, "QA_INPUT unmapped host_event kind=%d native_window=%u -> guest window=%u\n",
                             int(event.kind), event.window, window);
            }
        }

        if (!window) {
            continue;
        }

        auto post = [&](uint32_t message, uint32_t wparam, uint32_t lparam) {
            const Message posted{window, message, wparam, lparam, event.time, uint32_t(event.x), uint32_t(event.y)};
            if (message == wm_mousemove) {
                // Windows keeps at most one pending WM_MOUSEMOVE per queue; a guest that polls once per frame
                // must see the newest position, not a backlog of stale ones.
                auto pending = std::find_if(messages_.begin(), messages_.end(), [&](const Message& m) {
                    return m.window == window && m.message == wm_mousemove;
                });
                if (pending != messages_.end()) {
                    *pending = posted;
                    if (input_log_enabled()) {
                        std::fprintf(stderr, "QA_INPUT coalesced WM_MOUSEMOVE x=%d y=%d queue=%zu\n", event.x, event.y,
                                     messages_.size());
                    }

                    return;
                }
            }

            messages_.push_back(posted);
        };
        if (input_log_enabled() && event.kind != HostEventKind::Exposed) {
            std::fprintf(stderr, "QA_INPUT host_event kind=%d window=%u x=%d y=%d value=%d queue=%zu\n",
                         int(event.kind), window, event.x, event.y, event.value, messages_.size());
        }

        // A wheel event carries its notches in y, not a position: it happens where the pointer is (WM_MOUSEWHEEL's
        // lParam is the cursor's position), and the notches must not go through the inset's scaling, which turned a
        // notch towards the user into 0 in HD mode.
        int notches = 0;
        if (event.kind == HostEventKind::Wheel) {
            notches = event.y;
            const HostInputState input = host_->input_state();
            event.x = input.x;
            event.y = input.y;
        }

        if (event.kind == HostEventKind::MouseMove || event.kind == HostEventKind::MouseDown ||
            event.kind == HostEventKind::MouseUp || event.kind == HostEventKind::Wheel) {
            to_inset(event.x, event.y);
        }

        const uint32_t position = uint32_t(event.y) << 16 | (uint32_t(event.x) & 0xffff);
        // Button flags as they were when the event happened, not when the guest got around to it.
        auto buttons = [&] {
            return (event.buttons & 1 ? 1u : 0u) | (event.buttons & 2 ? 2u : 0u) | (event.buttons & 4 ? 0x10u : 0u);
        };
        switch (event.kind) {
        case HostEventKind::MouseMove:
            post(wm_mousemove, buttons(), position);
            break;
        case HostEventKind::MouseDown:
        case HostEventKind::MouseUp: {
            // X1/X2 and any other extra button have no handler in the game; dropped like unmapped keys.
            if (event.value < 1 || event.value > 3) {
                break;
            }

            const uint32_t base = event.value == 1   ? wm_lbuttondown
                                  : event.value == 3 ? wm_rbuttondown
                                                     : wm_mbuttondown;
            post(base + (event.kind == HostEventKind::MouseUp ? 1 : 0), buttons(), position);
            break;
        }
        case HostEventKind::Wheel:
            post(wm_mousewheel, uint32_t(int16_t(notches * 120)) << 16, position);
            break;
        case HostEventKind::KeyDown:
        case HostEventKind::KeyUp: {
            const bool down = event.kind == HostEventKind::KeyDown, alt = event.value == 18;
            post(down ? (alt ? wm_syskeydown : wm_keydown) : (alt ? wm_syskeyup : wm_keyup), uint32_t(event.value),
                 1u | (down ? (event.y ? 1u << 30 : 0u) : 0xc0000000u));
            break;
        }
        case HostEventKind::Text:
            for (unsigned char character : utf8_to_cp1252(event.text)) {
                post(wm_char, character, 1);
            }

            break;
        case HostEventKind::Close:
            post(wm_close, 0, 0);
            break;
        case HostEventKind::FocusGained:
            post(wm_activate, 1, 0);
            post(wm_setfocus, 0, 0);
            break;
        case HostEventKind::FocusLost: {
            // The host forgets held buttons on focus loss, so the matching releases will never arrive; hand them to the
            // guest first, or its own button tracking stays pressed until some later click happens to clear it.
            const HostInputState input = host_->input_state();
            const uint32_t at = uint32_t(input.y) << 16 | (uint32_t(input.x) & 0xffff);
            for (uint32_t bit = 1; bit <= 4; bit <<= 1) {
                if (event.buttons & bit) {
                    if (input_log_enabled()) {
                        std::fprintf(stderr, "QA_INPUT synthesized button release on focus loss bit=%u\n", bit);
                    }

                    post(bit == 1 ? wm_lbuttondown + 1 : bit == 2 ? wm_rbuttondown + 1 : wm_mbuttondown + 1, 0, at);
                }
            }

            post(wm_activate, 0, 0);
            post(wm_killfocus, 0, 0);
            break;
        }
        case HostEventKind::Exposed:
            windows_[window].invalid = true;
            if (std::none_of(messages_.begin(), messages_.end(),
                             [&](const Message& m) { return m.window == window && m.message == wm_paint; })) {
                post(wm_paint, 0, 0);
            }

            break;
        default:
            break;
        }
    }
}

bool Menu::fetch_message(uint32_t window, uint32_t low, uint32_t high, bool remove, Message& out) {
    translate_events();
    const uint32_t thread = thread_of(active_control());
    auto allowed = [&](uint32_t message) {
        return (!low && !high) || (message >= low && message <= high);
    };
    for (auto it = messages_.begin(); it != messages_.end();) {
        auto owner = windows_.find(it->window);
        if (owner == windows_.end()) {
            it = messages_.erase(it);
            continue;
        }

        if (owner->second.owner_thread == thread && (!window || it->window == window) && allowed(it->message)) {
            out = *it;
            if (remove) {
                messages_.erase(it);
            }

            return true;
        }

        ++it;
    }

    if (quit_posted_ && !window && allowed(wm_quit)) {
        out = {0, wm_quit, quit_code_, 0, tick(), 0, 0};
        if (remove) {
            quit_posted_ = false;
        }

        return true;
    }

    return false;
}

bool Menu::destroy_window(uint32_t handle) {
    auto found = windows_.find(handle);
    if (found == windows_.end()) {
        return false;
    }

    for (const auto& window : windows_) {
        if (window.second.parent == handle) {
            throw GuestFault("unqualified destruction of a window with children");
        }
    }

    if (found->second.owner_thread != thread_of(active_control())) {
        throw GuestFault("unqualified cross-thread window destruction");
    }

    window_message(handle, 0x2, 0, 0);
    window_message(handle, 0x82, 0, 0);
    host_->destroy_window(windows_.at(handle).native);
    messages_.erase(std::remove_if(messages_.begin(), messages_.end(),
                                   [handle](const Message& message) { return message.window == handle; }),
                    messages_.end());
    windows_.erase(handle);
    return true;
}

void Menu::bind_input() {
    auto translate_key = [](Control& c, uint32_t vk, uint32_t scan, uint32_t state, uint16_t& out) {
        uint8_t keys[256];
        c.memory.read(state, keys, sizeof keys);
        return us_layout_translate(vk, scan, keys, out);
    };
    control_.host(
        loader_.resolve("user32.dll", "ToAscii"),
        {Convention::Stdcall,
         ReturnChannel::Eax,
         {{4}, {4}, {4, PointerDirection::In}, {4, PointerDirection::Out}, {4}},
         ""},
        [translate_key](Control& c, const std::vector<uint64_t>& a) {
            uint16_t character = 0;
            const int count = translate_key(c, uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), character);
            if (count) {
                const uint16_t word = character & 0xff;
                c.memory.write(uint32_t(a[3]), &word, sizeof word);
            }

            return Result{uint32_t(count), {}};
        },
        "user32.dll!ToAscii");
    control_.host(
        loader_.resolve("user32.dll", "ToUnicode"),
        {Convention::Stdcall,
         ReturnChannel::Eax,
         {{4}, {4}, {4, PointerDirection::In}, {4, PointerDirection::Out}, {4}, {4}},
         ""},
        [translate_key](Control& c, const std::vector<uint64_t>& a) {
            uint16_t character = 0;
            const int count = translate_key(c, uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), character);
            if (!count || int32_t(a[4]) < 1) {
                return Result{0, {}};
            }

            c.memory.write(uint32_t(a[3]), &character, sizeof character);
            return Result{uint32_t(count), {}};
        },
        "user32.dll!ToUnicode");
    if (!host_) {
        return;
    }

    host_->set_event_notifier([this] { control_.scheduler().changed(); });
    auto write_message = [](Control& c, uint32_t address, const Message& m) {
        const uint32_t fields[7] = {m.window, m.message, m.wparam, m.lparam, m.time, m.x, m.y};
        c.memory.write(address, fields, sizeof fields);
    };
    const Signature peek{Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::Out}, {4}, {4}, {4}, {4}}, ""};
    const Signature get{Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::Out}, {4}, {4}, {4}}, ""};
    control_.host(
        loader_.resolve("user32.dll", "PeekMessageA"), peek,
        [this, write_message](Control& c, const std::vector<uint64_t>& a) {
            Message message;
            if (a[1] && !windows_.count(uint32_t(a[1]))) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1400);
                return Result{};
            }

            if (!fetch_message(uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3]), a[4] & 1, message)) {
                return Result{};
            }

            if (input_log_enabled() && message.message == wm_mousemove) {
                std::fprintf(stderr, "QA_INPUT peek WM_MOUSEMOVE x=%u y=%u age_ms=%u remove=%u queue=%zu\n", message.x,
                             message.y, tick() - message.time, uint32_t(a[4] & 1), messages_.size());
            }

            write_message(c, uint32_t(a[0]), message);
            return Result{1, {}};
        },
        "user32.dll!PeekMessageA");
    control_.host(
        loader_.resolve("user32.dll", "GetMessageA"), get,
        [this, write_message](Control& c, const std::vector<uint64_t>& a) {
            Message message;
            const uint32_t window = uint32_t(a[1]);
            bool delivered = false;
            control_.scheduler().wait(
                [&] {
                    delivered = fetch_message(window, uint32_t(a[2]), uint32_t(a[3]), true, message);
                    return delivered || (window && !windows_.count(window)) || host_->stopped();
                },
                true, {});
            if (!delivered && host_->stopped()) {
                throw HostStopped();
            }

            if (!delivered) {
                c.memory.store32(c.cpu.fs_base + 0x34, 1400);
                return Result{uint32_t(-1), {}};
            }

            write_message(c, uint32_t(a[0]), message);
            return Result{message.message == wm_quit ? 0u : 1u, {}};
        },
        "user32.dll!GetMessageA");
    control_.host(
        loader_.resolve("user32.dll", "WaitMessage"), {Convention::Stdcall, ReturnChannel::Eax, {}, ""},
        [this](Control&, const std::vector<uint64_t>&) {
            Message message;
            control_.scheduler().wait([&] { return fetch_message(0, 0, 0, false, message) || host_->stopped(); }, true,
                                      {});
            if (host_->stopped()) {
                throw HostStopped();
            }

            return Result{1, {}};
        },
        "user32.dll!WaitMessage");
    control_.host(
        loader_.resolve("user32.dll", "GetQueueStatus"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            translate_events();
            const uint32_t thread = thread_of(c);
            uint32_t mask = quit_posted_ ? 8u : 0u;
            for (const auto& message : messages_) {
                auto owner = windows_.find(message.window);
                if (owner != windows_.end() && owner->second.owner_thread == thread) {
                    mask |= queue_flags(message.message);
                }
            }

            mask &= uint32_t(a[0]);
            return Result{mask | mask << 16, {}};
        },
        "user32.dll!GetQueueStatus");
    control_.host(
        loader_.resolve("user32.dll", "TranslateMessage"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::In}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            // Printable characters come from the host's SDL text input. SDL produces no text for Esc, Backspace, Tab,
            // Enter or the Ctrl combinations, so those control characters are synthesized here exactly where Windows
            // does: a WM_CHAR queued right behind the WM_KEYDOWN. Sacred reads Esc only this way outside text entry.
            const uint32_t msg = uint32_t(a[0]);
            const uint32_t window = c.memory.load32(msg), message = c.memory.load32(msg + 4);
            if (message != wm_keydown) {
                return Result{};
            }

            const HostInputState state = host_->input_state();
            uint8_t keys[256] = {};
            for (unsigned key = 0; key < 256; ++key) {
                if (state.keys.test(key)) {
                    keys[key] = 0x80;
                }
            }

            uint16_t character = 0;
            if (!us_layout_translate(c.memory.load32(msg + 8), c.memory.load32(msg + 12) >> 16 & 0xff, keys,
                                     character)) {
                return Result{};
            }

            if (character < 0x20 || character == 0x7f) {
                if (input_log_enabled()) {
                    std::fprintf(stderr, "QA_INPUT TranslateMessage WM_KEYDOWN vk=%u -> WM_CHAR 0x%02x\n",
                                 c.memory.load32(msg + 8), character);
                }

                messages_.push_front({window, wm_char, character, c.memory.load32(msg + 12), tick(), 0, 0});
            }

            return Result{1, {}};
        },
        "user32.dll!TranslateMessage");
    control_.host(
        loader_.resolve("user32.dll", "DispatchMessageA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::In}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            const uint32_t window = c.memory.load32(uint32_t(a[0]));
            if (!window) {
                throw GuestFault("unqualified thread message dispatch");
            }

            return window_message(window, c.memory.load32(uint32_t(a[0]) + 4), c.memory.load32(uint32_t(a[0]) + 8),
                                  c.memory.load32(uint32_t(a[0]) + 12));
        },
        "user32.dll!DispatchMessageA");
    control_.host(
        loader_.resolve("user32.dll", "PostQuitMessage"), {Convention::Stdcall, ReturnChannel::Void, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            quit_posted_ = true;
            quit_code_ = uint32_t(a[0]);
            control_.scheduler().changed();
            return Result{};
        },
        "user32.dll!PostQuitMessage");
    control_.host(
        loader_.resolve("user32.dll", "PostMessageA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (!windows_.count(uint32_t(a[0]))) {
                throw GuestFault("unqualified PostMessage target");
            }

            messages_.push_back({uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3]), tick(), 0, 0});
            control_.scheduler().changed();
            (void)c;
            return Result{1, {}};
        },
        "user32.dll!PostMessageA");
    control_.host(
        loader_.resolve("user32.dll", "SendMessageA"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            auto target = windows_.find(uint32_t(a[0]));
            if (target == windows_.end()) {
                throw GuestFault("unqualified SendMessage target");
            }

            if (target->second.owner_thread != thread_of(c)) {
                throw GuestFault("unqualified cross-thread SendMessage");
            }

            return window_message(uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2]), uint32_t(a[3]));
        },
        "user32.dll!SendMessageA");

    auto window_long = [this](Control& c, uint32_t handle, int32_t index) -> uint32_t* {
        auto found = windows_.find(handle);
        if (found == windows_.end()) {
            throw GuestFault("unqualified window-long target");
        }

        auto& window = found->second;
        switch (index) {
        case -4:
            return &window.procedure;
        case -6:
            return &window.instance;
        case -8:
            return &window.parent;
        case -12:
            return &window.menu;
        case -16:
            return &window.style;
        case -20:
            return &window.extended;
        case -21:
            return &window.userdata;
        default:
            break;
        }

        (void)c;
        if (index < 0 || index % 4 || size_t(index) + 4 > window.extra.size()) {
            throw GuestFault("unqualified window-long index " + std::to_string(index));
        }

        return reinterpret_cast<uint32_t*>(window.extra.data() + index);
    };
    control_.host(
        loader_.resolve("user32.dll", "GetWindowLongA"), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [window_long](Control& c, const std::vector<uint64_t>& a) {
            uint32_t value;
            std::memcpy(&value, window_long(c, uint32_t(a[0]), int32_t(a[1])), 4);
            return Result{value, {}};
        },
        "user32.dll!GetWindowLongA");
    control_.host(
        loader_.resolve("user32.dll", "SetWindowLongA"), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4}}, ""},
        [window_long](Control& c, const std::vector<uint64_t>& a) {
            const int32_t index = int32_t(a[1]);
            if (index == -16 || index == -20 || index == -6 || index == -8 || index == -12) {
                throw GuestFault("unqualified window-long mutation " + std::to_string(index));
            }

            uint32_t* slot = window_long(c, uint32_t(a[0]), index);
            uint32_t previous;
            std::memcpy(&previous, slot, 4);
            const uint32_t value = uint32_t(a[2]);
            std::memcpy(slot, &value, 4);
            return Result{previous, {}};
        },
        "user32.dll!SetWindowLongA");
    control_.host(
        loader_.resolve("user32.dll", "DestroyWindow"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            if (destroy_window(uint32_t(a[0]))) {
                return Result{1, {}};
            }

            c.memory.store32(c.cpu.fs_base + 0x34, 1400);
            return Result{};
        },
        "user32.dll!DestroyWindow");
    control_.host(
        loader_.resolve("user32.dll", "GetCursorPos"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::Out}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            const HostInputState input = host_->input_state();
            std::pair<int, int> position = host_->global_mouse();
            if (input.window) {
                const HostWindowState window = host_->window_state(input.window);
                int32_t x = input.x, y = input.y;
                to_inset(x, y);
                position = {window.x + x, window.y + y};
            }

            if (input_log_enabled()) {
                std::fprintf(stderr, "QA_INPUT GetCursorPos x=%d y=%d window=%u local=%d,%d\n", position.first,
                             position.second, input.window, input.x, input.y);
            }

            c.memory.store32(uint32_t(a[0]), uint32_t(position.first));
            c.memory.store32(uint32_t(a[0]) + 4, uint32_t(position.second));
            return Result{1, {}};
        },
        "user32.dll!GetCursorPos");
    control_.host(
        loader_.resolve("user32.dll", "SetCursorPos"), {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (input_log_enabled()) {
                std::fprintf(stderr, "QA_INPUT SetCursorPos x=%d y=%d\n", int32_t(a[0]), int32_t(a[1]));
            }

            // A guest position inside an inset screen is shown where the inset box puts it in the client area.
            // SetCursorPos takes screen positions: the box is applied in client coordinates.
            int32_t x = int32_t(a[0]), y = int32_t(a[1]);
            if (inset_on_) {
                const HostInputState input = host_->input_state();
                const HostWindowState window = input.window ? host_->window_state(input.window) : HostWindowState{};
                x -= window.x;
                y -= window.y;
                from_inset(x, y);
                x += window.x;
                y += window.y;
            }

            host_->set_cursor_pos(x, y);
            return Result{1, {}};
        },
        "user32.dll!SetCursorPos");
    control_.host(
        loader_.resolve("user32.dll", "GetAsyncKeyState"), {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            const HostInputState state = host_->input_state();
            const uint32_t key = uint32_t(a[0]);
            bool down = false;
            if (key == 1) {
                down = state.buttons & 1;
            } else if (key == 2) {
                down = state.buttons & 2;
            } else if (key == 4) {
                down = state.buttons & 4;
            } else if (key < 256) {
                down = state.keys.test(key);
            }

            if (input_log_enabled() && (key == 1 || key == 2 || key == 4 || key == 27)) {
                static bool last[256];
                if (last[key] != down) {
                    last[key] = down;
                    std::fprintf(stderr, "QA_INPUT GetAsyncKeyState vk=%u down=%d host_buttons=0x%x\n", key, down,
                                 state.buttons);
                }
            }

            return Result{down ? 0x8000u : 0u, {}};
        },
        "user32.dll!GetAsyncKeyState");
    control_.host(
        loader_.resolve("user32.dll", "GetKeyboardState"),
        {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::Out}}, ""},
        [this](Control& c, const std::vector<uint64_t>& a) {
            const HostInputState state = host_->input_state();
            uint8_t keys[256] = {};
            for (unsigned key = 0; key < 256; ++key) {
                if (state.keys.test(key)) {
                    keys[key] = 0x80;
                }
            }

            if (state.buttons & 1) {
                keys[1] = 0x80;
            }

            if (state.buttons & 2) {
                keys[2] = 0x80;
            }

            if (state.buttons & 4) {
                keys[4] = 0x80;
            }

            c.memory.write(uint32_t(a[0]), keys, sizeof keys);
            return Result{1, {}};
        },
        "user32.dll!GetKeyboardState");
}
}
