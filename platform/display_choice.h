#pragma once
#include <cstdlib>
#include <string>
#include <vector>

namespace sacred {
// The host display the game runs on: chosen once at start-up and kept for the whole run, because the guest reads
// the desktop size once (GetSystemMetrics, the primary surface, the HD resolution) and cannot follow a change.
struct DisplayRect {
    int x = 0, y = 0, width = 0, height = 0;
};

struct DisplayChoice {
    int index = 0;      // 0-based, as SDL numbers displays
    std::string source; // "setting", "cursor" or "first"
};

// requested: 1-based display number from DISPLAY in Settings.cfg or SACRED_DISPLAY (0 or out of range: automatic).
// Automatic picks the display under the pointer (where the player just started the game), else the first one.
inline DisplayChoice choose_display(const std::vector<DisplayRect>& displays, int requested, bool pointer_known,
                                    int pointer_x, int pointer_y) {
    DisplayChoice choice{0, "first"};
    if (displays.empty()) {
        return choice;
    }

    if (requested >= 1 && size_t(requested) <= displays.size()) {
        return {requested - 1, "setting"};
    }

    if (pointer_known) {
        for (size_t i = 0; i < displays.size(); ++i) {
            const DisplayRect& d = displays[i];
            if (pointer_x >= d.x && pointer_x < d.x + d.width && pointer_y >= d.y && pointer_y < d.y + d.height) {
                return {int(i), "cursor"};
            }
        }
    }
    return choice;
}

// A display number as written by the player: "2" -> 2; anything else (empty, "auto", "0", garbage) -> 0 (automatic).
inline int parse_display_number(const std::string& text) {
    if (text.empty() || text.size() > 2) {
        return 0;
    }

    for (char c : text) {
        if (c < '0' || c > '9') {
            return 0;
        }
    }
    return std::atoi(text.c_str());
}
} // namespace sacred
