/* Guest x86 SEH checkpoints. Included by generated C as well as the runtime. */
#ifndef RECOMP_SEH_H
#define RECOMP_SEH_H
#include "x86.h"
#include <setjmp.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* setjmp must be executed by the live generated function, never in enter(). */
jmp_buf *recomp_seh_frame_enter(X86 *c);
/* Sacred driver mode: guest records only; no native jump buffer is registered. */
void recomp_seh_register(X86 *c);
void recomp_seh_driver_leave(X86 *c, uint64_t owner);
uint32_t recomp_seh_driver_records(X86 *c);
void recomp_guest_setjmp(X86 *c);
void recomp_guest_longjmp(X86 *c);
int sacred_transfer_pending(void);
/* Per guest thread, in its CPU context (X86.source_pc, X86.budget): the start of the last block entered, for
 * diagnostics and host call sources, and the instructions executed since the thread last offered the execution baton.
 * Inline so that a block costs two stores, not a call. */
#define RECOMP_BUDGET 50000u
RECOMP_COLD void recomp_yield(X86 *c);
/* A straight run of `count` instructions starting at `address`. */
RECOMP_HOT_INLINE void recomp_block(X86 *c, uint32_t address, uint32_t count) {
    c->source_pc = address;
    c->budget += count;
    if (RECOMP_UNLIKELY(c->budget >= RECOMP_BUDGET))
        recomp_yield(c);
}
RECOMP_HOT_INLINE void recomp_source(X86 *c, uint32_t address) { recomp_block(c, address, 1); }
/* A block that is not a transfer: only its instructions are counted (no source, no EIP). */
RECOMP_HOT_INLINE void recomp_tick(X86 *c, uint32_t count) {
    c->budget += count;
    if (RECOMP_UNLIKELY(c->budget >= RECOMP_BUDGET))
        recomp_yield(c);
}
/* Direct calls (Sacred's generator): a CALL rel32 into the same module runs the callee's region function as a C call
 * and continues at its return address only when the callee came back there with no non-local transfer pending (the
 * driver's Control::transferring(), found at recomp_transfer_offset from the CPU context). The runtime keeps them off
 * until WinMain (its entry observer must see every call) and whenever something replaces code of the module.
 * RECOMP_DIRECT_DEPTH bounds the host stack: deeper calls go through the driver. */
extern int recomp_direct_enabled;
extern ptrdiff_t recomp_transfer_offset;
#define RECOMP_DIRECT_DEPTH 160u
#define RECOMP_DIRECT_OK(c) (RECOMP_LIKELY(recomp_direct_enabled) && (c)->direct_depth < RECOMP_DIRECT_DEPTH)
#define RECOMP_DIRECT_CALL(c, function)                                                                          \
    do {                                                                                                         \
        ++(c)->direct_depth;                                                                                     \
        function(c);                                                                                             \
        --(c)->direct_depth;                                                                                     \
    } while (0)
/* Leaving a region for code of another region: a guaranteed tail call, so a loop across regions never grows the
 * host stack. musttail needs `return function(c)` although both return void, which ISO C does not allow (a
 * -Wpedantic diagnostic): silenced for that statement only. */
#define RECOMP_DIRECT_JUMP(c, function)                                                                          \
    {                                                                                                            \
        _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wpedantic\"")                        \
        __attribute__((musttail)) return function(c);                                                            \
        _Pragma("GCC diagnostic pop")                                                                            \
    }
/* What runs an indirect call or jump target from the calling region, from this thread's dispatch cache: its compiled
 * function, or for a host function one that has the driver dispatch it there and then; null for anything the driver
 * has to handle itself. */
void (*recomp_direct_target(X86 *c, uint32_t target))(X86 *);
#define RECOMP_DIRECT_BACK(c, ret) \
    ((c)->eip == (ret) && !*(const uint64_t *)((const char *)(c) + recomp_transfer_offset))
/* Helpers orphan their surviving records before returning. The caller adopts
 * the newest one and executes setjmp in its own live host frame. */
uint64_t recomp_seh_frame_mark(X86 *c);
void recomp_seh_frame_orphan(X86 *c, uint64_t mark);
jmp_buf *recomp_seh_frame_adopt(X86 *c);
/* Drop records below ESP at this callback level; retain active landings. */
void recomp_seh_frame_leave(X86 *c);
void recomp_seh_land(X86 *c);
uint32_t recomp_seh_pending_target(void);
void recomp_seh_intercept(X86 *c, uint32_t target);
/* Verbose-only, bounded validation. Logs the first failure and last good point. */
void recomp_seh_validate_chain(X86 *c, const char *phase, const char *detail);

/* Context reuse/teardown on its owning host thread; NULL drops all its state. */
void recomp_seh_reset(X86 *c);
/* A callback return abandons frames/landings created at this call depth. */
void recomp_seh_callback_leave(X86 *c, uint32_t depth);
/* Returns zero for an exhausted chain; kernel32 retains its abort diagnostics.
 * A nonzero return is possible only through the test-only unhandled hook. */
int recomp_seh_raise(X86 *c, uint32_t code, uint32_t flags, uint32_t nargs, uint32_t args);
void recomp_seh_unwind(X86 *c, uint32_t target, uint32_t target_ip, uint32_t record,
                       uint32_t retval);

#ifdef POPM_TESTING
typedef void (*RecompSehUnhandledHook)(X86 *, uint32_t record, uint32_t context);
void recomp_seh_test_unhandled_hook(RecompSehUnhandledHook hook);
uint32_t recomp_seh_test_frame_count(X86 *c);
#endif

#ifdef __cplusplus
}
#endif
#endif
