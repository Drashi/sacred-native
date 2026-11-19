/* x86.h - guest CPU state and instruction helpers for the static recompiler.
 *
 * Emitted/owned by tools/recomp/translate.py (Task 1).  The generated C in
 * build/recomp/gen/ includes this header, and so does the runtime in
 * runtime/.  Field names are fixed by the m1-recomp plan's
 * "Global constraints" section; do not rename them.
 *
 * Conventions
 *   - All guest pointers are uint32_t.  Guest address `a` lives at g_mem + a.
 *   - Flag fields hold 0 or 1.  Every defined flag is written eagerly at each
 *     instruction, including function, dispatch and exception boundaries.
 *   - x87 registers are explicit software ext80 values, with physical MMX aliases.
 */
#ifndef RECOMP_X86_H
#define RECOMP_X86_H

#include <stdint.h>
#include "../../../runtime/cpu/types.h"
#include <limits.h>
#include <string.h>
#include <math.h>
#ifndef M_LN2 /* glibc and MSVC hide the M_ constants without feature macros */
#define M_LN2 0.693147180559945309417
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The hot guest-memory and x87 helpers below are large enough that the
 * compiler stops inlining them on its own, which costs a call per guest
 * memory access, so these insist. */
#if defined(__GNUC__) || defined(__clang__)
#define RECOMP_HOT_INLINE static inline __attribute__((always_inline))
/* A slow path (fault, watch hit, unknown target, yield): the compiler moves the code that leads to it out of the hot
 * body of a generated region, which keeps the straight-line guest code dense in the instruction cache. */
#define RECOMP_COLD __attribute__((cold))
#define RECOMP_OUT_OF_LINE static __attribute__((noinline, unused))
#define RECOMP_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define RECOMP_LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define RECOMP_HOT_INLINE static inline
#define RECOMP_COLD
#define RECOMP_OUT_OF_LINE static inline
#define RECOMP_UNLIKELY(x) (x)
#define RECOMP_LIKELY(x) (x)
#endif

/* ---------------------------------------------------------------- memory */

#ifndef GUEST_SIZE
#define GUEST_SIZE 0x10000000u /* 256 MB arena; RECOMP_GUEST_SIZE grows it for auxiliary modules */
#endif

extern uint8_t *g_mem;
#if defined(SACRED_DRIVER_CHECKPOINTS) && defined(RECOMP_MMU_GUEST)
/* The host MMU checks guest accesses: the arena is a 4 GiB reservation in which only committed guest memory is
 * mapped, so every 32-bit address (plus an access of up to 16 bytes, which spills at most into the guard of the
 * next page) lands inside it, and an access to anything not mapped faults in the host. The runtime's signal
 * handler grows a guest stack on such a fault and reports everything else as the fatal guest fault it is. The page
 * table stays for the runtime's own checked accesses. */
extern const uint8_t *g_page_access;
#define RECOMP_PAGE_FITS(a, n) (((a) & 0xfffu) <= 0x1000u - (uint32_t)(n))
#define RECOMP_READ(a, out, n) memcpy((out), g_mem + (a), (n))
#define RECOMP_WRITE(a, in, n) memcpy(g_mem + (a), (in), (n))
#elif defined(SACRED_DRIVER_CHECKPOINTS)
RECOMP_COLD void sacred_guest_read(uint32_t address, void *out, size_t bytes);
RECOMP_COLD void sacred_guest_write(uint32_t address, const void *in, size_t bytes);
/* One byte per 4 KiB guest page, owned by sacred::Memory (access_table()): bit 0 readable, bit 1 writable, both only
 * on committed pages. An access that stays inside one readable/writable page is a plain copy; every other access
 * (page span, null page, stack guard page, protected or uncommitted page) takes the checked call, which grows the
 * stack or raises the guest fault exactly as before. With no active guest control the table is all zero. */
extern const uint8_t *g_page_access;
#define RECOMP_PAGE_FITS(a, n) (((a) & 0xfffu) <= 0x1000u - (uint32_t)(n))
#define RECOMP_READ(a, out, n)                                                                     \
    do {                                                                                           \
        const uint32_t ra_ = (a);                                                                  \
        if (RECOMP_LIKELY(RECOMP_PAGE_FITS(ra_, n) && (g_page_access[ra_ >> 12] & 1u)))           \
            memcpy((out), g_mem + ra_, (n));                                                       \
        else                                                                                       \
            sacred_guest_read(ra_, (out), (n));                                                    \
    } while (0)
#define RECOMP_WRITE(a, in, n)                                                                     \
    do {                                                                                           \
        const uint32_t wa_ = (a);                                                                  \
        if (RECOMP_LIKELY(RECOMP_PAGE_FITS(wa_, n) && (g_page_access[wa_ >> 12] & 2u)))           \
            memcpy(g_mem + wa_, (in), (n));                                                        \
        else                                                                                       \
            sacred_guest_write(wa_, (in), (n));                                                    \
    } while (0)
#else
#define RECOMP_READ(a, out, n) memcpy((out), g_mem + (a), (n))
#define RECOMP_WRITE(a, in, n) memcpy(g_mem + (a), (in), (n))
#endif

/* Guest memory layout (see plan Global constraints, as corrected):
 *   0x00400000 + SizeOfImage  image (preferred base, no relocation).  The end
 *                             is a PE header value, not a constant; ask the
 *                             loader (loader_image_limit()) for it.
 *   GUEST_HEAP_BASE..0x0e000000  heap arena (default 0x01000000)
 *   0x0f000000                stack top, grows down (1 MB)
 *   0x0fe00000                TEB (FS base)
 *   0x0ff00000 + 16*i         import shim trampoline for import i
 */
/* The build passes the game's image base; this default serves the test harness. */
#ifndef GUEST_IMAGE_BASE
#define GUEST_IMAGE_BASE 0x00400000u
#endif
/* The build passes the game's heap start ([game] heap_base); the default
 * serves the test harness and games whose image ends below 16 MB. */
#ifndef GUEST_HEAP_BASE
#define GUEST_HEAP_BASE 0x01000000u
#endif
#define GUEST_HEAP_END 0x0e000000u
#define GUEST_STACK_TOP 0x0f000000u
#define GUEST_TEB_BASE 0x0fe00000u
#define GUEST_SHIM_BASE 0x0ff00000u
#define GUEST_SHIM_END 0x10000000u
#define GUEST_SHIM_STRIDE 16u
#define GUEST_RETURN_SENTINEL 0x0fdfff00u
/* Windows maps nothing in the first 64 KB, so a read or a write through a
 * null pointer faults and the program's own handlers see it. A flat arena
 * hands back a zero instead, which turns a null dereference into ordinary
 * data: Need for Speed Most Wanted walked an std::map whose node pointer was
 * null, read _Isnil from guest 0x15, got zero from the arena and looped on
 * the same node until the watchdog ended the run. recomp_unknown_call has
 * modelled this for calls all along; these are the other two ways to reach
 * the page. */
#define GUEST_NULL_LIMIT 0x10000u
/* Compiled in only when the build asks for it. The check is one compare
 * against a constant, but it sits in rd8/rd16/rd32 and their writers, which
 * are the hottest code in the system, and a guest that dereferences null is
 * broken whether or not we are looking. Leaving it in every build spends that
 * on every access of every game to catch a bug in one of them - and it
 * perturbs timing, which is how it was noticed: the game this found a spin in
 * has an intermittent race whose odds moved when the check went in.
 *
 * -DRECOMP_NULL_CHECKS=1 builds it; RECOMP_NULL_FAULTS=1 then arms it. */
#if defined(RECOMP_NULL_CHECKS) && RECOMP_NULL_CHECKS
#define RECOMP_NULL_GUARD(a, write)                                                                \
    do {                                                                                           \
        if (RECOMP_UNLIKELY((a) < GUEST_NULL_LIMIT))                                               \
            recomp_null_access((a), (write));                                                      \
    } while (0)
#else
#define RECOMP_NULL_GUARD(a, write) ((void)0)
#endif
/* Raises the access violation Windows would for an address in that page.
 * `write` picks the parameter the dispatcher reports. Declared here because
 * the accessors below are what call it. */
RECOMP_COLD void recomp_null_access(uint32_t addr, int write);

/* Little-endian host (ARM64) matches the guest, so memcpy is a plain load. */
RECOMP_HOT_INLINE uint8_t rd8(uint32_t a) {
    RECOMP_NULL_GUARD(a, 0);
    uint8_t v;
    RECOMP_READ(a, &v, 1);
    return v;
}
RECOMP_HOT_INLINE uint16_t rd16(uint32_t a) {
    RECOMP_NULL_GUARD(a, 0);
    uint16_t v;
    RECOMP_READ(a, &v, 2);
    return v;
}
RECOMP_HOT_INLINE uint32_t rd32(uint32_t a) {
    RECOMP_NULL_GUARD(a, 0);
    uint32_t v;
    RECOMP_READ(a, &v, 4);
    return v;
}
RECOMP_HOT_INLINE uint64_t rd64(uint32_t a) {
    uint64_t v;
    RECOMP_READ(a, &v, 8);
    return v;
}
/* A guest-memory watchpoint.  RECOMP_WATCH=<hex address>[:<length>] reports
 * every write that touches those bytes together with the host backtrace, which
 * names the generated function and so the guest routine doing the writing.
 * This is the instrument for a stray write: the corrupted value says what the
 * damage is, never who did it, and the writer is usually nowhere near the
 * reader.  Unarmed - which is always, unless the switch is set - g_watch_len is
 * zero and a write costs one compare the branch predictor never takes. */
extern uint32_t g_watch_base;
extern uint32_t g_watch_len;
RECOMP_COLD void recomp_watch_hit(uint32_t addr, uint32_t n, uint64_t value);

/* The spans written inside ranges the runtime is watching for itself: a
 * DirectDraw surface under an open Lock. Its Unlock has to find what the guest
 * changed through the pointer it was handed, and comparing the whole surface
 * to find a line of text is most of a frame; every store here widens the
 * written span of each range it lands in, so the Unlock compares only that.
 * No range is open almost always, and then this is one compare. */
#define RECOMP_DIRTY_SLOTS 4
typedef struct RecompDirty {
    uint32_t base, len; /* the range */
    uint32_t lo, hi;    /* what was written inside it; lo >= hi is nothing */
} RecompDirty;
extern RecompDirty g_dirty[RECOMP_DIRTY_SLOTS];
extern uint32_t g_dirty_count;
/* Taken only while a surface is locked: one copy per translation unit instead of the loop at every guest store. */
RECOMP_OUT_OF_LINE void recomp_dirty(uint32_t a, uint32_t n) {
    for (uint32_t i = 0; i < g_dirty_count; ++i) {
        RecompDirty *d = &g_dirty[i];
        if (a < d->base + d->len && a + n > d->base) {
            if (a < d->lo)
                d->lo = a;
            if (a + n > d->hi)
                d->hi = a + n;
        }
    }
}

/* A runtime that arms neither (RECOMP_NO_STORE_HOOKS) leaves both checks out of every guest store. */
#ifdef RECOMP_NO_STORE_HOOKS
#define RECOMP_STORE_DIRTY(a, n) ((void)(a), (void)(n))
#else
#define RECOMP_STORE_DIRTY(a, n)                                                                   \
    do {                                                                                           \
        if (g_dirty_count != 0)                                                                    \
            recomp_dirty((a), (n));                                                                \
    } while (0)
#endif
RECOMP_HOT_INLINE void recomp_watch(uint32_t a, uint32_t n, uint64_t v) {
#ifdef RECOMP_NO_STORE_HOOKS
    (void)a, (void)n, (void)v;
#else
    if (g_watch_len != 0 && a < g_watch_base + g_watch_len && g_watch_base < a + n)
        recomp_watch_hit(a, n, v);
    RECOMP_STORE_DIRTY(a, n);
#endif
}

RECOMP_HOT_INLINE void wr8(uint32_t a, uint8_t v) {
    RECOMP_NULL_GUARD(a, 1);
    RECOMP_WRITE(a, &v, 1);
    recomp_watch(a, 1, v);
}
RECOMP_HOT_INLINE void wr16(uint32_t a, uint16_t v) {
    RECOMP_NULL_GUARD(a, 1);
    RECOMP_WRITE(a, &v, 2);
    recomp_watch(a, 2, v);
}
RECOMP_HOT_INLINE void wr32(uint32_t a, uint32_t v) {
    RECOMP_NULL_GUARD(a, 1);
    RECOMP_WRITE(a, &v, 4);
    recomp_watch(a, 4, v);
}
RECOMP_HOT_INLINE void wr64(uint32_t a, uint64_t v) {
    RECOMP_WRITE(a, &v, 8);
    recomp_watch(a, 8, v);
}
RECOMP_HOT_INLINE float rdf32(uint32_t a) {
    float v;
    RECOMP_READ(a, &v, 4);
    return v;
}
RECOMP_HOT_INLINE double rdf64(uint32_t a) {
    double v;
    RECOMP_READ(a, &v, 8);
    return v;
}
RECOMP_HOT_INLINE void wrf32(uint32_t a, float v) {
    RECOMP_WRITE(a, &v, 4);
    RECOMP_STORE_DIRTY(a, 4);
}
RECOMP_HOT_INLINE void wrf64(uint32_t a, double v) {
    RECOMP_WRITE(a, &v, 8);
    RECOMP_STORE_DIRTY(a, 8);
}

/* The same accessors on a guest base and page table the caller holds in locals. Generated region functions load
 * g_mem and g_page_access once on entry and call these (the shards define rd32 and the rest as macros over them):
 * every guest store goes through a byte pointer that may alias the two globals, so read directly they are loaded
 * again before every guest access. Neither pointer changes while guest code runs. */
#if defined(SACRED_DRIVER_CHECKPOINTS) && defined(RECOMP_MMU_GUEST)
#define RECOMP_LOCAL_READ(m, t, a, out, n) ((void)(t), memcpy((out), (m) + (a), (n)))
#define RECOMP_LOCAL_WRITE(m, t, a, in, n) ((void)(t), memcpy((m) + (a), (in), (n)))
#define RECOMP_PAGES_OR_NULL ((const uint8_t *)0)
#elif defined(SACRED_DRIVER_CHECKPOINTS)
#define RECOMP_LOCAL_READ(m, t, a, out, n)                                                         \
    do {                                                                                           \
        const uint32_t ra_ = (a);                                                                  \
        if (RECOMP_LIKELY(RECOMP_PAGE_FITS(ra_, n) && ((t)[ra_ >> 12] & 1u)))                     \
            memcpy((out), (m) + ra_, (n));                                                         \
        else                                                                                       \
            sacred_guest_read(ra_, (out), (n));                                                    \
    } while (0)
#define RECOMP_LOCAL_WRITE(m, t, a, in, n)                                                         \
    do {                                                                                           \
        const uint32_t wa_ = (a);                                                                  \
        if (RECOMP_LIKELY(RECOMP_PAGE_FITS(wa_, n) && ((t)[wa_ >> 12] & 2u)))                     \
            memcpy((m) + wa_, (in), (n));                                                          \
        else                                                                                       \
            sacred_guest_write(wa_, (in), (n));                                                    \
    } while (0)
#define RECOMP_PAGES_OR_NULL g_page_access
#else
#define RECOMP_LOCAL_READ(m, t, a, out, n) ((void)(t), memcpy((out), (m) + (a), (n)))
#define RECOMP_LOCAL_WRITE(m, t, a, in, n) ((void)(t), memcpy((m) + (a), (in), (n)))
#define RECOMP_PAGES_OR_NULL ((const uint8_t *)0)
#endif
#define RECOMP_LOCAL_RD(name, type, n)                                                             \
    RECOMP_HOT_INLINE type name##_in(uint8_t *m, const uint8_t *t, uint32_t a) {                   \
        RECOMP_NULL_GUARD(a, 0);                                                                   \
        type v;                                                                                    \
        RECOMP_LOCAL_READ(m, t, a, &v, n);                                                         \
        return v;                                                                                  \
    }
#define RECOMP_LOCAL_WR(name, type, n)                                                             \
    RECOMP_HOT_INLINE void name##_in(uint8_t *m, const uint8_t *t, uint32_t a, type v) {           \
        RECOMP_NULL_GUARD(a, 1);                                                                   \
        RECOMP_LOCAL_WRITE(m, t, a, &v, n);                                                        \
        recomp_watch(a, n, (uint64_t)v);                                                           \
    }
RECOMP_LOCAL_RD(rd8, uint8_t, 1)
RECOMP_LOCAL_RD(rd16, uint16_t, 2)
RECOMP_LOCAL_RD(rd32, uint32_t, 4)
RECOMP_LOCAL_RD(rd64, uint64_t, 8)
RECOMP_LOCAL_RD(rdf32, float, 4)
RECOMP_LOCAL_RD(rdf64, double, 8)
RECOMP_LOCAL_WR(wr8, uint8_t, 1)
RECOMP_LOCAL_WR(wr16, uint16_t, 2)
RECOMP_LOCAL_WR(wr32, uint32_t, 4)
RECOMP_LOCAL_WR(wr64, uint64_t, 8)
RECOMP_HOT_INLINE void wrf32_in(uint8_t *m, const uint8_t *t, uint32_t a, float v) {
    RECOMP_LOCAL_WRITE(m, t, a, &v, 4);
    RECOMP_STORE_DIRTY(a, 4);
}
RECOMP_HOT_INLINE void wrf64_in(uint8_t *m, const uint8_t *t, uint32_t a, double v) {
    RECOMP_LOCAL_WRITE(m, t, a, &v, 8);
    RECOMP_STORE_DIRTY(a, 8);
}
/* Opens a function that uses the local accessors. */
#define RECOMP_LOCAL_MEMORY                                                                        \
    uint8_t *const gm_ = g_mem;                                                                    \
    const uint8_t *const pt_ = RECOMP_PAGES_OR_NULL;                                               \
    (void)gm_, (void)pt_

/* ------------------------------------------------------------- cpu state */

/* Register indices into X86.r */
enum { R_EAX = 0, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };

struct X86 {
    uint32_t r[8]; /* EAX ECX EDX EBX ESP EBP ESI EDI */
    uint32_t eip;
    uint32_t eflags_cf, eflags_zf, eflags_sf, eflags_of, eflags_pf, eflags_af, eflags_df;
    /* Every EFLAGS bit other than CF PF AF ZF SF DF OF, so PUSHFD/POPFD
     * round-trip (the CPUID probe toggles the ID bit).  Initialise to 0x202. */
    uint32_t eflags_misc;
    X87Value st[8]; /* physical ext80 registers; MMX aliases signif */
    uint32_t fpu_top; /* ST(i) == st[(fpu_top + i) & 7] */
    uint16_t fpu_cw, fpu_sw;
    /* x87 tag word, 2 bits per PHYSICAL register st[i]: 3 = empty, else in
     * use.  Initialise to 0xffff (the post-FINIT all-empty state). */
    uint16_t fpu_tag;
    uint16_t fpu_pending, fpu_opcode, fpu_cs, fpu_ds;
    uint32_t fpu_ip, fpu_dp;
    uint32_t mxcsr;
    uint32_t fs_base;
    /* SSE2. The translator models the data-movement subset only -
     * loads, stores, a dword broadcast - which is all a Delphi runtime's
     * FillChar and Move use, and byte order is then the only thing that has
     * to be right. There are eight in 32-bit mode. CPUID advertises no SSE,
     * so nothing chooses one of these paths from a feature test; the RTL
     * takes them unconditionally because every CPU it supports has SSE2.
     * Held as dwords: every modelled form - load, store, MOVD, MOVQ and
     * PSHUFD's selector - addresses the register in dword lanes, and on a
     * little-endian host the byte order of a copy then takes care of
     * itself. Lane 0 is the low four bytes. */
    uint32_t xmm[8][4];
    uint16_t cs, ds; /* guest selectors used by saved x87 instruction/data pointers */
    /* Host scheduling bookkeeping of this guest thread, not architectural state (seh.h recomp_block): the start of
     * the last block entered and the instructions executed since the thread last offered the execution baton.
     * Here rather than thread-local: on Darwin every access to a thread-local from the generated code is a call. */
    uint32_t source_pc, budget;
    /* Direct calls (seh.h RECOMP_DIRECT_*) nested in the host frames of this thread. */
    uint32_t direct_depth;

};
typedef struct X86 X86;

/* ----------------------------------------------------------------- SSE */

/* An XMM register holds dwords (see struct X86), so a scalar double or float
 * in its low lanes is read and written through these. Scalar SSE arithmetic
 * leaves the lanes above its result alone, which is why nothing here clears
 * them. */
static inline double xmm_f64(const X86 *c, int reg) {
    uint64_t bits = (uint64_t)c->xmm[reg][0] | ((uint64_t)c->xmm[reg][1] << 32);
    double v;
    memcpy(&v, &bits, 8);
    return v;
}
static inline void xmm_set_f64(X86 *c, int reg, double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    c->xmm[reg][0] = (uint32_t)bits;
    c->xmm[reg][1] = (uint32_t)(bits >> 32);
}
static inline float xmm_f32(const X86 *c, int reg) {
    float v;
    memcpy(&v, &c->xmm[reg][0], 4);
    return v;
}
static inline void xmm_set_f32(X86 *c, int reg, float v) {
    memcpy(&c->xmm[reg][0], &v, 4);
}

/* SQRTSD and SQRTSS of a negative operand return the "indefinite" quiet NaN,
 * which has its sign bit set; the host's sqrt returns a positive NaN and sets
 * errno. Everything else is the same operation in the same rounding mode. */
/* The sign is tested as a bit rather than by comparing, because a compiler
 * that may assume a NaN's payload is unspecified folds the comparison away
 * and leaves the host's own sqrt to produce its own NaN - which on ARM is the
 * positive one. */
static inline double recomp_sse_sqrt(double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    if ((bits >> 63) && (bits << 1) != 0) { /* negative, and not -0.0 */
        uint64_t indefinite = 0xfff8000000000000ull;
        double r;
        memcpy(&r, &indefinite, 8);
        return r;
    }
    return sqrt(v);
}
static inline float recomp_sse_sqrtf(float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    if ((bits >> 31) && (bits << 1) != 0) {
        uint32_t indefinite = 0xffc00000u;
        float r;
        memcpy(&r, &indefinite, 4);
        return r;
    }
    return sqrtf(v);
}

/* COMISD/UCOMISD and their single-precision forms: the one place SSE writes
 * the guest's flags. Unordered - either operand a NaN - sets ZF, PF and CF
 * together, and the rest of EFLAGS is cleared either way. The difference
 * between the two forms is only which NaNs raise an exception, and this kit
 * raises none. */
static inline void recomp_comis(X86 *c, double a, double b) {
    c->eflags_of = c->eflags_af = c->eflags_sf = 0;
    if (a != a || b != b) {
        c->eflags_zf = c->eflags_pf = c->eflags_cf = 1;
    } else {
        c->eflags_pf = 0;
        c->eflags_zf = a == b;
        c->eflags_cf = a < b;
    }
}

/* ------------------------------------------------- runtime call-outs
 * Implemented in generated table.c (recomp_call) or by runtime/.
 */

/* Indirect CALL: dispatch `target` to a translated function, an import shim,
 * or recomp_unknown_call.  Generated into build/recomp/gen/table.c. */
void recomp_call(X86 *c, uint32_t target);
/* Entry/callback driver for images that switch cooperative guest stacks. */
void recomp_run(X86 *c, uint32_t target);
extern const int recomp_resumable_stacks;

/* RECOMP_WATCH_FRAME=1 reports a guest call that returns with EBP changed.
 * A routine that loses the frame pointer corrupts nothing and crashes nowhere:
 * its caller simply reads its locals from somewhere else afterwards, and the
 * damage surfaces as a wrong value far away. Off, this costs one register copy
 * and one compare per call, both of which the optimiser folds away. */
extern uint32_t recomp_frame_watch;
void recomp_frame_changed(X86 *c, uint32_t target, uint32_t before, uint32_t after);
extern const int recomp_profile_enabled;
void recomp_profile_push(uint32_t index);
void recomp_profile_pop(void);

/* Indirect JMP or tail call: dispatch `target` through the generated
 * block-entry table (every function start plus every decoded jump-table
 * target), then the import shim range.  Anything else aborts. */
void recomp_jump(X86 *c, uint32_t target);

/* Indirect JMP whose target is neither a block entry nor a shim.  Aborts with
 * the target and the jumping instruction's address. */
RECOMP_COLD void recomp_unknown_jump(X86 *c, uint32_t target);

/* Provided by runtime/ ------------------------------------- */

/* target is in [GUEST_SHIM_BASE, GUEST_SHIM_END): run import shim
 * (target - GUEST_SHIM_BASE) / GUEST_SHIM_STRIDE. */
void recomp_shim_call(X86 *c, uint32_t target);
/* target is neither a translated function nor a shim.  Logs; may abort. */
RECOMP_COLD void recomp_unknown_call(X86 *c, uint32_t target);
/* A popped callback sentinel can bypass translated callers. The innermost
 * guest_call owns the live host return checkpoint and its guest stack range. */
void recomp_callback_return(X86 *c);
uint32_t recomp_callback_depth(void);
void recomp_callback_truncate(uint32_t depth);
void recomp_callback_reset(X86 *c);
/* DIV/IDIV with a zero divisor or a quotient that does not fit. */
void recomp_div_error(X86 *c, uint32_t addr);

void recomp_rdtsc(X86 *c); /* -> EDX:EAX */
void recomp_cpuid(X86 *c); /* EAX in -> EAX EBX ECX EDX */
uint32_t recomp_in(X86 *c, uint32_t port, int size);
void recomp_out(X86 *c, uint32_t port, uint32_t val, int size);
void recomp_cli(X86 *c);
void recomp_sti(X86 *c);
void recomp_hlt(X86 *c);
void recomp_int(X86 *c, uint32_t vec);
/* An instruction the translator could not model, reached at run time. The
 * translation carries a trap at that address instead of refusing the whole
 * image, because a listing routinely decodes the data past a function's last
 * instruction as code. Reaching one is fatal and says where. */
RECOMP_COLD void recomp_unmodelled(X86 *c, uint32_t addr);
void recomp_breakpoint(X86 *c, uint32_t addr);

/* ------------------------------------------------------ hook dispatch -- */

/* Three tables, generated into build/recomp/gen/table.c:
 *   recomp_func_addrs   sorted address index, immutable
 *   recomp_base_ptrs    the original for each entry, fixed at build time
 *   recomp_hook_ptrs    mutable dispatch, initially "run the base"
 * plus recomp_hooked, the per-function flag every call site reads with an
 * ACQUIRE load. The installer publishes the pointer with a release store and
 * then the flag with a release store, so a set flag always implies a visible
 * pointer. The index is passed to the dispatcher because one C function serves
 * every hooked entry. */
typedef void (*RecompHookFn)(X86 *c, uint32_t index);

/* Sorted table of every translated entry point (function starts, alternate
 * entries and jump-table block entries), from the generated table.c. */
extern const uint32_t recomp_func_addrs[];
extern const uint32_t recomp_func_count;
extern void (*const recomp_base_ptrs[])(X86 *c);
/* The untouched translation of each entry. recomp_base_ptrs holds this build's
 * FN_<addr> override where one is defined, so the two tables differ exactly
 * over the override set - which is how a run record states what was replaced,
 * instead of trusting an environment variable read after the build. */
extern void (*const recomp_raw_ptrs[])(X86 *c);
extern RecompHookFn recomp_hook_ptrs[];
extern uint8_t recomp_hooked[];

uint32_t recomp_override_count(void);
uint64_t recomp_override_hash(void);

/* Index into the three tables, or -1 when addr is not an entry. Being an entry
 * is NOT the same as being hookable: eligibility comes from symbols.json. */
int32_t recomp_index_of(uint32_t addr);
int recomp_is_call_return(uint32_t target);

/* ------------------------------------------------ auxiliary modules -- */

/* A second guest image (a DLL the game loads by name) translated by
 * `translate.py --module <key>` into its own tables. Its table.c registers
 * the module from a constructor; the main image's recomp_call/recomp_jump
 * fall back to the registry after their own table misses. The runtime
 * loader maps the module's sections at [base, end), and LoadLibrary /
 * GetProcAddress answer from the module's export directory. */
typedef struct RecompModule {
    const char *name;
    uint32_t base, end;
    const uint32_t *func_addrs;
    uint32_t func_count;
    void (*const *base_ptrs)(X86 *c);
    RecompHookFn *hook_ptrs;
    uint8_t *hooked;
    const uint32_t *call_returns;
    uint32_t call_return_count;
    const char *const *profile_names;
} RecompModule;

void recomp_module_register(const RecompModule *m);
uint32_t recomp_module_count(void);
const RecompModule *recomp_module_at(uint32_t i);
const RecompModule *recomp_module_named(const char *name);
const RecompModule *recomp_module_containing(uint32_t addr);
/* Index into the owning module's tables, or -1 when no module has addr as an entry. */
int32_t recomp_module_lookup(uint32_t target);
int recomp_module_is_call_return(uint32_t target);
/* Dispatches target through the owning module's tables; 0 when no module owns it. */
int recomp_module_call(X86 *c, uint32_t target);

/* RET has already popped EIP and applied any immediate stack adjustment.
 * A CALL continuation belongs to the pending host caller, even when it is
 * also an alternate entry. Other entries are tail calls, as in interface
 * adapters that exchange a vtable method onto the guest stack before RET.
 * Delay-load adapters also RET into resolved import shims. Unknown returns
 * retain the existing EIP/host-return behaviour. */
static inline void recomp_return(X86 *c) {
#ifdef SACRED_DRIVER_CHECKPOINTS
    (void)c; /* Actual EIP/ESP are consumed by the owning C driver. */
#else
    if (c->eip == GUEST_RETURN_SENTINEL) {
        recomp_callback_return(c);
        return;
    }
    if (recomp_resumable_stacks)
        return;
    if (recomp_is_call_return(c->eip) || recomp_module_is_call_return(c->eip))
        return;
    if ((c->eip >= GUEST_SHIM_BASE && c->eip < GUEST_SHIM_END) || recomp_index_of(c->eip) >= 0 ||
        recomp_module_lookup(c->eip) >= 0)
        recomp_call(c, c->eip);
#endif
}

/* --------------------------------------------------------------- flags */

RECOMP_HOT_INLINE uint32_t parity8(uint32_t v) {
    v &= 0xffu;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (~v) & 1u;
}

/* Bits x86_get_eflags/x86_set_eflags build from the individual fields; every
 * other bit lives in eflags_misc so PUSHFD/POPFD is lossless. */
#define X86_EFLAGS_SPLIT 0x00000cd5u /* CF PF AF ZF SF DF OF */

static inline uint32_t x86_get_eflags(const X86 *c) {
    return (c->eflags_misc & ~X86_EFLAGS_SPLIT) | 0x00000002u /* reserved bit 1 reads as 1 */
           | (c->eflags_cf << 0) | (c->eflags_pf << 2) | (c->eflags_af << 4) | (c->eflags_zf << 6) |
           (c->eflags_sf << 7) | (c->eflags_df << 10) | (c->eflags_of << 11);
}

static inline void x86_set_eflags(X86 *c, uint32_t v) {
    c->eflags_misc = (v & ~X86_EFLAGS_SPLIT) | 0x00000002u;
    c->eflags_cf = (v >> 0) & 1u;
    c->eflags_pf = (v >> 2) & 1u;
    c->eflags_af = (v >> 4) & 1u;
    c->eflags_zf = (v >> 6) & 1u;
    c->eflags_sf = (v >> 7) & 1u;
    c->eflags_df = (v >> 10) & 1u;
    c->eflags_of = (v >> 11) & 1u;
}

/* SAHF: AH -> SF ZF AF PF CF */
static inline void x86_sahf(X86 *c) {
    uint32_t ah = (c->r[R_EAX] >> 8) & 0xffu;
    c->eflags_cf = (ah >> 0) & 1u;
    c->eflags_pf = (ah >> 2) & 1u;
    c->eflags_af = (ah >> 4) & 1u;
    c->eflags_zf = (ah >> 6) & 1u;
    c->eflags_sf = (ah >> 7) & 1u;
}

/* ------------------------------------------------------ shifts / rotates
 * The `_f` variants also write the flags the instruction defines.  A shift
 * or rotate with a zero count leaves every flag untouched, which is why
 * these are functions rather than inline expressions.
 */
/* Every shift is done on uint32_t: a uint8_t or uint16_t operand would
 * otherwise promote to signed int and `(uint16_t)0xffff << 16` is UB even
 * though the count is masked.  SAR shifts the sign-extended value, and a
 * count >= BITS saturates to BITS-1 so the arithmetic shift stays defined. */
#define SHIFT_OPS(BITS, UT, ST)                                                                    \
    static inline UT shl##BITS(UT v, uint32_t n) {                                                 \
        return (UT)((uint32_t)v << (n & 31u));                                                     \
    }                                                                                              \
    static inline UT shr##BITS(UT v, uint32_t n) {                                                 \
        return (UT)((uint32_t)v >> (n & 31u));                                                     \
    }                                                                                              \
    static inline UT sar##BITS(UT v, uint32_t n) {                                                 \
        uint32_t k = n & 31u;                                                                      \
        if (k >= BITS)                                                                             \
            k = BITS - 1;                                                                          \
        return (UT)((int32_t)(ST)v >> k);                                                          \
    }                                                                                              \
    static inline UT shl##BITS##_f(X86 *c, UT v, uint32_t n) {                                     \
        uint32_t cnt = n & 31u;                                                                    \
        UT r;                                                                                      \
        if (!cnt)                                                                                  \
            return v;                                                                              \
        if (cnt <= BITS)                                                                           \
            c->eflags_cf = (uint32_t)(((uint32_t)v >> (BITS - cnt)) & 1u);                         \
        else                                                                                       \
            c->eflags_cf = 0;                                                                      \
        r = (UT)(cnt >= BITS ? 0u : ((uint32_t)v << cnt));                                         \
        c->eflags_of = ((uint32_t)(r >> (BITS - 1)) & 1u) ^ c->eflags_cf;                          \
        c->eflags_zf = (r == 0);                                                                   \
        c->eflags_sf = (uint32_t)(r >> (BITS - 1)) & 1u;                                           \
        c->eflags_pf = parity8((uint32_t)r);                                                       \
        return r;                                                                                  \
    }                                                                                              \
    static inline UT shr##BITS##_f(X86 *c, UT v, uint32_t n) {                                     \
        uint32_t cnt = n & 31u;                                                                    \
        UT r;                                                                                      \
        if (!cnt)                                                                                  \
            return v;                                                                              \
        c->eflags_cf = cnt <= BITS ? (uint32_t)(((uint32_t)v >> (cnt - 1)) & 1u) : 0u;             \
        r = (UT)(cnt >= BITS ? 0u : ((uint32_t)v >> cnt));                                         \
        c->eflags_of = (uint32_t)(v >> (BITS - 1)) & 1u;                                           \
        c->eflags_zf = (r == 0);                                                                   \
        c->eflags_sf = (uint32_t)(r >> (BITS - 1)) & 1u;                                           \
        c->eflags_pf = parity8((uint32_t)r);                                                       \
        return r;                                                                                  \
    }                                                                                              \
    static inline UT sar##BITS##_f(X86 *c, UT v, uint32_t n) {                                     \
        uint32_t cnt = n & 31u;                                                                    \
        UT r;                                                                                      \
        if (!cnt)                                                                                  \
            return v;                                                                              \
        if (cnt >= BITS)                                                                           \
            cnt = BITS;                                                                            \
        c->eflags_cf = (uint32_t)(((int32_t)(ST)v >> (cnt - 1)) & 1);                              \
        r = (UT)((int32_t)(ST)v >> (cnt == BITS ? BITS - 1 : cnt));                                \
        c->eflags_of = 0;                                                                          \
        c->eflags_zf = (r == 0);                                                                   \
        c->eflags_sf = (uint32_t)(r >> (BITS - 1)) & 1u;                                           \
        c->eflags_pf = parity8((uint32_t)r);                                                       \
        return r;                                                                                  \
    }                                                                                              \
    static inline UT rol##BITS(UT v, uint32_t n) {                                                 \
        uint32_t cnt = (n & 31u) % BITS, w = (uint32_t)v;                                          \
        return cnt ? (UT)((w << cnt) | (w >> (BITS - cnt))) : v;                                   \
    }                                                                                              \
    static inline UT ror##BITS(UT v, uint32_t n) {                                                 \
        uint32_t cnt = (n & 31u) % BITS, w = (uint32_t)v;                                          \
        return cnt ? (UT)((w >> cnt) | (w << (BITS - cnt))) : v;                                   \
    }                                                                                              \
    static inline UT rol##BITS##_f(X86 *c, UT v, uint32_t n) {                                     \
        UT r;                                                                                      \
        if (!(n & 31u))                                                                            \
            return v;                                                                              \
        r = rol##BITS(v, n);                                                                       \
        c->eflags_cf = (uint32_t)r & 1u;                                                           \
        c->eflags_of = ((uint32_t)(r >> (BITS - 1)) & 1u) ^ c->eflags_cf;                          \
        return r;                                                                                  \
    }                                                                                              \
    static inline UT ror##BITS##_f(X86 *c, UT v, uint32_t n) {                                     \
        UT r;                                                                                      \
        if (!(n & 31u))                                                                            \
            return v;                                                                              \
        r = ror##BITS(v, n);                                                                       \
        c->eflags_cf = (uint32_t)(r >> (BITS - 1)) & 1u;                                           \
        c->eflags_of = c->eflags_cf ^ ((uint32_t)(r >> (BITS - 2)) & 1u);                          \
        return r;                                                                                  \
    }                                                                                              \
    static inline UT rcl##BITS##_f(X86 *c, UT v, uint32_t n) {                                     \
        uint32_t cnt = (n & 31u) % (BITS + 1), i;                                                  \
        for (i = 0; i < cnt; i++) {                                                                \
            uint32_t hi = (uint32_t)(v >> (BITS - 1)) & 1u;                                        \
            v = (UT)((v << 1) | c->eflags_cf);                                                     \
            c->eflags_cf = hi;                                                                     \
        }                                                                                          \
        if (n & 31u)                                                                               \
            c->eflags_of = ((uint32_t)(v >> (BITS - 1)) & 1u) ^ c->eflags_cf;                      \
        return v;                                                                                  \
    }                                                                                              \
    static inline UT rcr##BITS##_f(X86 *c, UT v, uint32_t n) {                                     \
        uint32_t cnt = (n & 31u) % (BITS + 1), i;                                                  \
        if (n & 31u)                                                                               \
            c->eflags_of = ((uint32_t)(v >> (BITS - 1)) & 1u) ^ c->eflags_cf;                      \
        for (i = 0; i < cnt; i++) {                                                                \
            uint32_t lo = (uint32_t)v & 1u;                                                        \
            v = (UT)(((uint32_t)v >> 1) | (c->eflags_cf << (BITS - 1)));                           \
            c->eflags_cf = lo;                                                                     \
        }                                                                                          \
        return v;                                                                                  \
    }
SHIFT_OPS(8, uint8_t, int8_t)
SHIFT_OPS(16, uint16_t, int16_t)
SHIFT_OPS(32, uint32_t, int32_t)
#undef SHIFT_OPS

/* SHLD/SHRD (32-bit forms only; the corpus has no 16-bit double shift). */
static inline uint32_t shld32_f(X86 *c, uint32_t d, uint32_t s, uint32_t n) {
    uint32_t cnt = n & 31u, r;
    if (!cnt)
        return d;
    r = (d << cnt) | (s >> (32 - cnt));
    c->eflags_cf = (d >> (32 - cnt)) & 1u;
    c->eflags_of = ((r >> 31) & 1u) ^ c->eflags_cf;
    c->eflags_zf = (r == 0);
    c->eflags_sf = (r >> 31) & 1u;
    c->eflags_pf = parity8(r);
    return r;
}
static inline uint32_t shrd32_f(X86 *c, uint32_t d, uint32_t s, uint32_t n) {
    uint32_t cnt = n & 31u, r;
    if (!cnt)
        return d;
    r = (d >> cnt) | (s << (32 - cnt));
    c->eflags_cf = (d >> (cnt - 1)) & 1u;
    c->eflags_of = ((r >> 31) & 1u) ^ ((d >> 31) & 1u);
    c->eflags_zf = (r == 0);
    c->eflags_sf = (r >> 31) & 1u;
    c->eflags_pf = parity8(r);
    return r;
}

/* ------------------------------------------------------ multiply / divide */

static inline void mul8(X86 *c, uint8_t s) {
    uint32_t p = (uint32_t)(uint8_t)c->r[R_EAX] * s;
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | (p & 0xffffu);
    c->eflags_cf = c->eflags_of = ((p >> 8) != 0);
}
static inline void mul16(X86 *c, uint16_t s) {
    uint32_t p = (uint32_t)(uint16_t)c->r[R_EAX] * s;
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | (p & 0xffffu);
    c->r[R_EDX] = (c->r[R_EDX] & 0xffff0000u) | ((p >> 16) & 0xffffu);
    c->eflags_cf = c->eflags_of = ((p >> 16) != 0);
}
static inline void mul32(X86 *c, uint32_t s) {
    uint64_t p = (uint64_t)c->r[R_EAX] * s;
    c->r[R_EAX] = (uint32_t)p;
    c->r[R_EDX] = (uint32_t)(p >> 32);
    c->eflags_cf = c->eflags_of = (c->r[R_EDX] != 0);
}

static inline void imul8(X86 *c, uint8_t s) {
    int32_t p = (int32_t)(int8_t)c->r[R_EAX] * (int8_t)s;
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | ((uint32_t)p & 0xffffu);
    c->eflags_cf = c->eflags_of = (p != (int8_t)p);
}
static inline void imul16(X86 *c, uint16_t s) {
    int32_t p = (int32_t)(int16_t)c->r[R_EAX] * (int16_t)s;
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | ((uint32_t)p & 0xffffu);
    c->r[R_EDX] = (c->r[R_EDX] & 0xffff0000u) | (((uint32_t)p >> 16) & 0xffffu);
    c->eflags_cf = c->eflags_of = (p != (int16_t)p);
}
static inline void imul32(X86 *c, uint32_t s) {
    int64_t p = (int64_t)(int32_t)c->r[R_EAX] * (int32_t)s;
    c->r[R_EAX] = (uint32_t)p;
    c->r[R_EDX] = (uint32_t)((uint64_t)p >> 32);
    c->eflags_cf = c->eflags_of = (p != (int32_t)p);
}

/* Two/three-operand IMUL: value is the truncated product; CF/OF on overflow. */
/* The low half of a signed product is the low half of the unsigned product;
 * computing it unsigned keeps the wraparound defined. */
static inline uint32_t imul2_32(uint32_t a, uint32_t b) {
    return a * b;
}
static inline uint32_t imul2_32_f(X86 *c, uint32_t a, uint32_t b) {
    int64_t p = (int64_t)(int32_t)a * (int32_t)b;
    c->eflags_cf = c->eflags_of = (p != (int32_t)p);
    return (uint32_t)p;
}
static inline uint16_t imul2_16(uint16_t a, uint16_t b) {
    return (uint16_t)((uint32_t)a * b);
}
static inline uint16_t imul2_16_f(X86 *c, uint16_t a, uint16_t b) {
    int32_t p = (int32_t)(int16_t)a * (int16_t)b;
    c->eflags_cf = c->eflags_of = (p != (int16_t)p);
    return (uint16_t)p;
}

static inline void div8(X86 *c, uint8_t s, uint32_t at) {
    uint32_t n = (uint16_t)c->r[R_EAX], q;
    if (!s) {
        recomp_div_error(c, at);
        return;
    }
    q = n / s;
    if (q > 0xffu) {
        recomp_div_error(c, at);
        return;
    }
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | (q & 0xffu) | ((n % s) << 8);
}
static inline void div16(X86 *c, uint16_t s, uint32_t at) {
    uint32_t n = ((uint32_t)(uint16_t)c->r[R_EDX] << 16) | (uint16_t)c->r[R_EAX], q;
    if (!s) {
        recomp_div_error(c, at);
        return;
    }
    q = n / s;
    if (q > 0xffffu) {
        recomp_div_error(c, at);
        return;
    }
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | q;
    c->r[R_EDX] = (c->r[R_EDX] & 0xffff0000u) | (n % s);
}
static inline void div32(X86 *c, uint32_t s, uint32_t at) {
    uint64_t n = ((uint64_t)c->r[R_EDX] << 32) | c->r[R_EAX], q;
    if (!s) {
        recomp_div_error(c, at);
        return;
    }
    q = n / s;
    if (q > 0xffffffffu) {
        recomp_div_error(c, at);
        return;
    }
    c->r[R_EAX] = (uint32_t)q;
    c->r[R_EDX] = (uint32_t)(n % s);
}

static inline void idiv8(X86 *c, uint8_t s, uint32_t at) {
    int32_t n = (int16_t)c->r[R_EAX], q;
    if (!s) {
        recomp_div_error(c, at);
        return;
    }
    if ((int8_t)s == -1 && n == INT32_MIN) {
        recomp_div_error(c, at);
        return;
    }
    q = n / (int8_t)s;
    if (q != (int8_t)q) {
        recomp_div_error(c, at);
        return;
    }
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | ((uint32_t)q & 0xffu) |
                  (((uint32_t)(n % (int8_t)s) & 0xffu) << 8);
}
static inline void idiv16(X86 *c, uint16_t s, uint32_t at) {
    int32_t n = (int32_t)(((uint32_t)(uint16_t)c->r[R_EDX] << 16) | (uint16_t)c->r[R_EAX]), q;
    if (!s) {
        recomp_div_error(c, at);
        return;
    }
    if ((int16_t)s == -1 && n == INT32_MIN) {
        recomp_div_error(c, at);
        return;
    }
    q = n / (int16_t)s;
    if (q != (int16_t)q) {
        recomp_div_error(c, at);
        return;
    }
    c->r[R_EAX] = (c->r[R_EAX] & 0xffff0000u) | ((uint32_t)q & 0xffffu);
    c->r[R_EDX] = (c->r[R_EDX] & 0xffff0000u) | ((uint32_t)(n % (int16_t)s) & 0xffffu);
}
static inline void idiv32(X86 *c, uint32_t s, uint32_t at) {
    int64_t n = (int64_t)(((uint64_t)c->r[R_EDX] << 32) | c->r[R_EAX]), q;
    if (!s) {
        recomp_div_error(c, at);
        return;
    }
    if ((int32_t)s == -1 && n == INT64_MIN) {
        recomp_div_error(c, at);
        return;
    }
    q = n / (int32_t)s;
    if (q != (int32_t)q) {
        recomp_div_error(c, at);
        return;
    }
    c->r[R_EAX] = (uint32_t)q;
    c->r[R_EDX] = (uint32_t)(n % (int32_t)s);
}

/* -------------------------------------------------------------- bit scan */

/* With a zero source the destination is architecturally undefined; unicorn,
 * like AMD, leaves it unchanged, so the old value is passed in. */
static inline uint32_t bsr32_f(X86 *c, uint32_t old, uint32_t v) {
    c->eflags_zf = (v == 0);
    return v ? 31u - (uint32_t)__builtin_clz(v) : old;
}
static inline uint32_t bsf32_f(X86 *c, uint32_t old, uint32_t v) {
    c->eflags_zf = (v == 0);
    return v ? (uint32_t)__builtin_ctz(v) : old;
}

/* ---------------------------------------------------------- push and pop */

static inline void x86_pushad(X86 *c) {
    uint32_t sp = c->r[R_ESP];
    c->r[R_ESP] -= 32;
    wr32(sp - 4, c->r[R_EAX]);
    wr32(sp - 8, c->r[R_ECX]);
    wr32(sp - 12, c->r[R_EDX]);
    wr32(sp - 16, c->r[R_EBX]);
    wr32(sp - 20, sp);
    wr32(sp - 24, c->r[R_EBP]);
    wr32(sp - 28, c->r[R_ESI]);
    wr32(sp - 32, c->r[R_EDI]);
}
static inline void x86_popad(X86 *c) {
    uint32_t sp = c->r[R_ESP];
    c->r[R_EDI] = rd32(sp + 0);
    c->r[R_ESI] = rd32(sp + 4);
    c->r[R_EBP] = rd32(sp + 8); /* skip saved ESP at sp+12 */
    c->r[R_EBX] = rd32(sp + 16);
    c->r[R_EDX] = rd32(sp + 20);
    c->r[R_ECX] = rd32(sp + 24);
    c->r[R_EAX] = rd32(sp + 28);
    c->r[R_ESP] = sp + 32;
}

/* ---------------------------------------------------------- string ops
 * DF selects the step direction.  The REP forms use ECX as the counter.
 */
/* Bulk REP MOVS / REP STOS (forward, no watch armed): every run that stays inside one readable source page and one
 * writable destination page is one copy, with the registers advanced as the element loop would leave them. Whatever
 * it does not take (a page span, an unmapped or protected page, a forward overlap that replicates a pattern, DF set)
 * is left to the element loop, which also raises the faults. */
#if defined(SACRED_DRIVER_CHECKPOINTS) && !(defined(RECOMP_NULL_CHECKS) && RECOMP_NULL_CHECKS)
RECOMP_HOT_INLINE void recomp_rep_movs_bulk(X86 *c, uint32_t size) {
    if (c->eflags_df || g_watch_len)
        return;
    while (c->r[R_ECX]) {
        const uint32_t s = c->r[R_ESI], d = c->r[R_EDI];
        const uint32_t room_s = 0x1000u - (s & 0xfffu), room_d = 0x1000u - (d & 0xfffu);
        uint32_t n = (room_s < room_d ? room_s : room_d) / size;
        if (n > c->r[R_ECX])
            n = c->r[R_ECX];
        const uint32_t bytes = n * size;
        if (!n || !(g_page_access[s >> 12] & 1u) || !(g_page_access[d >> 12] & 2u) || (d > s && d - s < bytes))
            return;
        memmove(g_mem + d, g_mem + s, bytes);
        if (g_dirty_count != 0)
            recomp_dirty(d, bytes);
        c->r[R_ESI] = s + bytes;
        c->r[R_EDI] = d + bytes;
        c->r[R_ECX] -= n;
    }
}
RECOMP_HOT_INLINE void recomp_rep_stos_bulk(X86 *c, uint32_t size) {
    if (c->eflags_df || g_watch_len)
        return;
    const uint32_t value = c->r[R_EAX];
    while (c->r[R_ECX]) {
        const uint32_t d = c->r[R_EDI];
        uint32_t n = (0x1000u - (d & 0xfffu)) / size;
        if (n > c->r[R_ECX])
            n = c->r[R_ECX];
        if (!n || !(g_page_access[d >> 12] & 2u))
            return;
        uint8_t *out = g_mem + d;
        if (size == 1)
            memset(out, (int)(value & 0xffu), n);
        else
            for (uint32_t i = 0; i < n; ++i)
                memcpy(out + i * size, &value, size);
        if (g_dirty_count != 0)
            recomp_dirty(d, n * size);
        c->r[R_EDI] = d + n * size;
        c->r[R_ECX] -= n;
    }
}
#else
#define recomp_rep_movs_bulk(c, size) ((void)0)
#define recomp_rep_stos_bulk(c, size) ((void)0)
#endif
#define STRING_OPS(SUF, BITS, UT, RD, WR)                                                          \
    static inline void stos##SUF(X86 *c) {                                                         \
        WR(c->r[R_EDI], (UT)c->r[R_EAX]);                                                          \
        c->r[R_EDI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
    }                                                                                              \
    static inline void rep_stos##SUF(X86 *c) {                                                     \
        recomp_rep_stos_bulk(c, BITS / 8);                                                         \
        while (c->r[R_ECX]) {                                                                      \
            stos##SUF(c);                                                                          \
            c->r[R_ECX]--;                                                                         \
        }                                                                                          \
    }                                                                                              \
    static inline void movs##SUF(X86 *c) {                                                         \
        WR(c->r[R_EDI], RD(c->r[R_ESI]));                                                          \
        c->r[R_EDI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
        c->r[R_ESI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
    }                                                                                              \
    static inline void rep_movs##SUF(X86 *c) {                                                     \
        recomp_rep_movs_bulk(c, BITS / 8);                                                         \
        while (c->r[R_ECX]) {                                                                      \
            movs##SUF(c);                                                                          \
            c->r[R_ECX]--;                                                                         \
        }                                                                                          \
    }                                                                                              \
    /* The port string forms. A user-mode guest that reaches one has been      \
     * misdecoded - Windows would fault - so they exist to be translatable      \
     * rather than useful: the port shims answer, and the pointer and count     \
     * advance exactly as the other string forms do. */                   \
    static inline void ins##SUF(X86 *c) {                                                          \
        WR(c->r[R_EDI], (UT)recomp_in(c, c->r[R_EDX] & 0xffffu, BITS / 8));                        \
        c->r[R_EDI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
    }                                                                                              \
    static inline void rep_ins##SUF(X86 *c) {                                                      \
        while (c->r[R_ECX]) {                                                                      \
            ins##SUF(c);                                                                           \
            c->r[R_ECX]--;                                                                         \
        }                                                                                          \
    }                                                                                              \
    static inline void outs##SUF(X86 *c) {                                                         \
        recomp_out(c, c->r[R_EDX] & 0xffffu, RD(c->r[R_ESI]), BITS / 8);                           \
        c->r[R_ESI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
    }                                                                                              \
    static inline void rep_outs##SUF(X86 *c) {                                                     \
        while (c->r[R_ECX]) {                                                                      \
            outs##SUF(c);                                                                          \
            c->r[R_ECX]--;                                                                         \
        }                                                                                          \
    }                                                                                              \
    static inline void lods##SUF(X86 *c) {                                                         \
        UT v = RD(c->r[R_ESI]);                                                                    \
        if (BITS == 32)                                                                            \
            c->r[R_EAX] = (uint32_t)v;                                                             \
        else                                                                                       \
            c->r[R_EAX] = (c->r[R_EAX] & ~(uint32_t)((1ull << BITS) - 1)) |                        \
                          ((uint32_t)v & (uint32_t)((1ull << BITS) - 1));                          \
        c->r[R_ESI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
    }
STRING_OPS(b, 8, uint8_t, rd8, wr8)
STRING_OPS(w, 16, uint16_t, rd16, wr16)
STRING_OPS(d, 32, uint32_t, rd32, wr32)
#undef STRING_OPS

/* SCAS/CMPS set the flags of a SUB.  Defined out of line below the generic
 * SUB flag helper. */
static inline void x86_sub_flags8(X86 *c, uint32_t a, uint32_t b);
static inline void x86_sub_flags16(X86 *c, uint32_t a, uint32_t b);
static inline void x86_sub_flags32(X86 *c, uint32_t a, uint32_t b);

static inline void x86_sub_flags8(X86 *c, uint32_t a, uint32_t b) {
    uint32_t r = (a - b) & 0xffu;
    c->eflags_cf = ((a & 0xffu) < (b & 0xffu));
    c->eflags_of = (((a ^ b) & (a ^ r)) >> 7) & 1u;
    c->eflags_af = ((a ^ b ^ r) >> 4) & 1u;
    c->eflags_zf = (r == 0);
    c->eflags_sf = (r >> 7) & 1u;
    c->eflags_pf = parity8(r);
}
static inline void x86_sub_flags16(X86 *c, uint32_t a, uint32_t b) {
    uint32_t r = (a - b) & 0xffffu;
    c->eflags_cf = ((a & 0xffffu) < (b & 0xffffu));
    c->eflags_of = (((a ^ b) & (a ^ r)) >> 15) & 1u;
    c->eflags_af = ((a ^ b ^ r) >> 4) & 1u;
    c->eflags_zf = (r == 0);
    c->eflags_sf = (r >> 15) & 1u;
    c->eflags_pf = parity8(r);
}
static inline void x86_sub_flags32(X86 *c, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    c->eflags_cf = (a < b);
    c->eflags_of = (((a ^ b) & (a ^ r)) >> 31) & 1u;
    c->eflags_af = ((a ^ b ^ r) >> 4) & 1u;
    c->eflags_zf = (r == 0);
    c->eflags_sf = (r >> 31) & 1u;
    c->eflags_pf = parity8(r);
}

static inline void scasb(X86 *c) {
    x86_sub_flags8(c, c->r[R_EAX] & 0xffu, rd8(c->r[R_EDI]));
    c->r[R_EDI] += c->eflags_df ? (uint32_t)-1 : 1u;
}
static inline void repne_scasb(X86 *c) {
    while (c->r[R_ECX]) {
        scasb(c);
        c->r[R_ECX]--;
        if (c->eflags_zf)
            break;
    }
}
static inline void repe_scasb(X86 *c) {
    while (c->r[R_ECX]) {
        scasb(c);
        c->r[R_ECX]--;
        if (!c->eflags_zf)
            break;
    }
}
static inline void cmpsb(X86 *c) {
    x86_sub_flags8(c, rd8(c->r[R_ESI]), rd8(c->r[R_EDI]));
    c->r[R_EDI] += c->eflags_df ? (uint32_t)-1 : 1u;
    c->r[R_ESI] += c->eflags_df ? (uint32_t)-1 : 1u;
}
static inline void repe_cmpsb(X86 *c) {
    while (c->r[R_ECX]) {
        cmpsb(c);
        c->r[R_ECX]--;
        if (!c->eflags_zf)
            break;
    }
}
static inline void repne_cmpsb(X86 *c) {
    while (c->r[R_ECX]) {
        cmpsb(c);
        c->r[R_ECX]--;
        if (c->eflags_zf)
            break;
    }
}

/* The same at word and dword width.  A REP-prefixed compare with ECX = 0
 * runs nothing and leaves every flag as it found it, like the byte forms. */
#define X86_CMPS_SCAS(SUF, BITS, MASK)                                                             \
    static inline void scas##SUF(X86 *c) {                                                         \
        x86_sub_flags##BITS(c, c->r[R_EAX] & (MASK), rd##BITS(c->r[R_EDI]));                       \
        c->r[R_EDI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
    }                                                                                              \
    static inline void cmps##SUF(X86 *c) {                                                         \
        x86_sub_flags##BITS(c, rd##BITS(c->r[R_ESI]), rd##BITS(c->r[R_EDI]));                      \
        c->r[R_EDI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
        c->r[R_ESI] += c->eflags_df ? (uint32_t)-(BITS / 8) : (uint32_t)(BITS / 8);                \
    }                                                                                              \
    static inline void repe_scas##SUF(X86 *c) {                                                    \
        while (c->r[R_ECX]) {                                                                      \
            scas##SUF(c);                                                                          \
            c->r[R_ECX]--;                                                                         \
            if (!c->eflags_zf)                                                                     \
                break;                                                                             \
        }                                                                                          \
    }                                                                                              \
    static inline void repne_scas##SUF(X86 *c) {                                                   \
        while (c->r[R_ECX]) {                                                                      \
            scas##SUF(c);                                                                          \
            c->r[R_ECX]--;                                                                         \
            if (c->eflags_zf)                                                                      \
                break;                                                                             \
        }                                                                                          \
    }                                                                                              \
    static inline void repe_cmps##SUF(X86 *c) {                                                    \
        while (c->r[R_ECX]) {                                                                      \
            cmps##SUF(c);                                                                          \
            c->r[R_ECX]--;                                                                         \
            if (!c->eflags_zf)                                                                     \
                break;                                                                             \
        }                                                                                          \
    }                                                                                              \
    static inline void repne_cmps##SUF(X86 *c) {                                                   \
        while (c->r[R_ECX]) {                                                                      \
            cmps##SUF(c);                                                                          \
            c->r[R_ECX]--;                                                                         \
            if (c->eflags_zf)                                                                      \
                break;                                                                             \
        }                                                                                          \
    }
X86_CMPS_SCAS(w, 16, 0xffffu)
X86_CMPS_SCAS(d, 32, 0xffffffffu)
#undef X86_CMPS_SCAS

#include "../../../runtime/cpu/x87.h"
#include "../../../runtime/cpu/simd.h"

/* ----------------------------------------------------------------- MMX ---
 * The MMn registers, as translate.py's MMX_BINARY/MMX_SHIFT emit them. Each
 * helper takes and returns a packed 64-bit register; the lane width is the
 * caller's, so one helper serves every element size of an operation. */
static inline uint64_t mmx_mask(unsigned bits) {
    return bits >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << bits) - 1);
}
static inline uint64_t mmx_lane(uint64_t v, unsigned bits, unsigned i) {
    return (v >> (bits * i)) & mmx_mask(bits);
}
static inline int64_t mmx_slane(uint64_t v, unsigned bits, unsigned i) {
    uint64_t x = mmx_lane(v, bits, i);
    if (bits < 64 && (x >> (bits - 1)) & 1)
        x |= ~mmx_mask(bits);
    return (int64_t)x;
}
static inline uint64_t mmx_put(uint64_t acc, unsigned bits, unsigned i, uint64_t x) {
    uint64_t m = mmx_mask(bits);
    return (acc & ~(m << (bits * i))) | ((x & m) << (bits * i));
}
static inline int64_t mmx_sat_s(int64_t x, unsigned bits) {
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -hi - 1;
    return x > hi ? hi : x < lo ? lo : x;
}
static inline int64_t mmx_sat_u(int64_t x, unsigned bits) {
    int64_t hi = ((int64_t)1 << bits) - 1;
    return x > hi ? hi : x < 0 ? 0 : x;
}
#define MMX_LANES(bits) (64u / (bits))
static inline uint64_t mmx_padd(uint64_t a, uint64_t b, unsigned bits) {
    uint64_t r = 0;
    for (unsigned i = 0; i < MMX_LANES(bits); i++)
        r = mmx_put(r, bits, i, mmx_lane(a, bits, i) + mmx_lane(b, bits, i));
    return r;
}
static inline uint64_t mmx_psub(uint64_t a, uint64_t b, unsigned bits) {
    uint64_t r = 0;
    for (unsigned i = 0; i < MMX_LANES(bits); i++)
        r = mmx_put(r, bits, i, mmx_lane(a, bits, i) - mmx_lane(b, bits, i));
    return r;
}
static inline uint64_t mmx_padds(uint64_t a, uint64_t b, unsigned bits) {
    uint64_t r = 0;
    for (unsigned i = 0; i < MMX_LANES(bits); i++)
        r = mmx_put(r, bits, i,
                    (uint64_t)mmx_sat_s(mmx_slane(a, bits, i) + mmx_slane(b, bits, i), bits));
    return r;
}
static inline uint64_t mmx_psubs(uint64_t a, uint64_t b, unsigned bits) {
    uint64_t r = 0;
    for (unsigned i = 0; i < MMX_LANES(bits); i++)
        r = mmx_put(r, bits, i,
                    (uint64_t)mmx_sat_s(mmx_slane(a, bits, i) - mmx_slane(b, bits, i), bits));
    return r;
}
static inline uint64_t mmx_paddus(uint64_t a, uint64_t b, unsigned bits) {
    uint64_t r = 0;
    for (unsigned i = 0; i < MMX_LANES(bits); i++)
        r = mmx_put(r, bits, i,
                    (uint64_t)mmx_sat_u(
                        (int64_t)mmx_lane(a, bits, i) + (int64_t)mmx_lane(b, bits, i), bits));
    return r;
}
static inline uint64_t mmx_psubus(uint64_t a, uint64_t b, unsigned bits) {
    uint64_t r = 0;
    for (unsigned i = 0; i < MMX_LANES(bits); i++)
        r = mmx_put(r, bits, i,
                    (uint64_t)mmx_sat_u(
                        (int64_t)mmx_lane(a, bits, i) - (int64_t)mmx_lane(b, bits, i), bits));
    return r;
}
static inline uint64_t mmx_pmullw(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (unsigned i = 0; i < 4; i++)
        r = mmx_put(r, 16, i, (uint64_t)(mmx_slane(a, 16, i) * mmx_slane(b, 16, i)));
    return r;
}
static inline uint64_t mmx_pmulhw(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (unsigned i = 0; i < 4; i++)
        r = mmx_put(r, 16, i, (uint64_t)((mmx_slane(a, 16, i) * mmx_slane(b, 16, i)) >> 16));
    return r;
}
static inline uint64_t mmx_pmaddwd(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (unsigned i = 0; i < 2; i++) {
        int64_t s = mmx_slane(a, 16, 2 * i) * mmx_slane(b, 16, 2 * i) +
                    mmx_slane(a, 16, 2 * i + 1) * mmx_slane(b, 16, 2 * i + 1);
        r = mmx_put(r, 32, i, (uint64_t)s);
    }
    return r;
}
static inline uint64_t mmx_pcmpeq(uint64_t a, uint64_t b, unsigned bits) {
    uint64_t r = 0;
    for (unsigned i = 0; i < MMX_LANES(bits); i++)
        r = mmx_put(r, bits, i, mmx_lane(a, bits, i) == mmx_lane(b, bits, i) ? ~(uint64_t)0 : 0);
    return r;
}
static inline uint64_t mmx_pcmpgt(uint64_t a, uint64_t b, unsigned bits) {
    uint64_t r = 0;
    for (unsigned i = 0; i < MMX_LANES(bits); i++)
        r = mmx_put(r, bits, i, mmx_slane(a, bits, i) > mmx_slane(b, bits, i) ? ~(uint64_t)0 : 0);
    return r;
}
/* Packing: the destination's lanes narrow into the low half, the source's
 * into the high half. */
static inline uint64_t mmx_pack(uint64_t a, uint64_t b, unsigned from, int is_signed) {
    unsigned to = from / 2, n = 64 / from;
    uint64_t r = 0;
    for (unsigned i = 0; i < n; i++) {
        int64_t x = mmx_slane(a, from, i), y = mmx_slane(b, from, i);
        r = mmx_put(r, to, i, (uint64_t)(is_signed ? mmx_sat_s(x, to) : mmx_sat_u(x, to)));
        r = mmx_put(r, to, n + i, (uint64_t)(is_signed ? mmx_sat_s(y, to) : mmx_sat_u(y, to)));
    }
    return r;
}
static inline uint64_t mmx_packsswb(uint64_t a, uint64_t b) {
    return mmx_pack(a, b, 16, 1);
}
static inline uint64_t mmx_packssdw(uint64_t a, uint64_t b) {
    return mmx_pack(a, b, 32, 1);
}
static inline uint64_t mmx_packuswb(uint64_t a, uint64_t b) {
    return mmx_pack(a, b, 16, 0);
}
/* Unpacking interleaves the low (or high) half of each operand, destination
 * lane first. */
static inline uint64_t mmx_punpck(uint64_t a, uint64_t b, unsigned bits, unsigned half) {
    unsigned n = 64 / bits / 2;
    uint64_t r = 0;
    for (unsigned i = 0; i < n; i++) {
        r = mmx_put(r, bits, 2 * i, mmx_lane(a, bits, half + i));
        r = mmx_put(r, bits, 2 * i + 1, mmx_lane(b, bits, half + i));
    }
    return r;
}
static inline uint64_t mmx_punpckl(uint64_t a, uint64_t b, unsigned bits) {
    return mmx_punpck(a, b, bits, 0);
}
static inline uint64_t mmx_punpckh(uint64_t a, uint64_t b, unsigned bits) {
    return mmx_punpck(a, b, bits, 64 / bits / 2);
}
static inline uint64_t mmx_pand(uint64_t a, uint64_t b) {
    return a & b;
}
static inline uint64_t mmx_pandn(uint64_t a, uint64_t b) {
    return ~a & b;
}
static inline uint64_t mmx_por(uint64_t a, uint64_t b) {
    return a | b;
}
static inline uint64_t mmx_pxor(uint64_t a, uint64_t b) {
    return a ^ b;
}
/* Shifts take their count from all 64 bits of the source; a count past the
 * lane width clears the lane, or fills it with the sign for PSRA. */
static inline uint64_t mmx_psll(uint64_t a, uint64_t count, unsigned bits) {
    uint64_t r = 0;
    for (unsigned i = 0; i < MMX_LANES(bits); i++)
        r = mmx_put(r, bits, i, count >= bits ? 0 : mmx_lane(a, bits, i) << count);
    return r;
}
static inline uint64_t mmx_psrl(uint64_t a, uint64_t count, unsigned bits) {
    uint64_t r = 0;
    for (unsigned i = 0; i < MMX_LANES(bits); i++)
        r = mmx_put(r, bits, i, count >= bits ? 0 : mmx_lane(a, bits, i) >> count);
    return r;
}
static inline uint64_t mmx_psra(uint64_t a, uint64_t count, unsigned bits) {
    uint64_t r = 0;
    unsigned c = count >= bits ? bits - 1 : (unsigned)count;
    for (unsigned i = 0; i < MMX_LANES(bits); i++)
        r = mmx_put(r, bits, i, (uint64_t)(mmx_slane(a, bits, i) >> c));
    return r;
}

/* ----------------------------------------------------------------- misc */

static inline void xlat(X86 *c) {
    c->r[R_EAX] = (c->r[R_EAX] & 0xffffff00u) | rd8(c->r[R_EBX] + (c->r[R_EAX] & 0xffu));
}
static inline uint32_t bswap32(uint32_t v) {
    return __builtin_bswap32(v);
}

#ifdef __cplusplus
}
#endif
#endif /* RECOMP_X86_H */
