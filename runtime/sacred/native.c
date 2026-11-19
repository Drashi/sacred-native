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
