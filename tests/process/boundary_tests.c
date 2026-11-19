#include "runtime/process/boundary.h"
#include <signal.h>
#include <stdio.h>

static unsigned steps;
static int failed;

int sacred_boundary_step(X86* cpu, uint64_t owner, uint32_t sentinel) {
    (void)cpu;
    if (owner != 17 || sentinel != 29) {
        failed = 1;
    }

    sigset_t mask;
    if (sigprocmask(SIG_BLOCK, NULL, &mask)) {
        return 1;
    }

    if (++steps == 1) {
        if (sigismember(&mask, SIGUSR1)) {
            failed = 1;
        }

        sigaddset(&mask, SIGUSR1);
        if (sigprocmask(SIG_SETMASK, &mask, NULL)) {
            failed = 1;
        }

        return 2;
    }

    if (!sigismember(&mask, SIGUSR1)) {
        failed = 1;
    }

    return 0;
}

int main(void) {
    sigset_t original, mask;
    if (sigprocmask(SIG_BLOCK, NULL, &original)) {
        return 1;
    }

    mask = original;
    sigdelset(&mask, SIGUSR1);
    if (sigprocmask(SIG_SETMASK, &mask, NULL)) {
        return 1;
    }

    int result = sacred_drive(NULL, 17, 29);
    if (sigprocmask(SIG_SETMASK, &original, NULL)) {
        return 1;
    }

    if (result || failed || steps != 2) {
        return 1;
    }

    puts("DRIVER_CHECKPOINT_PASS signal_mask_not_restored=1");
    return 0;
}
