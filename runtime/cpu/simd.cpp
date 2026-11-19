#include "softfloat_bridge.h"

uint32_t simd_float(X86* c, uint32_t a, uint32_t b, enum SimdOperation op, int now) {
    softfloat_status_t status{};
    status.softfloat_exceptionMasks = now ? 63 : (c->mxcsr >> 7) & 63;
    status.softfloat_roundingMode = now ? (unsigned)softfloat_round_near_even : (c->mxcsr >> 13) & 3;
    status.softfloat_denormals_are_zeros = !now && (c->mxcsr & 0x40);
    status.softfloat_flush_underflow_to_zero = !now && (c->mxcsr & 0x8000);
    uint32_t result = 0;
    int relation;
    switch (op) {
    case SIMD_ADD:
        result = f32_add(a, b, &status);
        break;
    case SIMD_SUB:
        result = f32_sub(a, b, &status);
        break;
    case SIMD_MUL:
        result = f32_mul(a, b, &status);
        break;
    case SIMD_MIN:
        result = now ? (f32_compare(b, a, 0, &status) < 0 ? b : a) : f32_min(a, b, &status);
        break;
    case SIMD_MAX:
        result = now ? (f32_compare(a, b, 0, &status) < 0 ? b : a) : f32_max(a, b, &status);
        break;
    case SIMD_CMP_EQ:
    case SIMD_CMP_GT:
    case SIMD_CMP_GE:
    case SIMD_CMP_NE:
    case SIMD_COMIS:
        relation = f32_compare(a, b, op == SIMD_CMP_NE, &status);
        if (op == SIMD_COMIS) {
            c->eflags_of = c->eflags_af = c->eflags_sf = 0;
            c->eflags_zf = relation == 0 || relation == 2;
            c->eflags_pf = relation == 2;
            c->eflags_cf = relation < 0 || relation == 2;
        } else {
            bool match = op == SIMD_CMP_EQ   ? relation == 0
                         : op == SIMD_CMP_GT ? relation == 1
                         : op == SIMD_CMP_GE ? relation == 0 || relation == 1
                                             : relation != 0;
            result = match ? UINT32_MAX : 0;
        }

        break;
    case SIMD_FROM_INT:
        result = i32_to_f32((int32_t)b, &status);
        break;
    case SIMD_TO_INT:
        result = (uint32_t)f32_to_i32(b, softfloat_round_minMag, true, &status);
        if (now && (status.softfloat_exceptionFlags & softfloat_flag_invalid)) {
            bool nan = (b & 0x7fffffffu) > 0x7f800000u;
            result = (b >> 31) && !nan ? 0x80000000u : 0x7fffffffu;
        }

        break;
    case SIMD_RCP:
    case SIMD_RSQRT: {
        /* Operation-specific QEMU policy: software binary32 divide/sqrt,
         * guest MXCSR RC/FTZ/DAZ (fixed 3DNow mode), no new exceptions. */
        uint32_t divisor = op == SIMD_RSQRT ? f32_sqrt(b, &status) : b;
        result = f32_div(0x3f800000, divisor, &status);
        status.softfloat_exceptionFlags = 0;
        break;
    }
    default:
        __builtin_trap();
    }

    if (!now) {
        c->mxcsr |= softfloat_getExceptionFlags(&status) & 63;
        if (softfloat_getExceptionFlags(&status) & ~status.softfloat_exceptionMasks & 63) {
            recomp_fpu_error(c);
        }
    }

    return result;
}
