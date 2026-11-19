#include "softfloat_bridge.h"

X87Value x87_integer(int64_t value) {
    return guest_value(i64_to_extF80(value));
}

X87Value x87_from_double(double value) {
    uint64_t bits;
    memcpy(&bits, &value, 8);
    softfloat_status_t status{};
    status.softfloat_exceptionMasks = 63;
    return guest_value(f64_to_extF80(bits, &status));
}

double x87_to_double(X87Value value) {
    softfloat_status_t status{};
    status.softfloat_exceptionMasks = 63;
    uint64_t bits = extF80_to_f64(soft_value(value), &status);
    double result;
    memcpy(&result, &bits, 8);
    return result;
}

X87Value x87_load_soft(X86* c, uint32_t address, unsigned width) {
    auto status = x87_begin(c);
    extFloat80_t value{};
    switch (width) {
    case 32:
        value = f32_to_extF80(rd32(address), &status);
        break;
    case 64:
        value = f64_to_extF80(rd64(address), &status);
        break;
    case 80:
        return {rd64(address), rd16(address + 8)};
    default:
        __builtin_trap();
    }

    x87_finish(c, status);
    return guest_value(value);
}

void x87_store_soft(X86* c, uint32_t address, unsigned width) {
    auto status = x87_begin(c);
    auto value = soft_value(x87_get(c, 0));
    switch (width) {
    case 32:
        wr32(address, extF80_to_f32(value, &status));
        break;
    case 64:
        wr64(address, extF80_to_f64(value, &status));
        break;
    case 80:
        wr64(address, value.signif);
        wr16(address + 8, value.signExp);
        c->fpu_sw &= (uint16_t)~0x200u;
        return;
    default:
        __builtin_trap();
    }

    x87_finish(c, status);
}

void x87_store_bcd(X86* c, uint32_t address) {
    auto status = x87_begin(c);
    auto value = soft_value(x87_get(c, 0));
    bool negative = value.signExp >> 15;
    auto rounded = extF80_roundToInt(value, &status);
    rounded.signExp &= 0x7fff;
    auto limit = i64_to_extF80(INT64_C(1000000000000000000));
    int relation = extF80_compare(rounded, limit, 0, &status);
    uint64_t digits = 0;
    bool invalid = relation == softfloat_relation_unordered || relation >= 0 ||
                   (status.softfloat_exceptionFlags & softfloat_flag_invalid);
    if (!invalid) {
        digits = (uint64_t)extF80_to_i64(rounded, softfloat_round_minMag, false, &status);
    }

    for (unsigned i = 0; i < 9; i++) {
        unsigned lo = digits % 10;
        digits /= 10;
        unsigned hi = digits % 10;
        digits /= 10;
        wr8(address + i, (uint8_t)(lo | (hi << 4)));
    }

    wr8(address + 9, negative ? 0x80 : 0);
    if (invalid) {
        wr64(address, UINT64_C(0xc000000000000000));
        wr16(address + 8, 0xffff);
        status.softfloat_exceptionFlags = softfloat_flag_invalid;
    }

    x87_finish(c, status);
}

int64_t x87_to_integer(X86* c, X87Value value, unsigned width, int truncate) {
    auto status = x87_begin(c);
    unsigned rounding = truncate ? (unsigned)softfloat_round_minMag : status.softfloat_roundingMode;
    int64_t result = extF80_to_i64(soft_value(value), rounding, true, &status);
    if (width == 16 && (result < INT16_MIN || result > INT16_MAX)) {
        status.softfloat_exceptionFlags = softfloat_flag_invalid;
        result = INT16_MIN;
    }

    if (width == 32 && (result < INT32_MIN || result > INT32_MAX)) {
        status.softfloat_exceptionFlags = softfloat_flag_invalid;
        result = INT32_MIN;
    }

    /* Invalid conversion suppresses inexact, including narrower overflow. */
    if (status.softfloat_exceptionFlags & softfloat_flag_invalid) {
        status.softfloat_exceptionFlags = softfloat_flag_invalid;
    }

    x87_finish(c, status);
    return result;
}

X87Value x87_binary_soft(X86* c, X87Value a, X87Value b, enum X87Operation op) {
    auto status = x87_begin(c);
    auto left = soft_value(a), right = soft_value(b);
    extFloat80_t result{};
    switch (op) {
    case X87_ADD:
        result = extF80_add(left, right, &status);
        break;
    case X87_SUB:
        result = extF80_sub(left, right, &status);
        break;
    case X87_MUL:
        result = extF80_mul(left, right, &status);
        break;
    case X87_DIV:
        result = extF80_div(left, right, &status);
        break;
    case X87_SCALE:
        result = extF80_scale(left, right, &status);
        break;
    default:
        __builtin_trap();
    }

    x87_finish(c, status);
    return guest_value(result);
}

void x87_compare(X86* c, X87Value a, X87Value b, int quiet, int eflags) {
    auto status = x87_begin(c);
    int relation = extF80_compare(soft_value(a), soft_value(b), quiet, &status);
    unsigned zf = relation == softfloat_relation_equal || relation == softfloat_relation_unordered;
    unsigned cf = relation == softfloat_relation_less || relation == softfloat_relation_unordered;
    unsigned pf = relation == softfloat_relation_unordered;
    if (eflags) {
        c->eflags_of = c->eflags_sf = c->eflags_af = 0;
        c->eflags_zf = zf;
        c->eflags_cf = cf;
        c->eflags_pf = pf;
    } else {
        c->fpu_sw = (uint16_t)((c->fpu_sw & ~0x4700u) | (zf << 14) | (pf << 10) | (cf << 8));
    }

    x87_finish(c, status);
}
