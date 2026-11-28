/* Native-double x87 runs. Included from x87.h; tools/lift/x87_fast.py emits the calls.
 *
 * A run is a straight stretch of x87 instructions (and register-only integer instructions between them) inside one
 * block of a region. The generator emits it twice: the usual per-instruction code, and in front of it a version that
 * keeps the x87 stack in host doubles. The double version is taken only when one check on entry holds:
 *   - control word PC = 53 bits, RC = nearest, every exception masked (Sacred's in-game 0x27f);
 *   - PE already set in the status word and nothing pending, so no instruction of the run can change an exception
 *     bit or raise anything: inexactness has nothing left to report;
 *   - every register the run reads is full and holds zero or a normal value with a 53-bit significand, every
 *     register it pushes into is empty.
 * Under those conditions an x87 operation on zero/normal doubles is IEEE double arithmetic, as long as its result
 * stays a normal double (one rounding to 53 bits; the extended exponent range changes nothing) or is an exact zero.
 * The generator proves that from exponent bounds where it can and checks the result where it cannot. A failing check
 * writes the stack as it stood before the instruction back to the CPU state and continues in the per-instruction
 * code at that instruction ("deoptimisation"). Every instruction that may deoptimise rewrites C1 there, so C1 is
 * computed only where something reads it: an FNSTSW in the run, or the end of the run for its last C1 writer. */
#ifndef SACRED_X87_FAST_H
#define SACRED_X87_FAST_H

#ifdef X87F_STATS /* tests: runs entered, deoptimised, completed */
extern unsigned long x87f_stats[3];
#define X87F_COUNT(i) ((void)x87f_stats[i]++)
#else
#define X87F_COUNT(i) ((void)0)
#endif
extern uint8_t x87f_enabled; /* SACRED_X87_FAST=0 turns the double runs off (A/B measurements) */

/* Exponent window of every checked value: a normal double in [2^-500, 2^500). */
#define X87F_WIN_LO 500u
#define X87F_WIN_SPAN 1000u

RECOMP_HOT_INLINE uint64_t x87f_bits(double d) {
    uint64_t b;
    memcpy(&b, &d, 8);
    return b;
}

RECOMP_HOT_INLINE double x87f_double(uint64_t b) {
    double d;
    memcpy(&d, &b, 8);
    return d;
}

/* Entry: CW, PE, nothing pending, and the rotated tag word (2 bits per register relative to TOP) under `mask`. */
RECOMP_HOT_INLINE int x87f_enter(const X86* c, uint32_t mask, uint32_t expect) {
    const uint32_t tag = c->fpu_tag, top = c->fpu_top;
    const uint32_t rot = ((tag | (tag << 16)) >> (2u * top)) & 0xffffu;
    const int enter = x87f_enabled & ((c->fpu_cw & 0xf3fu) == 0x23fu) & ((c->fpu_sw >> 5) & 1u) &
                      (c->fpu_pending == 0) & ((rot & mask) == expect);
    if (enter) {
        X87F_COUNT(0);
    }

    return enter;
}

/* A register value as a double: zero, or normal with a 53-bit significand inside the window. */
RECOMP_HOT_INLINE int x87f_in(X87Value v, double* out) {
    const uint32_t e = v.signExp & 0x7fffu;
    const uint64_t sign = (uint64_t)(v.signExp >> 15) << 63;
    if ((v.signif | e) == 0) {
        *out = x87f_double(sign);
        return 1;
    }

    *out = x87f_double(sign | ((uint64_t)(e - 16383u + 1023u) << 52) | ((v.signif >> 11) & UINT64_C(0xfffffffffffff)));
    return (v.signif >> 63) & !(v.signif & 0x7ffu) & (e - (16383u - X87F_WIN_LO) < X87F_WIN_SPAN);
}

/* A zero or normal double as the ext80 register value (exact). */
RECOMP_HOT_INLINE X87Value x87f_ext(double d) {
    const uint64_t b = x87f_bits(d);
    const uint32_t e = (uint32_t)(b >> 52) & 0x7ffu;
    X87Value v;
    v.signif = e ? (b << 11) | (UINT64_C(1) << 63) : 0;
    v.signExp = (uint16_t)(((uint32_t)(b >> 48) & 0x8000u) | (e ? e - 1023u + 16383u : 0u));
    return v;
}

/* Physical register (t0 + rel) gets v and its tag (full) or only the value with an empty tag (popped). */
RECOMP_HOT_INLINE void x87f_put(X86* c, unsigned slot, double v, int full) {
    slot &= 7u;
    c->st[slot] = x87f_ext(v);
    ftag_put(c, slot, full ? (v == 0 ? FTAG_ZERO : FTAG_VALID) : FTAG_EMPTY);
}

/* The window check of a computed result. */
RECOMP_HOT_INLINE int x87f_win(double r) {
    const uint32_t e = (uint32_t)(x87f_bits(r) >> 52) & 0x7ffu;
    return e - (1023u - X87F_WIN_LO) < X87F_WIN_SPAN;
}

RECOMP_HOT_INLINE int x87f_ok_add(double r) {
    return RECOMP_LIKELY(x87f_win(r)) || r == 0;
}

RECOMP_HOT_INLINE int x87f_ok_mul(double r, double x, double y) {
    return RECOMP_LIKELY(x87f_win(r)) || (r == 0 && (x == 0 || y == 0));
}

RECOMP_HOT_INLINE int x87f_ok_div(double r, double x, double y) {
    return RECOMP_LIKELY(x87f_win(r)) || (r == 0 && x == 0 && y != 0);
}

RECOMP_HOT_INLINE int x87f_ok_sqrt(double r) {
    return RECOMP_LIKELY(x87f_win(r)) || r == 0;
}

/* Loads: zero, or a normal single (no DE) / a double inside the window. */
RECOMP_HOT_INLINE int x87f_ok_f32(uint32_t b) {
    return ((b >> 23) & 0xffu) - 1u < 254u || !(b << 1);
}

RECOMP_HOT_INLINE int x87f_ok_f64(uint64_t b) {
    return ((uint32_t)(b >> 52) & 0x7ffu) - (1023u - X87F_WIN_LO) < X87F_WIN_SPAN || !(b << 1);
}

RECOMP_HOT_INLINE double x87f_f32(uint32_t b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}

/* FST m32: the single is the x87 result unless the store overflows or underflows. */
RECOMP_HOT_INLINE int x87f_ok_st32(double d, float f) {
    return RECOMP_LIKELY(fabs(d) >= 0x1p-126 && fabsf(f) <= FLT_MAX) || d == 0;
}
/* C1 ("rounded up in magnitude") of an observed result. The error terms are exact: TwoSum always, the fma terms for
 * values in [2^-900, 2^1000] (anything else deoptimises; the per-instruction code decides it). */
RECOMP_HOT_INLINE int x87f_c1_wide(double v) {
    const uint32_t e = (uint32_t)(x87f_bits(v) >> 52) & 0x7ffu;
    return e - (1023u - 900u) <= 1900u;
}

RECOMP_HOT_INLINE unsigned x87f_c1_add(double x, double y, double r) {
    const double yv = r - x, error = (x - (r - yv)) + (y - yv);
    return error != 0 && ((error < 0) != (r < 0));
}

RECOMP_HOT_INLINE int x87f_c1_mul(double x, double y, double r, unsigned* c1) {
    if (r == 0) {
        *c1 = 0;
        return 1;
    }

    const double error = fma(x, y, -r);
    *c1 = error != 0 && ((error < 0) != (r < 0));
    return x87f_c1_wide(x) & x87f_c1_wide(y) & x87f_c1_wide(r);
}

RECOMP_HOT_INLINE int x87f_c1_div(double x, double y, double r, unsigned* c1) {
    if (r == 0) {
        *c1 = 0;
        return 1;
    }

    const double error = fma(-r, y, x);
    *c1 = error != 0 && ((error < 0) != (x < 0));
    return x87f_c1_wide(x) & x87f_c1_wide(y) & x87f_c1_wide(r);
}

RECOMP_HOT_INLINE int x87f_c1_sqrt(double x, double r, unsigned* c1) {
    if (r == 0) {
        *c1 = 0;
        return 1;
    }

    *c1 = fma(-r, r, x) < 0;
    return x87f_c1_wide(x) & x87f_c1_wide(r);
}
/* CALL _ftol inside a run (tools/lift/x87_fast.py, Compiler.call). The routine is the C runtime's (generate.py pins
 * its bytes): PUSH EBP; MOV EBP,ESP; ADD ESP,-12; FSTCW [EBP-2]; WAIT; MOV AX,[EBP-2]; OR AH,0xc; MOV [EBP-4],AX;
 * FLDCW [EBP-4]; FISTP qword [EBP-12]; FLDCW [EBP-2]; MOV EAX,[EBP-12]; MOV EDX,[EBP-8]; LEAVE; RET. Under the entry
 * conditions of a run (every exception masked, PE set, nothing pending) and for a value that truncates into 64 bits,
 * a call from ESP = S leaves exactly this, and the run writes all of it:
 *   - EDX:EAX = the value truncated; EBP, ESP and the other registers as they were;
 *   - CF = OF = ZF = 0, SF and PF of (control word >> 8) | 0xc (the OR), AF of the ADD ESP,-12;
 *   - below ESP: the return address at S-4, EBP at S-8, the control word at S-10, the truncating one at S-12, the
 *     64-bit result at S-20;
 *   - x87: ST0 popped (its register keeps the value, tagged empty), the control word as before, C1 clear (a
 *     truncation never rounds up; PE is set already), the instruction pointer and opcode of the FISTP, the data
 *     pointer S-20;
 *   - EIP at the return address, and the block accounting of the call (seh.h): the routine's RET as the last
 *     source, its instructions on the budget (the run offers the baton at its next tick, not inside).
 * A NaN or a value outside the 64-bit range (the routine's invalid operation, integer indefinite) leaves the run and
 * takes the call. SACRED_FTOL_INLINE=0 takes the call always; =verify takes it always too, runs the one-call run in
 * front of it and compares everything listed above with what the routine left (FTOL_VERIFY lines on stderr). */
extern uint8_t x87f_ftol_mode; /* 0 the call, 1 in the run, 2 verify */
RECOMP_HOT_INLINE int x87f_ftol_ok(double v, unsigned mode) {
    return (x87f_ftol_mode == mode) & (v >= -0x1p63) & (v < 0x1p63);
}

/* Verify mode: the state in front of the call is kept; the one-call run's result is recorded and the state put
 * back; after the routine has returned the two are compared. */
void x87f_ftol_verify_begin(X86* c);
void x87f_ftol_verify_inline(X86* c);
void x87f_ftol_verify_end(X86* c);
/* Deoptimisation: n registers at TOP-relative positions rel[i] (bit i of `full`: still on the stack), the TOP of the
 * moment, the status word and, when the resumed instruction does not set it itself, the data pointer. */
RECOMP_COLD void x87f_spill(X86* c, unsigned t0, unsigned n, const int8_t* rel, const double* v, unsigned full, int top,
                            uint16_t sw, int dp_set, uint32_t dp);
#endif
