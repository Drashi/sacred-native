/* The C execution boundary of one guest driver. C++ dispatch/adapters have returned before control comes back
 * here; neither generated functions nor returned helpers own a native checkpoint. A deferred transfer is already
 * applied to the guest CPU/ESP when step() returns, so the next step simply continues from it (the former local
 * sigsetjmp/siglongjmp pair round-tripped to this same frame on every region exit and was never published). */
#include "boundary.h"

int sacred_drive(X86* cpu, uint64_t owner, uint32_t sentinel) {
    int action;
    while ((action = sacred_boundary_step(cpu, owner, sentinel)) == 2) {
    }

    return action;
}
