#pragma once
#include <cstdint>

namespace sacred {
// US keyboard layout (KLID 00000409) translation of one key transition, as ToAscii/ToUnicode define it.
// state is the 256-byte GetKeyboardState array. Returns the number of characters produced (0 or 1; the US
// layout has no dead keys) and stores the character in out.
inline int us_layout_translate(uint32_t vk, uint32_t scan, const uint8_t state[256], uint16_t& out) {
    if (scan & 0x8000) {
        return 0;
    }

    auto down = [&](unsigned a, unsigned b, unsigned c) {
        return (state[a] | state[b] | state[c]) & 0x80;
    };
    const bool shift = down(0x10, 0xa0, 0xa1), ctrl = down(0x11, 0xa2, 0xa3), alt = down(0x12, 0xa4, 0xa5);
    const bool caps = state[0x14] & 1, numlock = state[0x90] & 1;
    if (alt && !ctrl) {
        return 0;
    }

    auto emit = [&](uint16_t value) {
        out = value;
        return 1;
    };
    if (vk >= 'A' && vk <= 'Z') {
        if (ctrl) {
            return emit(uint16_t(vk - 'A' + 1));
        }

        return emit(uint16_t(shift != caps ? vk : vk + 32));
    }

    if (vk >= '0' && vk <= '9') {
        if (ctrl && shift && vk == '6') {
            return emit(0x1e);
        }

        return emit(uint16_t(shift ? ")!@#$%^&*("[vk - '0'] : vk));
    }

    switch (vk) {
    case 8:
        return emit(ctrl ? 0x7f : 0x08);
    case 9:
        return emit('\t');
    case 13:
        return emit(ctrl ? '\n' : '\r');
    case 27:
        return emit(0x1b);
    case 32:
        return emit(' ');
    case 106:
        return emit('*');
    case 107:
        return emit('+');
    case 109:
        return emit('-');
    case 111:
        return emit('/');
    case 110:
        return numlock && !shift ? emit('.') : 0;
    case 186:
        return emit(shift ? ':' : ';');
    case 187:
        return emit(shift ? '+' : '=');
    case 188:
        return emit(shift ? '<' : ',');
    case 189:
        return emit(ctrl && shift ? 0x1f : shift ? '_' : '-');
    case 190:
        return emit(shift ? '>' : '.');
    case 191:
        return emit(shift ? '?' : '/');
    case 192:
        return emit(shift ? '~' : '`');
    case 219:
        return emit(ctrl ? 0x1b : shift ? '{' : '[');
    case 220:
        return emit(ctrl ? 0x1c : shift ? '|' : '\\');
    case 221:
        return emit(ctrl ? 0x1d : shift ? '}' : ']');
    case 222:
        return emit(shift ? '"' : '\'');
    default:
        break;
    }

    if (vk >= 96 && vk <= 105) {
        return numlock && !shift ? emit(uint16_t('0' + vk - 96)) : 0;
    }

    return 0;
}
}
