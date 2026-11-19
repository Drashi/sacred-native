/* Native stand-ins for the hottest granny.dll functions (runtime/granny/native.c holds the contract). */
#ifndef SACRED_GRANNY_NATIVE_H
#define SACRED_GRANNY_NATIVE_H
#include "x86.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*GrannyRegion)(X86*);
struct GrannyNative {
    uint32_t address;       /* the guest function's entry */
    const uint32_t* code;   /* (address, length) of the code the stand-in does the work of: the function, the
                               functions it calls in its place and their jump tables; ends with 0 */
    uint32_t hash;          /* FNV-1a of those bytes in that order */
    GrannyRegion native;    /* registered in place of the generated code of the entry */
    GrannyRegion* original; /* receives the generated code it replaces */
};
extern const struct GrannyNative granny_natives[];
extern const unsigned granny_native_count;
enum { GRANNY_NATIVE_OFF, GRANNY_NATIVE_ON, GRANNY_NATIVE_VERIFY };
extern int granny_native_mode;
/* Provided by the runtime: runs the generated code of the guest function at c->eip (just entered by a CALL) until
 * it returns. Zero when a non-local transfer left it instead. */
int sacred_granny_run_original(X86* c);
void granny_native_report(void);
/* GPU skinning (runtime/ddraw/gpu_skin.cpp): called after every successful deform of positions with the mesh, its
 * output buffers (normals 0 when not deformed), its vertex count, whether normals are normalised, and the bone rows
 * (per bone three rows of four floats: matrix row and translation). */
typedef void (*GrannySkinHook)(uint32_t mesh, uint32_t positions, uint32_t normals, uint32_t vertices, int normalize,
                               const float* rows, uint32_t bones, int cpu_skipped);
/* Before a rendering deform (called from GrannyLockNextRenderingState's 10034200): nonzero when the GPU will skin
 * this mesh, so the stand-in computes only the bone matrices and leaves the vertices to the hook (cpu_skipped). */
extern int (*granny_skin_take)(uint32_t mesh, uint32_t vertices);
/* Any other deform of `positions`: what the GPU knew of that buffer is stale. */
extern void (*granny_skin_forget)(uint32_t positions);
/* Deforms of positions counted by kind: [0] not for rendering, [1] other rendering (shadows), [2] mesh draws. */
extern unsigned long granny_skin_calls[3];
extern GrannySkinHook granny_skin_hook; /* defined beside its user (runtime/ddraw/gpu_skin.cpp) */
/* 14 floats per vertex (source position, source normal, four bone indices, four weights); 0 when the mesh does not
 * fit that form. Reads guest memory: call on the guest thread. */
int granny_skin_template(uint32_t mesh, uint32_t vertices, float* out);
/* Set to granny_skin_template by install_granny_natives (the DirectDraw side does not link the stand-ins). */
extern int (*granny_skin_template_fn)(uint32_t mesh, uint32_t vertices, float* out);
#ifdef __cplusplus
}
#endif
#endif
