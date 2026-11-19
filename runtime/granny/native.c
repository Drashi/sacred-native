/* Native stand-ins for the hottest granny.dll functions.
 *
 * Granny's vertex and bone arithmetic is x87 code. Even on the native-double runs of the generated code a call costs
 * many times what the same arithmetic costs in C, and a handful of functions are a third of the frame in a crowded
 * scene. Each function here does what the guest function at its address does and is registered in place of that
 * function's generated code (runtime/granny/install.cpp).
 *
 * Contract of a stand-in:
 *   - It runs only on the x87 state the native-double runs require: control word PC = 53 bits, RC = nearest, all
 *     exceptions masked, PE already set, nothing pending, and an empty register stack. Under that state an x87
 *     operation on finite values that stays in the normal double range is the IEEE double operation, and a store to
 *     a float is the double-to-float conversion. The functions below load floats, multiply and add them a few levels
 *     deep, so a result can leave that range only as an infinity or a NaN that came in with the data. Anything else
 *     goes to the generated code.
 *   - Every guest memory value the function writes outside its own stack frame is bit-identical, and written in the
 *     same order relative to its reads (the buffers may overlap). A result that is not finite is not written: the
 *     stand-in leaves the rest of the work to the generated code (NaN signs and payloads differ between x87 and the
 *     host).
 *   - EAX, ECX, EDX, the callee-saved registers, ESP and EIP are left as the function leaves them. Not reproduced:
 *     EFLAGS, the function's own stack frame (dead after the return), the x87 status word (C1 and the sticky bits
 *     other than PE) and the contents of the freed x87 registers.
 *
 * SACRED_GRANNY_NATIVE=verify checks the contract while the game runs: every call is executed by the stand-in, undone,
 * executed by the generated code, and the writes and registers of both are compared (GRANNY_NATIVE lines on stderr).
 * SACRED_GRANNY_NATIVE=0 leaves the generated code in place. */
#include "native.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CMake also passes -ffp-contract=off; gcc does not know the standard pragma (-Werror=unknown-pragmas). */
#ifdef __clang__
#pragma STDC FP_CONTRACT OFF
#endif

int granny_native_mode = GRANNY_NATIVE_OFF;

/* ------------------------------------------------------------------------------------------------ verification */
typedef struct {
    uint32_t address, before, after, order;
    uint8_t bytes;    /* 1 or 4 */
    uint8_t compared; /* 0: scratch the generated code does not have to reproduce (rewritten arguments) */
} GrannyWrite;
typedef struct {
    const char* name;
    unsigned long calls, declined, resumed, abandoned, writes, bad_writes, bad_registers;
} GrannyStat;
static _Thread_local struct {
    GrannyWrite* items;
    unsigned count, capacity;
    int bypass;
} t_log;
static GrannyStat g_stats[16];

static void log_write(uint32_t address, uint32_t value, unsigned bytes, int compared) {
    if (t_log.count == t_log.capacity) {
        t_log.capacity = t_log.capacity ? t_log.capacity * 2 : 4096;
        t_log.items = (GrannyWrite*)realloc(t_log.items, t_log.capacity * sizeof *t_log.items);
        if (!t_log.items) {
            abort();
        }
    }

    GrannyWrite* w = &t_log.items[t_log.count];
    w->address = address;
    w->before = bytes == 4 ? rd32(address) : rd8(address);
    w->after = value;
    w->order = t_log.count++;
    w->bytes = (uint8_t)bytes;
    w->compared = (uint8_t)compared;
}

/* A guest store of the function. */
RECOMP_HOT_INLINE void put32(uint32_t address, uint32_t value) {
    if (RECOMP_UNLIKELY(granny_native_mode == GRANNY_NATIVE_VERIFY)) {
        log_write(address, value, 4, 1);
    }

    wr32(address, value);
}

RECOMP_HOT_INLINE void put8(uint32_t address, uint8_t value) {
    if (RECOMP_UNLIKELY(granny_native_mode == GRANNY_NATIVE_VERIFY)) {
        log_write(address, value, 1, 1);
    }

    wr8(address, value);
}

RECOMP_HOT_INLINE void putf(uint32_t address, float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    put32(address, bits);
}

/* A store that prepares the generated code's run (never compared with what that code leaves there). */
RECOMP_HOT_INLINE void put32_scratch(uint32_t address, uint32_t value) {
    if (RECOMP_UNLIKELY(granny_native_mode == GRANNY_NATIVE_VERIFY)) {
        log_write(address, value, 4, 0);
    }

    wr32(address, value);
}

RECOMP_HOT_INLINE uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    return bits;
}

RECOMP_HOT_INLINE int native_state(const X86* c) {
    return (c->fpu_cw & 0xf3fu) == 0x23fu && (c->fpu_sw & 0x20u) && !c->fpu_pending && c->fpu_tag == 0xffffu;
}

static int by_address(const void* a, const void* b) {
    const GrannyWrite *x = (const GrannyWrite*)a, *y = (const GrannyWrite*)b;
    if (x->address != y->address) {
        return x->address < y->address ? -1 : 1;
    }

    return x->order < y->order ? -1 : x->order > y->order;
}

static void verify(X86* c, int (*implementation)(X86*), GrannyRegion original, unsigned index) {
    GrannyStat* stat = &g_stats[index];
    if (t_log.bypass) {
        t_log.bypass = 0;
        original(c);
        return;
    }

    static unsigned long total; /* The game is usually killed, not exited: report as it runs. */
    if ((++total & 0x3fffu) == 0) {
        granny_native_report();
    }

    ++stat->calls;
    if (!native_state(c)) {
        ++stat->declined;
        original(c);
        return;
    }

    const X86 entry = *c;
    const unsigned base = t_log.count;
    const int done = implementation(c);
    const X86 native = *c;
    const unsigned end = t_log.count;
    for (unsigned i = end; i-- > base;) {
        const GrannyWrite* w = &t_log.items[i];
        if (w->bytes == 4) {
            wr32(w->address, w->before);
        } else {
            wr8(w->address, (uint8_t)w->before);
        }
    }

    *c = entry;
    if (done == 0) {
        ++stat->resumed;
    } else if (done < 0) {
        ++stat->abandoned;
    }

    t_log.bypass = 1;
    if (!sacred_granny_run_original(c) || done < 0) {
        /* An abandoned call left everything to the generated code: nothing of the stand-in to compare. */
        t_log.count = base;
        return;
    }

    /* The generated code may have run other stand-ins, whose records sit above `end` and are gone again. */
    qsort(t_log.items + base, end - base, sizeof *t_log.items, by_address);
    for (unsigned i = base; i < end; ++i) {
        const GrannyWrite* w = &t_log.items[i];
        if (!w->compared || (i + 1 < end && w[1].address == w->address && w[1].compared)) {
            continue;
        }

        ++stat->writes;
        const uint32_t now = w->bytes == 4 ? rd32(w->address) : rd8(w->address);
        if (now != w->after && stat->bad_writes++ < 8) {
            fprintf(stderr, "GRANNY_NATIVE_MISMATCH %s write %08x native=%08x generated=%08x\n", stat->name, w->address,
                    w->after, now);
        }
    }

    t_log.count = base;
    if (done) {
        int bad = c->eip != native.eip;
        for (unsigned r = 0; r < 8; ++r) {
            bad |= c->r[r] != native.r[r];
        }

        if (bad && stat->bad_registers++ < 8) {
            fprintf(stderr,
                    "GRANNY_NATIVE_MISMATCH %s registers native eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x "
                    "esi=%08x edi=%08x eip=%08x generated eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x "
                    "esi=%08x edi=%08x eip=%08x\n",
                    stat->name, native.r[0], native.r[1], native.r[2], native.r[3], native.r[4], native.r[5],
                    native.r[6], native.r[7], native.eip, c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6],
                    c->r[7], c->eip);
        }
    }
}

void granny_native_report(void) {
    if (granny_native_mode != GRANNY_NATIVE_VERIFY) {
        return;
    }

    for (unsigned i = 0; i < granny_native_count; ++i) {
        const GrannyStat* s = &g_stats[i];
        fprintf(stderr,
                "GRANNY_NATIVE_VERIFY %s calls=%lu declined=%lu resumed=%lu abandoned=%lu writes=%lu bad_writes=%lu "
                "bad_registers=%lu\n",
                s->name, s->calls, s->declined, s->resumed, s->abandoned, s->writes, s->bad_writes, s->bad_registers);
    }
}
/* The entry of a stand-in. An implementation returns
 *    1 when it has done the whole call (registers, ESP and EIP as after the RET),
 *    0 when the generated code has to run from the function's entry on the guest state of the moment: untouched,
 *      or with the arguments advanced past the part already done,
 *   -1 when it abandoned the call: the generated code does all of it again from the entry, on guest memory where
 *      the stand-in's writes are taken back or are ones the generated code makes again before it reads them.
 * It changes the guest registers only when it returns 1. */
RECOMP_HOT_INLINE void run(X86* c, int (*implementation)(X86*), GrannyRegion original, unsigned index) {
    if (RECOMP_LIKELY(granny_native_mode == GRANNY_NATIVE_ON)) {
        if (!native_state(c) || implementation(c) <= 0) {
            original(c);
        }

        return;
    }

    verify(c, implementation, original, index);
}
#define STAND_IN(index, address)                                                                                       \
    static GrannyRegion original_##address;                                                                            \
    static int implementation_##address(X86* c);                                                                       \
    static void native_##address(X86* c) {                                                                             \
        run(c, implementation_##address, original_##address, index);                                                   \
    }                                                                                                                  \
    static int implementation_##address(X86* c)

/* The block accounting of the generated code (seh.h), without yielding in the middle of a function. */
RECOMP_HOT_INLINE void charge(X86* c, uint32_t address, uint32_t instructions) {
    c->source_pc = address;
    c->budget += instructions;
}

RECOMP_HOT_INLINE int finite3(double x, double y, double z) {
    return isfinite(x) && isfinite(y) && isfinite(z);
}

/* The few writes of a short function, so that it can take them back and leave the call to the generated code. */
typedef struct {
    uint32_t address[24], before[24];
    unsigned count;
} Undo;
RECOMP_HOT_INLINE void keep32(Undo* u, uint32_t address, uint32_t value) {
    u->address[u->count] = address;
    u->before[u->count++] = rd32(address);
    put32(address, value);
}

RECOMP_HOT_INLINE void keepf(Undo* u, uint32_t address, float value) {
    keep32(u, address, float_bits(value));
}

static int undo(Undo* u) {
    while (u->count) {
        --u->count;
        put32_scratch(u->address[u->count], u->before[u->count]);
    }

    return 0;
}

/* cdecl return: the caller removes the arguments. */
RECOMP_HOT_INLINE int leave(X86* c, uint32_t esp, uint32_t address, uint32_t instructions) {
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 4;
    charge(c, address, instructions);
    return 1;
}
/* The writes of a function that does the work of the functions it calls as well. A NaN result abandons the call
 * (the stand-in returns -1): from then on nothing more is written, and with `keep` the writes made so far are taken
 * back. Without `keep` the function must be one whose run from the entry makes every write again before it reads
 * the location. Writes into the function's own stack frame [frame_low, frame_high) are dead after the return and
 * are not taken back. */
typedef struct {
    uint32_t frame_low, frame_high;
    int keep, nan;
    unsigned count;
    uint32_t address[64], before[64];
} Call;
RECOMP_HOT_INLINE void call_put32(Call* k, uint32_t address, uint32_t value) {
    if (RECOMP_UNLIKELY(k->nan)) {
        return;
    }

    if (address - k->frame_low < k->frame_high - k->frame_low) {
        put32_scratch(address, value);
        return;
    }

    if (k->keep) {
        if (RECOMP_UNLIKELY(k->count == 64)) {
            abort();
        }

        k->address[k->count] = address;
        k->before[k->count++] = rd32(address);
    }

    put32(address, value);
}

RECOMP_HOT_INLINE void call_putf(Call* k, uint32_t address, double value) {
    const float f = (float)value;
    if (RECOMP_UNLIKELY(isnan(f))) {
        k->nan = 1;
        return;
    }

    call_put32(k, address, float_bits(f));
}

/* A float copied through the x87 stack: a NaN would come out quieted. */
RECOMP_HOT_INLINE void call_copyf(Call* k, uint32_t address, uint32_t bits) {
    if (RECOMP_UNLIKELY((bits & 0x7fffffffu) > 0x7f800000u)) {
        k->nan = 1;
        return;
    }

    call_put32(k, address, bits);
}

static int call_abandon(Call* k) {
    while (k->count) {
        --k->count;
        put32_scratch(k->address[k->count], k->before[k->count]);
    }

    return -1;
}

RECOMP_HOT_INLINE uint32_t with_al(uint32_t eax, uint8_t al) {
    return (eax & ~0xffu) | al;
}

/* ------------------------------------------------------------------------------------------- skinning: positions
 * 1001df00  void __thiscall(this, count, const float (*source)[3], const uint32_t *weights, float (*out)[3]); ret 16
 * For each vertex: out = sum over its bones of weight * (rotation * source + translation), in doubles, starting
 * from [1005c250] (0.0f). `weights` is one stream for all vertices: a bone count, then (bone index, weight) pairs.
 * A bone is 124 bytes in the array at [this + 0x14]: 3x3 rotation at +0x4c, translation at +0x70. */
/* The arguments of a skinning call while it runs, and the ECX and EDX the loop leaves. */
typedef struct {
    uint32_t count, source, weights, out, ecx, edx;
    unsigned work;
} Skin;
/* 1 when every vertex is done; 0 at the first vertex whose result is not finite, which is not written. */
static int skin_positions(uint32_t self, Skin* s) {
    if (!s->count) {
        return 1;
    }

    const double zero = rdf32(0x1005c250u);
    const uint32_t bones = rd32(self + 0x14);
    do {
        const uint32_t xb = rd32(s->source), yb = rd32(s->source + 4), zb = rd32(s->source + 8);
        float xf, yf, zf;
        memcpy(&xf, &xb, 4), memcpy(&yf, &yb, 4), memcpy(&zf, &zb, 4);
        const double x = xf, y = yf, z = zf;
        double a1 = zero, a2 = zero, a3 = zero;
        uint32_t n = rd32(s->weights), at = s->weights + 4, ecx = yb;
        for (uint32_t i = 0; i < n; ++i, at += 8) {
            const uint32_t index = rd32(at);
            const double w = rdf32(at + 4);
            const uint32_t bone = bones + (index * 31u) * 4u;
            ecx = index * 31u;
            a1 += (x * rdf32(bone + 0x4c) + y * rdf32(bone + 0x50) + z * rdf32(bone + 0x54) + rdf32(bone + 0x70)) * w;
            a2 += (x * rdf32(bone + 0x58) + y * rdf32(bone + 0x5c) + z * rdf32(bone + 0x60) + rdf32(bone + 0x74)) * w;
            a3 += (x * rdf32(bone + 0x64) + y * rdf32(bone + 0x68) + z * rdf32(bone + 0x6c) + rdf32(bone + 0x78)) * w;
        }

        if (RECOMP_UNLIKELY(!finite3(a1, a2, a3))) {
            return 0;
        }

        putf(s->out, (float)a1);
        put32(s->out + 8, float_bits((float)a3));
        putf(s->out + 4, (float)a2);
        s->work += 27 + 44 * n;
        s->ecx = ecx;
        s->weights = at, s->source += 12, s->out += 12;
    } while (--s->count);
    s->edx = s->weights;
    return 1;
}

STAND_IN(0, 1001df00) {
    const uint32_t esp = c->r[R_ESP], self = c->r[R_ECX];
    Skin s = {rd32(esp + 4), rd32(esp + 8), rd32(esp + 12), rd32(esp + 16), self, 0, 6};
    s.edx = s.weights;
    if (RECOMP_UNLIKELY(!skin_positions(self, &s))) {
        /* The generated code does this vertex and the rest: same call, arguments advanced. */
        put32_scratch(esp + 4, s.count), put32_scratch(esp + 8, s.source);
        put32_scratch(esp + 12, s.weights), put32_scratch(esp + 16, s.out);
        charge(c, 0x1001df00u, s.work);
        return 0;
    }

    c->r[R_EAX] = 0;
    c->r[R_ECX] = s.ecx;
    c->r[R_EDX] = s.edx;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 4 + 16;
    charge(c, 0x1001df00u, s.work);
    return 1;
}

/* --------------------------------------------------------------------------------------------- skinning: normals
 * 1001e490  void __thiscall(this, count, const float (*source)[3], const uint32_t *const *weights, float (*out)[3]);
 * ret 16. As above without the translation; `weights` holds one pointer per vertex to its own bone count and
 * (bone index, weight) pairs. */
static int skin_normals(uint32_t self, Skin* s) {
    if (!s->count) {
        return 1;
    }

    const double zero = rdf32(0x1005c250u);
    const uint32_t bones = rd32(self + 0x14);
    do {
        const float xf = rdf32(s->source);
        const uint32_t list = rd32(s->weights);
        const uint32_t n = rd32(list);
        const float yf = rdf32(s->source + 4), zf = rdf32(s->source + 8);
        const double x = xf, y = yf, z = zf;
        double a1 = zero, a2 = zero, a3 = zero;
        uint32_t at = list + 4;
        for (uint32_t i = 0; i < n; ++i, at += 8) {
            const uint32_t index = rd32(at);
            const double w = rdf32(at + 4);
            const uint32_t bone = bones + (index * 31u) * 4u + 0x4c;
            a1 += (y * rdf32(bone + 4) + x * rdf32(bone) + z * rdf32(bone + 8)) * w;
            a2 += (x * rdf32(bone + 12) + y * rdf32(bone + 16) + z * rdf32(bone + 20)) * w;
            a3 += (x * rdf32(bone + 24) + y * rdf32(bone + 28) + z * rdf32(bone + 32)) * w;
        }

        if (RECOMP_UNLIKELY(!finite3(a1, a2, a3))) {
            return 0;
        }

        s->edx = float_bits((float)a3);
        putf(s->out, (float)a1);
        put32(s->out + 8, s->edx);
        putf(s->out + 4, (float)a2);
        s->work += 30 + 35 * n;
        s->weights += 4, s->source += 12, s->out += 12;
        s->ecx = s->out;
    } while (--s->count);
    return 1;
}

STAND_IN(1, 1001e490) {
    const uint32_t esp = c->r[R_ESP], self = c->r[R_ECX];
    Skin s = {rd32(esp + 4), rd32(esp + 8), rd32(esp + 12), rd32(esp + 16), self, c->r[R_EDX], 5};
    if (RECOMP_UNLIKELY(!skin_normals(self, &s))) {
        put32_scratch(esp + 4, s.count), put32_scratch(esp + 8, s.source);
        put32_scratch(esp + 12, s.weights), put32_scratch(esp + 16, s.out);
        charge(c, 0x1001e490u, s.work);
        return 0;
    }

    c->r[R_EAX] = 0;
    c->r[R_ECX] = s.ecx;
    c->r[R_EDX] = s.edx;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 4 + 16;
    charge(c, 0x1001e490u, s.work);
    return 1;
}

/* ------------------------------------------------------------------------------------------------ small kernels
 * 10021540  void __cdecl(const float matrix[9], float vector[3]): vector = matrix * vector, in place. The vector is
 * read once; each row is read when it is used and its result stored before the next row is read. */
STAND_IN(2, 10021540) {
    const uint32_t esp = c->r[R_ESP], matrix = rd32(esp + 4), vector = rd32(esp + 8);
    const double v0 = rdf32(vector), v1 = rdf32(vector + 4), v2 = rdf32(vector + 8);
    Undo u = {.count = 0};
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t row = matrix + 12 * i;
        const double value = v2 * rdf32(row + 8) + v1 * rdf32(row + 4) + v0 * rdf32(row);
        if (RECOMP_UNLIKELY(isnan(value))) {
            return undo(&u);
        }

        keepf(&u, vector + 4 * i, (float)value);
    }

    c->r[R_EAX] = matrix + 36;
    c->r[R_ECX] = vector + 12;
    c->r[R_EDX] = 0;
    return leave(c, esp, 0x10021540u, 49);
}
/* 10021460  void __cdecl(const float a[9], const float b[9], float out[9]): out = a * b, row by row, every operand
 * read from memory when it is used (the buffers may overlap). */
STAND_IN(3, 10021460) {
    const uint32_t esp = c->r[R_ESP], a = rd32(esp + 4), b = rd32(esp + 8), out = rd32(esp + 12);
    Undo u = {.count = 0};
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t row = a + 12 * i, to = out + 12 * i;
        const double e0 = (double)rdf32(row) * rdf32(b) + (double)rdf32(b + 12) * rdf32(row + 4) +
                          (double)rdf32(b + 24) * rdf32(row + 8);
        if (RECOMP_UNLIKELY(isnan(e0))) {
            return undo(&u);
        }

        keepf(&u, to, (float)e0);
        const double e1 = (double)rdf32(row) * rdf32(b + 4) + (double)rdf32(row + 8) * rdf32(b + 28) +
                          (double)rdf32(row + 4) * rdf32(b + 16);
        if (RECOMP_UNLIKELY(isnan(e1))) {
            return undo(&u);
        }

        keepf(&u, to + 4, (float)e1);
        const double e2 = (double)rdf32(b + 32) * rdf32(row + 8) + (double)rdf32(b + 20) * rdf32(row + 4) +
                          (double)rdf32(b + 8) * rdf32(row);
        if (RECOMP_UNLIKELY(isnan(e2))) {
            return undo(&u);
        }

        keepf(&u, to + 8, (float)e2);
    }

    c->r[R_EAX] = a + 36;
    c->r[R_ECX] = b;
    c->r[R_EDX] = out + 36;
    return leave(c, esp, 0x10021460u, 94);
}

/* 100214d0  void __cdecl(const float a[9], const float b[9], float out[9]): out = transpose(a) * b, as above. */
STAND_IN(4, 100214d0) {
    const uint32_t esp = c->r[R_ESP], a = rd32(esp + 4), b = rd32(esp + 8), out = rd32(esp + 12);
    Undo u = {.count = 0};
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t column = a + 4 * i, to = out + 12 * i;
        const double e0 = (double)rdf32(column) * rdf32(b) + (double)rdf32(b + 12) * rdf32(column + 12) +
                          (double)rdf32(b + 24) * rdf32(column + 24);
        if (RECOMP_UNLIKELY(isnan(e0))) {
            return undo(&u);
        }

        keepf(&u, to, (float)e0);
        const double e1 = (double)rdf32(column) * rdf32(b + 4) + (double)rdf32(column + 24) * rdf32(b + 28) +
                          (double)rdf32(column + 12) * rdf32(b + 16);
        if (RECOMP_UNLIKELY(isnan(e1))) {
            return undo(&u);
        }

        keepf(&u, to + 4, (float)e1);
        const double e2 = (double)rdf32(column + 24) * rdf32(b + 32) + (double)rdf32(b + 20) * rdf32(column + 12) +
                          (double)rdf32(b + 8) * rdf32(column);
        if (RECOMP_UNLIKELY(isnan(e2))) {
            return undo(&u);
        }

        keepf(&u, to + 8, (float)e2);
    }

    c->r[R_EAX] = a + 12;
    c->r[R_ECX] = b;
    c->r[R_EDX] = out + 36;
    return leave(c, esp, 0x100214d0u, 94);
}
/* 10009d30  void __cdecl(float out[9], const float a[9], const float b[9]): out = a * b through a temporary, so
 * every operand is read before the first result is stored. */
STAND_IN(5, 10009d30) {
    const uint32_t esp = c->r[R_ESP], out = rd32(esp + 4), a = rd32(esp + 8), b = rd32(esp + 12);
    float result[9];
    for (uint32_t k = 0; k < 3; ++k) {
        for (uint32_t j = 0; j < 3; ++j) {
            const double value = (double)rdf32(b + 4 * j) * rdf32(a + 12 * k) +
                                 (double)rdf32(a + 12 * k + 4) * rdf32(b + 4 * j + 12) +
                                 (double)rdf32(a + 12 * k + 8) * rdf32(b + 4 * j + 24);
            if (RECOMP_UNLIKELY(isnan(value))) {
                return 0;
            }

            result[3 * k + j] = (float)value;
        }
    }

    for (uint32_t i = 0; i < 9; ++i) {
        putf(out + 4 * i, result[i]);
    }

    c->r[R_EAX] = out;
    c->r[R_ECX] = 0;
    c->r[R_EDX] = 3;
    return leave(c, esp, 0x10009d30u, 170);
}
/* The rotation matrix of a quaternion as 10009ad0 and 10021250 compute it: the squares and x*y, z*w stay doubles,
 * the other four products go through float temporaries. Zero when a result is a NaN. */
RECOMP_HOT_INLINE int quaternion_matrix(float q0, float q1, float q2, float q3, float m[9]) {
    const double one = rdf32(0x1005c0fcu);
    const double xx = (double)q0 * q0, yy = (double)q1 * q1, zz = (double)q2 * q2, xy = (double)q0 * q1;
    const float xz = (float)((double)q2 * q0), yz = (float)((double)q2 * q1), xw = (float)((double)q3 * q0),
                yw = (float)((double)q3 * q1);
    const double zw = (double)q3 * q2;
    double t;
    t = zz + yy, m[0] = (float)(one - (t + t));
    t = xy - zw, m[1] = (float)(t + t);
    t = (double)yw + xz, m[2] = (float)(t + t);
    t = zw + xy, m[3] = (float)(t + t);
    t = zz + xx, m[4] = (float)(one - (t + t));
    t = (double)yz - xw, m[5] = (float)(t + t);
    t = (double)xz - yw, m[6] = (float)(t + t);
    t = (double)xw + yz, m[7] = (float)(t + t);
    t = yy + xx, m[8] = (float)(one - (t + t));
    for (unsigned i = 0; i < 9; ++i) {
        if (RECOMP_UNLIKELY(isnan(m[i]))) {
            return 0;
        }
    }

    return 1;
}

/* 10009ad0  void __cdecl(float out[9], const float quaternion[4]). */
STAND_IN(6, 10009ad0) {
    const uint32_t esp = c->r[R_ESP], out = rd32(esp + 4), q = rd32(esp + 8);
    float m[9];
    if (RECOMP_UNLIKELY(!quaternion_matrix(rdf32(q), rdf32(q + 4), rdf32(q + 8), rdf32(q + 12), m))) {
        return 0;
    }

    for (uint32_t i = 0; i < 9; ++i) {
        putf(out + 4 * i, m[i]);
    }

    c->r[R_EAX] = out;
    c->r[R_ECX] = 0;
    return leave(c, esp, 0x10009ad0u, 76);
}
/* 10021250  void __cdecl(float *to, const float *from). `from`: translation[3], quaternion[4], matrix[9] at +0x1c.
 * `to`: quaternion[4], translation[3] at +0x10, matrix[9] at +0x1c. The quaternion is copied, renormalised by
 * (3 - |q|^2) / 2 in place, turned into a rotation matrix and multiplied by the matrix of `from`. */
STAND_IN(7, 10021250) {
    const uint32_t esp = c->r[R_ESP], to = rd32(esp + 4), from = rd32(esp + 8);
    Undo u = {.count = 0};
    for (uint32_t i = 0; i < 4; ++i) {
        keep32(&u, to + 4 * i, rd32(from + 12 + 4 * i));
    }

    const double q0 = rdf32(to), q1 = rdf32(to + 4), q2 = rdf32(to + 8), q3 = rdf32(to + 12);
    const double factor = (rdf32(0x1005c258u) - (q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3)) * rdf32(0x1005c254u);
    for (uint32_t i = 0; i < 4; ++i) {
        const double value = factor * rdf32(to + 4 * i);
        if (RECOMP_UNLIKELY(isnan(value))) {
            return undo(&u);
        }

        keepf(&u, to + 4 * i, (float)value);
    }

    float rotation[9], product[9];
    if (RECOMP_UNLIKELY(!quaternion_matrix(rdf32(to), rdf32(to + 4), rdf32(to + 8), rdf32(to + 12), rotation))) {
        return undo(&u);
    }

    for (uint32_t k = 0; k < 3; ++k) {
        for (uint32_t j = 0; j < 3; ++j) {
            const double value = (double)rdf32(from + 0x1c + 4 * j) * rotation[3 * k] +
                                 (double)rdf32(from + 0x28 + 4 * j) * rotation[3 * k + 1] +
                                 (double)rdf32(from + 0x34 + 4 * j) * rotation[3 * k + 2];
            if (RECOMP_UNLIKELY(isnan(value))) {
                return undo(&u);
            }

            product[3 * k + j] = (float)value;
        }
    }

    const uint32_t t0 = rd32(from);
    for (uint32_t i = 0; i < 9; ++i) {
        keepf(&u, to + 0x1c + 4 * i, product[i]);
    }

    const uint32_t t1 = rd32(from + 4);
    keep32(&u, to + 0x10, t0);
    const uint32_t t2 = rd32(from + 8);
    keep32(&u, to + 0x14, t1);
    keep32(&u, to + 0x18, t2);
    c->r[R_EAX] = t2;
    c->r[R_ECX] = t1;
    c->r[R_EDX] = to + 0x10;
    return leave(c, esp, 0x10021250u, 330);
}
/* ---------------------------------------------------------------------------------------------- fill and copy
 * 100435a0  void __cdecl(void *to, uint8_t value, uint32_t bytes): REP STOSD then REP STOSB. */
STAND_IN(8, 100435a0) {
    const uint32_t esp = c->r[R_ESP], bytes = rd32(esp + 12);
    if (c->eflags_df) {
        return 0;
    }

    if (bytes) {
        const uint32_t pattern = (rd32(esp + 8) & 0xffu) * 0x01010101u;
        uint32_t to = rd32(esp + 4);
        for (uint32_t n = bytes >> 2; n; --n, to += 4) {
            put32(to, pattern);
        }

        for (uint32_t n = bytes & 3u; n; --n, ++to) {
            put8(to, (uint8_t)pattern);
        }

        c->r[R_EAX] = pattern;
        c->r[R_EDX] = bytes;
    }

    c->r[R_ECX] = 0;
    return leave(c, esp, 0x100435a0u, 21 + (bytes >> 2));
}

/* 10043570  void __cdecl(void *to, const void *from, uint32_t bytes): REP MOVSD then REP MOVSB, ascending. */
STAND_IN(9, 10043570) {
    const uint32_t esp = c->r[R_ESP], bytes = rd32(esp + 12);
    if (c->eflags_df) {
        return 0;
    }

    if (bytes) {
        uint32_t to = rd32(esp + 4), from = rd32(esp + 8);
        for (uint32_t n = bytes >> 2; n; --n, to += 4, from += 4) {
            put32(to, rd32(from));
        }

        for (uint32_t n = bytes & 3u; n; --n, ++to, ++from) {
            put8(to, rd8(from));
        }

        c->r[R_EAX] = bytes;
    }

    c->r[R_ECX] = 0;
    return leave(c, esp, 0x10043570u, 16 + (bytes >> 2));
}

/* ------------------------------------------------------------------------------- the kernels inside larger calls
 * 10021460, 100214d0 and 10021540 as above, writing through a Call. */
static void call_mat3_mul(Call* k, uint32_t a, uint32_t b, uint32_t out) {
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t row = a + 12 * i, to = out + 12 * i;
        call_putf(k, to,
                  (double)rdf32(row) * rdf32(b) + (double)rdf32(b + 12) * rdf32(row + 4) +
                      (double)rdf32(b + 24) * rdf32(row + 8));
        call_putf(k, to + 4,
                  (double)rdf32(row) * rdf32(b + 4) + (double)rdf32(row + 8) * rdf32(b + 28) +
                      (double)rdf32(row + 4) * rdf32(b + 16));
        call_putf(k, to + 8,
                  (double)rdf32(b + 32) * rdf32(row + 8) + (double)rdf32(b + 20) * rdf32(row + 4) +
                      (double)rdf32(b + 8) * rdf32(row));
    }
}

static void call_mat3_tmul(Call* k, uint32_t a, uint32_t b, uint32_t out) {
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t column = a + 4 * i, to = out + 12 * i;
        call_putf(k, to,
                  (double)rdf32(column) * rdf32(b) + (double)rdf32(b + 12) * rdf32(column + 12) +
                      (double)rdf32(b + 24) * rdf32(column + 24));
        call_putf(k, to + 4,
                  (double)rdf32(column) * rdf32(b + 4) + (double)rdf32(column + 24) * rdf32(b + 28) +
                      (double)rdf32(column + 12) * rdf32(b + 16));
        call_putf(k, to + 8,
                  (double)rdf32(column + 24) * rdf32(b + 32) + (double)rdf32(b + 20) * rdf32(column + 12) +
                      (double)rdf32(b + 8) * rdf32(column));
    }
}

static void call_mat3_vec(Call* k, uint32_t matrix, uint32_t vector) {
    const double v0 = rdf32(vector), v1 = rdf32(vector + 4), v2 = rdf32(vector + 8);
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t row = matrix + 12 * i;
        call_putf(k, vector + 4 * i, v2 * rdf32(row + 8) + v1 * rdf32(row + 4) + v0 * rdf32(row));
    }
}

/* 10009ad0 through a Call. */
static void call_quaternion_matrix(Call* k, uint32_t out, uint32_t q) {
    float m[9];
    if (RECOMP_UNLIKELY(!quaternion_matrix(rdf32(q), rdf32(q + 4), rdf32(q + 8), rdf32(q + 12), m))) {
        k->nan = 1;
    }

    for (uint32_t i = 0; i < 9; ++i) {
        call_put32(k, out + 4 * i, float_bits(m[i]));
    }
}

/* ----------------------------------------------------------------------------------- composing bone transforms
 * A transform here is translation[3] at +0, quaternion[4] at +0xc and a 3x3 matrix at +0x1c.
 *
 * 10020990  void __thiscall(transform *this, const transform *by); ret 4. this.quaternion = this.quaternion * by.q,
 * this.translation = by.matrix * this.translation + by.translation, this.matrix = by.matrix * this.matrix (through
 * a temporary and 10043570). */
STAND_IN(10, 10020990) {
    const uint32_t esp = c->r[R_ESP], t = c->r[R_ECX], a = rd32(esp + 4);
    Call k = {.keep = 1};
    const double q0 = rdf32(t), q1 = rdf32(t + 4), q2 = rdf32(t + 8), q3 = rdf32(t + 12);
    call_putf(&k, t, ((q2 * rdf32(a + 4) - q1 * rdf32(a + 8)) + q3 * rdf32(a)) + q0 * rdf32(a + 12));
    call_putf(&k, t + 4, ((q0 * rdf32(a + 8) - q2 * rdf32(a)) + q3 * rdf32(a + 4)) + q1 * rdf32(a + 12));
    call_putf(&k, t + 8, ((q1 * rdf32(a) - q0 * rdf32(a + 4)) + q3 * rdf32(a + 8)) + q2 * rdf32(a + 12));
    call_putf(&k, t + 12, q3 * rdf32(a + 12) - ((q2 * rdf32(a + 8) + q1 * rdf32(a + 4)) + q0 * rdf32(a)));
    const uint32_t m = a + 0x1c, n = t + 0x1c;
    const double x = rdf32(t + 0x10), y = rdf32(t + 0x14), z = rdf32(t + 0x18);
    for (uint32_t i = 0; i < 3; ++i) {
        call_putf(&k, t + 0x10 + 4 * i,
                  (z * rdf32(m + 12 * i + 8) + y * rdf32(m + 12 * i + 4)) + x * rdf32(m + 12 * i));
    }

    for (uint32_t i = 0; i < 3; ++i) {
        call_putf(&k, t + 0x10 + 4 * i, (double)rdf32(a + 0x10 + 4 * i) + rdf32(t + 0x10 + 4 * i));
    }

    float product[9];
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t row = m + 12 * i;
        const double e0 = ((double)rdf32(row + 4) * rdf32(n + 12) + (double)rdf32(n + 24) * rdf32(row + 8)) +
                          (double)rdf32(row) * rdf32(n);
        const double e1 = ((double)rdf32(n + 28) * rdf32(row + 8) + (double)rdf32(row + 4) * rdf32(n + 16)) +
                          (double)rdf32(row) * rdf32(n + 4);
        const double e2 = ((double)rdf32(row + 4) * rdf32(n + 20) + (double)rdf32(n + 8) * rdf32(row)) +
                          (double)rdf32(n + 32) * rdf32(row + 8);
        product[3 * i] = (float)e0, product[3 * i + 1] = (float)e1, product[3 * i + 2] = (float)e2;
    }

    for (uint32_t i = 0; i < 9; ++i) {
        call_putf(&k, n + 4 * i, product[i]);
    }

    if (RECOMP_UNLIKELY(k.nan)) {
        return call_abandon(&k);
    }

    c->r[R_EAX] = 0x24;
    c->r[R_ECX] = 0;
    c->r[R_EDX] = n;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 8;
    charge(c, 0x10020990u, 215);
    return 1;
}
/* 10020e10  void __thiscall(transform *this, const transform *parent); ret 4. The quaternion becomes
 * parent.q * this.q renormalised by (3 - |q|^2) / 2 (the fourth component kept in double); the matrix becomes
 * parent.matrix * R * this.matrix * transpose(R) with R the rotation of the new quaternion; the translation becomes
 * parent.matrix * this.translation, then turned by the rotation of parent.q (10009ad0's), plus parent.translation.
 * It keeps three 3x3 matrices in its frame and runs 10009ad0, 10021460, 100214d0, 10043570 and 10021540 on them;
 * the stand-in does the same in the same frame. */
STAND_IN(11, 10020e10) {
    const uint32_t esp = c->r[R_ESP], t = c->r[R_ECX], b = rd32(esp + 4);
    const uint32_t frame = esp - 0x8c, m = frame + 0x20, p = frame + 0x44, parent_rotation = frame + 0x68;
    const uint32_t n = t + 0x1c;
    Call k = {.frame_low = frame, .frame_high = esp, .keep = 1};
    const double a0 = rdf32(t + 0xc), a1 = rdf32(t + 0x10), a2 = rdf32(t + 0x14), a3 = rdf32(t + 0x18);
    const float r0 =
        (float)(((a2 * rdf32(b + 0x10) - a1 * rdf32(b + 0x14)) + a0 * rdf32(b + 0x18)) + a3 * rdf32(b + 0xc));
    const float r1 =
        (float)(((a0 * rdf32(b + 0x14) - a2 * rdf32(b + 0xc)) + a1 * rdf32(b + 0x18)) + a3 * rdf32(b + 0x10));
    const float r2 =
        (float)(((a1 * rdf32(b + 0xc) - a0 * rdf32(b + 0x10)) + a2 * rdf32(b + 0x18)) + a3 * rdf32(b + 0x14));
    const double r3 = a3 * rdf32(b + 0x18) - ((a0 * rdf32(b + 0xc) + a2 * rdf32(b + 0x14)) + a1 * rdf32(b + 0x10));
    const double scale =
        (rdf32(0x1005c258u) - (((r3 * r3 + (double)r2 * r2) + (double)r1 * r1) + (double)r0 * r0)) * rdf32(0x1005c254u);
    call_putf(&k, t + 0xc, r0 * scale);
    call_putf(&k, t + 0x10, r1 * scale);
    call_putf(&k, t + 0x14, r2 * scale);
    call_putf(&k, t + 0x18, r3 * scale);
    call_quaternion_matrix(&k, parent_rotation, b + 0xc);
    call_quaternion_matrix(&k, m, t + 0xc);
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t row = m + 12 * i, to = p + 12 * i;
        call_putf(&k, to,
                  ((double)rdf32(row + 4) * rdf32(n + 12) + (double)rdf32(n + 24) * rdf32(row + 8)) +
                      (double)rdf32(row) * rdf32(n));
        call_putf(&k, to + 4,
                  ((double)rdf32(row + 4) * rdf32(n + 16) + (double)rdf32(row + 8) * rdf32(n + 28)) +
                      (double)rdf32(row) * rdf32(n + 4));
        call_putf(&k, to + 8,
                  ((double)rdf32(row + 4) * rdf32(n + 20) + (double)rdf32(n + 8) * rdf32(row)) +
                      (double)rdf32(n + 32) * rdf32(row + 8));
    }

    call_mat3_mul(&k, b + 0x1c, p, n);
    call_mat3_tmul(&k, m, n, p);
    for (uint32_t i = 0; i < 9; ++i) {
        call_put32(&k, n + 4 * i, rd32(p + 4 * i));
    }

    call_mat3_vec(&k, b + 0x1c, t);
    call_mat3_vec(&k, parent_rotation, t);
    for (uint32_t i = 0; i < 3; ++i) {
        call_putf(&k, t + 4 * i, (double)rdf32(b + 4 * i) + rdf32(t + 4 * i));
    }

    if (RECOMP_UNLIKELY(k.nan)) {
        return call_abandon(&k);
    }

    c->r[R_EAX] = esp;
    c->r[R_ECX] = t + 12;
    c->r[R_EDX] = 0;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 8;
    charge(c, 0x10020e10u, 640);
    return 1;
}

/* ------------------------------------------------------------------------------------------- deforming a mesh
 * EAX, ECX and EDX while a stand-in does the work of the functions its function calls. */
typedef struct {
    uint32_t eax, ecx, edx;
} Regs;
/* 100435a0 with value 0 and a multiple of 4 bytes. */
static void call_zero(Call* k, uint32_t to, uint32_t bytes, Regs* r) {
    r->ecx = 0;
    if (!bytes) {
        return;
    }

    uint32_t at = to, end = to + (bytes & ~3u);
    /* Whole writable pages at a time when nothing has to be recorded: the stores of a call that keeps no undo log,
     * outside its own frame, outside verification and watches. */
    if (!k->keep && !k->nan && granny_native_mode == GRANNY_NATIVE_ON && !g_watch_len && end > at &&
        (end <= k->frame_low || at >= k->frame_high)) {
        while (at < end) {
            uint32_t n = 0x1000u - (at & 0xfffu);
            if (n > end - at) {
                n = end - at;
            }

            if (!(g_page_access[at >> 12] & 2u)) {
                break;
            }

            memset(g_mem + at, 0, n);
            if (g_dirty_count != 0) {
                recomp_dirty(at, n);
            }

            at += n;
        }
    }

    for (; at < end; at += 4) {
        call_put32(k, at, 0);
    }

    r->eax = 0;
    r->edx = bytes;
}
/* 1001de00  void __cdecl(const float t[3], const float m[9], const uint32_t *groups, const float (*from)[3],
 * float (*to)[3], uint32_t dwords). For each group of the list (a vertex offset from where the previous group
 * ended, a vertex count, that many weights): to[v] += (m * from[v] + t) * weight. */
static void call_deform(Call* k, uint32_t t, uint32_t m, uint32_t groups, uint32_t from, uint32_t to, uint32_t dwords,
                        Regs* r, unsigned* work) {
    r->eax = dwords;
    *work += 3;
    if (!dwords) {
        return;
    }

    r->eax = m;
    unsigned guard = 0;
    do {
        const uint32_t skip = rd32(groups), n = rd32(groups + 4);
        groups += 8;
        dwords += 0xfffffffeu - n;
        from += skip * 12, to += skip * 12;
        for (uint32_t j = 0; j < n; ++j, groups += 4, from += 12, to += 12) {
            const double x = rdf32(from), y = rdf32(from + 4), z = rdf32(from + 8), w = rdf32(groups);
            for (uint32_t i = 0; i < 3; ++i) {
                const uint32_t row = m + 12 * i;
                call_putf(k, to + 4 * i,
                          ((((x * rdf32(row) + y * rdf32(row + 4)) + z * rdf32(row + 8)) + rdf32(t + 4 * i)) * w) +
                              rdf32(to + 4 * i));
            }
        }

        *work += 17 + 41 * n;
        if (RECOMP_UNLIKELY(++guard == 1u << 20)) { /* a list that does not add up: the generated code's problem */
            k->nan = 1;
        }
    } while (dwords && !k->nan);
    r->ecx = to;
    r->edx = from;
}
/* 1001e050 (with the translation) and 1001e0e0 (without)  void __thiscall(mesh, uint32_t bone, uint32_t count,
 * const float (*from)[3], float (*to)[3]); ret 16: the vertices of a mesh bound to one bone. */
static void call_rigid(Call* k, uint32_t self, uint32_t bone, uint32_t count, uint32_t from, uint32_t to, int translate,
                       Regs* r, unsigned* work) {
    *work += 4;
    if (!count) {
        return;
    }

    const uint32_t b = rd32(self + 0x14) + bone * 124, m = b + 0x4c;
    for (; count; --count, from += 12, to += 12) {
        const double x = rdf32(from), y = rdf32(from + 4), z = rdf32(from + 8);
        for (uint32_t i = 0; i < 3; ++i) {
            const uint32_t row = m + 12 * i;
            double v = (x * rdf32(row) + y * rdf32(row + 4)) + z * rdf32(row + 8);
            if (translate) {
                v += rdf32(b + 0x70 + 4 * i);
            }

            call_putf(k, to + 4 * i, v);
        }

        *work += 30;
    }

    r->eax = translate ? b : m;
    r->ecx = from;
    r->edx = to;
}
/* 1001e660  void __thiscall(mesh *this, const uint32_t *skeleton, buffer *positions, char do_positions,
 * buffer *normals, char do_normals, char normalize); ret 24. A buffer is {capacity, float (*data)[3]}.
 * For each bone of the mesh: its world matrix (the skeleton record's matrix at +0x108 times the bone's own) and
 * translation, and the bone's share of the positions and normals of the vertices it deforms (1001de00). Then the
 * skinned vertices (1001df00 / 1001e490, or 1001e050 / 1001e0e0 for a mesh on one bone), the copies of duplicated
 * positions, and with `normalize` the normals scaled to length 1.
 * A buffer that is too small is grown by the generated code. Everything the function writes it computes again from
 * what it does not write, so a NaN abandons the call without taking writes back. */
/* ------------------------------------------------------------------------------- the deform as bone weights
 * What 1001e660 computes, per output vertex, as a list of (bone, weight, source vertex) terms: position =
 * sum of weight * (bone matrix * source position + bone translation), normal = sum of weight * (bone matrix *
 * source normal), normalised when asked. The per-bone vertex groups (1001de00) add terms, the skinned vertices
 * (1001df00 / 1001e490) replace the list of their vertex, the copies of split vertices take another vertex's list,
 * a mesh on one bone without skinned vertices (1001e050 / 1001e0e0) has one term of weight 1. */
typedef struct {
    uint16_t bone;
    float weight;
    uint32_t source;
} SkinTerm;
typedef struct {
    SkinTerm terms[8];
    uint8_t count, overflow;
} SkinList;
static void skin_add(SkinList* list, uint32_t bone, float weight, uint32_t source) {
    if (list->count < 8) {
        list->terms[list->count++] = (SkinTerm){(uint16_t)bone, weight, source};
    } else {
        list->overflow = 1;
    }
}

/* 0 when the mesh's lists do not add up (the deform would leave it to the generated code anyway). */
int granny_skin_build(uint32_t self, uint32_t vertices, SkinList* positions, SkinList* normals) {
    const uint32_t bone_count = rd32(self + 0xc), bones = rd32(self + 0x14);
    memset(positions, 0, sizeof *positions * vertices);
    memset(normals, 0, sizeof *normals * vertices);
    for (uint32_t b = 0; b < bone_count; ++b) {
        const uint32_t bone = bones + b * 124;
        uint32_t groups = rd32(bone + 0x48), dwords = rd32(bone + 0x44), v = 0;
        unsigned guard = 0;
        while (dwords) {
            const uint32_t skip = rd32(groups), n = rd32(groups + 4);
            groups += 8;
            if (n + 2 > dwords || ++guard > (1u << 20)) {
                return 0;
            }

            dwords -= 2 + n;
            v += skip;
            for (uint32_t j = 0; j < n; ++j, ++v, groups += 4) {
                if (v >= vertices) {
                    return 0;
                }

                skin_add(&positions[v], b, rdf32(groups), v);
                skin_add(&normals[v], b, rdf32(groups), v);
            }
        }
    }

    const uint32_t skinned = rd32(self + 0x4c);
    const int rigid = !skinned && bone_count < 2;
    if (rigid) {
        for (uint32_t v = 0; v < vertices; ++v) {
            positions[v].count = 0, positions[v].overflow = 0, skin_add(&positions[v], 0, 1.0f, v);
        }
    } else if (rd32(self + 0x50)) {
        if (skinned > vertices) {
            return 0;
        }

        uint32_t at = rd32(self + 0x54);
        for (uint32_t v = 0; v < skinned; ++v) {
            positions[v].count = 0, positions[v].overflow = 0;
            const uint32_t n = rd32(at);
            at += 4;
            for (uint32_t i = 0; i < n; ++i, at += 8) {
                skin_add(&positions[v], rd32(at), rdf32(at + 4), v);
            }
        }

        uint32_t copies = rd32(self + 0x5c);
        for (uint32_t v = 0; v < skinned; ++v) {
            uint32_t count = rd32(copies);
            copies += 4;
            for (; count; --count, copies += 4) {
                const uint32_t to = v + rd32(copies);
                if (to >= vertices) {
                    return 0;
                }

                positions[to] = positions[v];
            }
        }
    }

    const uint32_t normal_count = rd32(self + 0x20);
    if (normal_count > vertices) {
        return 0;
    }

    if (rigid) {
        for (uint32_t v = 0; v < normal_count; ++v) {
            normals[v].count = 0, normals[v].overflow = 0, skin_add(&normals[v], 0, 1.0f, v);
        }
    } else if (rd32(self + 0x60)) {
        const uint32_t lists = rd32(self + 0x64);
        for (uint32_t v = 0; v < normal_count; ++v) {
            normals[v].count = 0, normals[v].overflow = 0;
            const uint32_t list = rd32(lists + 4 * v), n = rd32(list);
            for (uint32_t i = 0; i < n; ++i) {
                skin_add(&normals[v], rd32(list + 4 + 8 * i), rdf32(list + 8 + 8 * i), v);
            }
        }
    }

    return 1;
}
/* The static vertex data of a mesh for skinning on the GPU: 14 floats per output vertex (source position, source
 * normal, four bone indices, four weights). 0 when a vertex has more than four terms, or its normal's terms differ
 * from its position's (the deform stays on the CPU then). */
int granny_skin_template(uint32_t self, uint32_t vertices, float* out) {
    static SkinList *P, *N;
    static uint32_t capacity;
    if (vertices > capacity) {
        capacity = vertices;
        P = (SkinList*)realloc(P, sizeof *P * vertices), N = (SkinList*)realloc(N, sizeof *N * vertices);
        if (!P || !N) {
            abort();
        }
    }

    if (!granny_skin_build(self, vertices, P, N)) {
        return 0;
    }

    const uint32_t pos_from = rd32(self + 0x1c), nrm_from = rd32(self + 0x24), bone_count = rd32(self + 0xc);
    for (uint32_t v = 0; v < vertices; ++v, out += 14) {
        const SkinList *p = &P[v], *n = &N[v];
        if (p->overflow || n->overflow || p->count > 4) {
            return 0;
        }

        if (n->count && n->count != p->count) {
            return 0;
        }

        for (unsigned i = 0; i < n->count; ++i) {
            if (n->terms[i].bone != p->terms[i].bone || n->terms[i].weight != p->terms[i].weight ||
                n->terms[i].source != v) {
                return 0;
            }
        }

        const uint32_t src = p->count ? p->terms[0].source : v;
        for (unsigned i = 1; i < p->count; ++i) {
            if (p->terms[i].source != src) {
                return 0;
            }
        }

        for (unsigned i = 0; i < 3; ++i) {
            out[i] = rdf32(pos_from + src * 12 + 4 * i);
            out[3 + i] = n->count ? rdf32(nrm_from + v * 12 + 4 * i) : 0.0f;
        }

        for (unsigned i = 0; i < 4; ++i) {
            const int used = i < p->count;
            if (used && p->terms[i].bone >= bone_count) {
                return 0;
            }

            out[6 + i] = used ? (float)p->terms[i].bone : 0.0f;
            out[10 + i] = used ? p->terms[i].weight : 0.0f;
        }
    }

    return 1;
}

/* After a deform: the bone rows (3 x (matrix row, translation) per bone) to the hook. */
static void skin_notify(uint32_t self, uint32_t pos_to, uint32_t nrm_to, int do_normals, int normalize, int skipped) {
    const uint32_t bone_count = rd32(self + 0xc), bones = rd32(self + 0x14);
    static float* rows;
    static uint32_t capacity;
    if (bone_count > capacity) {
        capacity = bone_count;
        rows = (float*)realloc(rows, sizeof *rows * 12 * bone_count);
        if (!rows) {
            abort();
        }
    }

    for (uint32_t b = 0; b < bone_count; ++b) {
        const uint32_t m = bones + b * 124 + 0x4c, t = bones + b * 124 + 0x70;
        for (unsigned r = 0; r < 3; ++r) {
            rows[12 * b + 4 * r] = rdf32(m + 12 * r);
            rows[12 * b + 4 * r + 1] = rdf32(m + 12 * r + 4);
            rows[12 * b + 4 * r + 2] = rdf32(m + 12 * r + 8);
            rows[12 * b + 4 * r + 3] = rdf32(t + 4 * r);
        }
    }

    granny_skin_hook(self, pos_to, do_normals ? nrm_to : 0, rd32(self + 0x18), normalize, rows, bone_count, skipped);
}

/* SACRED_SKIN_CHECK=1: every rendering deform is computed again from the lists and compared (SKIN_CHECK lines). */
static struct {
    unsigned long calls, built, failed, overflow, differ_pn, copies_src, bad;
    double max_pos, max_nrm;
    unsigned max_terms;
} g_skin;
static int g_skin_check = -1;
static void skin_check_report(void) {
    fprintf(stderr,
            "SKIN_CHECK calls=%lu built=%lu failed=%lu overflow=%lu pos_vs_nrm_differ=%lu copies=%lu bad=%lu "
            "max_terms=%u max_pos_err=%g max_nrm_err=%g\n",
            g_skin.calls, g_skin.built, g_skin.failed, g_skin.overflow, g_skin.differ_pn, g_skin.copies_src, g_skin.bad,
            g_skin.max_terms, g_skin.max_pos, g_skin.max_nrm);
}

static void skin_check(uint32_t self, uint32_t pos_to, uint32_t nrm_to, int do_positions, int do_normals,
                       int normalize) {
    if (g_skin_check < 0) {
        const char* on = getenv("SACRED_SKIN_CHECK");
        g_skin_check = on && *on == '1';
        if (g_skin_check) {
            atexit(skin_check_report);
        }
    }

    if (!g_skin_check) {
        return;
    }

    const uint32_t n = rd32(self + 0x18);
    static SkinList *P, *N;
    static uint32_t capacity;
    if (n > capacity) {
        capacity = n;
        P = (SkinList*)realloc(P, sizeof *P * n), N = (SkinList*)realloc(N, sizeof *N * n);
    }

    if (++g_skin.calls % 5000 == 0) {
        skin_check_report();
    }

    if (!granny_skin_build(self, n, P, N)) {
        ++g_skin.failed;
        return;
    }

    ++g_skin.built;
    const uint32_t bones = rd32(self + 0x14), pos_from = rd32(self + 0x1c), nrm_from = rd32(self + 0x24);
    for (uint32_t v = 0; v < n; ++v) {
        if (P[v].overflow || N[v].overflow) {
            ++g_skin.overflow;
        }

        if (P[v].count > g_skin.max_terms) {
            g_skin.max_terms = P[v].count;
        }

        if (P[v].count != N[v].count && v < rd32(self + 0x20)) {
            ++g_skin.differ_pn;
        } else {
            for (unsigned i = 0; i < P[v].count && v < rd32(self + 0x20); ++i) {
                if (P[v].terms[i].bone != N[v].terms[i].bone || P[v].terms[i].weight != N[v].terms[i].weight) {
                    ++g_skin.differ_pn;
                    break;
                }
            }
        }

        if (P[v].count && P[v].terms[0].source != v) {
            ++g_skin.copies_src;
        }

        if (do_positions) {
            double a[3] = {0, 0, 0};
            for (unsigned i = 0; i < P[v].count; ++i) {
                const SkinTerm* t = &P[v].terms[i];
                const uint32_t bone = bones + t->bone * 124, m = bone + 0x4c, src = pos_from + t->source * 12;
                for (unsigned r = 0; r < 3; ++r) {
                    a[r] += t->weight * (rdf32(src) * rdf32(m + 12 * r) + rdf32(src + 4) * rdf32(m + 12 * r + 4) +
                                         rdf32(src + 8) * rdf32(m + 12 * r + 8) + rdf32(bone + 0x70 + 4 * r));
                }
            }

            for (unsigned r = 0; r < 3; ++r) {
                const double e = fabs(a[r] - rdf32(pos_to + v * 12 + 4 * r));
                if (e > g_skin.max_pos) {
                    g_skin.max_pos = e;
                }

                if (e > 0.05) {
                    ++g_skin.bad;
                }
            }
        }

        if (do_normals && v < rd32(self + 0x20)) {
            double a[3] = {0, 0, 0};
            for (unsigned i = 0; i < N[v].count; ++i) {
                const SkinTerm* t = &N[v].terms[i];
                const uint32_t m = bones + t->bone * 124 + 0x4c, src = nrm_from + t->source * 12;
                for (unsigned r = 0; r < 3; ++r) {
                    a[r] += t->weight * (rdf32(src) * rdf32(m + 12 * r) + rdf32(src + 4) * rdf32(m + 12 * r + 4) +
                                         rdf32(src + 8) * rdf32(m + 12 * r + 8));
                }
            }

            if (normalize) {
                const double l = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
                if (l > 0) {
                    a[0] /= l, a[1] /= l, a[2] /= l;
                }
            }

            for (unsigned r = 0; r < 3; ++r) {
                const double e = fabs(a[r] - rdf32(nrm_to + v * 12 + 4 * r));
                if (e > g_skin.max_nrm) {
                    g_skin.max_nrm = e;
                }
            }
        }
    }
}
/* The deform is for FUN_00405c90, which draws the mesh with DrawIndexedPrimitiveStrided after reading a dozen of its
 * positions for a bounding box, or for cGranny::renderShadow, which draws it strided with a diffuse colour: their
 * calls of GrannyLockNextRenderingState (return addresses 0x405e20, 0x40721c) are on the guest stack above. */
static int skin_for_mesh_draw(uint32_t esp) {
    for (uint32_t at = esp; at < esp + 0x800; at += 4) {
        const uint32_t value = rd32(at);
        if (value == 0x00405e20u || value == 0x0040721cu) {
            return 1;
        }
    }

    return 0;
}

STAND_IN(12, 1001e660) {
    const uint32_t esp = c->r[R_ESP], self = c->r[R_ECX];
    const uint32_t skeleton = rd32(esp + 4), positions = rd32(esp + 8), normals = rd32(esp + 16);
    const uint32_t n = rd32(self + 0x18);
    uint8_t do_positions = rd8(esp + 12), do_normals = rd8(esp + 20);
    const uint8_t normalize = rd8(esp + 24);
    if (!n) {
        do_positions = do_normals = 0;
    }

    if ((do_positions && n > rd32(positions)) || (do_normals && n > rd32(normals))) {
        return 0;
    }
    /* GPU skinning: a rendering deform (the two calls in 10034200) of a mesh the GPU can skin computes only the
     * bone matrices here; the vertices are skinned in the vertex shader (runtime/ddraw/gpu_skin.cpp). */
    const uint8_t want_positions = do_positions, want_normals = do_normals;
    const uint32_t gpu_pos = do_positions ? rd32(positions + 4) : 0, gpu_nrm = do_normals ? rd32(normals + 4) : 0;
    int gpu = 0;
    if (granny_skin_hook && do_positions) {
        const uint32_t back = rd32(esp);
        const int render = back == 0x100343c0u || back == 0x10034634u, mesh_draw = render && skin_for_mesh_draw(esp);
        ++granny_skin_calls[render ? (mesh_draw ? 2 : 1) : 0];
        if (granny_native_mode == GRANNY_NATIVE_ON && mesh_draw && granny_skin_take && granny_skin_take(self, n)) {
            gpu = 1, do_positions = do_normals = 0;
        } else if (granny_skin_forget) {
            granny_skin_forget(gpu_pos);
        }
    }

    Call k = {.frame_low = esp - 0x3c, .frame_high = esp};
    Regs r = {with_al(skeleton, do_positions), rd32(skeleton), c->r[R_EDX]};
    unsigned work = 40;
    uint32_t pos_from = 0, pos_to = 0, nrm_from = 0, nrm_to = 0;
    if (do_positions) {
        pos_to = rd32(positions + 4);
        r.eax = n * 12;
        call_zero(&k, pos_to, n * 12, &r);
        r.edx = pos_from = rd32(self + 0x1c);
        r.eax = pos_to;
    }

    r.eax = with_al(r.eax, do_normals);
    if (do_normals) {
        nrm_to = rd32(normals + 4);
        r.eax = nrm_to;
        r.edx = n * 12;
        call_zero(&k, nrm_to, n * 12, &r);
        r.ecx = nrm_from = rd32(self + 0x24);
        r.edx = nrm_to;
    }

    const uint32_t zero = esp - 0xc; /* t = 0 for the normals */
    call_put32(&k, zero, 0), call_put32(&k, zero + 4, 0), call_put32(&k, zero + 8, 0);
    const uint32_t records = rd32(skeleton) + 0x18; /* 10007d70: record i of [skeleton] is at [+0x18] + 300 * i */
    uint32_t ids = rd32(skeleton + 0x10), bone = rd32(self + 0x14);
    r.eax = bone;
    r.edx = rd32(self + 0xc) * 31;
    r.ecx = bone + r.edx * 4;
    for (uint32_t end = r.ecx; bone != end && !k.nan;) {
        const uint32_t record = rd32(records) + rd32(ids) * 300;
        const uint32_t groups = rd32(bone + 0x48), dwords = rd32(bone + 0x44);
        const uint32_t world = record + 0x108, local = bone + 0x20, m = bone + 0x4c, t = bone + 0x70;
        for (uint32_t i = 0; i < 3; ++i) {
            const uint32_t row = world + 12 * i, to = m + 12 * i;
            call_putf(&k, to,
                      ((double)rdf32(local + 24) * rdf32(row + 8) + (double)rdf32(row + 4) * rdf32(local + 12)) +
                          (double)rdf32(row) * rdf32(local));
            call_putf(&k, to + 4,
                      ((double)rdf32(local + 28) * rdf32(row + 8) + (double)rdf32(row) * rdf32(local + 4)) +
                          (double)rdf32(row + 4) * rdf32(local + 16));
            call_putf(&k, to + 8,
                      ((double)rdf32(row + 4) * rdf32(local + 20) + (double)rdf32(local + 32) * rdf32(row + 8)) +
                          (double)rdf32(row) * rdf32(local + 8));
        }

        const double x = rdf32(bone + 0x14), y = rdf32(bone + 0x18), z = rdf32(bone + 0x1c);
        for (uint32_t i = 0; i < 3; ++i) {
            call_putf(&k, t + 4 * i, (z * rdf32(m + 12 * i + 8) + y * rdf32(m + 12 * i + 4)) + x * rdf32(m + 12 * i));
        }

        for (uint32_t i = 0; i < 3; ++i) {
            call_putf(&k, t + 4 * i, (double)rdf32(record + 0xfc + 4 * i) + rdf32(t + 4 * i));
        }

        r.eax = t, r.ecx = bone + 0x7c, r.edx = record + 0xec;
        work += 104;
        if (dwords) {
            r.eax = with_al(r.eax, do_positions);
            if (do_positions) {
                call_deform(&k, t, m, groups, pos_from, pos_to, dwords, &r, &work);
            }

            r.eax = with_al(r.eax, do_normals);
            if (do_normals) {
                call_deform(&k, zero, m, groups, nrm_from, nrm_to, dwords, &r, &work);
            }
        }

        ids += 4;
        bone += 124;
        r.eax = rd32(self + 0x14);
        r.edx = r.eax + rd32(self + 0xc) * 124;
        r.ecx = bone;
        end = r.edx;
    }

    r.eax = with_al(r.eax, do_positions);
    if (do_positions && !k.nan) {
        const uint32_t skinned = rd32(self + 0x4c);
        if (!skinned && rd32(self + 0xc) < 2) {
            r.eax = pos_from, r.ecx = self;
            call_rigid(&k, self, 0, rd32(self + 0x18), pos_from, pos_to, 1, &r, &work);
        } else if ((r.eax = rd32(self + 0x50)) != 0) {
            Skin s = {skinned, pos_from, rd32(self + 0x54), pos_to, self, rd32(self + 0x54), 0};
            if (RECOMP_UNLIKELY(!skin_positions(self, &s))) {
                return call_abandon(&k);
            }

            work += s.work;
            /* Vertices split for the renderer: copies of the skinned position. */
            uint32_t copies = rd32(self + 0x5c), from = pos_to;
            r.eax = 0;
            for (uint32_t v = skinned; v; --v, from += 12) {
                uint32_t count = rd32(copies);
                copies += 4;
                if (count) {
                    const uint32_t xb = rd32(from), yb = rd32(from + 4), zb = rd32(from + 8);
                    for (; count; --count, copies += 4) {
                        r.eax = from + rd32(copies) * 12;
                        call_copyf(&k, r.eax, xb), call_copyf(&k, r.eax + 4, yb), call_copyf(&k, r.eax + 8, zb);
                        work += 10;
                    }
                }

                work += 7;
            }

            r.ecx = copies, r.edx = from;
        }
    }

    r.eax = with_al(r.eax, do_normals);
    if (do_normals && !k.nan) {
        r.eax = rd32(self + 0x4c);
        if (!r.eax && rd32(self + 0xc) < 2) {
            r.eax = rd32(self + 0x20), r.ecx = self, r.edx = nrm_from;
            call_rigid(&k, self, 0, r.eax, nrm_from, nrm_to, 0, &r, &work);
        } else if ((r.eax = rd32(self + 0x60)) != 0) {
            Skin s = {rd32(self + 0x20), nrm_from, rd32(self + 0x64), nrm_to, self, rd32(self + 0x64), 0};
            if (RECOMP_UNLIKELY(!skin_normals(self, &s))) {
                return call_abandon(&k);
            }

            work += s.work;
            r.eax = 0, r.ecx = s.ecx, r.edx = s.edx;
        }

        r.eax = with_al(r.eax, normalize);
        if (normalize) {
            const double one = rdf32(0x1005c0fcu);
            r.eax = rd32(normals + 4);
            for (r.ecx = n; r.ecx; --r.ecx, r.eax += 12) {
                const double x = rdf32(r.eax), y = rdf32(r.eax + 4), z = rdf32(r.eax + 8);
                const double inverse = one / sqrt((y * y + x * x) + z * z);
                for (uint32_t i = 0; i < 3; ++i) {
                    call_putf(&k, r.eax + 4 * i, inverse * rdf32(r.eax + 4 * i));
                }

                work += 26;
            }
        }
    }

    if (RECOMP_UNLIKELY(k.nan)) {
        if (gpu && granny_skin_forget) {
            granny_skin_forget(gpu_pos);
        }

        return call_abandon(&k);
    }

    if (RECOMP_UNLIKELY(g_skin_check) && !gpu) {
        skin_check(self, pos_to, nrm_to, do_positions, do_normals, normalize);
    }

    if (gpu) {
        skin_notify(self, gpu_pos, gpu_nrm, want_normals, normalize, 1);
    } else if (granny_skin_hook && want_positions) {
        skin_notify(self, pos_to, nrm_to, do_normals, normalize, 0);
    }

    c->r[R_EAX] = r.eax;
    c->r[R_ECX] = r.ecx;
    c->r[R_EDX] = r.edx;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 4 + 24;
    charge(c, 0x1001e660u, work);
    return 1;
}

/* ------------------------------------------------------------------------------------------- sampling a curve
 * The first knot from the fourth on that is not below t (knots[i] < t compares the float knot with the double t;
 * a NaN stops the search), and the last-but-three at most. */
static uint32_t curve_knot(uint32_t knots, uint32_t count, double t, unsigned* work) {
    uint32_t i = 3;
    const uint32_t last = count - 3;
    if (last > 3) {
        while (rdf32(knots + 4 * i) < t && ++i < last) {
        }
    }

    *work += 10 + 7 * (i - 3);
    return i;
}
/* The B-spline of degree 1 to 3 with weights `w` through the `points` control points ending at `at` (`dim` floats
 * each): out[c] = ((w0 * P0 + w1 * P1) + w2 * P2) + w3 * P3. */
static void blend(Call* k, const double* w, unsigned points, uint32_t at, uint32_t dim, uint32_t out) {
    const uint32_t first = at - (points - 1) * 4 * dim;
    for (uint32_t c = 0; c < dim; ++c) {
        double sum = w[0] * rdf32(first + 4 * c);
        for (unsigned j = 1; j < points; ++j) {
            sum = sum + w[j] * rdf32(first + 4 * (j * dim + c));
        }

        call_putf(k, out + 4 * c, sum);
    }
}
/* 100025f0 (dim 4) and 10002900 (dim 9)  void __cdecl(uint32_t degree, float t, const float *knot,
 * const float *point, float *out): the curve of the given degree at t, with knot[-degree..degree-1] and
 * point[-degree..0]. EAX, ECX and EDX as they leave them; ECX and EDX hold the caller's values on entry. */
static void curve_sample(Call* k, uint32_t degree, float t, uint32_t knot, uint32_t point, uint32_t dim, uint32_t out,
                         Regs* r, unsigned* work) {
    const double one = rdf32(0x1005c0fcu);
    double w[4];
    unsigned points;
    switch (degree) {
    case 0:
        for (uint32_t c = 0; c < dim; ++c) {
            call_put32(k, out + 4 * c, rd32(point + 4 * c));
        }

        r->eax = rd32(point + 4 * (dim - 1)), r->ecx = out, r->edx = rd32(point + 4 * (dim - 2));
        *work += 5 + 2 * dim;
        return;
    case 1: {
        const double u = (t - (double)rdf32(knot - 4)) / ((double)rdf32(knot) - rdf32(knot - 4));
        w[0] = one - u, w[1] = u, points = 2;
        break;
    }
    case 2: {
        const float a = (float)(t - (double)rdf32(knot - 4));
        const float c1 = (float)(a / ((double)rdf32(knot) - rdf32(knot - 4)));
        const double v = (t - (double)rdf32(knot - 8)) / ((double)rdf32(knot) - rdf32(knot - 8));
        const float s = (float)(((double)c1 + v) - v * c1);
        const double w2 = a / ((double)rdf32(knot + 4) - rdf32(knot - 4)) * c1;
        w[0] = one - s, w[1] = s - w2, w[2] = w2, points = 3;
        break;
    }
    case 3: {
        const double km3 = rdf32(knot - 12), km2 = rdf32(knot - 8), km1 = rdf32(knot - 4);
        const double k0 = rdf32(knot), k1 = rdf32(knot + 4), k2 = rdf32(knot + 8);
        const float a = (float)(t - km1), b = (float)(t - km2);
        const float c1 = (float)(a / (k0 - km1)), d = (float)(b / (k0 - km2)), e = (float)(a / (k1 - km1));
        const float g = (float)(b / (k1 - km2)), h = (float)(a / (k2 - km1));
        const float p = (float)((one - d) * (one - c1)), q = (float)((double)e * c1);
        const double u = (t - km3) / (k0 - km3);
        const float s = (float)(((one - e) * c1) + ((one - c1) * d));
        w[0] = (float)((one - u) * p);
        w[1] = ((one - g) * s) + (p * u);
        w[2] = ((one - h) * q) + ((double)s * g);
        w[3] = (double)q * h;
        points = 4;
        break;
    }
    default:
        r->eax = degree;
        *work += 4;
        return;
    }

    blend(k, w, points, point, dim, out);
    r->eax = point, r->ecx = out;
    if (degree == 3) {
        r->edx = rd32(knot - 8);
    }

    *work += 30 + 12 * degree + (2 + 2 * degree) * dim;
}

/* The position curve of 100022b0, whose weights come from 100035a0, 100035d0 and 10003640 as floats. */
static void position_sample(Call* k, uint32_t degree, float t, uint32_t knot, uint32_t point, uint32_t out,
                            uint32_t esp, Regs* r, unsigned* work) {
    const double one = rdf32(0x1005c0fcu);
    double w[4];
    unsigned points;
    switch (degree) {
    case 0:
        r->eax = rd32(point), r->ecx = rd32(point + 4), r->edx = rd32(point + 8);
        call_put32(k, out, r->eax), call_put32(k, out + 4, r->ecx), call_put32(k, out + 8, r->edx);
        *work += 8;
        return;
    case 1: {
        const double u = (t - (double)rdf32(knot - 4)) / ((double)rdf32(knot) - rdf32(knot - 4));
        w[0] = (float)(one - u), w[1] = (float)u, points = 2;
        r->eax = esp - 0xc, r->ecx = esp + 0x10, r->edx = rd32(knot);
        break;
    }
    case 2: {
        const double km2 = rdf32(knot - 8), km1 = rdf32(knot - 4), k0 = rdf32(knot), k1 = rdf32(knot + 4);
        const double a = (t - km2) / (k0 - km2), b = (t - km1) / (k0 - km1), c1 = (t - km1) / (k1 - km1);
        const float s = (float)((a + b) - a * b);
        const double bc = b * c1;
        w[0] = (float)(one - s), w[1] = (float)(s - bc), w[2] = (float)bc, points = 3;
        r->eax = esp - 8, r->ecx = esp - 0xc, r->edx = esp + 0x10;
        break;
    }
    case 3: {
        const double km3 = rdf32(knot - 12), km2 = rdf32(knot - 8), km1 = rdf32(knot - 4);
        const double k0 = rdf32(knot), k1 = rdf32(knot + 4), k2 = rdf32(knot + 8);
        const float a1 = (float)((t - km1) / (k0 - km1)), a2 = (float)((t - km2) / (k0 - km2));
        const float a3 = (float)((t - km1) / (k1 - km1)), a4 = (float)((t - km3) / (k0 - km3));
        const float a5 = (float)((t - km1) / (k2 - km1)), a6 = (float)((double)a3 * a1);
        const double g = (t - km2) / (k1 - km2), both = (one - a2) * (one - a1);
        const float s = (float)(((one - a3) * a1) + ((one - a1) * a2));
        w[0] = (float)((one - a4) * both);
        w[1] = (float)(((one - g) * s) + (both * a4));
        w[2] = (float)(((one - a5) * a6) + ((double)s * g));
        w[3] = (float)((double)a6 * a5);
        points = 4;
        r->eax = esp - 4, r->ecx = esp - 8, r->edx = esp - 0xc;
        break;
    }
    default:
        *work += 2;
        return;
    }

    blend(k, w, points, point, 3, out);
    *work += 30 + 14 * degree;
}
/* 100022b0  bool __thiscall(const track *this, double t, uint32_t unused, float *out); ret 16. Samples the
 * position (out[0..2]), orientation (out[3..6], renormalised by (3 - |q|^2) / 2) and scale-shear (out[7..15])
 * curves of an animation track at t; a curve is (knot count, knots) at this + 4 + 8 * index, with its degree,
 * index and control points at +0x1c/+0x20/+0x28, +0x2c/+0x30/+0x38 and +0x3c/+0x40/+0x48. AL = 1 when any curve
 * has knots. */
STAND_IN(13, 100022b0) {
    const uint32_t esp = c->r[R_ESP], self = c->r[R_ECX], out = rd32(esp + 0x10);
    const double time = rdf64(esp + 4);
    const float t = (float)time;
    Call k = {.keep = 1};
    Regs r = {with_al(c->r[R_EAX], 0), 0, 0};
    unsigned work = 20;
    uint32_t index = rd32(self + 0x20), count = rd32(self + index * 8 + 4);
    r.ecx = index, r.edx = count;
    if (count) {
        const uint32_t knots = rd32(self + index * 8 + 8), i = curve_knot(knots, count, time, &work);
        const uint32_t degree = rd32(self + 0x1c), knot = knots + 4 * i;
        r.eax = knot, r.ecx = degree, r.edx = knots;
        position_sample(&k, degree, t, knot, rd32(self + 0x28) + 12 * i, out, esp, &r, &work);
        r.eax = with_al(r.eax, 1);
    }

    index = rd32(self + 0x30), count = rd32(self + index * 8 + 4);
    r.ecx = index, r.edx = count;
    if (count) {
        const uint32_t knots = rd32(self + index * 8 + 8), i = curve_knot(knots, count, time, &work);
        const uint32_t degree = rd32(self + 0x2c), knot = knots + 4 * i, q = out + 0xc;
        r.ecx = knot, r.edx = degree;
        curve_sample(&k, degree, t, knot, rd32(self + 0x38) + 16 * i, 4, q, &r, &work);
        const double q0 = rdf32(q), q1 = rdf32(q + 4), q2 = rdf32(q + 8), q3 = rdf32(q + 12);
        const double scale = (rdf32(0x1005c258u) - (((q0 * q0 + q1 * q1) + q2 * q2) + q3 * q3)) * rdf32(0x1005c254u);
        for (uint32_t j = 0; j < 4; ++j) {
            call_putf(&k, q + 4 * j, scale * rdf32(q + 4 * j));
        }

        r.eax = with_al(r.eax, 1);
        work += 40;
    }

    index = rd32(self + 0x40), count = rd32(self + index * 8 + 4);
    r.ecx = index, r.edx = count;
    if (count) {
        const uint32_t knots = rd32(self + index * 8 + 8), i = curve_knot(knots, count, time, &work);
        const uint32_t degree = rd32(self + 0x3c), knot = knots + 4 * i;
        r.ecx = knot, r.edx = degree;
        curve_sample(&k, degree, t, knot, rd32(self + 0x48) + 36 * i, 9, out + 0x1c, &r, &work);
        r.eax = with_al(r.eax, 1);
    }

    if (RECOMP_UNLIKELY(k.nan)) {
        return call_abandon(&k);
    }

    c->r[R_EAX] = r.eax;
    c->r[R_ECX] = r.ecx;
    c->r[R_EDX] = r.edx;
    c->eip = rd32(esp);
    c->r[R_ESP] = esp + 4 + 16;
    charge(c, 0x100022b0u, work);
    return 1;
}

static GrannyStat g_stats[16] = {{.name = "1001df00"}, {.name = "1001e490"}, {.name = "10021540"}, {.name = "10021460"},
                                 {.name = "100214d0"}, {.name = "10009d30"}, {.name = "10009ad0"}, {.name = "10021250"},
                                 {.name = "100435a0"}, {.name = "10043570"}, {.name = "10020990"}, {.name = "10020e10"},
                                 {.name = "1001e660"}, {.name = "100022b0"}};
#define CODE(...) ((const uint32_t[]){__VA_ARGS__, 0})
const struct GrannyNative granny_natives[] = {
    {0x1001df00u, CODE(0x1001df00u, 278), 0x85bd92ffu, native_1001df00, &original_1001df00},
    {0x1001e490u, CODE(0x1001e490u, 285), 0x94a5e638u, native_1001e490, &original_1001e490},
    {0x10021540u, CODE(0x10021540u, 59), 0x3d0f7f63u, native_10021540, &original_10021540},
    {0x10021460u, CODE(0x10021460u, 102), 0xcca50b00u, native_10021460, &original_10021460},
    {0x100214d0u, CODE(0x100214d0u, 102), 0x6316dc92u, native_100214d0, &original_100214d0},
    {0x10009d30u, CODE(0x10009d30u, 139), 0x0d647084u, native_10009d30, &original_10009d30},
    {0x10009ad0u, CODE(0x10009ad0u, 223), 0xf1582684u, native_10009ad0, &original_10009ad0},
    {0x10021250u, CODE(0x10021250u, 465), 0xc4bbb9c1u, native_10021250, &original_10021250},
    {0x100435a0u, CODE(0x100435a0u, 47), 0x26611d5f, native_100435a0, &original_100435a0},
    {0x10043570u, CODE(0x10043570u, 35), 0x7f99b66eu, native_10043570, &original_10043570},
    {0x10020990u, CODE(0x10020990u, 343, 0x10043570u, 35), 0x072cc061u, native_10020990, &original_10020990},
    {0x10020e10u,
     CODE(0x10020e10u, 700, 0x10009ad0u, 223, 0x10021460u, 102, 0x100214d0u, 102, 0x10043570u, 35, 0x10021540u, 59),
     0x2334f3abu, native_10020e10, &original_10020e10},
    {0x1001e660u,
     CODE(0x1001e660u, 981, 0x10007d70u, 22, 0x100435a0u, 47, 0x1001de00u, 220, 0x1001e050u, 130, 0x1001e0e0u, 121,
          0x1001df00u, 278, 0x1001e490u, 285),
     0x49661eb8u, native_1001e660, &original_1001e660},
    /* with the jump tables at 100025d4, 100028e4 and 10002d68 */
    {0x100022b0u,
     CODE(0x100022b0u, 820, 0x100025f0u, 772, 0x10002900u, 1144, 0x100035a0u, 43, 0x100035d0u, 103, 0x10003640u, 263),
     0xf6ca492bu, native_100022b0, &original_100022b0},
};
#undef CODE
const unsigned granny_native_count = sizeof granny_natives / sizeof *granny_natives;
