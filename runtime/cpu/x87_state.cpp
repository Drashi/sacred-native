#include "softfloat_bridge.h"
#include <stdlib.h>

void fxam(X86* c) {
    auto value = X87_ST(c, 0);
    unsigned exp = value.signExp & 0x7fff, cc;
    if (fempty(c, 0)) {
        cc = 0x4100;
    } else if (exp == 0 && !value.signif) {
        cc = 0x4000;
    } else if (exp == 0) {
        cc = 0x4400;
    } else if (!(value.signif >> 63)) {
        cc = 0; /* unsupported */
    } else if (exp != 0x7fff) {
        cc = 0x400;
    } else {
        cc = value.signif << 1 ? 0x100 : 0x500;
    }

    c->fpu_sw = (uint16_t)((c->fpu_sw & ~0x4700u) | cc | ((value.signExp & 0x8000) >> 6));
}

void x87_fnstenv(X86* c, uint32_t a) {
    wr32(a, 0xffff0000u | c->fpu_cw);
    wr32(a + 4, 0xffff0000u | fstsw(c));
    wr32(a + 8, 0xffff0000u | c->fpu_tag);
    wr32(a + 12, c->fpu_ip);
    wr32(a + 16, c->fpu_cs | ((uint32_t)c->fpu_opcode << 16));
    wr32(a + 20, c->fpu_dp);
    wr32(a + 24, c->fpu_ds);
    x87_set_cw(c, (uint16_t)(c->fpu_cw | 63));
}

void x87_fldenv(X86* c, uint32_t a) {
    c->fpu_cw = rd16(a);
    c->fpu_sw = rd16(a + 4) & ~0x3800u;
    c->fpu_top = (rd16(a + 4) >> 11) & 7;
    c->fpu_tag = rd16(a + 8);
    c->fpu_ip = rd32(a + 12);
    c->fpu_cs = rd16(a + 16);
    c->fpu_opcode = rd16(a + 18) & 0x7ff;
    c->fpu_dp = rd32(a + 20);
    c->fpu_ds = rd16(a + 24);
    x87_pending(c);
}

void x87_fnsave(X86* c, uint32_t a) {
    x87_fnstenv(c, a);
    for (unsigned i = 0; i < 8; i++) {
        auto value = X87_ST(c, i);
        wr64(a + 28 + 10 * i, value.signif);
        wr16(a + 36 + 10 * i, value.signExp);
    }

    x87_finit(c);
}

void x87_frstor(X86* c, uint32_t a) {
    x87_fldenv(c, a);
    for (unsigned i = 0; i < 8; i++) {
        X87_ST(c, i).signif = rd64(a + 28 + 10 * i);
        X87_ST(c, i).signExp = rd16(a + 36 + 10 * i);
    }
}

/* SACRED_X87_FAST=0 keeps every x87 instruction on the per-instruction path (A/B measurements, diagnosis). */
static uint8_t x87f_from_environment() {
    const char* value = getenv("SACRED_X87_FAST");
    return !(value && value[0] == '0');
}

uint8_t x87f_enabled = x87f_from_environment();
void x87f_spill(X86* c, unsigned t0, unsigned n, const int8_t* rel, const double* v, unsigned full, int top,
                uint16_t sw, int dp_set, uint32_t dp) {
    for (unsigned i = 0; i < n; i++) {
        x87f_put(c, t0 + (unsigned)rel[i], v[i], (full >> i) & 1u);
    }

    c->fpu_top = (t0 + (unsigned)top) & 7u;
    c->fpu_sw = sw;
    if (dp_set) {
        c->fpu_dp = dp;
        c->fpu_ds = c->ds;
    }
}
