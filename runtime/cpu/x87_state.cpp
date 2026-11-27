#include "softfloat_bridge.h"
#include <atomic>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
/* SACRED_FTOL_INLINE: 0 keeps CALL _ftol a call, verify compares the run's version with the routine (x87_fast.h). */
static uint8_t x87f_ftol_from_environment() {
    const char* value = getenv("SACRED_FTOL_INLINE");
    if (value && !strcmp(value, "verify")) {
        return 2;
    }

    return !(value && value[0] == '0');
}

uint8_t x87f_ftol_mode = x87f_ftol_from_environment();
namespace {
constexpr uint32_t ftol_frame = 20; /* bytes the routine writes below the caller's ESP */
struct FtolVerify {
    X86 before, inlined;
    uint8_t stack_before[ftol_frame], stack_inlined[ftol_frame];
    bool armed;
};
thread_local FtolVerify t_ftol;
std::atomic<unsigned long> g_ftol_sites{0}, g_ftol_compared{0}, g_ftol_mismatches{0};
void ftol_stack(const X86& c, uint8_t* out) {
    for (uint32_t i = 0; i < ftol_frame; i++) {
        out[i] = rd8(c.r[R_ESP] - ftol_frame + i);
    }
}

void ftol_report() {
    fprintf(stderr, "FTOL_VERIFY calls=%lu compared=%lu mismatches=%lu\n", g_ftol_sites.load(), g_ftol_compared.load(),
            g_ftol_mismatches.load());
}
}

void x87f_ftol_verify_begin(X86* c) {
    static std::once_flag once;
    std::call_once(once, [] { atexit(ftol_report); });
    /* The game is usually killed, not exited: report as it runs. */
    if ((++g_ftol_sites & 0x3ffffu) == 0) {
        ftol_report();
    }

    t_ftol.before = *c;
    ftol_stack(*c, t_ftol.stack_before);
    t_ftol.armed = false;
}

void x87f_ftol_verify_inline(X86* c) {
    t_ftol.inlined = *c;
    ftol_stack(t_ftol.before, t_ftol.stack_inlined);
    for (uint32_t i = 0; i < ftol_frame; i++) {
        wr8(t_ftol.before.r[R_ESP] - ftol_frame + i, t_ftol.stack_before[i]);
    }

    *c = t_ftol.before;
    t_ftol.armed = true;
}

void x87f_ftol_verify_end(X86* c) {
    if (!t_ftol.armed) {
        return;
    }

    t_ftol.armed = false;
    ++g_ftol_compared;
    const X86 &a = t_ftol.inlined, &b = *c;
    char text[512];
    int n = 0;
    const auto differ = [&](const char* name, uint64_t mine, uint64_t routine) {
        if (mine != routine && n < int(sizeof text) - 64) {
            n += snprintf(text + n, sizeof text - size_t(n), " %s=%llx/%llx", name, (unsigned long long)mine,
                          (unsigned long long)routine);
        }
    };
    static const char* const registers[] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    for (unsigned i = 0; i < 8; i++) {
        differ(registers[i], a.r[i], b.r[i]);
        differ("st.signif", a.st[i].signif, b.st[i].signif);
        differ("st.signExp", a.st[i].signExp, b.st[i].signExp);
    }

    differ("eip", a.eip, b.eip);
    differ("cf", a.eflags_cf, b.eflags_cf);
    differ("zf", a.eflags_zf, b.eflags_zf);
    differ("sf", a.eflags_sf, b.eflags_sf);
    differ("of", a.eflags_of, b.eflags_of);
    differ("pf", a.eflags_pf, b.eflags_pf);
    differ("af", a.eflags_af, b.eflags_af);
    differ("df", a.eflags_df, b.eflags_df);
    differ("eflags", a.eflags_misc, b.eflags_misc);
    differ("top", a.fpu_top, b.fpu_top);
    differ("cw", a.fpu_cw, b.fpu_cw);
    differ("sw", a.fpu_sw, b.fpu_sw);
    differ("tag", a.fpu_tag, b.fpu_tag);
    differ("pending", a.fpu_pending, b.fpu_pending);
    differ("fop", a.fpu_opcode, b.fpu_opcode);
    differ("fcs", a.fpu_cs, b.fpu_cs);
    differ("fds", a.fpu_ds, b.fpu_ds);
    differ("fip", a.fpu_ip, b.fpu_ip);
    differ("fdp", a.fpu_dp, b.fpu_dp);
    differ("mxcsr", a.mxcsr, b.mxcsr);
    differ("fs", a.fs_base, b.fs_base);
    differ("source", a.source_pc, b.source_pc);
    differ("depth", a.direct_depth, b.direct_depth);
    /* The budget is equal unless the routine's own tick offered the baton, which starts it again from zero. */
    if (b.budget >= a.budget - t_ftol.before.budget) {
        differ("budget", a.budget, b.budget);
    }

    uint8_t stack[ftol_frame];
    ftol_stack(t_ftol.before, stack);
    for (uint32_t i = 0; i < ftol_frame; i++) {
        if (stack[i] != t_ftol.stack_inlined[i]) {
            char name[16];
            snprintf(name, sizeof name, "[esp-%u]", ftol_frame - i);
            differ(name, t_ftol.stack_inlined[i], stack[i]);
        }
    }

    if (n && g_ftol_mismatches++ < 16) {
        fprintf(stderr, "FTOL_VERIFY_MISMATCH return=%08x st0=%04x:%016llx run/routine:%s\n", b.eip,
                t_ftol.before.st[t_ftol.before.fpu_top & 7u].signExp,
                (unsigned long long)t_ftol.before.st[t_ftol.before.fpu_top & 7u].signif, text);
    }
}

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
