/* The inline x87 fast paths (runtime/cpu/x87.h) against their SoftFloat reference: every result bit, status word
 * and pending-exception state must match, for operands the fast path takes and for its boundaries. */
#include <stdio.h>
#include <stdlib.h>
#include "x86.h"

uint8_t* g_mem;
uint32_t g_watch_base, g_watch_len, g_dirty_count;
RecompDirty g_dirty[RECOMP_DIRTY_SLOTS];
void recomp_watch_hit(uint32_t a, uint32_t n, uint64_t v) {
    (void)a;
    (void)n;
    (void)v;
}

void recomp_null_access(uint32_t a, int write) {
    (void)a;
    (void)write;
    abort();
}

void recomp_fpu_error(X86* c) {
    (void)c;
    abort();
}

static uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
static uint64_t next(void) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

/* Mostly 24-bit significands with exponents around the single range and each other; some wider or special. */
static X87Value operand(int center) {
    X87Value v;
    const uint64_t r = next();
    int exponent = center + (int)(r % 61) - 30;
    switch ((r >> 8) % 16) {
    case 0:
        exponent = 16383 - 126 + (int)((r >> 16) % 5) - 2;
        break; /* single minimum */
    case 1:
        exponent = 16383 + 127 + (int)((r >> 16) % 5) - 2;
        break; /* single maximum */
    case 2:
        exponent = (int)((r >> 16) % 0x8000);
        break; /* anywhere */
    case 3:
        exponent = 16383 + ((r >> 16) & 1 ? 1000 : -900) + (int)((r >> 17) % 5) - 2;
        break; /* double margin */
    default:
        break;
    }

    v.signExp = (uint16_t)(((r >> 20) & 1) << 15 | ((unsigned)exponent & 0x7fffu));
    v.signif = next() | UINT64_C(1) << 63;
    switch ((r >> 24) % 16) {
    case 0:
        break; /* 64-bit significand */
    case 1:
    case 7:
    case 8:
    case 9:
        v.signif &= ~UINT64_C(0x7ff);
        break; /* 53-bit significand */
    case 2:
        v.signif = 0;
        v.signExp &= 0x8000u;
        break; /* zero */
    case 3:
        v.signif &= ~(UINT64_C(1) << 63);
        break; /* denormal / unnormal */
    case 4:
        v.signExp |= 0x7fffu;
        break; /* infinity or NaN */
    case 5:
        v.signif = UINT64_C(1) << 63 | (next() & 0x7) << 40;
        break; /* few significant bits */
    default:
        v.signif &= ~UINT64_C(0xffffffffff);
        break; /* 24-bit significand */
    }

    return v;
}

static unsigned checks, fast_taken;
#define CHECK(condition, ...)                                                                                          \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(condition)) {                                                                                            \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fprintf(stderr, " (line %d)\n", __LINE__);                                                                 \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static void start(X86* c, uint16_t cw, uint16_t sw) {
    x87_finit(c);
    c->fpu_cw = cw;
    c->fpu_sw = sw;
    c->fpu_pending = 0;
}

int main(void) {
    g_mem = calloc(1, 0x100000);
    if (!g_mem) {
        return 2;
    }

    const uint16_t controls[] = {0x007f, 0x005f, 0x003f, 0x027f, 0x047f, 0x0c7f, 0x037f, 0x0060};
    static const char* const names[] = {"add", "sub", "mul", "div"};
    for (unsigned i = 0; i < 6000000; i++) {
        const uint16_t cw = controls[i % 8 < 3 ? 0 : i % 8 < 6 ? 3 : i % 8];
        const uint16_t sw = (uint16_t)(next() & 0x3f);
        const int center = 16383 + (int)(next() % 260) - 130;
        const X87Value a = operand(center), b = operand(center);
        const enum X87Operation op = (enum X87Operation)(i % 4);
        X86 fast, soft;
        start(&fast, (uint16_t)(cw ^ (i & 0x20 ? 0x20 : 0)), sw);
        start(&soft, fast.fpu_cw, sw);
        const X87Value f = x87_binary(&fast, a, b, op);
        const X87Value s = x87_binary_soft(&soft, a, b, op);
        CHECK(f.signif == s.signif && f.signExp == s.signExp && fast.fpu_sw == soft.fpu_sw &&
                  fast.fpu_pending == soft.fpu_pending,
              "%s cw=%04x %04x:%016llx %04x:%016llx -> fast %04x:%016llx sw=%04x, soft %04x:%016llx sw=%04x", names[op],
              fast.fpu_cw, a.signExp, (unsigned long long)a.signif, b.signExp, (unsigned long long)b.signif, f.signExp,
              (unsigned long long)f.signif, fast.fpu_sw, s.signExp, (unsigned long long)s.signif, soft.fpu_sw);
        uint32_t ab, bb;
        double ad, bd;
        fast_taken += (!(fast.fpu_cw & 0xf00u) && x87_as_single(a, &ab) && x87_as_single(b, &bb)) ||
                      ((fast.fpu_cw & 0xf00u) == 0x200u && x87_as_double(a, &ad) && x87_as_double(b, &bd));
    }

    for (unsigned i = 0; i < 2000000; i++) {
        const unsigned width = i & 1 ? 64 : 32;
        const uint64_t r = next();
        uint64_t bits = r;
        if (width == 32 && (r >> 40) % 4 == 0) {
            bits = (r & 0x807fffffu) | ((r >> 50) % 3 == 0 ? 0u : 0x7f800000u);
        }

        if (width == 64 && (r >> 40) % 4 == 0) {
            bits &= UINT64_C(0x800fffffffffffff) | ((r >> 50) % 2 ? UINT64_C(0x7ff0000000000000) : 0);
        }

        if ((r >> 44) % 8 == 0) {
            bits &= UINT64_C(0x8000000000000000) | (width == 32 ? UINT64_C(0x80000000) : 0);
        }

        memcpy(g_mem + 0x10000, &bits, 8);
        X86 fast, soft;
        const uint16_t cw = controls[i % 8], sw = (uint16_t)(next() & 0x23f);
        start(&fast, cw, sw);
        start(&soft, cw, sw);
        const X87Value f = x87_load(&fast, 0x10000, width), s = x87_load_soft(&soft, 0x10000, width);
        CHECK(f.signif == s.signif && f.signExp == s.signExp && fast.fpu_sw == soft.fpu_sw &&
                  fast.fpu_pending == soft.fpu_pending,
              "load%u %016llx", width, (unsigned long long)bits);
    }

    for (unsigned i = 0; i < 2000000; i++) {
        const unsigned width = i & 1 ? 64 : 32;
        const X87Value v = operand(16383 + (int)(next() % 2100) - 1050);
        uint8_t written[2][8];
        X86 c[2];
        for (int k = 0; k < 2; k++) {
            start(&c[k], controls[i % 8], (uint16_t)(i & 0x23f));
            if (i % 64 != 0) {
                x87_push(&c[k], v); /* sometimes an empty ST0: the stack-fault path */
            }

            memset(g_mem + 0x10000, 0xa5, 8);
            if (k == 0) {
                x87_store(&c[k], 0x10000, width);
            } else {
                x87_store_soft(&c[k], 0x10000, width);
            }

            memcpy(written[k], g_mem + 0x10000, 8);
        }

        CHECK(!memcmp(written[0], written[1], 8) && c[0].fpu_sw == c[1].fpu_sw && c[0].fpu_pending == c[1].fpu_pending,
              "store%u %04x:%016llx", width, v.signExp, (unsigned long long)v.signif);
    }

    printf("x87 fast paths: %u checks passed, %u binary operations eligible for the fast path\n", checks, fast_taken);
    return 0;
}
