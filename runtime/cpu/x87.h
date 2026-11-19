/* Included after the pinned X86 state and memory accessors are defined. */
#ifndef SACRED_X87_H
#define SACRED_X87_H
#include <float.h>
#define FTAG_VALID 0u
#define FTAG_ZERO 1u
#define FTAG_SPECIAL 2u
#define FTAG_EMPTY 3u
#define X87_ST(c, i) ((c)->st[((c)->fpu_top + (unsigned)(i)) & 7u])
RECOMP_HOT_INLINE unsigned ftag_of(const X86* c, unsigned slot) {
    return (c->fpu_tag >> (2u * (slot & 7u))) & 3u;
}

RECOMP_HOT_INLINE void ftag_put(X86* c, unsigned slot, unsigned tag) {
    unsigned shift = 2u * (slot & 7u);
    c->fpu_tag = (uint16_t)((c->fpu_tag & ~(3u << shift)) | (tag << shift));
}

RECOMP_HOT_INLINE unsigned fempty(const X86* c, unsigned i) {
    return ftag_of(c, c->fpu_top + i) == FTAG_EMPTY;
}

RECOMP_HOT_INLINE unsigned x87_tag(X87Value v) {
    unsigned exp = v.signExp & 0x7fffu;
    if (exp == 0 && v.signif == 0) {
        return FTAG_ZERO;
    }

    return exp == 0 || exp == 0x7fff || !(v.signif >> 63) ? FTAG_SPECIAL : FTAG_VALID;
}

static inline uint16_t fstsw(const X86* c) {
    return (uint16_t)((c->fpu_sw & ~0x3800u) | (c->fpu_top << 11));
}

RECOMP_HOT_INLINE X87Value x87_indefinite(void) {
    X87Value value = {UINT64_C(0xc000000000000000), 0xffff};
    return value;
}

RECOMP_HOT_INLINE void x87_pending(X86* c) {
    c->fpu_pending = c->fpu_sw & ~c->fpu_cw & 0x3f;
    if (c->fpu_pending) {
        c->fpu_sw |= 0x8080;
    }
}

RECOMP_COLD void recomp_fpu_error(X86* c);
RECOMP_HOT_INLINE void x87_wait(X86* c) {
    if (c->fpu_pending) {
        recomp_fpu_error(c);
    }
}

RECOMP_HOT_INLINE X87Value x87_get(X86* c, unsigned i) {
    if (!fempty(c, i)) {
        return X87_ST(c, i);
    }

    c->fpu_sw = (uint16_t)((c->fpu_sw & ~0x200u) | 0x41u);
    x87_pending(c);
    return x87_indefinite();
}

RECOMP_HOT_INLINE void x87_set(X86* c, unsigned i, X87Value value) {
    unsigned slot = (c->fpu_top + i) & 7u;
    c->st[slot] = value;
    ftag_put(c, slot, x87_tag(value));
}

RECOMP_HOT_INLINE void x87_push(X86* c, X87Value value) {
    c->fpu_top = (c->fpu_top - 1u) & 7u;
    c->fpu_sw &= (uint16_t)~0x200u;
    if (ftag_of(c, c->fpu_top) != FTAG_EMPTY) {
        c->fpu_sw |= 0x241;
        x87_pending(c);
        value = x87_indefinite();
    }

    x87_set(c, 0, value);
}

RECOMP_HOT_INLINE void fdrop(X86* c) {
    (void)x87_get(c, 0);
    ftag_put(c, c->fpu_top, FTAG_EMPTY);
    c->fpu_top = (c->fpu_top + 1u) & 7u;
}

RECOMP_HOT_INLINE void fxch(X86* c, unsigned i) {
    X87Value a = x87_get(c, 0), b = x87_get(c, i);
    x87_set(c, 0, b);
    x87_set(c, i, a);
}

static inline void x87_set_cw(X86* c, uint16_t cw) {
    c->fpu_cw = cw;
    x87_pending(c);
}

static inline void x87_finit(X86* c) {
    c->fpu_cw = 0x37f;
    c->fpu_sw = c->fpu_top = c->fpu_pending = 0;
    c->fpu_tag = 0xffff;
    c->fpu_ip = c->fpu_dp = c->fpu_opcode = 0;
    c->fpu_cs = c->fpu_ds = 0;
}

RECOMP_HOT_INLINE uint64_t mmx_get(const X86* c, unsigned i) {
    return c->st[i].signif;
}

RECOMP_HOT_INLINE void mmx_enter(X86* c) {
    c->fpu_top = 0;
    c->fpu_tag = 0;
}

RECOMP_HOT_INLINE void mmx_set(X86* c, unsigned i, uint64_t bits) {
    c->st[i].signif = bits;
    c->st[i].signExp = 0xffff;
    mmx_enter(c);
}

static inline void x87_emms(X86* c) {
    c->fpu_tag = 0xffff;
}

/* SoftFloat implementations: the reference for every case, and the only path outside the fast cases below. */
X87Value x87_load_soft(X86* c, uint32_t address, unsigned width);
void x87_store_soft(X86* c, uint32_t address, unsigned width);
X87Value x87_binary_soft(X86* c, X87Value a, X87Value b, enum X87Operation op);
void x87_store_bcd(X86* c, uint32_t address);
X87Value x87_integer(int64_t value);
int64_t x87_to_integer(X86* c, X87Value value, unsigned width, int truncate);

/* ------------------------------------------------------------ fast paths
 * Exact shortcuts for round to nearest with precision control 53 bits (Sacred's in-game control word 0x27f) or 24 bits
 * (Direct3D's default), with operands that are normal IEEE doubles or singles respectively. Every result, status bit
 * and pending-exception decision equals the SoftFloat path (tests/cpu/x87_fast_tests.c compares them):
 *   - loads of normal or zero singles/doubles and stores of values exactly representable in the target format are
 *     exact conversions: no flags, C1 cleared, as SoftFloat reports them;
 *   - ADD/SUB/MUL/DIV of two 24-bit-significand operands, computed in IEEE double and rounded once to single, are
 *     correctly rounded to 24 bits (double rounding is innocuous for +,-,*,/ when q >= 2p+2: 53 >= 50); while the
 *     result is a normal single the x87 extended exponent range changes nothing. The products/sums used for the
 *     inexact (PE) and round-up (C1) bits are exact in double: a sum is taken only for an exponent difference up to
 *     28, and r*b of two 24-bit significands needs at most 48 bits.
 * Anything else (other precision or rounding control, zero/denormal/special/wide operands, a result outside the
 * normal single range) takes the SoftFloat path. Host arithmetic runs in the default round-to-nearest environment and
 * the CPU code is built with -ffp-contract=off, so no FMA contraction changes a rounding. */
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0
#define X87_FAST 0 /* the host evaluates float/double expressions in wider precision: never take a shortcut */
#else
#define X87_FAST 1
#include "x87_fast.h"
#endif
RECOMP_HOT_INLINE void x87_exact_status(X86* c, unsigned flags) {
    c->fpu_sw = (uint16_t)((c->fpu_sw & ~0x200u) | flags);
    x87_pending(c);
}

/* A normal extended value with a 24-bit significand and a normal-single exponent, as single bits. */
RECOMP_HOT_INLINE int x87_as_single(X87Value v, uint32_t* bits) {
    const uint32_t e = v.signExp & 0x7fffu;
    if ((v.signif & UINT64_C(0xffffffffff)) || !(v.signif >> 63) || e - (16383u - 126u) > 253u) {
        return 0;
    }

    *bits =
        ((uint32_t)(v.signExp >> 15) << 31) | ((e - 16383u + 127u) << 23) | ((uint32_t)(v.signif >> 40) & 0x7fffffu);
    return 1;
}

/* Single bits of a normal or zero single as an extended value. */
RECOMP_HOT_INLINE int x87_from_single(uint32_t bits, X87Value* v) {
    const uint32_t e = (bits >> 23) & 0xffu, sign = bits >> 31;
    if (e == 0 || e == 0xff) {
        if (bits << 1) {
            return 0;
        }

        v->signif = 0;
        v->signExp = (uint16_t)(sign << 15);
        return 1;
    }

    v->signif = (uint64_t)((bits & 0x7fffffu) | 0x800000u) << 40;
    v->signExp = (uint16_t)((sign << 15) | (e - 127u + 16383u));
    return 1;
}
/* A normal extended value with a 53-bit significand and an unbiased exponent in [-900, 1000], as a double; the margin
 * keeps every result and error term below normal and finite. */
RECOMP_HOT_INLINE int x87_as_double(X87Value v, double* out) {
    const uint32_t e = v.signExp & 0x7fffu;
    if ((v.signif & 0x7ffu) || !(v.signif >> 63) || e - (16383u - 900u) > 1900u) {
        return 0;
    }

    const uint64_t bits = ((uint64_t)(v.signExp >> 15) << 63) | ((uint64_t)(e - 16383u + 1023u) << 52) |
                          ((v.signif >> 11) & UINT64_C(0xfffffffffffff));
    memcpy(out, &bits, 8);
    return 1;
}

static inline X87Value x87_load(X86* c, uint32_t address, unsigned width) {
    X87Value v;
    if (X87_FAST && width == 32) {
        if (x87_from_single(rd32(address), &v)) {
            x87_exact_status(c, 0);
            return v;
        }
    } else if (X87_FAST && width == 64) {
        const uint64_t bits = rd64(address);
        const uint32_t e = (uint32_t)(bits >> 52) & 0x7ffu, sign = (uint32_t)(bits >> 63);
        if (e != 0 && e != 0x7ff) {
            v.signif = ((bits & UINT64_C(0xfffffffffffff)) | (UINT64_C(1) << 52)) << 11;
            v.signExp = (uint16_t)((sign << 15) | (e - 1023u + 16383u));
            x87_exact_status(c, 0);
            return v;
        }

        if (!(bits << 1)) {
            v.signif = 0;
            v.signExp = (uint16_t)(sign << 15);
            x87_exact_status(c, 0);
            return v;
        }
    }

    return x87_load_soft(c, address, width);
}

static inline void x87_store(X86* c, uint32_t address, unsigned width) {
    if (X87_FAST && !fempty(c, 0)) {
        const X87Value v = X87_ST(c, 0);
        const uint32_t e = v.signExp & 0x7fffu;
        uint32_t single;
        double wide;
        if (width == 32 && x87_as_single(v, &single)) {
            wr32(address, single);
            x87_exact_status(c, 0);
            return;
        }

        if (width == 32 && !(c->fpu_cw & 0xc00u) && x87_as_double(v, &wide)) {
            /* Round to nearest single (stores round by RC only). f and the double are within a factor of two, so
             * their comparison is exact; a normal single result is the x87 result and its PE/C1 bits. */
            const float f = (float)wide;
            memcpy(&single, &f, 4);
            const uint32_t fe = (single >> 23) & 0xffu;
            if (fe != 0 && fe != 0xff) {
                wr32(address, single);
                x87_exact_status(c, (double)f != wide ? (fabs((double)f) > fabs(wide) ? 0x220u : 0x20u) : 0u);
                return;
            }
        }

        if (width == 64 && !(v.signif & 0x7ffu) && (v.signif >> 63) && e - (16383u - 1022u) <= 2045u) {
            wr64(address, ((uint64_t)(v.signExp >> 15) << 63) | ((uint64_t)(e - 16383u + 1023u) << 52) |
                              ((v.signif >> 11) & UINT64_C(0xfffffffffffff)));
            x87_exact_status(c, 0);
            return;
        }
    }

    x87_store_soft(c, address, width);
}
/* PC = 53 bits, RC = nearest: IEEE double arithmetic is the x87 result exactly (one rounding to 53 bits, and the
 * extended exponent range changes nothing for a result in [2^-900, 2^1001)). The rounding error is exact in double
 * (TwoSum for +/-, fma for the product error and the division remainder), so PE is "error != 0" and C1 (magnitude
 * rounded up) is "error opposite in sign to the result" (for division: remainder opposite in sign to the dividend). */
static inline int x87_binary_double(X86* c, X87Value a, X87Value b, enum X87Operation op, X87Value* out) {
    double x, y, r, error;
    if (!x87_as_double(a, &x) || !x87_as_double(b, &y)) {
        return 0;
    }

    int against;
    switch (op) {
    case X87_ADD:
    case X87_SUB: {
        if (op == X87_SUB) {
            y = -y;
        }

        r = x + y;
        const double yv = r - x;
        error = (x - (r - yv)) + (y - yv);
        against = (error < 0) != (r < 0);
        break;
    }
    case X87_MUL:
        r = x * y;
        error = fma(x, y, -r);
        against = (error < 0) != (r < 0);
        break;
    default: /* X87_DIV */
        r = x / y;
        error = fma(-r, y, x);
        against = (error < 0) != (x < 0);
        break;
    }

    uint64_t bits;
    memcpy(&bits, &r, 8);
    const uint32_t e = (uint32_t)(bits >> 52) & 0x7ffu;
    if (e - (1023u - 900u) > 1900u) { /* zero, tiny, huge, infinite or NaN: SoftFloat decides */
        return 0;
    }

    out->signif = ((bits & UINT64_C(0xfffffffffffff)) | (UINT64_C(1) << 52)) << 11;
    out->signExp = (uint16_t)(((uint32_t)(bits >> 63) << 15) | (e - 1023u + 16383u));
    x87_exact_status(c, error != 0 ? (against ? 0x220u : 0x20u) : 0u);
    return 1;
}

static inline X87Value x87_binary(X86* c, X87Value a, X87Value b, enum X87Operation op) {
    uint32_t ab, bb;
    X87Value fast;
    if (X87_FAST && (c->fpu_cw & 0xf00u) == 0x200u && op <= X87_DIV && x87_binary_double(c, a, b, op, &fast)) {
        return fast;
    }

    if (X87_FAST && !(c->fpu_cw & 0xf00u) && op <= X87_DIV && x87_as_single(a, &ab) && x87_as_single(b, &bb)) {
        float fa, fb, r;
        memcpy(&fa, &ab, 4);
        memcpy(&fb, &bb, 4);
        const double da = fa, db = fb;
        double exact = 0, reference = 0, rounded;
        int ok = 1;
        switch (op) {
        case X87_ADD:
        case X87_SUB: {
            const int gap = (int)((ab >> 23) & 0xffu) - (int)((bb >> 23) & 0xffu);
            ok = gap <= 28 && gap >= -28;
            exact = op == X87_ADD ? da + db : da - db;
            r = (float)exact;
            rounded = r;
            reference = exact;
            break;
        }
        case X87_MUL:
            exact = da * db;
            r = (float)exact;
            rounded = r;
            reference = exact;
            break;
        default: /* X87_DIV: r*b and a are exact, so they order |r| against |a/b| */
            r = (float)(da / db);
            rounded = (double)r * db;
            reference = da;
            break;
        }

        uint32_t rb;
        memcpy(&rb, &r, 4);
        const uint32_t re = (rb >> 23) & 0xffu;
        if (ok && re != 0 && re != 0xff) {
            const double ra = fabs(rounded), xa = fabs(reference);
            X87Value result;
            x87_from_single(rb, &result);
            x87_exact_status(c, (ra != xa ? 0x20u : 0u) | (ra > xa ? 0x200u : 0u));
            return result;
        }
    }

    return x87_binary_soft(c, a, b, op);
}

void x87_unary(X86* c, enum X87Operation op);
void x87_compare(X86* c, X87Value a, X87Value b, int quiet, int eflags);
void fxam(X86* c);
void x87_fnstenv(X86* c, uint32_t address);
void x87_fldenv(X86* c, uint32_t address);
void x87_fnsave(X86* c, uint32_t address);
void x87_frstor(X86* c, uint32_t address);
/* IEEE conversion for ABI boundaries and tests, not register storage. */
X87Value x87_from_double(double value);
double x87_to_double(X87Value value);
#include "x87_fast.h"
#endif
