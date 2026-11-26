#pragma once

namespace sacred {
// The part of a display a fullscreen window can cover, as insets from its bounds in points. On a MacBook with a
// camera notch a fullscreen window starts below the menu bar strip (1512x982 becomes 1512x949); everywhere else, and
// on other systems, the insets are 0.
struct DisplayInsets {
    int top = 0, left = 0, bottom = 0, right = 0;
};

// The display with these bounds in SDL's global coordinates (top-left origin, points). Call on the SDL thread.
DisplayInsets display_safe_insets(int x, int y, int width, int height);
} // namespace sacred
