#pragma once
// What a controller can point at in the game's UI, read from guest memory: the stops of D-pad navigation (buttons,
// menu entries, list rows, check boxes, edit fields, NPC answers) in the menus and windows the UI manager shows, the
// open in-game windows (for switching between them) and the inventory's tabs. Positions are client pixels, where
// the cursor is placed.
//
// Controls are told apart by the class names in the executable's RTTI (the vtable's complete object locator). A
// window's controls are its children (cUI_Window2 +0x78, a std::vector) and, for a few classes that draw and hit-test
// members of their own (the message box, the character screens, the savegame list, the game menu, the start menu),
// those members. A control's position is relative to its parent's (+0x50).
#include "../../platform/letterbox.h"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sacred {
class Memory;

struct UiPoint {
    float x = 0, y = 0;
};

struct UiRect {
    float left = 0, top = 0, right = 0, bottom = 0;
    bool contains(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
    UiPoint centre() const { return {(left + right) * 0.5f, (top + bottom) * 0.5f}; }
};

struct UiView {
    enum class Scope {
        none,        // nothing to navigate (the world, cinematics, loading)
        message_box, // a message box, alone
        game_menu,   // the game menu (Esc in game)
        npc_dialog,  // an NPC's answers
        menus,       // the start menu and the screens reached from it
        windows,     // in-game windows (inventory, trader, chest, map ...)
    };
    Scope scope = Scope::none;
    uint64_t key = 0; // changes when another modal window, menu screen or answer set shows: the cursor goes home
    std::vector<UiPoint> stops;
    bool has_home = false;
    UiPoint home; // where the cursor starts: OK, the first entry, the first answer
    // Scope windows: the shown windows (not the HUD), by address.
    struct Window {
        uint32_t address;
        UiRect rect;
    };
    std::vector<Window> windows;
    // The savegame list (load / save screens): its first and last listed rows and whether it scrolls further, so that
    // stepping past either end scrolls it (the mouse wheel over it moves it by one savegame).
    struct List {
        bool active = false;
        UiPoint first, last;
        float half_row = 0; // half a row's height
        bool more_above = false, more_below = false;
    } list;
};

class GamepadUi {
public:
    // The runtime shows a 1024x768 screen (the menus in HD mode) as an inset of the client area: layout positions go
    // through it.
    void set_inset(bool on, const InsetView& view) {
        inset_on_ = on;
        inset_ = view;
    }

    // `line_height`: a UI font's line height (layout pixels), for the portal box's rows; 0 if unknown.
    UiView read(Memory& memory, const std::function<int(uint32_t font)>& line_height);
    // Diagnostics (SACRED_GAMEPAD=trace).
    std::function<void(const std::string&)> trace_;
    // The inventory's tab `step` (-1 / 1) places from the open one, false if the inventory is closed or there is none
    // that way.
    bool inventory_tab(Memory& memory, int step, UiPoint& out);
    // The class of a game object from the executable's RTTI ("cUI_Button2", "cCreature" ...), "?" if it has none.
    static std::string class_name(Memory& memory, uint32_t object);
    // Diagnostics: a control's class, rect and flags, and its children's, `depth` levels down.
    std::string describe(Memory& memory, uint32_t control, int depth);
    // The nearest stop from (x, y) in direction (dx, dy), straight ahead first; false if there is none that way.
    static bool next(const std::vector<UiPoint>& stops, float x, float y, float dx, float dy, UiPoint& out);

private:
    enum class Kind : uint8_t {
        other,
        window,
        control, // something to point at (text entries, lists, edit fields)
        button,
        game_menu,
        main_menu,
        savegame,
        character,
        message_box,
        tooltip,
        tab_control,
    };
    struct Info {
        Kind kind = Kind::other;
        bool needs_enabled = true; // takes clicks only while enabled
    };

    Info info(Memory& memory, uint32_t object);
    UiPoint client(float x, float y) const;
    bool rect(Memory& memory, uint32_t control, UiRect& out);
    void add(Memory& memory, uint32_t control, std::vector<UiPoint>& out);
    void collect(Memory& memory, uint32_t control, std::vector<UiPoint>& out, int depth);
    void savegame_list(Memory& memory, uint32_t window, UiView& view);
    void portals(Memory& memory, uint32_t box, const std::function<int(uint32_t font)>& line_height,
                 std::vector<UiPoint>& out);
    void members(Memory& memory, uint32_t object, uint32_t from, uint32_t to, std::vector<UiPoint>& out, int depth);

    bool inset_on_ = false;
    InsetView inset_;
    std::unordered_map<uint32_t, Info> infos_; // by vtable
};
}
