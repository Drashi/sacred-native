/* Native stand-ins for hot Sacred.exe functions. The generator (tools/lift/generate.py, NATIVE_CALLS) makes a direct
 * CALL rel32 of the function run its stand-in instead of the region code; every other way in (the driver, a
 * disabled direct path) runs the generated code, so a stand-in must leave exactly what that code leaves: registers,
 * flags, ESP/EIP after the RET, and its stack stores. Generated code is pinned to the executable's hash, and so are
 * these. SACRED_EXE_NATIVE=verify runs the generated code after each stand-in and stops on any difference. */
#include "x86.h"
#include "seh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void region_00640304(X86* c);

static int verify_mode = -1;
static int verifying(void) {
    if (verify_mode < 0) {
        const char* v = getenv("SACRED_EXE_NATIVE");
        verify_mode = v && !strcmp(v, "verify");
    }

    return verify_mode;
}

/* CMP a, b */
RECOMP_HOT_INLINE void flags_cmp(X86* c, uint32_t a, uint32_t b) {
    const uint32_t r = a - b;
    c->eflags_cf = a < b;
    c->eflags_zf = r == 0;
    c->eflags_sf = r >> 31;
    c->eflags_of = ((a ^ b) & (a ^ r)) >> 31;
    c->eflags_af = ((a ^ b ^ r) >> 4) & 1u;
    c->eflags_pf = parity8(r);
}

/* 00640410  iterator __thiscall std::map<uint32_t, T>::find(const uint32_t *key); ret 4. Node: left +8, right +0xc,
 * key +0x10; the head node at [this], the root at head+4. Used by the render record caches (0x635e50, 0x635fe0),
 * which look a record up for every render list entry. */
static void find_640410(X86* c) {
    const uint32_t esp = c->r[R_ESP];
    const uint32_t key_at = rd32(esp + 4), head = rd32(c->r[R_ECX]);
    wr32(esp - 4, c->r[R_ESI]);
    wr32(esp - 8, c->r[R_EDI]);
    uint32_t node = rd32(head + 4), best = head, ecx = 0, steps = 0;
    const uint32_t key = node ? rd32(key_at) : 0;
    while (node) {
        const uint32_t node_key = rd32(node + 0x10);
        flags_cmp(c, node_key, key);
        if (node_key < key) {
            node = rd32(node + 0xc);
        } else {
            best = node;
            node = rd32(node + 8);
        }

        ++steps;
    }

    flags_cmp(c, best, head);
    uint32_t eax = head;
    if (best != head) {
        ecx = rd32(key_at);
        const uint32_t best_key = rd32(best + 0x10);
        flags_cmp(c, ecx, best_key);
        if (ecx >= best_key) {
            eax = best;
        }
    }

    c->r[R_EAX] = eax;
    c->r[R_ECX] = ecx;
    c->r[R_EDX] = best;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 8;
    c->source_pc = 0x00640445u;
    c->budget += 12 + steps * 6;
}

static void verify(X86* c, void (*native)(X86*), void (*original)(X86*), uint32_t address, const char* name) {
    X86 before = *c;
    native(c);
    X86 mine = *c;
    *c = before;
    c->eip = address;
    original(c);
    if (c->eip != mine.eip || memcmp(c->r, mine.r, sizeof c->r) || c->eflags_cf != mine.eflags_cf ||
        c->eflags_zf != mine.eflags_zf || c->eflags_sf != mine.eflags_sf || c->eflags_of != mine.eflags_of ||
        c->eflags_af != mine.eflags_af || c->eflags_pf != mine.eflags_pf) {
        fprintf(stderr, "SACRED_EXE_NATIVE mismatch %s eip %08x/%08x", name, mine.eip, c->eip);
        for (int i = 0; i < 8; ++i) {
            fprintf(stderr, " r%d %08x/%08x", i, mine.r[i], c->r[i]);
        }

        fprintf(stderr, " flags %u%u%u%u%u%u/%u%u%u%u%u%u\n", mine.eflags_cf, mine.eflags_zf, mine.eflags_sf,
                mine.eflags_of, mine.eflags_af, mine.eflags_pf, c->eflags_cf, c->eflags_zf, c->eflags_sf, c->eflags_of,
                c->eflags_af, c->eflags_pf);
        abort();
    }
}

void sacred_native_00640410(X86* c) {
    if (RECOMP_UNLIKELY(verifying())) {
        verify(c, find_640410, region_00640304, 0x00640410u, "640410");
        return;
    }

    find_640410(c);
}

/* ADD a, b */
RECOMP_HOT_INLINE void flags_add(X86* c, uint32_t a, uint32_t b) {
    const uint64_t wide = (uint64_t)a + b;
    const uint32_t r = (uint32_t)wide;
    c->eflags_cf = (uint32_t)(wide >> 32) & 1u;
    c->eflags_zf = r == 0;
    c->eflags_sf = r >> 31;
    c->eflags_of = (~(a ^ b) & (a ^ r)) >> 31;
    c->eflags_af = ((a ^ b ^ r) >> 4) & 1u;
    c->eflags_pf = parity8(r);
}

/* 0041b510 worldState_lightAt(x, y, const uint32_t *scale), thiscall, ret 0xc: the colour of the light at a place.
 * The stand-in does the mode the lit outdoor world uses ([this+0x9e0c] clear, flags [this+0x10] & 0x20), with the
 * light index in place (0x183036c bit 0 set, 0x183037c clear); every other call is the generated code's, from the
 * entry. That mode: lightIndex_find (0x41de70, a lower bound over the 16-byte index records [0x1830370, 0x1830374)
 * by x) names an interval, whose record in front holds a list of int16 light numbers at +4/+8; a light is three
 * ints at this + 12 * (number + 0xd2c): x, y, reach. Each light with |y - light.y| <= reach adds
 * reach - _ftol(1 / (r * (1.5 - 0.5 * d2 * r * r))) to the level 32, clamped per light to 0..223 and stopping at
 * 255, where d2 is the squared distance as a signed int and r the float with the bits 0x5f3759df - (bits of
 * (float)d2 >> 1). The level picks the colour from three ramps of 256 dwords at +0xfe18, lifted by
 * 255 - [this+0x12e54], and *scale multiplies each channel >> 8.
 *
 * The x87 part is computed in doubles under the conditions of the native-double runs (runtime/cpu/x87_fast.h); when
 * they do not hold, or a result is not one such a run would keep, the call is the generated code's. Nothing is
 * written before that is decided: the stack image the function leaves is written once, at the end. That image, from
 * F = ESP - 0x20 (the frame after SUB ESP,0x10 and four pushes): the saved registers at F..F+0xc; the locals x at
 * F+0x14, the list position at F+0x10, the lights done at F+0x18 and their count at F+0x1c; the level in the first
 * argument's slot F+0x24; in the second's, F+0x28, the bits of r of the last light in reach; below F the frame of
 * lightIndex_find (its return address at F-0x18, ESI = 32 at F-0x1c, EDI at F-0x20 when the index is not empty) and
 * above it either that call's five arguments or, after a light in reach, the frame of _ftol (return address, EBP,
 * both control words, the 64-bit result). ECX and EDX are left as the last colour computation leaves them, the
 * flags as the closing ADD ESP,0x10 does. Returns 0 when the call is left to the generated code. */
void region_0041b4a0(X86* c);
static int light_at_41b510(X86* c) {
    const uint32_t entry = c->r[R_ESP], self = c->r[R_ECX], frame = entry - 0x20u;
    if (rd8(self + 0x9e0cu) || !(rd8(self + 0x10u) & 0x20u) || !(rd8(0x0183036cu) & 1u) || rd8(0x0183037cu)) {
        return 0;
    }

    const uint32_t x = rd32(entry + 4), y = rd32(entry + 8), scale = rd32(entry + 0xc);
    const uint32_t begin = rd32(0x01830370u), end = rd32(0x01830374u);
    unsigned work = 35 + 7;
    /* lightIndex_find: the first record whose x is not below ours. */
    const int32_t records = (int32_t)(end - begin) >> 4;
    uint32_t at = begin;
    if (records > 0) {
        work += 3 + 1;
        for (int32_t n = records; n > 0;) {
            const int32_t half = n >> 1;
            const uint32_t middle = at + ((uint32_t)half << 4);
            if ((int32_t)rd32(middle) < (int32_t)x) {
                at = middle + 16;
                n = n - 1 - half;
                work += 9 + 5;
            } else {
                n = half;
                work += 9 + 1;
            }
        }
    }

    work += 2;
    uint32_t sum = 0x20, count = 0, done = 0, list = 0, last_bits = 0;
    int64_t last_result = 0;
    double last_value = 0;
    unsigned in_reach = 0;
    const int interval = at != begin && at != end;
    if (!interval) {
        work += (at == begin ? 4 : 6) + 1;
    } else {
        list = rd32(at - 0xcu);
        count = (uint32_t)((int32_t)(rd32(at - 8u) - list) >> 1);
        work += 13 + (count ? 2 : 1);
    }

    if (count) {
        const double half = rdf32(0x00890670u), three_halves = rdf32(0x008909a8u), one = rdf32(0x0089066cu);
        for (;;) {
            work += 2;
            if ((int32_t)sum >= 0xff) { /* enough light: the rest of the list is not looked at */
                work += 2 + (sum == 0xffu);
                break;
            }

            const uint32_t light = self + ((uint32_t)(int32_t)(int16_t)rd16(list + 2u * done) + 0xd2cu) * 12u;
            const uint32_t light_y = rd32(light + 4), reach = rd32(light + 8);
            const uint32_t away = light_y - y, sign = (uint32_t)((int32_t)away >> 31);
            work += 14;
            if ((int32_t)((away ^ sign) - sign) <= (int32_t)reach) {
                const uint32_t dx = x - rd32(light), dy = y - light_y, d2 = dy * dy + dx * dx;
                const unsigned slot = (c->fpu_top - 1u) & 7u;
                if (RECOMP_UNLIKELY((c->fpu_cw & 0xf3fu) != 0x23fu || !(c->fpu_sw & 0x20u) || c->fpu_pending ||
                                    ftag_of(c, slot) != FTAG_EMPTY)) {
                    return 0;
                }

                const double square = (double)(int32_t)d2;
                const float single = (float)square;
                uint32_t bits;
                memcpy(&bits, &single, 4);
                bits = 0x5f3759dfu - (uint32_t)((int32_t)bits >> 1);
                if (RECOMP_UNLIKELY(!x87f_ok_f32(bits) || !x87f_ok_f32(rd32(0x00890670u)) ||
                                    !x87f_ok_f32(rd32(0x008909a8u)) || !x87f_ok_f32(rd32(0x0089066cu)))) {
                    return 0;
                }

                const double r = x87f_f32(bits), a = square * half, b = a * r, e = b * r, f = three_halves - e;
                const double g = f * r, value = one / g;
                if (RECOMP_UNLIKELY(!(x87f_ok_mul(a, square, half) & x87f_ok_mul(b, a, r) & x87f_ok_mul(e, b, r) &
                                      x87f_ok_add(f) & x87f_ok_mul(g, f, r) & x87f_ok_div(value, one, g)) ||
                                    !(value >= -0x1p63 && value < 0x1p63))) {
                    return 0;
                }

                last_result = (int64_t)value;
                last_value = value;
                last_bits = bits;
                ++in_reach;
                const uint32_t left = reach - (uint32_t)last_result;
                work += 25 + 15 + 3 + 3;
                if ((int32_t)left >= 0xe0) {
                    sum += 0xffu - 0x20u;
                    work += 2;
                } else if ((int32_t)left > 0) {
                    sum += left;
                    work += 3;
                } else {
                    work += 4;
                }
            }

            work += 10;
            if (!(++done < count)) {
                work += 3 + !((int32_t)sum > 0xff);
                break;
            }
        }
    }

    const uint32_t level = (int32_t)sum > 0xff ? 0xffu : sum;
    /* The colour of that level, lifted by the darkness setting. */
    const uint32_t ramp = self + level * 4u, darkness = rd32(self + 0x12e54u);
    uint32_t red = rd32(ramp + 0xfe18u), green = rd32(ramp + 0x10218u), blue = rd32(ramp + 0x10618u);
    work += 3;
    if (darkness == 0xffu) {
        work += 4;
    } else {
        const uint32_t lift = 0xffu - darkness;
        red = (red >> 16) + lift;
        green = (green >> 8) + lift;
        blue += lift;
        work += 18 + (red > 0xffu) + (green > 0xffu) + (blue > 0xffu ? 2 : 0);
        red = (red > 0xffu ? 0xffu : red) << 16;
        green = (green > 0xffu ? 0xffu : green) << 8;
        blue = blue > 0xffu ? 0xffu : blue;
    }

    uint32_t edx = 0;
    work += 3 + 8;
    if (scale) {
        const uint32_t factor = rd32(scale);
        red = ((factor * red) >> 8) & 0xff0000u;
        green = ((factor * green) >> 8) & 0xff00u;
        edx = factor * blue;
        blue = (edx >> 8) & 0xffu;
        work += 15;
    }

    /* Everything is decided: the stack as the function leaves it. */
    wr32(frame + 0xc, c->r[R_EBX]);
    wr32(frame + 8, c->r[R_EBP]);
    wr32(frame + 4, c->r[R_ESI]);
    wr32(frame, c->r[R_EDI]);
    wr32(frame + 0x14, x);
    wr32(frame + 0x24, sum);
    wr32(frame - 0x18u, 0x0041b7ceu);
    wr32(frame - 0x1cu, 0x20u);
    if (records > 0) {
        wr32(frame - 0x20u, c->r[R_EDI]);
    }

    if (interval) {
        wr32(frame + 0x18, done);
        wr32(frame + 0x1c, count);
        if (count) {
            wr32(frame + 0x10, list + 2u * done);
        }
    }

    if (in_reach) {
        /* The last light in reach: the bits of r, the frame of its _ftol call, and that call's x87 state (the
         * popped register keeps the quotient; C1 clear; the pointers of the routine's FISTP). */
        wr32(frame + 0x28, last_bits);
        wr32(frame - 4u, 0x0041b89eu);
        wr32(frame - 8u, self);
        wr16(frame - 10u, c->fpu_cw);
        wr16(frame - 12u, (uint16_t)(c->fpu_cw | 0xc00u));
        wr64(frame - 20u, (uint64_t)last_result);
        c->st[(c->fpu_top - 1u) & 7u] = x87f_ext(last_value);
        c->fpu_sw &= (uint16_t)~0x200u;
        c->fpu_ip = 0x0084a20du;
        c->fpu_opcode = 0x77d;
        c->fpu_cs = c->cs;
        c->fpu_dp = frame - 20u;
        c->fpu_ds = c->ds;
    } else {
        wr32(frame - 4u, 0);
        wr32(frame - 8u, y);
        wr32(frame - 0xcu, frame + 0x14);
        wr32(frame - 0x10u, end);
        wr32(frame - 0x14u, begin);
    }

    c->r[R_EAX] = blue | green | red;
    c->r[R_ECX] = green;
    c->r[R_EDX] = edx;
    c->eip = rd32(entry);
    c->r[R_ESP] = entry + 4 + 0xc;
    flags_add(c, entry - 0x10u, 0x10u);
    c->source_pc = 0x0041b9bfu;
    c->budget += work;
    return 1;
}

static struct {
    unsigned long calls, taken, compared, unfinished;
} g_light;
static void light_report(void) {
    fprintf(stderr, "SACRED_EXE_NATIVE 41b510 calls=%lu taken=%lu compared=%lu unfinished=%lu mismatches=0\n",
            g_light.calls, g_light.taken, g_light.compared, g_light.unfinished);
}

/* SACRED_EXE_NATIVE=verify: the stand-in, then (stack and context put back) the generated code, and everything
 * either leaves compared: registers, flags, EIP, the x87 state, the block accounting and the stack from the lowest
 * byte the function's callees write (F - 0x20) to the end of its arguments. */
static void light_verify(X86* c) {
    enum { below = 0x40, size = 0x50 };
    const X86 before = *c;
    const uint32_t low = c->r[R_ESP] - below;
    uint8_t stack[size], mine_stack[size];
    for (uint32_t i = 0; i < size; ++i) {
        stack[i] = rd8(low + i);
    }

    if ((++g_light.calls & 0x3fffffu) == 0) { /* the game is usually killed, not exited: report as it runs */
        light_report();
    }

    const int taken = light_at_41b510(c);
    const X86 mine = *c;
    for (uint32_t i = 0; i < size; ++i) {
        mine_stack[i] = rd8(low + i);
        wr8(low + i, stack[i]);
    }

    *c = before;
    c->eip = 0x0041b510u;
    region_0041b4a0(c);
    if (!taken) {
        return;
    }

    ++g_light.taken;
    if (c->eip != mine.eip) { /* the generated code handed a call to the driver (direct depth): it goes on there */
        ++g_light.unfinished;
        return;
    }

    ++g_light.compared;
    int bad = memcmp(c->r, mine.r, sizeof c->r) || c->eflags_cf != mine.eflags_cf || c->eflags_zf != mine.eflags_zf ||
              c->eflags_sf != mine.eflags_sf || c->eflags_of != mine.eflags_of || c->eflags_af != mine.eflags_af ||
              c->eflags_pf != mine.eflags_pf || c->eflags_df != mine.eflags_df || c->fpu_top != mine.fpu_top ||
              c->fpu_cw != mine.fpu_cw || c->fpu_sw != mine.fpu_sw || c->fpu_tag != mine.fpu_tag ||
              c->fpu_pending != mine.fpu_pending || c->fpu_opcode != mine.fpu_opcode || c->fpu_cs != mine.fpu_cs ||
              c->fpu_ds != mine.fpu_ds || c->fpu_ip != mine.fpu_ip || c->fpu_dp != mine.fpu_dp ||
              c->source_pc != mine.source_pc;
    /* The budget is equal unless a tick of the generated code offered the baton, which starts it from zero. */
    bad |= c->budget >= mine.budget - before.budget && c->budget != mine.budget;
    for (unsigned i = 0; i < 8; ++i) {
        bad |= c->st[i].signif != mine.st[i].signif || c->st[i].signExp != mine.st[i].signExp;
    }

    for (uint32_t i = 0; i < size; ++i) {
        bad |= rd8(low + i) != mine_stack[i];
    }

    if (bad) {
        fprintf(stderr, "SACRED_EXE_NATIVE mismatch 41b510 (stand-in/generated)");
        for (int i = 0; i < 8; ++i) {
            fprintf(stderr, " r%d %08x/%08x", i, mine.r[i], c->r[i]);
        }

        fprintf(stderr,
                " flags %u%u%u%u%u%u/%u%u%u%u%u%u sw %04x/%04x tag %04x/%04x top %u/%u fip %08x/%08x "
                "fdp %08x/%08x source %08x/%08x budget +%u/+%u stack",
                mine.eflags_cf, mine.eflags_zf, mine.eflags_sf, mine.eflags_of, mine.eflags_af, mine.eflags_pf,
                c->eflags_cf, c->eflags_zf, c->eflags_sf, c->eflags_of, c->eflags_af, c->eflags_pf, mine.fpu_sw,
                c->fpu_sw, mine.fpu_tag, c->fpu_tag, mine.fpu_top, c->fpu_top, mine.fpu_ip, c->fpu_ip, mine.fpu_dp,
                c->fpu_dp, mine.source_pc, c->source_pc, mine.budget - before.budget, c->budget - before.budget);
        for (uint32_t i = 0; i < size; ++i) {
            if (rd8(low + i) != mine_stack[i]) {
                fprintf(stderr, " [esp%+d] %02x/%02x", (int)i - below, mine_stack[i], rd8(low + i));
            }
        }

        fprintf(stderr, "\n");
        abort();
    }
}

void sacred_native_0041b510(X86* c) {
    if (RECOMP_UNLIKELY(verifying())) {
        light_verify(c);
        return;
    }

    if (!light_at_41b510(c)) {
        c->eip = 0x0041b510u;
        region_0041b4a0(c);
    }
}

/* The ground kept in vertex buffers on the renderer (runtime/ddraw/ground_mesh.cpp, SACRED_GROUND_MESH=0 off): the row
 * walk, the layer pass and the quad batcher's adds and texture changes inside the walk. Each returns to the generated
 * code when the ground mesh does not take the call. */
int sacred_ground_row(X86* c);
int sacred_ground_layers(X86* c);
int sacred_ground_skip_quad(X86* c, unsigned argument_bytes);
void region_0062ae4f(X86* c);
void region_0062d2f0(X86* c);
void region_006291c5(X86* c);
void region_00628f7e(X86* c);
/* 0062ae90 cWorldView::renderTileRow(device, RowPos *, detail), thiscall, ret 0xc */
void sacred_native_0062ae90(X86* c) {
    if (sacred_ground_row(c)) {
        return;
    }

    c->eip = 0x0062ae90u;
    region_0062ae4f(c);
}

/* 0062d3c0 cWorldView::drawTileLayers(device), thiscall, ret 4 */
void sacred_native_0062d3c0(X86* c) {
    if (sacred_ground_layers(c)) {
        return;
    }

    c->eip = 0x0062d3c0u;
    region_0062d2f0(c);
}

/* 00629180 cQuadBatcher::add(device, quad), thiscall, ret 8 */
void sacred_native_00629180(X86* c) {
    if (sacred_ground_skip_quad(c, 8)) {
        return;
    }

    c->eip = 0x00629180u;
    region_00628f7e(c);
}

/* 006292c0 cQuadBatcher::setTexture(device, handle), thiscall, ret 8 */
void sacred_native_006292c0(X86* c) {
    if (sacred_ground_skip_quad(c, 8)) {
        return;
    }

    c->eip = 0x006292c0u;
    region_006291c5(c);
}
