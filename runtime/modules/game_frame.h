#pragma once
#include "../process/control.h"
#include <cstdint>

namespace sacred {
// Sacred.exe 2.28: the return address after cEngine_renderThread_run's (0x60e350) CALL dxDriver7_flip (0x645e40).
constexpr uint32_t main_loop_flip_return = 0x0060ea8f;

// Whether the present being observed runs inside dxDriver7_flip called from the in-game main loop, after that frame's
// game logic (fades and menus flip from elsewhere): only there do the in-game helpers (auto_pickup.h, gamepad.h) call
// into the game. The return address lies a fixed, small distance above the import's arguments (flip's frame is 0x8c
// bytes).
inline bool main_loop_present(Control& control) {
    const uint32_t esp = control.cpu.r[R_ESP];
    for (uint32_t offset = 0; offset < 0x100; offset += 4) {
        if (control.memory.load32(esp + offset) == main_loop_flip_return) {
            return true;
        }
    }

    return false;
}
}
