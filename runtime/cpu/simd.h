#ifndef SACRED_SIMD_H
#define SACRED_SIMD_H
enum SimdOperation {
    SIMD_ADD,
    SIMD_SUB,
    SIMD_MUL,
    SIMD_MIN,
    SIMD_MAX,
    SIMD_CMP_EQ,
    SIMD_CMP_GT,
    SIMD_CMP_GE,
    SIMD_CMP_NE,
    SIMD_RCP,
    SIMD_RSQRT,
    SIMD_TO_INT,
    SIMD_FROM_INT,
    SIMD_COMIS
};
uint32_t simd_float(X86* c, uint32_t a, uint32_t b, enum SimdOperation op, int now);
RECOMP_COLD void recomp_alignment_error(X86* c, uint32_t address);
static inline void simd_align(X86* c, uint32_t address) {
    if (address & 15u) {
        recomp_alignment_error(c, address);
    }
}
#endif
