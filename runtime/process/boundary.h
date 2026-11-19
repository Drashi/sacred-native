#pragma once
#include <stdint.h>
#include "x86.h"
#ifdef __cplusplus
extern "C" {
#endif
int sacred_drive(X86* cpu, uint64_t owner, uint32_t sentinel);
int sacred_boundary_step(X86* cpu, uint64_t owner, uint32_t sentinel);
uint64_t sacred_driver_owner(X86* cpu);
int sacred_transfer_pending(void);
void sacred_defer_transfer(X86* cpu, uint64_t owner, uint32_t target);
void sacred_guest_read(uint32_t address, void* out, size_t bytes);
void sacred_guest_write(uint32_t address, const void* in, size_t bytes);
void recomp_seh_register(X86* cpu);
void recomp_seh_driver_leave(X86* cpu, uint64_t owner);
void recomp_guest_setjmp(X86* cpu);
void recomp_guest_longjmp(X86* cpu);
#ifdef __cplusplus
}
#endif
