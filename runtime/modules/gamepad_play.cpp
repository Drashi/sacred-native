#include "gamepad_play.h"
#include "game_frame.h"
#include "../process/control.h"
#include "../../platform/window_host.h"
#include "../ole/movie_playback.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <exception>
#include <vector>

namespace sacred {
namespace {
// Sacred.exe 2.28 (the executable the generated code is pinned to).
constexpr uint32_t ui_manager_cell = 0x017eebbc;     // cUI_Manager*
constexpr uint32_t engine_cell = 0x00ad6ef8;         // cEngine* (cEngine_create 0x60d6c0)
constexpr uint32_t object_manager_cell = 0x00ad5c40; // cObjectManager*
constexpr uint32_t font_manager_cell = 0x00cdca70;   // cFontManager*
constexpr uint32_t mouse_cell = 0x00cddb5c;          // cMouse*
constexpr uint32_t world_input_flags = 0x00ab73dc;   // byte; bit 1: the world takes no mouse

constexpr uint32_t object_data = 0x005fe000;     // thiscall (object manager, id) -> cObject*
constexpr uint32_t get_view_offset = 0x006113e0; // thiscall (engine, int* x, int* y)
constexpr uint32_t send_order = 0x00617030;      // thiscall (engine, cCreature*, cOrder*)
constexpr uint32_t is_enemy = 0x00548f60;        // thiscall (creature, target creature) -> bool
constexpr uint32_t dynamic_cast_ = 0x0084a961;   // cdecl (object, 0, source type, target type, 0) -> object or 0
constexpr uint32_t object_type_descriptor = 0x008eb648, creature_type_descriptor = 0x008eb660;
constexpr uint32_t order_vtable = 0x0089095c;

// cUI_Manager and its controls.
constexpr uint32_t ui_flags = 0x08, ui_windows = 0x80, ui_focus = 0x148;
constexpr uint32_t ui_in_menus = 0x01, ui_in_game = 0x04, ui_windows_created = 0x08, ui_cinematic = 0x10;
constexpr uint32_t control_flags = 0x10, control_visible = 0x01, control_focus = 0x8000;
constexpr int window_console = 9;

// cEngine, cWorldView, cObject, cMouse, cOrder.
constexpr uint32_t engine_views = 0x08, engine_view_index = 0x48, engine_flags = 0x54, engine_controlled = 0x13c;
constexpr uint32_t engine_no_world_input = 0x4000;
constexpr uint32_t view_screen_to_world = 0x18; // vtable slot: thiscall (view, x, y, WorldPoint*)
constexpr uint32_t view_pick_begin = 0x96b24, view_pick_end = 0x96b28, pick_entry_size = 0x1c;
constexpr uint32_t object_id = 0x0c, object_x = 0x1c, object_y = 0x20;
constexpr uint32_t mouse_held_item = 0x68;
constexpr uint16_t mouse_left_held = 0x100;
constexpr uint32_t order_size = 0x44, order_type = 0x04, order_mode = 0x14, order_x = 0x18, order_y = 0x1c,
                   order_follow = 0x20;
constexpr uint32_t order_move = 4, order_walk = 2, order_search = 7;
constexpr uint32_t cell_coordinate = 0x006224e0;   // thiscall (coordinate*, position*): world position -> cell
constexpr uint32_t map_cell_of = 0x00634c50;       // thiscall (map, coordinate*, key) -> cell*
constexpr uint32_t creature_map_key = 0x00449930;  // thiscall (creature) -> the key map_cell_of takes
constexpr uint32_t hiding_place_at = 0x004a6770;   // thiscall (scripts, coordinate by value, 0) -> index
constexpr uint32_t hiding_places_sqw = 0x00aacf80; // the quest script interpreter it is called on
constexpr uint32_t hiding_places_begin = 0x00ab45b8, hiding_places_end = 0x00ab45bc, hiding_place_size = 0x58;
constexpr uint32_t object_position = 0x18, creature_layer = 0x24;

// Scratch block in guest memory: the order, a world point and the view offset.
constexpr uint32_t scratch_order = 0x00, scratch_point = 0x48, scratch_offset = 0x60, scratch_size = 0x80;

constexpr float dead = 0.24f;          // of a stick's travel
constexpr float cursor_speed = 900.0f; // 768-line pixels per second at full deflection
constexpr float wheel_rate = 10.0f;    // notches per second at full deflection
constexpr float move_radius = 160.0f;  // 768-line pixels ahead of the hero
constexpr float aim_range = 450.0f;    // 768-line pixels around the hero
constexpr float use_range = 260.0f;
constexpr float aim_cone = 90.0f; // degrees around the stick's direction
constexpr uint64_t order_check_ns = 200'000'000, search_ns = 150'000'000;
constexpr uint64_t repeat_delay_ns = 400'000'000, repeat_interval_ns = 120'000'000; // a held direction
constexpr float nudge = 32.0f; // an inventory cell in screen pixels (the UI keeps its sizes)
constexpr size_t max_pick_entries = 4096;

constexpr int vk_return = 13, vk_escape = 27, vk_space = 32, vk_tab = 9, vk_control = 17, vk_menu = 18, vk_add = 107,
              vk_subtract = 109;

const Signature thiscall2{Convention::Thiscall, ReturnChannel::Eax, {{4}, {4}}, ""};
const Signature thiscall3{Convention::Thiscall, ReturnChannel::Eax, {{4}, {4}, {4}}, ""};
const Signature thiscall4{Convention::Thiscall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}}, ""};
const Signature thiscall1{Convention::Thiscall, ReturnChannel::Eax, {{4}}, ""};
const Signature thiscall6{Convention::Thiscall, ReturnChannel::Eax, {{4}, {4}, {4}, {4}, {4}, {4}}, ""};
const Signature cdecl5{Convention::Cdecl, ReturnChannel::Eax, {{4}, {4}, {4}, {4}, {4}}, ""};

uint64_t now_ns() {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint32_t call(Control& control, uint32_t target, const Signature& signature, const std::vector<uint64_t>& args) {
    return uint32_t(control.invoke(target, signature, args).integer);
}

int bit_index(uint32_t bit) {
    int index = 0;
    while (bit > 1) {
        bit >>= 1;
        ++index;
    }

    return index;
}

struct Binding {
    uint32_t button;
    int plain, layered; // with LT held
};

// The world's layout; the other contexts change a few buttons (binding()).
constexpr Binding world_layout[] = {
    {pad::A, -1 /*attack*/, '0'},
    {pad::X, -2 /*art*/, '1'},
    {pad::Y, '6', '2'},
    {pad::B, '7', '3'},
    {pad::RB, '8', '4'},
    {pad::RT, '9', '5'},
    {pad::LB, vk_control, 0},
    {pad::Up, vk_space, 'Q'},
    {pad::Down, 'B', 'W'},
    {pad::Left, 'E', vk_subtract},
    {pad::Right, 'R', vk_add},
    {pad::Start, vk_escape, 'O'},
    {pad::Back, 'I', 'M'},
    {pad::L3, vk_menu, 'A'},
    {pad::R3, -4 /*overview*/, -3 /*cursor*/},
};

// Lower is better: inside the cone by distance (a little more toward its edge), then the rest by distance.
float score(float vx, float vy, float dir_x, float dir_y) {
    const float distance = std::sqrt(vx * vx + vy * vy);
    const float dir_length = std::sqrt(dir_x * dir_x + dir_y * dir_y);
    if (dir_length < 1e-3f || distance < 1.0f) {
        return distance;
    }

    const float cosine = std::clamp((vx * dir_x + vy * dir_y) / (distance * dir_length), -1.0f, 1.0f);
    const float angle = std::acos(cosine) * 57.29578f;
    const float half = aim_cone * 0.5f;
    return angle <= half ? distance * (1.0f + 0.5f * angle / half) : 1e6f + distance;
}
}

GamepadPlay::GamepadPlay(WindowHost& host, std::function<void(const std::string&)> log, bool trace, int width,
                         int height)
    : host_(host), log_(std::move(log)), trace_(trace), width_(std::max(width, 1)), height_(std::max(height, 1)),
      scale_(float(std::max(height, 1)) / 768.0f) {
    if (trace_) {
        ui_.trace_ = [this](const std::string& line) {
            note(line);
        };
    }

#ifdef SACRED_QA
    const char* probe = std::getenv("SACRED_GAMEPAD_PROBE");
    probe_ = probe && *probe && *probe != '0';
    probe_skip_ = probe && std::string(probe) == "skip";
    probe_tabs_ = probe && std::string(probe) == "tabs";
    probe_mode_ = probe ? probe : "";
#endif
}

void GamepadPlay::note(const std::string& line) {
    if (trace_ && line != last_note_) {
        last_note_ = line;
        log_("GAMEPAD_TRACE " + line);
    }
}

GamepadPlay::Context GamepadPlay::context(Memory& memory, const UiView& view) {
    typing_ = false;
    if (movie_playing()) {
        return Context::movie;
    }

    const uint32_t ui = memory.load32(ui_manager_cell);
    if (!ui) {
        return Context::menus;
    }

    auto visible = [&](uint32_t control) {
        return control && (memory.load32(control + control_flags) & control_visible);
    };
    // An edit field keeps the focus (and its visible flag) after its window closed: the savegame name, a member of
    // the savegame window without a parent, after saving. It counts only while some window or menu shows.
    if (const uint32_t focus = memory.load32(ui + ui_focus);
        focus && view.scope != UiView::Scope::none && (memory.load32(focus + control_flags) & control_focus)) {
        typing_ = visible(focus);
    }

    const uint32_t flags = memory.load32(ui + ui_flags);
    if ((flags & (ui_cinematic | ui_in_menus)) || !(flags & ui_in_game) || !(flags & ui_windows_created)) {
        return Context::menus;
    }

    if (visible(memory.load32(ui + ui_windows + 4 * window_console))) {
        note("chat line open");
        typing_ = true;
    }

    if (typing_ || view.scope != UiView::Scope::none) {
        note(typing_ ? "cursor: typing" : "cursor: ui");
        return Context::cursor;
    }

    if (const uint32_t mouse = memory.load32(mouse_cell); mouse && memory.load32(mouse + mouse_held_item)) {
        note("cursor: item held");
        return Context::cursor; // an item on the cursor goes where the cursor puts it
    }

    return cursor_mode_ ? Context::cursor : Context::world;
}

int GamepadPlay::action_for(uint32_t bit, bool layer, const UiView& view) const {
    if (context_ == Context::movie) {
        return bit == pad::A || bit == pad::B || bit == pad::Start ? skip_movie : none;
    }

    const bool windows = view.scope == UiView::Scope::windows;
    if (context_ != Context::world) {
        layer = layer && !windows; // LT is a button of its own there
        switch (bit) {
        case pad::A:
            return left_button;
        case pad::X:
            return right_button;
        case pad::B:
            return vk_escape;
        case pad::Up:
        case pad::Down:
        case pad::Left:
        case pad::Right:
            return none; // navigation (navigate())
        default:
            break;
        }

        if (context_ == Context::menus) {
            return bit == pad::Start ? vk_return : none;
        }

        if (typing_ && !(bit == pad::R3 && layer)) {
            return none;
        }

        if (windows) {
            switch (bit) {
            case pad::LB:
                return tab_previous;
            case pad::RB:
                return tab_next;
            case pad::LT:
                return window_left;
            case pad::RT:
                return window_right;
            default:
                break;
            }
        } else if (view.scope != UiView::Scope::none) {
            // The game menu, a message box, an NPC's answers: only the list's buttons (Start closes the game menu).
            return bit == pad::Start && view.scope == UiView::Scope::game_menu ? vk_escape : none;
        }
    }

    for (const Binding& binding : world_layout) {
        if (binding.button == bit) {
            return layer ? binding.layered : binding.plain;
        }
    }

    return none;
}

void GamepadPlay::present(Control& control) {
    if (broken_) {
        return;
    }

    try {
        const uint64_t now = now_ns();
        const float dt = last_ns_ ? std::clamp(float(now - last_ns_) * 1e-9f, 0.0f, 0.1f) : 0.0f;
        last_ns_ = now;
        GamepadState pad = host_.gamepad_state();
#ifdef SACRED_QA
        if (probe_) {
            pad.connected = true; // QA: no controller, but everything that reads the game runs
        }
#endif
        if (!pad.connected) {
            pad = {};
            if (!previous_ && !walking_ && !fight_.active && !overview_) {
                return;
            }
        }

        ++pointer_age_;
        if (resend_) {
            // The cursor once more where it went: the game keeps the position before the last move too (cMouse +0xc)
            // and some windows pick by that one, so a single move after a jump clicks the cell it came from.
            resend_ = false;
            host_.pad_pointer(set_x_, set_y_);
        }

        if (!pending_.empty()) {
            // A click spread over frames (the game reads the cursor once a frame): one step now.
            pending_.front()(); // runs while it is still queued (QA probes see a click in progress)
            pending_.pop_front();
        }

        view_ = ui_.read(control.memory, [&](uint32_t font) { return line_height(control, font); });
        const Context now_context = context(control.memory, view_);
        // In the world everything waits for the main loop's present (loading screens and fades flip elsewhere).
        if (now_context == Context::world && !main_loop_present(control)) {
            return;
        }

        if (now_context != context_) {
            static const char* const names[] = {"movie", "menus", "cursor", "world"};
            note(std::string("context ") + names[int(now_context)]);
            leave(control);
            suppressed_ = pad.buttons;
            previous_ = 0;
            context_ = now_context;
            set_x_ = set_y_ = -1; // take the cursor where it is
#ifdef SACRED_QA
            probe_frames_ = 0;
#endif
        }

#ifdef SACRED_QA
        // SACRED_GAMEPAD_PROBE=skip: B held from the 30th to the 40th present of every movie.
        if (probe_skip_ && context_ == Context::movie && ++probe_frames_ >= 30 && probe_frames_ < 40) {
            pad.buttons |= pad::B;
        }

        // =down: the D-pad down tapped every 40 presents in menus and windows; =use: A held from the 60th to the 70th
        // present in the world.
        if (probe_mode_ == "down" && context_ != Context::world && context_ != Context::movie &&
            ++probe_frames_ % 40 < 5) {
            pad.buttons |= pad::Down;
        }

        if (probe_mode_ == "use" && context_ == Context::world && ++probe_frames_ >= 60 && probe_frames_ < 70) {
            pad.buttons |= pad::A;
        }
#endif

        suppressed_ &= pad.buttons;
        const uint32_t buttons = pad.buttons & ~suppressed_;
        const uint32_t pressed = buttons & ~previous_, released = previous_ & ~buttons;
        previous_ = buttons;
        if (context_ == Context::menus || context_ == Context::cursor) {
            navigate(control.memory, pad, view_, now, dt);
        }

        for (uint32_t bit = 1; bit < (1u << pad::button_count); bit <<= 1) {
            if (released & bit) {
                release(control, bit);
            }
        }

        const bool layer = buttons & pad::LT;
        for (uint32_t bit = 1; bit < (1u << pad::button_count); bit <<= 1) {
            if ((pressed & bit) &&
                (bit != pad::LT || (context_ == Context::cursor && view_.scope == UiView::Scope::windows))) {
                press(control, bit, action_for(bit, layer, view_));
            }
        }

        if (context_ == Context::world) {
            world(control, pad, now);
        }
    } catch (const std::exception& error) {
        broken_ = true;
        log_(std::string("GAMEPAD_DISABLED ") + error.what());
    }
}

void GamepadPlay::leave(Control& control) {
    for (uint32_t bit = 1; bit < (1u << pad::button_count); bit <<= 1) {
        release(control, bit);
    }

    if (overview_) {
        overview_ = false;
        host_.pad_key(vk_tab, false);
    }

    if (walking_) {
        stop_walk(control, false);
    }

    wheel_ = 0;
    nav_x_ = nav_y_ = 0;
}

void GamepadPlay::press(Control& control, uint32_t bit, int action) {
    active_[size_t(bit_index(bit))] = action;
    if (action > 0) {
        host_.pad_key(action, true);
        return;
    }

    switch (action) {
    case left_button:
    case right_button: {
        const int button = action == left_button ? 1 : 3;
        mouse_down_[button == 1 ? 0 : 1] = true;
        if (pointer_age_ < 3 || !pending_.empty()) {
            // The cursor just moved: the press waits until the game has seen it settle there.
            while (pending_.size() + size_t(pointer_age_) < 3) {
                pending_.push_back([] {});
            }

            pending_.push_back([this, button] { host_.pad_button(button, true); });
        } else {
            host_.pad_button(button, true);
        }

        break;
    }
    case attack:
    case art:
        if (fight_.active) {
            active_[size_t(bit_index(bit))] = none; // one fight at a time
            break;
        }

        fight_ = {};
        fight_.active = true;
        fight_.button = action == attack ? 1 : 3;
        break;
    case cursor_mode:
        cursor_mode_ = !cursor_mode_;
        note(std::string("cursor mode ") + (cursor_mode_ ? "on" : "off"));
        break;
    case overview:
        overview_ = !overview_;
        host_.pad_key(vk_tab, overview_);
        break;
    case tab_previous:
    case tab_next: {
        UiPoint tab;
        if (ui_.inventory_tab(control.memory, action == tab_next ? 1 : -1, tab)) {
            click_at(tab.x, tab.y);
        }

        break;
    }
    case skip_movie:
        // A tap of Esc the host releases by itself: after Esc the player waits for the key to come up without
        // presenting a frame, so a release from here would never come.
        host_.pad_key_tap(vk_escape, 100);
        break;
    case window_left:
    case window_right:
        switch_window(view_, action == window_right ? 1 : -1);
        break;
    default:
        break;
    }
}

void GamepadPlay::release(Control&, uint32_t bit) {
    int& action = active_[size_t(bit_index(bit))];
    if (action > 0) {
        host_.pad_key(action, false);
    } else if (action == left_button || action == right_button) {
        const int button = action == left_button ? 1 : 3;
        if (mouse_down_[button == 1 ? 0 : 1]) {
            mouse_down_[button == 1 ? 0 : 1] = false;
            if (pending_.empty()) {
                host_.pad_button(button, false);
            } else {
                pending_.push_back([this, button] { host_.pad_button(button, false); }); // after its press
            }
        }
    } else if (action == attack || action == art) {
        if (fight_.down) {
            host_.pad_button(fight_.button, false);
        }

        if (fight_.ctrl) {
            host_.pad_key(vk_control, false);
        }

        fight_ = {};
    }

    action = none;
}

void GamepadPlay::pointer(int x, int y) {
    x = std::clamp(x, 0, width_ - 1);
    y = std::clamp(y, 0, height_ - 1);
    if (x != set_x_ || y != set_y_) {
        set_x_ = x;
        set_y_ = y;
        cursor_x_ = float(x);
        cursor_y_ = float(y);
#ifdef SACRED_QA
        if (probe_ && !(probe_tabs_ && !pending_.empty()) && !(probe_mode_ == "down" && view_.list.active)) {
            note("pointer " + std::to_string(x) + "," + std::to_string(y)); // the script owns the pointer
            return;
        }
#endif
        host_.pad_pointer(x, y);
        resend_ = true;
        pointer_age_ = 0;
    }
}

void GamepadPlay::sync_cursor() {
    const HostInputState input = host_.input_state();
    if (input.x != set_x_ || input.y != set_y_) {
        // The mouse moved it (or the cursor was just taken over).
        cursor_x_ = float(input.x);
        cursor_y_ = float(input.y);
        set_x_ = input.x;
        set_y_ = input.y;
    }
}

void GamepadPlay::move_cursor(float x, float y, float speed, float dt) {
    // The deflection beyond the deadzone, squared: fine moves near the centre, full speed at the rim.
    const Stick move = dead_zone(x, y, dead);
    if (move.length > 0) {
        const float step = cursor_speed * speed * scale_ * move.length * dt;
        const float to_x = std::clamp(cursor_x_ + move.x * step, 0.0f, float(width_ - 1));
        const float to_y = std::clamp(cursor_y_ + move.y * step, 0.0f, float(height_ - 1));
        pointer(int(std::lround(to_x)), int(std::lround(to_y)));
        cursor_x_ = to_x; // keep the fraction
        cursor_y_ = to_y;
    }
}

void GamepadPlay::scroll(float y, float dt) {
    const Stick stick = dead_zone(0, y, dead);
    if (stick.length > 0) {
        wheel_ -= stick.y * wheel_rate * dt; // stick up turns the wheel away from the player
        if (const int notches = int(wheel_)) {
            host_.pad_wheel(notches);
            wheel_ -= float(notches);
        }
    } else {
        wheel_ = 0;
    }
}

void GamepadPlay::click_at(float x, float y) {
    // Over four frames: the game hit-tests where it read the cursor that frame, and controls such as the inventory's
    // tabs react to the cursor over them before the press.
    const int back_x = set_x_, back_y = set_y_, to_x = int(std::lround(x)), to_y = int(std::lround(y));
    pending_.push_back([this, to_x, to_y] { pointer(to_x, to_y); });
    pending_.push_back([this] { host_.pad_button(1, true); });
    pending_.push_back([this] { host_.pad_button(1, false); });
    if (back_x >= 0) {
        pending_.push_back([this, back_x, back_y] { pointer(back_x, back_y); });
    }
}

int GamepadPlay::line_height(Control& control, uint32_t font) {
    if (auto known = line_heights_.find(font); known != line_heights_.end()) {
        return known->second;
    }

    // cFontManager (its first member a std::vector<cFont*> by font id); the font's vtable slot 0x28 is
    // lineHeight(character) -> pixels in the low 16 bits.
    Memory& memory = control.memory;
    const uint32_t manager = memory.load32(font_manager_cell);
    const uint32_t first = manager ? memory.load32(manager) : 0, last = manager ? memory.load32(manager + 4) : 0;
    if (!first || last <= first + 4 * font) {
        return 0;
    }

    const uint32_t object = memory.load32(first + 4 * font);
    const uint32_t function = object ? memory.load32(memory.load32(object) + 0x28) : 0;
    const int height = function ? int(int16_t(call(control, function, thiscall2, {object, 'A'}) & 0xffff)) : 0;
    line_heights_[font] = height;
    return height;
}

void GamepadPlay::snap_to_cell(const UiView& view, float& x, float& y) const {
    // The windows' cells (backpack, equipment) lie on a grid from the window's corner: the centre of the cell, where
    // the game's hover and click agree.
    for (const UiView::Window& window : view.windows) {
        if (window.rect.contains(x, y)) {
            x = window.rect.left + std::floor((x - window.rect.left) / nudge) * nudge + nudge * 0.5f;
            y = window.rect.top + std::floor((y - window.rect.top) / nudge) * nudge + nudge * 0.5f;
            return;
        }
    }
}

void GamepadPlay::switch_window(const UiView& view, int step) {
    if (view.windows.empty()) {
        return;
    }

    // Left to right; the cursor's window, else none (then the first one to that side).
    std::vector<UiView::Window> windows = view.windows;
    std::sort(windows.begin(), windows.end(), [](const UiView::Window& a, const UiView::Window& b) {
        const float ax = a.rect.left + a.rect.right, bx = b.rect.left + b.rect.right;
        return ax < bx || (ax == bx && a.rect.top > b.rect.top); // the lower one first
    });
    int current = -1;
    for (size_t index = 0; index < windows.size(); ++index) {
        if (windows[index].rect.contains(cursor_x_, cursor_y_)) {
            current = int(index); // the last one: drawn over the others
        }
    }

    const int count = int(windows.size());
    const int target = current < 0 ? (step > 0 ? 0 : count - 1) : ((current + step) % count + count) % count;
    const UiView::Window& window = windows[size_t(target)];
    UiPoint at = window.rect.centre();
    snap_to_cell(view, at.x, at.y);
    if (auto kept = window_cursor_.find(window.address);
        kept != window_cursor_.end() && window.rect.contains(kept->second.x, kept->second.y)) {
        at = kept->second;
    } else {
        // Its first stop: the topmost, then leftmost.
        bool found = false;
        for (const UiPoint& stop : view.stops) {
            if (window.rect.contains(stop.x, stop.y) &&
                (!found || stop.y < at.y - 4 || (std::fabs(stop.y - at.y) <= 4 && stop.x < at.x))) {
                at = stop;
                found = true;
            }
        }
    }

    pointer(int(std::lround(at.x)), int(std::lround(at.y)));
}

void GamepadPlay::navigate(Memory& memory, const GamepadState& pad, const UiView& view, uint64_t now, float dt) {
    sync_cursor();
    // A list without stops (a screen not understood) keeps the free cursor on the left stick.
    const bool list = (view.scope == UiView::Scope::message_box || view.scope == UiView::Scope::game_menu ||
                       view.scope == UiView::Scope::npc_dialog || view.scope == UiView::Scope::menus) &&
                      !view.stops.empty();
    if (view.scope != scope_ || view.key != view_key_) {
        static const char* const scopes[] = {"none", "message box", "game menu", "npc dialog", "menus", "windows"};
        std::string stops;
        for (size_t index = 0; trace_ && index < view.stops.size() && index < 24; ++index) {
            stops += " " + std::to_string(int(view.stops[index].x)) + "," + std::to_string(int(view.stops[index].y));
        }

        note(std::string("ui ") + scopes[int(view.scope)] + " stops=" + std::to_string(view.stops.size()) +
             " windows=" + std::to_string(view.windows.size()) + stops);
        for (size_t index = 0; trace_ && index < view.windows.size(); ++index) {
            log_("GAMEPAD_TRACE window " + ui_.describe(memory, view.windows[index].address, 3));
        }
#ifdef SACRED_QA
        if (probe_tabs_ && view.scope == UiView::Scope::windows) {
            UiPoint tab;
            if (ui_.inventory_tab(memory, 1, tab)) {
                note("probe tab click " + std::to_string(int(tab.x)) + "," + std::to_string(int(tab.y)));
                click_at(tab.x, tab.y);
            }
        }
#endif
        if (trace_ && view.scope == UiView::Scope::windows) {
            UiPoint left, right;
            const bool has_left = ui_.inventory_tab(memory, -1, left), has_right = ui_.inventory_tab(memory, 1, right);
            log_("GAMEPAD_TRACE tabs" +
                 (has_left ? " left " + std::to_string(int(left.x)) + "," + std::to_string(int(left.y))
                           : std::string()) +
                 (has_right ? " right " + std::to_string(int(right.x)) + "," + std::to_string(int(right.y))
                            : std::string()));
        }
        const bool entered_windows = view.scope == UiView::Scope::windows && scope_ != UiView::Scope::windows;
        scope_ = view.scope;
        view_key_ = view.key;
        nav_x_ = nav_y_ = 0;
        if (view.has_home) {
            // Next present: a screen's first frame is read before the runtime reports the inset it is shown in (the
            // previous screen's would place the cursor beside it).
            home_due_ = 2;
        } else if (entered_windows) {
            bool inside = false;
            for (const UiView::Window& window : view.windows) {
                inside = inside || window.rect.contains(cursor_x_, cursor_y_);
            }

            if (!inside) {
                switch_window(view, 1);
            }
        }
    }

    if (home_due_ && --home_due_ == 0 && view.has_home) {
        pointer(int(std::lround(view.home.x)), int(std::lround(view.home.y)));
    }

    if (view.scope == UiView::Scope::windows) {
        for (const UiView::Window& window : view.windows) {
            if (window.rect.contains(cursor_x_, cursor_y_)) {
                window_cursor_[window.address] = {cursor_x_, cursor_y_};
            }
        }
    }

    // The free cursor: the right stick in a list, else the left (the right one scrolls).
    if (list) {
        move_cursor(pad.rx, pad.ry, 0.6f, dt);
    } else {
        move_cursor(pad.lx, pad.ly, 1.0f, dt);
        scroll(pad.ry, dt);
    }

    // The direction held: the D-pad, or in a list the left stick (its stronger axis, past half its travel).
    int dx = (pad.buttons & pad::Right ? 1 : 0) - (pad.buttons & pad::Left ? 1 : 0);
    int dy = (pad.buttons & pad::Down ? 1 : 0) - (pad.buttons & pad::Up ? 1 : 0);
    if (!dx && !dy && list) {
        const Stick stick = dead_zone(pad.lx, pad.ly, dead);
        if (stick.length > 0.5f) {
            if (std::fabs(stick.x) > std::fabs(stick.y)) {
                dx = stick.x > 0 ? 1 : -1;
            } else {
                dy = stick.y > 0 ? 1 : -1;
            }
        }
    }

    bool step = false;
    if (dx != nav_x_ || dy != nav_y_) {
        nav_x_ = dx;
        nav_y_ = dy;
        nav_due_ns_ = now + repeat_delay_ns;
        step = dx || dy;
    } else if ((dx || dy) && now >= nav_due_ns_) {
        nav_due_ns_ = now + repeat_interval_ns;
        step = true;
    }

    if (view.list.active) {
        note(std::string("savegame list") + (view.list.more_above ? " more above" : "") +
             (view.list.more_below ? " more below" : ""));
    }

    if (step && dy && !dx && view.list.active) {
        // Past the savegame list's first or last row: the list scrolls (the wheel over it) and the cursor stays.
        const UiView::List& list = view.list;
        const UiPoint& end = dy > 0 ? list.last : list.first;
        if (std::fabs(cursor_y_ - end.y) <= list.half_row && std::fabs(cursor_x_ - end.x) <= 400 &&
            (dy > 0 ? list.more_below : list.more_above)) {
            note(dy > 0 ? "list scroll down" : "list scroll up");
            host_.pad_wheel(dy > 0 ? -1 : 1);
            step = false;
        }
    }

    if (step) {
        UiPoint to;
        if (GamepadUi::next(view.stops, cursor_x_, cursor_y_, float(dx), float(dy), to)) {
            pointer(int(std::lround(to.x)), int(std::lround(to.y)));
        } else if (!list) {
            // No button that way (an inventory's cells): a cell's width.
            float x = cursor_x_ + float(dx) * nudge, y = cursor_y_ + float(dy) * nudge;
            snap_to_cell(view, x, y);
            pointer(int(std::lround(x)), int(std::lround(y)));
        }
    }
}

void GamepadPlay::hold_left(Control& control, bool held) {
    const uint32_t mouse = control.memory.load32(mouse_cell);
    if (!mouse) {
        return;
    }

    uint16_t flags = 0;
    control.memory.read(mouse, &flags, sizeof flags);
    const uint16_t changed = held ? uint16_t(flags | mouse_left_held) : uint16_t(flags & ~mouse_left_held);
    if (changed != flags) {
        control.memory.write(mouse, &changed, sizeof changed);
    }
}

namespace {
// The creature the mouse commands (the hero, or its mount), 0 while the world takes no mouse.
uint32_t controlled(Control& control) {
    Memory& memory = control.memory;
    const uint32_t engine = memory.load32(engine_cell), manager = memory.load32(object_manager_cell);
    if (!engine || !manager || (memory.load32(engine + engine_flags) & engine_no_world_input) ||
        (memory.load32(world_input_flags) & 2)) {
        return 0;
    }

    const uint32_t id = memory.load32(engine + engine_controlled);
    return id && !(id & 0x80000000u) ? call(control, object_data, thiscall2, {manager, id}) : 0;
}

uint32_t world_view(Memory& memory) {
    const uint32_t engine = memory.load32(engine_cell);
    if (!engine) {
        return 0;
    }

    const uint32_t index = memory.load32(engine + engine_view_index) & 0xffff;
    return index < 8 ? memory.load32(engine + engine_views + 4 * index) : 0;
}
}

bool GamepadPlay::hero_point(Control& control, float& x, float& y) {
    const uint32_t engine = control.memory.load32(engine_cell);
    if (!engine || !world_view(control.memory)) {
        return false;
    }

    if (!scratch_) {
        scratch_ = control.heap.allocate(scratch_size);
    }

    // The hero stands at the screen centre minus twice the view's trailing offset; hold-to-walk measures its
    // direction from there (0x4fb8af).
    call(control, get_view_offset, thiscall3, {engine, scratch_ + scratch_offset, scratch_ + scratch_offset + 4});
    x = float(width_) * 0.5f - 2.0f * float(int32_t(control.memory.load32(scratch_ + scratch_offset)));
    y = float(height_) * 0.5f - 2.0f * float(int32_t(control.memory.load32(scratch_ + scratch_offset + 4)));
    return true;
}

bool GamepadPlay::send_walk(Control& control, int x, int y) {
    Memory& memory = control.memory;
    const uint32_t engine = memory.load32(engine_cell), view = world_view(memory), creature = controlled(control);
    if (!engine || !view || !creature) {
        return false;
    }

    if (!scratch_) {
        scratch_ = control.heap.allocate(scratch_size);
    }

    const uint32_t point = scratch_ + scratch_point, order = scratch_ + scratch_order;
    for (uint32_t offset = 0; offset < 0x10; offset += 4) {
        memory.store32(point + offset, 0);
    }

    const uint32_t screen_to_world = memory.load32(memory.load32(view) + view_screen_to_world);
    call(control, screen_to_world, thiscall4, {view, uint32_t(x), uint32_t(y), point});
    for (uint32_t offset = 0; offset < order_size; offset += 4) {
        memory.store32(order + offset, 0);
    }

    memory.store32(order, order_vtable);
    memory.store32(order + order_type, order_move);
    memory.store32(order + order_mode, order_walk);
    memory.store32(order + order_x, memory.load32(point + 4));
    memory.store32(order + order_y, memory.load32(point + 8));
    memory.store32(order + order_follow, 1);
    // The follow walk lasts while the left button is held: the bit without a press, whose click would path-find.
    hold_left(control, true);
    call(control, send_order, thiscall3, {engine, creature, order});
    walker_x_ = int32_t(memory.load32(creature + object_x));
    walker_y_ = int32_t(memory.load32(creature + object_y));
    return true;
}

void GamepadPlay::stop_walk(Control& control, bool order) {
    walking_ = false;
    hold_left(control, false);
    note("walk stop");
    if (!order) {
        return;
    }

    Memory& memory = control.memory;
    const uint32_t engine = memory.load32(engine_cell), creature = controlled(control);
    if (!engine || !creature || !scratch_) {
        return;
    }

    const uint32_t stop = scratch_ + scratch_order;
    for (uint32_t offset = 0; offset < order_size; offset += 4) {
        memory.store32(stop + offset, 0);
    }

    memory.store32(stop, order_vtable);
    memory.store32(stop + order_type, order_move);
    call(control, send_order, thiscall3, {engine, creature, stop});
}

void GamepadPlay::world(Control& control, const GamepadState& pad, uint64_t now) {
    const Stick walk = dead_zone(pad.lx, pad.ly, dead), aim = dead_zone(pad.rx, pad.ry, dead);
    if (walk.length > 0) {
        face_x_ = walk.x / walk.length;
        face_y_ = walk.y / walk.length;
    }

    if (fight_.active) {
        if (walking_) {
            stop_walk(control, false); // the attack's click takes over
        }

        if (aim.length > 0) {
            fight(control, aim.x, aim.y, now);
        } else {
            fight(control, face_x_, face_y_, now);
        }

        return;
    }

    if (walk.length <= 0) {
        if (walking_) {
            stop_walk(control, true);
        }

        return;
    }

    float hero_x = 0, hero_y = 0;
    if (!hero_point(control, hero_x, hero_y)) {
        if (walking_) {
            stop_walk(control, false);
        }

        return;
    }

    const float radius = move_radius * scale_;
    pointer(int(std::lround(hero_x + face_x_ * radius)), int(std::lround(hero_y + face_y_ * radius)));
    if (!walking_) {
        if (send_walk(control, set_x_, set_y_)) {
            walking_ = true;
            order_ns_ = now;
            note("walk");
        }

        return;
    }

    hold_left(control, true);
    if (now - order_ns_ >= order_check_ns) {
        // Each order restarts the walk: it goes out again only once the hero stands (blocked, or the walk ended).
        order_ns_ = now;
        const uint32_t creature = controlled(control);
        if (creature && int32_t(control.memory.load32(creature + object_x)) == walker_x_ &&
            int32_t(control.memory.load32(creature + object_y)) == walker_y_) {
            send_walk(control, set_x_, set_y_);
        } else if (creature) {
            walker_x_ = int32_t(control.memory.load32(creature + object_x));
            walker_y_ = int32_t(control.memory.load32(creature + object_y));
        }
    }
}

void GamepadPlay::fight(Control& control, float dir_x, float dir_y, uint64_t now) {
    if (fight_.done || fight_.ctrl) {
        return;
    }

    Aim aim;
    if (fight_.target && !locate(control, fight_.target, aim)) {
        note("target gone");
        fight_.target = 0;
        if (fight_.down) {
            host_.pad_button(fight_.button, false); // the next press goes to the next enemy
            fight_.down = false;
            return;
        }
    }

    if (!fight_.target) {
        if (now < fight_.search_ns) {
            return;
        }

        const bool first = fight_.search_ns == 0;
        fight_.search_ns = now + search_ns;
        if (find(control, Target::enemy, dir_x, dir_y, aim)) {
            fight_.target = aim.id;
            note("target " + std::to_string(aim.id));
        } else if (!first) {
            return; // keep looking while the button is held
        } else if (fight_.button == 1 && find(control, Target::use, dir_x, dir_y, aim)) {
            note("use " + std::to_string(aim.id));
            pointer(int(std::lround(aim.x)), int(std::lround(aim.y)));
            host_.pad_button(1, true);
            host_.pad_button(1, false);
            fight_.done = true;
            return;
        } else if (fight_.button == 1 && search_hiding_place(control)) {
            fight_.done = true;
            return;
        } else {
            // Nothing near: attack (left) in place with Ctrl, or the combat art (right) toward the stick.
            float hero_x = 0, hero_y = 0;
            if (!hero_point(control, hero_x, hero_y)) {
                return;
            }

            const float reach = move_radius * 0.5f * scale_;
            const float length = std::max(1e-3f, std::sqrt(dir_x * dir_x + dir_y * dir_y));
            pointer(int(std::lround(hero_x + dir_x / length * reach)),
                    int(std::lround(hero_y + dir_y / length * reach)));
            if (fight_.button == 1) {
                host_.pad_key(vk_control, true);
                fight_.ctrl = true;
            }

            host_.pad_button(fight_.button, true);
            fight_.down = true;
            note("in place");
            return;
        }
    }

    pointer(int(std::lround(aim.x)), int(std::lround(aim.y)));
    if (!fight_.down) {
        host_.pad_button(fight_.button, true);
        fight_.down = true;
    }
}

namespace {
struct PickEntry {
    uint32_t id;
    float x, y; // centre
};

template <class F> void scan_picks(Memory& memory, F&& visit) {
    const uint32_t view = world_view(memory);
    if (!view) {
        return;
    }

    const uint32_t begin = memory.load32(view + view_pick_begin), end = memory.load32(view + view_pick_end);
    if (!begin || end < begin || (end - begin) / pick_entry_size > max_pick_entries) {
        return;
    }

    for (uint32_t entry = begin; entry + pick_entry_size <= end; entry += pick_entry_size) {
        uint8_t raw[0x10];
        memory.read(entry, raw, sizeof raw);
        uint32_t id = 0;
        int32_t left = 0, top = 0;
        int16_t width = 0, height = 0;
        std::memcpy(&id, raw, 4);
        std::memcpy(&left, raw + 4, 4);
        std::memcpy(&top, raw + 8, 4);
        std::memcpy(&width, raw + 12, 2);
        std::memcpy(&height, raw + 14, 2);
        if (!visit(PickEntry{id, float(left) + float(width) * 0.5f, float(top) + float(height) * 0.5f})) {
            return;
        }
    }
}

// The cCreature `id` names, 0 if it is no creature.
uint32_t creature_of(Control& control, uint32_t manager, uint32_t id) {
    const uint32_t object = call(control, object_data, thiscall2, {manager, id});
    return object
               ? call(control, dynamic_cast_, cdecl5, {object, 0, object_type_descriptor, creature_type_descriptor, 0})
               : 0;
}
}

bool GamepadPlay::locate(Control& control, uint32_t id, Aim& out) {
    bool found = false;
    scan_picks(control.memory, [&](const PickEntry& entry) {
        if (entry.id != id) {
            return true;
        }

        out = {id, entry.x, entry.y};
        found = true;
        return false;
    });
    if (!found || fight_.kind != Target::enemy) {
        return found;
    }

    const uint32_t manager = control.memory.load32(object_manager_cell), hero = controlled(control);
    const uint32_t creature = manager ? creature_of(control, manager, id) : 0;
    return hero && creature && (call(control, is_enemy, thiscall2, {hero, creature}) & 0xff);
}

// A magical hiding place is no object but a mark on a map cell (cell +0x1e bit 0x80) that the quest scripts list
// (cInterpretSQW_hiding_place_at 0x4a6770 -> index into the table 0xab45b8..0xab45bc of 0x58-byte entries). A click
// on such a cell gives the hero an order of type 7 for it (the world mouse handler at 0x617a20); here the cells
// around the hero are searched for one and the hero gets that order.
bool GamepadPlay::search_hiding_place(Control& control) {
    Memory& memory = control.memory;
    const uint32_t manager = memory.load32(object_manager_cell), creature = controlled(control);
    const uint32_t map = manager ? memory.load32(manager) : 0;
    if (!map || !creature) {
        return false;
    }

    if (!scratch_) {
        scratch_ = control.heap.allocate(scratch_size);
    }

    const uint32_t centre = scratch_ + scratch_point;
    call(control, cell_coordinate, thiscall2, {centre, creature + object_position});
    memory.store32(centre + 12, memory.load32(creature + creature_layer) & 0xff);
    const int32_t cx = int32_t(memory.load32(centre + 4)), cy = int32_t(memory.load32(centre + 8));
    const uint32_t hidden_count =
        (memory.load32(hiding_places_end) - memory.load32(hiding_places_begin)) / hiding_place_size;
    const uint32_t key = call(control, creature_map_key, thiscall1, {creature});
    // Nearest first: the hero's cell, then the rings around it.
    for (int ring = 0; ring <= 2; ++ring) {
        for (int dy = -ring; dy <= ring; ++dy) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != ring) {
                    continue;
                }

                const uint32_t coordinate = scratch_ + scratch_order; // reused: no order is pending
                memory.store32(coordinate, memory.load32(centre));
                memory.store32(coordinate + 4, uint32_t(cx + dx));
                memory.store32(coordinate + 8, uint32_t(cy + dy));
                memory.store32(coordinate + 12, memory.load32(centre + 12));
                const uint32_t cell = call(control, map_cell_of, thiscall3, {map, coordinate, key});
                if (!cell || !(memory.load32(cell + 0x1c) & 0x00800000)) {
                    continue;
                }

                const uint32_t index = call(control, hiding_place_at, thiscall6,
                                            {hiding_places_sqw, memory.load32(coordinate), uint32_t(cx + dx),
                                             uint32_t(cy + dy), memory.load32(coordinate + 12), 0}) &
                                       0xffff;
                if (index >= hidden_count) {
                    continue;
                }

                const uint32_t order = scratch_ + scratch_order;
                for (uint32_t offset = 0; offset < order_size; offset += 4) {
                    memory.store32(order + offset, 0);
                }

                memory.store32(order, order_vtable);
                memory.store32(order + order_type, order_search);
                memory.store32(order + order_x, uint32_t(cx + dx));
                memory.store32(order + order_y, uint32_t(cy + dy));
                memory.store32(order + order_follow, 0xb);
                const uint32_t handler = memory.load32(memory.load32(creature) + 0x18);
                call(control, handler, thiscall2, {creature, order});
                note("hiding place " + std::to_string(index) + " at cell " + std::to_string(dx) + "," +
                     std::to_string(dy));
                return true;
            }
        }
    }

    return false;
}

#ifdef SACRED_QA
namespace {
// QA diagnostics: every object in the 5 x 5 map cells around the hero, with its class and position.
void dump_cells(Control& control, uint32_t hero, const std::function<void(const std::string&)>& log) {
    Memory& memory = control.memory;
    const uint32_t manager = memory.load32(object_manager_cell), map = manager ? memory.load32(manager) : 0;
    const uint32_t scratch = control.heap.allocate(32);
    call(control, 0x006224e0, thiscall2, {scratch, hero + 0x18});
    const int32_t cx = int32_t(memory.load32(scratch + 4)), cy = int32_t(memory.load32(scratch + 8));
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            memory.store32(scratch + 16, 0);
            memory.store32(scratch + 20, uint32_t(cx + dx));
            memory.store32(scratch + 24, uint32_t(cy + dy));
            memory.store32(scratch + 28, 0);
            const uint32_t cell = call(control, 0x00634c50, thiscall3, {map, scratch + 16, memory.load32(hero + 0x2c)});
            uint32_t id = cell ? memory.load32(cell + 8) : 0;
            for (int n = 0; id && n < 64; ++n) {
                const uint32_t object = call(control, object_data, thiscall2, {manager, id});
                if (!object) {
                    break;
                }

                log("GAMEPAD_TRACE cell " + std::to_string(dx) + "," + std::to_string(dy) +
                    " id=" + std::to_string(id) + " class=" + GamepadUi::class_name(memory, object) +
                    " type=" + std::to_string(memory.load32(object + 0x10)) +
                    " flags=" + std::to_string(memory.load32(object + 0x14)) +
                    " at=" + std::to_string(int32_t(memory.load32(object + object_x))) + "," +
                    std::to_string(int32_t(memory.load32(object + object_y))));
                id = memory.load32(object + 0x30);
            }
        }
    }

    control.heap.free(scratch);
}
}
#endif

bool GamepadPlay::find(Control& control, Target kind, float dir_x, float dir_y, Aim& out) {
    Memory& memory = control.memory;
#ifdef SACRED_QA
    if (probe_mode_ == "use" && kind == Target::use) {
        if (const uint32_t hero = controlled(control)) {
            log_("GAMEPAD_TRACE hero at=" + std::to_string(int32_t(memory.load32(hero + object_x))) + "," +
                 std::to_string(int32_t(memory.load32(hero + object_y))));
            dump_cells(control, hero, log_);
        }
    }
#endif
    const uint32_t manager = memory.load32(object_manager_cell), hero = controlled(control);
    float hero_x = 0, hero_y = 0;
    if (!manager || !hero || !hero_point(control, hero_x, hero_y)) {
        return false;
    }

    const uint32_t hero_id = memory.load32(hero + object_id);
    scan_picks(memory, [&](const PickEntry& entry) {
        if (entry.id == hero_id) {
            hero_x = entry.x; // the body's centre, where the pick list measures from
            hero_y = entry.y;
            return false;
        }

        return true;
    });
    const float range = (kind == Target::enemy ? aim_range : use_range) * scale_;
    float best = 0;
    bool found = false;
    scan_picks(memory, [&](const PickEntry& entry) {
        if (!entry.id || entry.id == hero_id) {
            return true;
        }

        const float vx = entry.x - hero_x, vy = entry.y - hero_y;
        if (vx * vx + vy * vy > range * range) {
            return true;
        }

        // Ids with bit 31 are no objects of the object manager; they can still be clicked (a use target).
        const bool object = !(entry.id & 0x80000000u);
        const uint32_t creature = object ? creature_of(control, manager, entry.id) : 0;
        const bool enemy = creature && (call(control, is_enemy, thiscall2, {hero, creature}) & 0xff);
        if (trace_ && kind == Target::use) {
            const uint32_t data = object ? call(control, object_data, thiscall2, {manager, entry.id}) : 0;
            log_("GAMEPAD_TRACE candidate id=" + std::to_string(entry.id) +
                 " class=" + (data ? GamepadUi::class_name(memory, data) : std::string("-")) +
                 " distance=" + std::to_string(int(std::sqrt(vx * vx + vy * vy))) + (enemy ? " enemy" : ""));
        }
        if (enemy != (kind == Target::enemy)) {
            return true;
        }

        const float rank = score(vx, vy, dir_x, dir_y);
        if (!found || rank < best) {
            best = rank;
            out = {entry.id, entry.x, entry.y};
            found = true;
        }

        return true;
    });
    if (found) {
        fight_.kind = kind;
    }

    return found;
}
}
