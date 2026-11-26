#pragma once
#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
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

// Where a fullscreen window's client area starts in SDL's global coordinates. SDL_GetWindowPosition reports the
// display's origin for every fullscreen window, but on a display with a camera notch (macOS) the window covers only
// the part below the menu bar strip: it is as wide as the display and ends at its bottom edge, so it starts lower by
// the difference in height. A window as large as the display starts at the display's origin.
inline std::pair<int, int> fullscreen_window_origin(const DisplayRect& display, int width, int height) {
    return {display.x + std::max(0, display.width - width) / 2, display.y + std::max(0, display.height - height)};
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
