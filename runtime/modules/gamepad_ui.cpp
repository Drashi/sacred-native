#include "gamepad_ui.h"
#include "../process/memory.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sacred {
namespace {
constexpr uint32_t ui_manager_cell = 0x017eebbc;
constexpr uint32_t character_slot_locked = 0x017ea3d0; // byte per character slot: clicks on it are ignored
constexpr uint32_t ui_flags = 0x08, ui_windows = 0x80, ui_popups = 0x124, ui_menus = 0x130, ui_dialog = 0x144;
constexpr uint32_t ui_in_menus = 0x01, ui_menus_created = 0x02, ui_in_game = 0x04, ui_windows_created = 0x08,
                   ui_cinematic = 0x10;
constexpr int savegame_menu = 3, menu_count = 5, window_count = 23, window_game_menu = 12, window_inventory = 1;
constexpr int exclusive_windows[] = {11, 15, 20, 5}; // savegame, options, character export, world map: drawn alone
constexpr bool hud_window(int index) {
    return index == 0 || index == 6 || index == 7 || index == 9 || index == 19;
}

// cUI_Control2 / cUI_Window2.
constexpr uint32_t control_flags = 0x10, control_x = 0x24, control_y = 0x28, control_size = 0x2c, control_parent = 0x50,
                   window_children = 0x78;
constexpr uint32_t flag_visible = 0x01, flag_enabled = 0x04;
// Members of particular windows.
constexpr uint32_t game_menu_entries = 0x158;                           // std::vector<cUI_StaticText64*>
constexpr uint32_t main_menu_screen = 0x154, main_menu_entries = 0x158; // a vector per screen (3 with entries)
constexpr uint32_t savegame_rows = 0x72c, savegame_buttons = 0x764;     // 4 rects (x, y, w | h << 16); 4 buttons
constexpr uint32_t button_size = 0xbc;
constexpr uint32_t savegame_entries = 0x15c, savegame_entry_size = 0x310, savegame_slider = 0x478,
                   savegame_mode = 0x75c, savegame_asked_mode = 0x760;
constexpr uint32_t slider_count = 0x88, slider_position = 0x8c;     // cUI_Slider_getValue 0x753430
constexpr uint32_t slider_rounding = 0x00890670;                    // the float it adds before truncating
constexpr uint32_t character_slots = 0x1b4, character_hero = 0x2f8; // 8 cUI_NetGranny*, its hero
constexpr uint32_t character_members[] = {0x300, 0x398, 0x39c, 0x3a0, 0x3b0, 0x3b4, 0x3b8, 0x3bc, 0x3c0};
constexpr uint32_t message_box_mode = 0x154, message_box_text = 0x158, message_box_portals = 0x170;
constexpr uint32_t portal_mode_surface = 5, portal_mode_underworld = 6, portal_font = 3, portal_count = 14;
constexpr uint32_t message_box_size = 0x644, window_size = 0x84, control_object_size = 0x78;
constexpr uint32_t popup_answers = 0x217c, answer_size = 0x14; // text id, ?, x, y (relative), width, height
constexpr uint32_t inventory_tabs = 0x154, tabs_control = 0x78, tab_pages = 0x78, tab_active = 0x84, page_size = 0x18,
                   page_button = 0x10;
constexpr size_t max_stops = 512, max_vector = 512;

bool readable(Memory& memory, uint32_t address, uint32_t bytes) {
    return address >= 0x10000 && address + bytes > address && (memory.permissions(address) & Read) &&
           (memory.permissions(address + bytes - 1) & Read);
}

uint32_t peek(Memory& memory, uint32_t address) {
    return readable(memory, address, 4) ? memory.load32(address) : 0;
}

uint8_t peek8(Memory& memory, uint32_t address) {
    uint8_t value = 0;
    if (readable(memory, address, 1)) {
        memory.read(address, &value, 1);
    }

    return value;
}

bool in_image(uint32_t address) {
    return address >= 0x00401000 && address < 0x01900000;
}

// A std::vector of 4-byte items (first, last, end of storage): its items, none if it looks broken.
std::vector<uint32_t> items(Memory& memory, uint32_t vector, size_t item = 4, size_t limit = max_vector) {
    std::vector<uint32_t> out;
    const uint32_t first = peek(memory, vector), last = peek(memory, vector + 4);
    if (!first || last < first || (last - first) % item || (last - first) / item > limit ||
        !readable(memory, first, last - first)) {
        return out;
    }

    for (uint32_t at = first; at < last; at += uint32_t(item)) {
        out.push_back(at);
    }

    return out;
}

bool visible(Memory& memory, uint32_t control) {
    return control && (peek(memory, control + control_flags) & flag_visible);
}
}

GamepadUi::Info GamepadUi::info(Memory& memory, uint32_t object) {
    const uint32_t vtable = peek(memory, object);
    if (!in_image(vtable)) {
        return {};
    }

    if (auto cached = infos_.find(vtable); cached != infos_.end()) {
        return cached->second;
    }

    // MSVC RTTI: vtable[-1] is the complete object locator (signature 0); +0x10 its class hierarchy (+8 the number of
    // base classes, +0xc their array, the class itself first); each base's type descriptor has the decorated name at
    // +8.
    Info result;
    bool button = false, control = false, window = false;
    const uint32_t locator = peek(memory, vtable - 4);
    const uint32_t hierarchy = in_image(locator) && peek(memory, locator) == 0 ? peek(memory, locator + 0x10) : 0;
    const uint32_t count = in_image(hierarchy) ? std::min<uint32_t>(peek(memory, hierarchy + 8), 32) : 0;
    const uint32_t bases = count ? peek(memory, hierarchy + 0x0c) : 0;
    for (uint32_t index = 0; in_image(bases) && index < count; ++index) {
        const uint32_t base = peek(memory, bases + 4 * index), type = in_image(base) ? peek(memory, base) : 0;
        if (!in_image(type) || !readable(memory, type + 8, 48)) {
            continue;
        }

        char raw[48] = {};
        memory.read(type + 8, raw, sizeof raw - 1);
        const std::string name(raw);
        if (name.rfind(".?AV", 0) != 0) {
            continue;
        }

        if (index == 0) {
            static const std::pair<const char*, Kind> classes[] = {
                {".?AVcUI_EscMenu@@", Kind::game_menu},      {".?AVcUI_MainMenu@@", Kind::main_menu},
                {".?AVcUI_Savegame@@", Kind::savegame},      {".?AVcUI_Character@@", Kind::character},
                {".?AVcUI_BusyDlg@@", Kind::message_box},    {".?AVcUI_Tooltip@@", Kind::tooltip},
                {".?AVcUI_TabControl@@", Kind::tab_control},
            };
            for (const auto& [class_name, kind] : classes) {
                if (name == class_name) {
                    result.kind = kind;
                }
            }
        }

        button = button || name == ".?AVcUI_Button2@@" || name == ".?AVcUI_RadioButton2@@";
        control = control || name == ".?AVcUI_StaticText64FX@@" || name == ".?AVcUI_Combobox@@" ||
                  name == ".?AVcUI_EditControl@@" || name == ".?AVcUI_DigitEdit@@" || name == ".?AVcUI_Listbox@@" ||
                  name == ".?AVcUI_Listbox2@@";
        window = window || name == ".?AVcUI_Window2@@";
        if (name == ".?AVcUI_Combobox@@" || name == ".?AVcUI_Listbox2@@") {
            result.needs_enabled = false;
        }
    }

    if (result.kind == Kind::other) {
        result.kind = button ? Kind::button : control ? Kind::control : window ? Kind::window : Kind::other;
    }

    infos_.emplace(vtable, result);
    return result;
}

std::string GamepadUi::class_name(Memory& memory, uint32_t object) {
    const uint32_t vtable = peek(memory, object), locator = in_image(vtable) ? peek(memory, vtable - 4) : 0;
    const uint32_t hierarchy = in_image(locator) && peek(memory, locator) == 0 ? peek(memory, locator + 0x10) : 0;
    const uint32_t bases = in_image(hierarchy) ? peek(memory, hierarchy + 0x0c) : 0;
    const uint32_t base = in_image(bases) ? peek(memory, bases) : 0, type = in_image(base) ? peek(memory, base) : 0;
    if (!in_image(type) || !readable(memory, type + 8, 48)) {
        return "?";
    }

    char raw[48] = {};
    memory.read(type + 8, raw, sizeof raw - 1);
    std::string name(raw);
    if (name.rfind(".?AV", 0) == 0 && name.size() > 6) {
        name = name.substr(4, name.find("@@") - 4);
    }

    return name;
}

std::string GamepadUi::describe(Memory& memory, uint32_t control, int depth) {
    std::string text = class_name(memory, control);
    UiRect r;
    if (rect(memory, control, r)) {
        text += "@" + std::to_string(int(r.left)) + "," + std::to_string(int(r.top)) + "+" +
                std::to_string(int(r.right - r.left)) + "x" + std::to_string(int(r.bottom - r.top));
    }

    char flags[16];
    std::snprintf(flags, sizeof flags, "/%x", peek(memory, control + control_flags));
    text += flags;
    const std::vector<uint32_t> children = items(memory, control + window_children);
    if (depth > 0 && !children.empty()) {
        text += "[";
        for (size_t index = 0; index < children.size() && index < 40; ++index) {
            text += (index ? " " : "") + describe(memory, peek(memory, children[index]), depth - 1);
        }

        text += "]";
    }

    return text;
}

UiPoint GamepadUi::client(float x, float y) const {
    if (!inset_on_) {
        return {x, y};
    }

    int ix = int(std::lround(x)), iy = int(std::lround(y));
    inset_from_source(inset_, ix, iy);
    return {float(ix), float(iy)};
}

bool GamepadUi::rect(Memory& memory, uint32_t control, UiRect& out) {
    if (!readable(memory, control, control_object_size)) {
        return false;
    }

    const int width = int16_t(memory.load32(control + control_size) & 0xffff);
    const int height = int16_t(memory.load32(control + control_size) >> 16);
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return false;
    }

    // The parents' positions added, as the game hit-tests it.
    int32_t x = int32_t(memory.load32(control + control_x)), y = int32_t(memory.load32(control + control_y));
    uint32_t parent = memory.load32(control + control_parent);
    for (int depth = 0; parent && depth < 8; ++depth) {
        if (!readable(memory, parent, control_object_size)) {
            return false;
        }

        x += int32_t(memory.load32(parent + control_x));
        y += int32_t(memory.load32(parent + control_y));
        parent = memory.load32(parent + control_parent);
    }

    const UiPoint a = client(float(x), float(y)), b = client(float(x + width), float(y + height));
    out = {a.x, a.y, b.x, b.y};
    return true;
}

void GamepadUi::add(Memory& memory, uint32_t control, std::vector<UiPoint>& out) {
    UiRect r;
    if (out.size() < max_stops && rect(memory, control, r)) {
        const UiPoint centre = r.centre();
        for (const UiPoint& p : out) {
            if (std::fabs(p.x - centre.x) < 1 && std::fabs(p.y - centre.y) < 1) {
                return; // a member that is a child too
            }
        }

        out.push_back(centre);
    }
}

void GamepadUi::members(Memory& memory, uint32_t object, uint32_t from, uint32_t to, std::vector<UiPoint>& out,
                        int depth) {
    for (uint32_t offset = from; offset + control_object_size <= to; offset += 4) {
        const uint32_t member = object + offset;
        if (!in_image(peek(memory, member))) {
            continue;
        }

        const Info kind = info(memory, member);
        if (kind.kind != Kind::button && kind.kind != Kind::control) {
            continue;
        }

        // Shown only while its parents up to the window are.
        bool shown = true;
        uint32_t parent = peek(memory, member + control_parent);
        for (int level = 0; parent && parent != object && level < 8; ++level) {
            shown = shown && visible(memory, parent);
            parent = peek(memory, parent + control_parent);
        }

        if (shown) {
            collect(memory, member, out, depth + 1);
        }
    }
}

void GamepadUi::collect(Memory& memory, uint32_t control, std::vector<UiPoint>& out, int depth) {
    if (depth > 8 || out.size() >= max_stops || !visible(memory, control)) {
        return;
    }

    const Info kind = info(memory, control);
    switch (kind.kind) {
    case Kind::button:
    case Kind::control:
        if (!kind.needs_enabled || (peek(memory, control + control_flags) & flag_enabled)) {
            add(memory, control, out);
        }

        return;
    case Kind::game_menu:
        for (const uint32_t at : items(memory, control + game_menu_entries, 4, 16)) {
            const uint32_t entry = peek(memory, at);
            if (visible(memory, entry) && (peek(memory, entry + control_flags) & flag_enabled)) {
                add(memory, entry, out);
            }
        }

        return;
    case Kind::main_menu: {
        const uint32_t screen = peek(memory, control + main_menu_screen);
        if (screen < 3) {
            for (const uint32_t at : items(memory, control + main_menu_entries + 12 * screen, 4, 16)) {
                const uint32_t entry = peek(memory, at);
                if (visible(memory, entry) && (peek(memory, entry + control_flags) & flag_enabled)) {
                    add(memory, entry, out);
                }
            }
        }

        return;
    }
    case Kind::window:
    case Kind::savegame:
    case Kind::character:
    case Kind::message_box:
    case Kind::tooltip:
        for (const uint32_t at : items(memory, control + window_children)) {
            collect(memory, peek(memory, at), out, depth + 1);
        }

        break;
    default:
        return;
    }

    if (kind.kind == Kind::message_box) {
        members(memory, control, window_size, message_box_size, out, depth);
    } else if (kind.kind == Kind::savegame && readable(memory, control + savegame_rows, 0x48 + 4 * button_size)) {
        for (int row = 0; row < 4; ++row) {
            const uint32_t at = control + savegame_rows + 12 * uint32_t(row);
            const int width = int16_t(memory.load32(at + 8) & 0xffff), height = int16_t(memory.load32(at + 8) >> 16);
            if (width > 0 && height > 0 && out.size() < max_stops) {
                const float x = float(int32_t(memory.load32(at))), y = float(int32_t(memory.load32(at + 4)));
                out.push_back(client(x + float(width) * 0.5f, y + float(height) * 0.5f));
            }
        }

        for (int button = 0; button < 4; ++button) {
            collect(memory, control + savegame_buttons + button_size * uint32_t(button), out, depth + 1);
        }
    } else if (kind.kind == Kind::character) {
        for (int slot = 0; slot < 8; ++slot) {
            const uint32_t hero = peek(memory, control + character_slots + 4 * uint32_t(slot));
            if (visible(memory, hero) && peek(memory, hero + character_hero) &&
                !peek8(memory, character_slot_locked + uint32_t(slot))) {
                add(memory, hero, out);
            }
        }

        for (const uint32_t offset : character_members) {
            if (const uint32_t member = peek(memory, control + offset)) {
                collect(memory, member, out, depth + 1);
            }
        }
    }
}

namespace {
// The topmost, then leftmost stop.
UiPoint first_stop(const std::vector<UiPoint>& stops) {
    UiPoint best = stops.front();
    for (const UiPoint& p : stops) {
        if (p.y < best.y - 4 || (std::fabs(p.y - best.y) <= 4 && p.x < best.x)) {
            best = p;
        }
    }

    return best;
}

// The bottommost, then leftmost stop: a message box's OK before its Cancel.
UiPoint default_button(const std::vector<UiPoint>& stops) {
    UiPoint best = stops.front();
    for (const UiPoint& p : stops) {
        if (p.y > best.y + 4 || (std::fabs(p.y - best.y) <= 4 && p.x < best.x)) {
            best = p;
        }
    }

    return best;
}
}

void GamepadUi::portals(Memory& memory, uint32_t box, const std::function<int(uint32_t font)>& line_height,
                        std::vector<UiPoint>& out) {
    const uint32_t mode = peek(memory, box + message_box_mode);
    if ((mode != portal_mode_surface && mode != portal_mode_underworld) || !line_height ||
        !readable(memory, box + message_box_text, 12)) {
        return;
    }

    const int height = line_height(portal_font);
    int32_t left = int32_t(memory.load32(box + message_box_text)),
            top = int32_t(memory.load32(box + message_box_text + 4));
    const int width = int16_t(memory.load32(box + message_box_text + 8) & 0xffff);
    if (height <= 0 || height > 200 || width <= 0) {
        return;
    }

    // The text rect is the box's own: placed relative to the box when it does not lie inside it.
    const int32_t box_x = int32_t(memory.load32(box + control_x)), box_y = int32_t(memory.load32(box + control_y));
    const int box_width = int16_t(memory.load32(box + control_size) & 0xffff);
    const int box_height = int16_t(memory.load32(box + control_size) >> 16);
    if (left < box_x || top < box_y || left >= box_x + box_width || top >= box_y + box_height) {
        left += box_x;
        top += box_y;
    }

    const bool underworld = mode == portal_mode_underworld;
    const uint32_t open = peek(memory, box + message_box_portals);
    for (uint32_t row = 0; row < portal_count; ++row) {
        const uint32_t portal = underworld ? portal_count + row : row;
        if (!(open & (1u << portal)) || (underworld && row >= 7 && row <= 12)) {
            continue; // dark, or a row the underworld list leaves empty
        }

        const float y = float(top - 4) + float(row + 1) * float(height + 4) + float(height) * 0.5f;
        out.push_back(client(float(left) + float(width) * 0.5f, y));
    }

    if (trace_) {
        trace_("portal box mode=" + std::to_string(mode) + " portals=" + std::to_string(open) +
               " text=" + std::to_string(left) + "," + std::to_string(top) + "+" + std::to_string(width) +
               " line=" + std::to_string(height) + " box=" + std::to_string(box_x) + "," + std::to_string(box_y));
    }
}

void GamepadUi::savegame_list(Memory& memory, uint32_t window, UiView& view) {
    if (!visible(memory, window) || info(memory, window).kind != Kind::savegame ||
        !readable(memory, window + savegame_rows, 0x40)) {
        return;
    }

    uint32_t mode = memory.load32(window + savegame_mode);
    if (mode > 2) {
        mode = memory.load32(window + savegame_asked_mode); // a message box asks about it
    }

    if (mode != 1 && mode != 2) {
        return;
    }

    // The first listed savegame, as the window works it out (0x71c2b0): the slider's value, at most count - 3 when
    // loading, count - 2 when saving (a row past the end is the new savegame).
    const uint32_t slider = window + savegame_slider;
    const uint32_t count = peek(memory, slider + slider_count);
    float position = 0, rounding = 0;
    memory.read(slider + slider_position, &position, 4);
    if (readable(memory, slider_rounding, 4)) {
        memory.read(slider_rounding, &rounding, 4);
    }

    uint32_t value = uint32_t(std::max(0.0f, float(count) * position + rounding));
    value = count ? std::min(value, count - 1) : 0;
    const uint32_t last = count > (mode == 1 ? 2u : 3u) ? count - (mode == 1 ? 2u : 3u) : 0;
    const uint32_t first = std::min(value, last);
    const size_t listed = items(memory, window + savegame_entries, savegame_entry_size, 4096).size();
    auto row = [&](int index, UiPoint& out, float& half) {
        const uint32_t at = window + savegame_rows + 12 * uint32_t(index);
        const int width = int16_t(memory.load32(at + 8) & 0xffff), height = int16_t(memory.load32(at + 8) >> 16);
        out = client(float(int32_t(memory.load32(at))) + float(width) * 0.5f,
                     float(int32_t(memory.load32(at + 4))) + float(height) * 0.5f);
        half = float(height) * 0.5f;
        return width > 0 && height > 0;
    };

    float half = 0;
    if (!row(1, view.list.first, half) || !row(3, view.list.last, half)) {
        return;
    }

    view.list.active = true;
    view.list.half_row = half;
    view.list.more_above = first > 0;
    view.list.more_below = first < last && first + 3 < listed + (mode == 1 ? 1 : 0);
}

UiView GamepadUi::read(Memory& memory, const std::function<int(uint32_t font)>& line_height) {
    UiView view;
    const uint32_t ui = peek(memory, ui_manager_cell);
    if (!ui) {
        return view;
    }

    const uint32_t flags = peek(memory, ui + ui_flags);
    if (flags & ui_cinematic) {
        return view;
    }

    // A message box goes first, alone, in the menus and in game.
    if (const uint32_t box = peek(memory, ui + ui_dialog); visible(memory, box)) {
        view.scope = UiView::Scope::message_box;
        view.key = box * 31 + peek(memory, box + message_box_mode);
        // The portal box lists the portals as text rows: they come first, the cursor starts on the first.
        portals(memory, box, line_height, view.stops);
        const size_t rows = view.stops.size();
        collect(memory, box, view.stops, 0);
        if (rows) {
            view.has_home = true;
            view.home = view.stops.front();
        } else if (!view.stops.empty()) {
            view.has_home = true;
            view.home = default_button(view.stops);
        }

        return view;
    }

    if (flags & ui_in_menus) {
        if (!(flags & ui_menus_created)) {
            return view;
        }

        view.scope = UiView::Scope::menus;
        for (int index = 0; index < menu_count; ++index) {
            const uint32_t menu = peek(memory, ui + ui_menus + 4 * uint32_t(index));
            if (visible(memory, menu)) {
                view.key = view.key * 31 + menu;
                if (info(memory, menu).kind == Kind::main_menu) {
                    view.key = view.key * 31 + peek(memory, menu + main_menu_screen);
                }

                collect(memory, menu, view.stops, 0);
            }
        }

        if (!view.stops.empty()) {
            view.has_home = true;
            view.home = first_stop(view.stops);
        }

        savegame_list(memory, peek(memory, ui + ui_menus + 4 * savegame_menu), view);
        return view;
    }

    if (!(flags & ui_in_game) || !(flags & ui_windows_created)) {
        return view;
    }

    auto window = [&](int index) {
        return peek(memory, ui + ui_windows + 4 * uint32_t(index));
    };
    if (const uint32_t menu = window(window_game_menu); visible(memory, menu)) {
        view.scope = UiView::Scope::game_menu;
        view.key = menu;
        collect(memory, menu, view.stops, 0);
        if (!view.stops.empty()) {
            view.has_home = true;
            view.home = first_stop(view.stops);
        }

        return view;
    }

    // An NPC dialog: a tooltip popup with answers (text ids), placed relative to the popup.
    for (const uint32_t at : items(memory, ui + ui_popups, 4, 256)) {
        const uint32_t popup = peek(memory, at);
        if (!visible(memory, popup) || info(memory, popup).kind != Kind::tooltip) {
            continue;
        }

        UiRect frame;
        if (!rect(memory, popup, frame) || !readable(memory, popup + popup_answers, 4 * answer_size)) {
            continue;
        }

        // The popup's own position in layout pixels (rect() maps through the inset).
        int32_t px = int32_t(memory.load32(popup + control_x)), py = int32_t(memory.load32(popup + control_y));
        for (uint32_t parent = memory.load32(popup + control_parent), depth = 0; parent && depth < 8; ++depth) {
            px += int32_t(peek(memory, parent + control_x));
            py += int32_t(peek(memory, parent + control_y));
            parent = peek(memory, parent + control_parent);
        }

        std::vector<UiPoint> answers;
        uint64_t key = popup;
        for (uint32_t index = 0; index < 4; ++index) {
            const uint32_t answer = popup + popup_answers + answer_size * index;
            const uint32_t text = memory.load32(answer);
            const int width = int16_t(memory.load32(answer + 0x10) & 0xffff);
            const int height = int16_t(memory.load32(answer + 0x10) >> 16);
            if (!text || width <= 0 || height <= 0) {
                continue;
            }

            key = key * 31 + text;
            const float x = float(px + int32_t(memory.load32(answer + 8))) + float(width) * 0.5f;
            const float y = float(py + int32_t(memory.load32(answer + 12))) + float(height) * 0.5f;
            answers.push_back(client(x, y));
        }

        if (!answers.empty()) {
            view.scope = UiView::Scope::npc_dialog;
            view.key = key;
            view.stops = answers;
            view.has_home = true;
            view.home = answers.front();
            return view;
        }
    }

    // The open windows; a full-screen one is drawn alone.
    std::vector<int> shown;
    for (const int index : exclusive_windows) {
        if (visible(memory, window(index))) {
            shown.push_back(index);
            break;
        }
    }

    if (shown.empty()) {
        for (int index = 0; index < window_count; ++index) {
            if (!hud_window(index) && visible(memory, window(index))) {
                shown.push_back(index);
            }
        }
    }

    if (shown.empty()) {
        return view;
    }

    view.scope = UiView::Scope::windows;
    for (const int index : shown) {
        const uint32_t address = window(index);
        UiRect frame;
        if (rect(memory, address, frame)) {
            view.windows.push_back({address, frame});
        }

        collect(memory, address, view.stops, 0);
        savegame_list(memory, address, view);
    }

    return view;
}

bool GamepadUi::inventory_tab(Memory& memory, int step, UiPoint& out) {
    const uint32_t ui = peek(memory, ui_manager_cell);
    if (!ui || !(peek(memory, ui + ui_flags) & ui_windows_created)) {
        return false;
    }

    const uint32_t inventory = peek(memory, ui + ui_windows + 4 * window_inventory);
    const uint32_t tabs = visible(memory, inventory) ? peek(memory, inventory + inventory_tabs) : 0;
    const uint32_t control = tabs ? peek(memory, tabs + tabs_control) : 0;
    if (!control || info(memory, control).kind != Kind::tab_control) {
        return false;
    }

    const std::vector<uint32_t> pages = items(memory, control + tab_pages, page_size, 16);
    const int active = int(peek(memory, control + tab_active) & 0xffff);
    for (int index = active + step; index >= 0 && index < int(pages.size()); index += step) {
        const uint32_t button = peek(memory, pages[size_t(index)] + page_button);
        UiRect r;
        if (visible(memory, button) && rect(memory, button, r)) {
            out = r.centre();
            out.y += 2; // the tab control hit-tests 4 pixels below the tabs' rects
            return true;
        }
    }

    return false;
}

bool GamepadUi::next(const std::vector<UiPoint>& stops, float x, float y, float dx, float dy, UiPoint& out) {
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-3f) {
        return false;
    }

    dx /= length;
    dy /= length;
    // Within 45 degrees of the direction first (down from a hero on the character screen goes to the buttons, not to
    // the next hero diagonally below), then within 60; straight ahead first.
    for (const float cone : {1.0f, 1.8f}) {
        bool found = false;
        float best = 0;
        for (const UiPoint& p : stops) {
            const float vx = p.x - x, vy = p.y - y;
            const float along = vx * dx + vy * dy, side = std::fabs(vx * dy - vy * dx);
            if (along < 6.0f || side > along * cone) {
                continue;
            }

            const float score = along + 2.5f * side;
            if (!found || score < best) {
                best = score;
                out = p;
                found = true;
            }
        }

        if (found) {
            return true;
        }
    }

    return false;
}
}
