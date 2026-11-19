#pragma once
/* The game module: everything generated from the player's own files (tools/lift/generate.py), seen by the runtime
 * through one exported function. A static build links it in; a distribution build loads it from a shared library
 * that the launcher compiled on the player's machine, so the shipped runtime contains no code of the game. */
#include <stdint.h>
#include "resolution_table.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Bumped whenever this structure, GeneratedEntry or the helper ABI the generated code calls changes; a module of
 * another version is rebuilt by the launcher. */
#define SACRED_GAME_MODULE_ABI 1u
struct X86;
/* One dispatch-table row; the generated shards declare the same structure. */
struct GeneratedEntry {
    uint32_t address;
    void (*function)(struct X86*);
};
struct SacredGeneratedTable {
    const struct GeneratedEntry* entries;
    const unsigned* count;
    const char* tag;
};
struct SacredGameModule {
    uint32_t abi;
    uint32_t qa;               /* compiled with SACRED_QA: must match the runtime */
    const char* sacred_sha256; /* the Sacred.exe it was generated from */
    unsigned table_count;
    const struct SacredGeneratedTable* tables;
    const struct ResolutionTable* resolution;
};
#if defined(__GNUC__)
#define SACRED_GAME_EXPORT __attribute__((visibility("default")))
#else
#define SACRED_GAME_EXPORT
#endif
SACRED_GAME_EXPORT const struct SacredGameModule* sacred_game_module(void);
#ifdef __cplusplus
}
#endif
