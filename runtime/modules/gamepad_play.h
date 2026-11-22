#pragma once
// Opt-in controller play (Settings.cfg GAMEPAD : 1, or SACRED_GAMEPAD=1, which wins; =trace logs what it does). The
// controller drives the game through the input a mouse and a keyboard would give (WindowHost::pad_*), so the
// translated code is not touched and the mouse and keyboard keep working beside it.
//
// Where the input goes is read from the game each frame:
// - A movie: A, B and Start skip it (Esc).
// - Lists (the start menu and its screens, the game menu, a message box, an NPC's answers): the D-pad and the left
//   stick jump between the entries (gamepad_ui.h), the cursor starts on the first one (a message box's OK); A / X are
//   the left / right mouse button, B is Esc (Start is Enter in the start menu); the right stick moves the cursor
//   freely.
// - Windows in game (inventory, equipment, stats, traders, chests, maps ...): the left stick moves the cursor, the
//   D-pad jumps between the windows' buttons (or a cell's width where there is none), LB / RB switch the inventory's
//   tabs, LT / RT move the cursor to the window on the left / right (each window keeps where the cursor was), the
//   right stick scrolls; A / X click, B is Esc, the other buttons keep their keys.
// - The world:
//   left stick   walk toward the stick (cEngine::sendOrder with follow, as hold-to-walk gives it; the cMouse's
//                left-held bit is set while it lasts, so walking never attacks or talks)
//   A            attack the nearest enemy in the stick's direction (right stick when pushed) while held, the next
//                one when it dies; without an enemy: use / open / pick up / talk to the nearest thing; with nothing
//                near: attack in place (Ctrl)
//   X            the same with the right mouse button (the active combat art)
//   Y B RB RT    combat art slots 1-4 (keys 6-9)          LT + A X Y B RB RT: art slot 5, weapon slots 1-5 (0-5)
//   D-pad        healing potion (Space), heal hirelings (B), antidote (E), concentration (R)
//                LT + up / down: Undead Death (Q), Mentor (W); LT + left / right: zoom out / in (numpad - / +)
//   LB           stand still (Ctrl, held)
//   Start, Back  game menu (Esc), inventory (I)        LT + Start / Back: options (O), world map (M)
//   L3           show item names (Alt, held)          LT + L3: collect all (A)
//   R3           overview map (Tab, on / off)         LT + R3: cursor mode on / off (the free cursor in the world)
//
// Calls into the game (the hero's screen position, the move order, the enemy test) happen only at presents of the
// in-game main loop (game_frame.h); the pick list the aim reads is the one the world renderer just rebuilt.
#include "../../platform/gamepad.h"
#include "gamepad_ui.h"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>

namespace sacred {
class Control;
class Memory;
class WindowHost;

class GamepadPlay {
public:
    // `width` x `height`: the game screen (client area) the cursor moves in.
    GamepadPlay(WindowHost& host, std::function<void(const std::string&)> log, bool trace, int width, int height);
    // At every present of the primary surface, on the guest thread that presents.
    void present(Control& control);
    // The 1024x768 inset the runtime shows the menus in (WindowHost client positions go through it).
    void set_inset(bool on, const InsetView& view) {
        ui_.set_inset(on, view);
        if (trace_) {
            note("inset " + std::string(on ? "on" : "off") + " source_x=" + std::to_string(view.source_x) + " " +
                 std::to_string(view.source_width) + "x" + std::to_string(view.source_height) +
                 " box=" + std::to_string(view.box.x) + "," + std::to_string(view.box.y) + "+" +
                 std::to_string(view.box.width) + "x" + std::to_string(view.box.height));
        }
    }

private:
    enum class Context { movie, menus, cursor, world };
    enum class Target { enemy, use };
    // What a pressed button does, resolved when it goes down (with or without LT) and undone when it comes up.
    enum Action : int {
        none = 0,
        attack = -1,      // world: left mouse button at the aimed enemy
        art = -2,         // world: right mouse button at the aimed enemy
        cursor_mode = -3, // LT + R3
        overview = -4,    // Tab held on / off
        left_button = -5,
        right_button = -6,
        tab_previous = -7, // windows: the inventory's tabs
        tab_next = -8,
        window_left = -9, // windows: the cursor to the window on the left / right
        window_right = -10,
        skip_movie = -11, // a movie: Esc tapped
    };

    struct Aim {
        uint32_t id = 0;
        float x = 0, y = 0;
    };

    Context context(Memory& memory, const UiView& view);
    int action_for(uint32_t bit, bool layer, const UiView& view) const;
    void navigate(Memory& memory, const GamepadState& pad, const UiView& view, uint64_t now, float dt);
    void click_at(float x, float y);
    int line_height(Control& control, uint32_t font);
    void switch_window(const UiView& view, int step);
    void snap_to_cell(const UiView& view, float& x, float& y) const;
    void leave(Control& control);
    void press(Control& control, uint32_t bit, int action);
    void release(Control& control, uint32_t bit);
    void sync_cursor();
    void move_cursor(float x, float y, float speed, float dt);
    void scroll(float y, float dt);
    void world(Control& control, const GamepadState& pad, uint64_t now);
    void fight(Control& control, float dir_x, float dir_y, uint64_t now);
    bool hero_point(Control& control, float& x, float& y);
    bool find(Control& control, Target kind, float dir_x, float dir_y, Aim& out);
    bool search_hiding_place(Control& control);
    bool locate(Control& control, uint32_t id, Aim& out);
    bool send_walk(Control& control, int x, int y);
    void stop_walk(Control& control, bool order);
    void hold_left(Control& control, bool held);
    void pointer(int x, int y);
    void note(const std::string& line);

    WindowHost& host_;
    std::function<void(const std::string&)> log_;
    bool trace_ = false, broken_ = false;
#ifdef SACRED_QA
    bool probe_ = false; // QA (SACRED_GAMEPAD_PROBE=1): run as if a controller were connected
    bool probe_skip_ = false, probe_tabs_ = false;
    int probe_frames_ = 0;
    std::string probe_mode_;
#endif
    int width_, height_;
    float scale_; // game screen pixels per 768-line pixel
    std::string last_note_;
    uint64_t last_ns_ = 0;

    uint32_t previous_ = 0;   // buttons at the last present
    uint32_t suppressed_ = 0; // held when the context changed: ignored until released
    std::array<int, pad::button_count> active_{};
    Context context_ = Context::menus;
    bool cursor_mode_ = false, overview_ = false;
    bool mouse_down_[2] = {false, false};
    bool typing_ = false;

    // The cursor the sticks move (client pixels) and the position last handed to the host.
    float cursor_x_ = 0, cursor_y_ = 0;
    int set_x_ = -1, set_y_ = -1;
    bool resend_ = false;  // the position goes to the game once more at the next present
    int pointer_age_ = 99; // presents since the cursor last moved
    float wheel_ = 0;

    // Menus and windows.
    GamepadUi ui_;
    UiView view_;
    uint64_t view_key_ = 0;
    UiView::Scope scope_ = UiView::Scope::none;
    int nav_x_ = 0, nav_y_ = 0;                           // the direction held for navigation
    int home_due_ = 0;                                    // presents until the cursor goes to the view's home
    uint64_t nav_due_ns_ = 0;                             // its next repeat
    std::unordered_map<uint32_t, UiPoint> window_cursor_; // where the cursor was in each window
    std::deque<std::function<void()>> pending_;           // a click's steps, one a present
    std::unordered_map<uint32_t, int> line_heights_;      // by UI font id

    // World.
    uint32_t scratch_ = 0;
    float face_x_ = 0, face_y_ = 1; // the last walking direction
    bool walking_ = false;
    uint64_t order_ns_ = 0;
    int32_t walker_x_ = 0, walker_y_ = 0;
    struct Fight {
        bool active = false;
        int button = 1; // 1 left (attack), 3 right (combat art)
        Target kind = Target::enemy;
        uint32_t target = 0;
        bool down = false; // the mouse button is down
        bool ctrl = false; // no target: attack in place
        bool done = false; // a use click went out; nothing more until released
        uint64_t search_ns = 0;
    } fight_;
};
}
