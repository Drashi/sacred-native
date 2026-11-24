#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sacred {
// A game controller as the window host reads it (SDL's game controller mapping: Xbox, PlayStation, Switch and most
// others). Buttons are named by their place on an Xbox pad; of several pads, the one that last gave input counts.
namespace pad {
enum Button : uint32_t {
    A = 1u << 0,
    B = 1u << 1,
    X = 1u << 2,
    Y = 1u << 3,
    LB = 1u << 4,
    RB = 1u << 5,
    LT = 1u << 6, // the triggers count as buttons past a third of their travel
    RT = 1u << 7,
    Back = 1u << 8,
    Start = 1u << 9,
    L3 = 1u << 10,
    R3 = 1u << 11,
    Up = 1u << 12, // D-pad
    Down = 1u << 13,
    Left = 1u << 14,
    Right = 1u << 15,
};
constexpr int button_count = 16;
constexpr float trigger_press = 1.0f / 3.0f;
}

struct GamepadState {
    bool connected = false;
    uint32_t buttons = 0; // pad::Button bits
    // Raw stick positions in -1..1, x to the right and y down (screen directions); triggers 0..1.
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
};

// A stick with a radial deadzone: the length below `deadzone` reads 0 and the rest is rescaled to 0..1, the
// direction kept.
struct Stick {
    float x = 0, y = 0, length = 0;
};

inline Stick dead_zone(float x, float y, float deadzone) {
    const float length = std::sqrt(x * x + y * y);
    if (length <= deadzone || length <= 0.0f) {
        return {};
    }

    const float scaled = std::min(1.0f, (length - deadzone) / std::max(1e-3f, 1.0f - deadzone));
    return {x / length * scaled, y / length * scaled, scaled};
}
}
