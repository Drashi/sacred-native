/* Architectural state invariants, including fields QEMU does not represent.
 * Expected bit patterns are independently constructed, not read from helpers. */
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

static unsigned checks;
#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(condition)) {                                                                                            \
            fprintf(stderr, "state check failed at line %d\n", __LINE__);                                              \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

int main(void) {
    g_mem = calloc(1, 0x100000);
    if (!g_mem) {
        return 2;
    }

    X86 c = {0};
    const unsigned pcs[] = {0, 0x200, 0x300};
    const unsigned bits[] = {24, 53, 64};
    for (unsigned p = 0; p < 3; p++) {
        for (unsigned rc = 0; rc < 4; rc++) {
            x87_finit(&c);
            x87_set_cw(&c, (uint16_t)(0x7f | pcs[p] | (rc << 10)));
            X87Value integer = x87_integer(INT64_MAX);
            CHECK(integer.signif == UINT64_C(0xfffffffffffffffe) && integer.signExp == 0x403d);
            CHECK(x87_to_integer(&c, integer, 64, 0) == INT64_MAX);
            wr64(0x10000, UINT64_C(0x3ff0000000000001));
            c.fpu_sw = 0;
            X87Value loaded = x87_load(&c, 0x10000, 64);
            CHECK(loaded.signif == UINT64_C(0x8000000000000800) && loaded.signExp == 0x3fff);
            CHECK(c.fpu_sw == 0); /* Loads never use PC to narrow the source. */
            X87Value large = {UINT64_C(0x8000000000000000), 0x6000};
            X87Value one = {UINT64_C(0x8000000000000000), 0x3fff};
            X87Value product = x87_binary(&c, large, one, X87_MUL);
            CHECK(product.signExp == 0x6000 && product.signif == large.signif);
            X86 remainder = c;
            X87Value precise = {UINT64_C(0x8000000000000001), 0x3ffe};
            x87_push(&remainder, x87_integer(1));
            x87_push(&remainder, precise);
            x87_unary(&remainder, X87_PREM);
            CHECK(X87_ST(&remainder, 0).signif == precise.signif && X87_ST(&remainder, 0).signExp == precise.signExp);
            for (unsigned sign = 0; sign < 2; sign++) {
                c.fpu_sw = 0;
                one.signExp = (uint16_t)(0x3fff | (sign << 15));
                X87Value half = {UINT64_C(0x8000000000000000), (uint16_t)((0x3fff - bits[p]) | (sign << 15))};
                X87Value sum = x87_binary(&c, one, half, X87_ADD);
                unsigned round_up_magnitude = rc == (sign ? 1u : 2u);
                uint64_t expected = UINT64_C(0x8000000000000000);
                if (round_up_magnitude) {
                    expected += UINT64_C(1) << (64 - bits[p]);
                }

                CHECK(sum.signif == expected && sum.signExp == one.signExp);
                CHECK((c.fpu_sw & 0x23f) == (0x20u | (round_up_magnitude << 9)));
            }
        }
    }

    x87_finit(&c);
    X87Value underflow = x87_get(&c, 0);
    CHECK(underflow.signExp == 0xffff && underflow.signif == UINT64_C(0xc000000000000000));
    CHECK((c.fpu_sw & 0x241) == 0x41);
    x87_set_cw(&c, 0x340);
    CHECK(c.fpu_pending == 1 && (c.fpu_sw & 0x8080) == 0x8080);
    x87_finit(&c);
    X87Value denormal = {1, 0};
    x87_push(&c, denormal);
    x87_store(&c, 0x10000, 64);
    CHECK(rd64(0x10000) == 0);
    CHECK((c.fpu_sw & 0x23f) == 0x30); /* FST has UE/PE, not input DE. */
    c.fpu_sw = 0;
    CHECK(x87_to_integer(&c, denormal, 64, 0) == 0);
    CHECK((c.fpu_sw & 0x23f) == 0x20); /* FIST has PE, not input DE. */
    c.fpu_sw = 0;
    x87_unary(&c, X87_ROUND);
    CHECK(X87_ST(&c, 0).signif == 0 && (c.fpu_sw & 0x23f) == 0x22);
    x87_finit(&c);
    for (unsigned i = 0; i < 9; i++) {
        x87_push(&c, x87_integer(1));
    }

    CHECK((c.fpu_sw & 0x241) == 0x241 && c.fpu_top == 7 && c.fpu_tag == 0x8000);
    CHECK(X87_ST(&c, 0).signExp == 0xffff);
    x87_finit(&c);
    mmx_set(&c, 3, UINT64_C(0x0123456789abcdef));
    CHECK(c.fpu_top == 0 && c.fpu_tag == 0);
    CHECK(c.st[3].signif == UINT64_C(0x0123456789abcdef) && c.st[3].signExp == 0xffff);
    x87_emms(&c);
    CHECK(c.fpu_tag == 0xffff && c.st[3].signif == UINT64_C(0x0123456789abcdef));
    x87_push(&c, x87_integer(7));
    CHECK(c.fpu_top == 7 && c.fpu_tag == 0x3fff);
    x87_set_cw(&c, 0x0b7f);
    c.fpu_ip = 0x12345678;
    c.fpu_dp = 0x87654321;
    c.fpu_opcode = 0x345;
    c.fpu_cs = 0x23;
    c.fpu_ds = 0x2b;
    x87_fnsave(&c, 0x20000);
    CHECK(c.fpu_cw == 0x37f && c.fpu_top == 0 && c.fpu_tag == 0xffff);
    CHECK(rd32(0x2000c) == 0x12345678 && rd32(0x20014) == 0x87654321);
    x87_frstor(&c, 0x20000);
    CHECK(c.fpu_cw == 0x0b7f && c.fpu_top == 7 && c.fpu_tag == 0x3fff);
    CHECK(c.fpu_ip == 0x12345678 && c.fpu_dp == 0x87654321);
    CHECK(c.fpu_opcode == 0x345 && c.fpu_cs == 0x23 && c.fpu_ds == 0x2b);
    CHECK(X87_ST(&c, 0).signif == UINT64_C(0xe000000000000000) && X87_ST(&c, 0).signExp == 0x4001);
    free(g_mem);
    printf("CPU_STATE_PASS checks=%u\n", checks);
    return 0;
}
