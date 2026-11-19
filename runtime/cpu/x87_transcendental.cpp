#include "softfloat_bridge.h"
#include "fpu/fpu_trans.h"

void x87_unary(X86* c, enum X87Operation op) {
    auto status = x87_begin(c);
    auto a = soft_value(x87_get(c, 0));
    extFloat80_t result = a, second{};
    bool pop = false, push = false, transcendental = false;
    int range = 0, remainder = -2;
    uint64_t quotient = 0;
    unsigned precision = status.extF80_roundingPrecision;
    switch (op) {
    case X87_SQRT:
        result = extF80_sqrt(a, &status);
        break;
    case X87_ROUND:
        result = extF80_roundToInt(a, &status);
        break;
    case X87_PREM:
    case X87_PREM1: {
        auto b = soft_value(x87_get(c, 1));
        remainder = op == X87_PREM ? floatx80_remainder(a, b, result, quotient, &status)
                                   : floatx80_ieee754_remainder(a, b, result, quotient, &status);
        break;
    }
    default:
        transcendental = true;
        status.extF80_roundingPrecision = 80;
        switch (op) {
        case X87_SIN:
            range = fsin(result, status);
            break;
        case X87_COS:
            range = fcos(result, status);
            break;
        case X87_SINCOS:
            range = fsincos(a, &result, &second, status);
            push = true;
            break;
        case X87_TAN:
            range = fptan(result, status);
            second = i32_to_extF80(1);
            push = true;
            break;
        case X87_ATAN:
            result = fpatan(a, soft_value(x87_get(c, 1)), status);
            pop = true;
            break;
        case X87_2XM1:
            result = f2xm1(a, status);
            break;
        case X87_YL2X:
            result = fyl2x(a, soft_value(x87_get(c, 1)), status);
            pop = true;
            break;
        case X87_YL2XP1:
            result = fyl2xp1(a, soft_value(x87_get(c, 1)), status);
            pop = true;
            break;
        default:
            __builtin_trap();
        }
    }

    if (range < 0) {
        c->fpu_sw |= 0x400;
        x87_finish(c, status);
        return;
    }

    if (transcendental) {
        c->fpu_sw &= (uint16_t)~0x400u;
        /* Binding project policy: final PC quantization, extended exponent kept. */
        auto rounding = x87_begin(c);
        rounding.extF80_roundingPrecision = precision;
        if ((result.signExp & 0x7fff) != 0x7fff && result.signif) {
            result = extF80_add(result, {0, (uint16_t)(result.signExp & 0x8000)}, &rounding);
        }

        if (push && (second.signExp & 0x7fff) != 0x7fff && second.signif) {
            second = extF80_add(second, {0, (uint16_t)(second.signExp & 0x8000)}, &rounding);
        }

        if (rounding.softfloat_exceptionFlags & softfloat_flag_inexact) {
            status.softfloat_exceptionFlags &= ~RAISE_SW_C1;
        }

        status.softfloat_exceptionFlags |= rounding.softfloat_exceptionFlags;
    }

    x87_set(c, pop ? 1 : 0, guest_value(result));
    if (pop) {
        fdrop(c);
    }

    if (push) {
        x87_push(c, guest_value(second));
    }

    x87_finish(c, status);
    if (remainder != -2) {
        c->fpu_sw &= (uint16_t)~0x4700u;
        if (remainder > 0) {
            c->fpu_sw |= 0x400;
        } else if (!remainder) {
            c->fpu_sw |= (uint16_t)(((quotient & 4) << 6) | ((quotient & 2) << 13) | ((quotient & 1) << 9));
        }
    }
}
