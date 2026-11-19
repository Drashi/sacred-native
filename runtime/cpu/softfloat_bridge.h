#ifndef SACRED_SOFTFLOAT_BRIDGE_H
#define SACRED_SOFTFLOAT_BRIDGE_H
#include "config.h"
#include "softfloat3e/include/softfloat.h"
#include "x86.h"
static inline extFloat80_t soft_value(X87Value v) {
    return {v.signif, v.signExp};
}

static inline X87Value guest_value(extFloat80_t v) {
    return {v.signif, v.signExp};
}

static inline softfloat_status_t x87_begin(const X86* c) {
    softfloat_status_t status{};
    status.softfloat_roundingMode = (c->fpu_cw >> 10) & 3;
    status.softfloat_exceptionMasks = c->fpu_cw & 63;
    static const uint8_t precision[] = {32, 80, 64, 80};
    status.extF80_roundingPrecision = precision[(c->fpu_cw >> 8) & 3];
    return status;
}

static inline void x87_finish(X86* c, const softfloat_status_t& status) {
    c->fpu_sw = (uint16_t)((c->fpu_sw & ~0x200u) | softfloat_getExceptionFlags(&status));
    x87_pending(c);
}
#endif
