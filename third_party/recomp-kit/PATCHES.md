# Local patches

## Complete-module adapter (2026-10-15)

`tools/lift/generate.py` feeds every byte-validated exported listing candidate
from Sacred, Granny and TinCat through the admitted instruction templates.
Its `RegionTranslator` specializes control transfers only: CALL writes the real
guest continuation and yields, outward JMP yields, and the existing live C driver
resumes the compiled block map. No inferred C function owns the guest stack.
Regions split only at exported basic-block leaders; shard packing accounts for
both generated instructions and their address tables. TinCat's operand bytes are
rebased using PE HIGHLOW records before offline lowering.

Unknown or unqualified candidates have precise fatal traps and an explicit
generation ledger. There is no runtime decoder, intrinsic replacement of original
CRT helpers, interpreter, or permissive missing import. This adapter does not
modify the previously qualified instruction templates or numerical helpers.

## Driver-owned control-transfer protocol (2026-10-15)

The port enables translator
`driver_checkpoints` with `resumable_stacks`. In this mode the emitter publishes
guest SEH records with `recomp_seh_register`, never native `setjmp`/`jmp_buf`.
This applies to ordinary functions, helper adoption, intrinsic setjmp, alternate
entries, and mismatched-EIP early returns. Legacy non-Sacred emission is retained
so its admitted tests continue to qualify the original API independently.

`runtime/seh.cpp` is compiled, not replaced with a different exception walker.
The Sacred conditional adapter retains its registration validation, bounded chain
walk, EXCEPTION_RECORD/CONTEXT layouts, handler search, and inner-to-outer unwind.
Frame records carry a live C-driver identity instead of a native jump buffer.
Accepting a guest landing defers a transfer; guest/native calls return normally
and release their resources. Only `runtime/process/boundary.c` can longjmp, to
its own still-live checkpoint. Intermediate callback drivers retire their records
and propagate the authoritative guest CPU without restoring interrupted state.
The destination resumes through the block-entry table.

The port also admits continuable disposition zero using the guest CONTEXT,
keeps noncontinuable/invalid dispositions fatal, and implements guest-width
setjmp buffers with driver associations in a side table. Generated memory
accesses in this mode use the checked guest arena. No original guest address is
cast to a host callback pointer. Unimplemented Win32 bodies remain strict misses.

The permanent minimized mismatch-return fixture is
`tests/control/test_emission.py`.

## Binding numerical qualification policy (2026-10-15)

QEMU and Bochs disagree on raw ext80 transcendental results. The project
resolves it as follows: FSIN, FCOS, FSINCOS, FPTAN,
FPATAN, F2XM1, FYL2X and FYL2XP1 use the pinned Bochs software FPU as their
exact reference. QEMU remains the reference for everything else. No epsilon
or status-bit masking conceals the transcendental difference. The project
additionally requires final transcendental results rounded to guest PC/RC,
with ext80 storage and exponent range retained. This deliberate project policy
extends precision control beyond the architectural basic-arithmetic/FSQRT set.

Implementation patches and reused-test fixture exclusions are recorded below
as they are qualified.

## Imported tests and fixture exclusions

The imported test files are kept as upstream wrote them, apart from the
adaptations named below; this repository's `ctest` does not register them
(`tests/control/` drives the instruction harness for its own fixtures).
`tests/cpu/requirements.txt` pins the Python packages of `build/venv`.
No executable synthetic test is deleted or marked skipped. The following
upstream fixtures were never imported, so the tests that need them cannot run
here:

- `tools/recomp/tests/{test_translate.py,test_eaxa.py,test_translate_hooks.py}`:
  Populous/original-game binary, original Ghidra listings and generated
  whole-game dispatch/symbol tables are absent; those are not Sacred CPU fixtures.
- `tests/test_game_config.py`: its import-time `tools/gen_game_config.py` and
  `games/stub` fixtures are absent from the pinned admitted subset.
- Six `ConfigureTests` cases of `tools/recomp/tests/test_translate_config.py`:
  they require the omitted `games/stub/game.toml`; its two self-contained cases
  need nothing else.
- `runtime/tests/{runtime_tests.cpp,seh_tests.cpp}`: they depend on upstream's
  HLE, platform and loader, which are intentionally not admitted.

The synthetic instruction harness is adapted to the installed GCC instead of
silently skipping for missing Clang, and to the ext80 CPU ABI/library. Its
behavioral assertions are retained; state-format assumptions are updated.

## Qualified local corrections

1. **Eager architectural flags** (`tools/recomp/translate.py`):
   disable flag-store elision both in liveness and at instruction emission.
   Every defined result is available across arbitrary entry/dispatch boundaries;
   INC/DEC carry preservation and zero-count behavior remain explicit.
2. **Software ext80 state and basic operations** (`runtime/x86.h`,
   project `runtime/cpu/{types.h,x87.h,softfloat_bridge.h,x87_arithmetic.cpp}`):
   replace the double stack and exact-integer side channel with explicit
   significand/sign-exponent registers. Route arithmetic, comparisons, integer
   conversion, operand-format loads/stores and BCD through pinned SoftFloat
   operations with per-guest PC/RC, exception masks and sticky status.
3. **Bochs transcendental adaptation** (project
   `runtime/cpu/x87_transcendental.cpp`, `third_party/bochs/`):
   reuse only numerical helpers, preserving all licenses and source identities.
   Implement the eight binding operations plus reached remainder/scale behavior.
   Keep full extended exponent range; apply the project's final transcendental
   PC/RC policy independently from the internal extended calculation.
4. **Physical stack and environment semantics** (`runtime/x86.h`, project
   `runtime/cpu/{x87.h,x87_state.cpp}`, `tools/recomp/x87_emit.py`):
   preserve TOP, physical tags, masked stack faults, pending exception state,
   FIP/FDP/FOP and saved guest selectors. CPU `cs`/`ds` are guest metadata,
   not host selectors. Implement save/reset/restore and FFREE rather than a
   no-op. MMX enters all-valid state, aliases the physical significand, writes
   exponent `ffff`, and EMMS/FEMMS empty tags without destroying register bits.
5. **Reached SSE/MMX/3DNow semantics** (`tools/recomp/simd_emit.py`, project
   `runtime/cpu/{simd.h,simd.cpp}`): portable lane operations, packed/scalar
   float math and comparisons, MXCSR rounding/FTZ/DAZ/status, raw moves and
   scalar upper lanes, correct 128-bit unpacking and integer shifts, saturation,
   aligned faults, 3DNow accumulation/conversion/refinement. Reciprocal and
   unsupported-input behavior has an explicit QEMU-specific policy, not host
   division or a silent success stub.
6. **Emitter routing and strictness** (`tools/recomp/translate.py`,
   `tools/recomp/{x87_emit.py,simd_emit.py}`): route the exported numerical
   forms to these helpers, including FISTTP and actual x87 operand direction.
   Preserve unknown-instruction generation errors and fatal vector quarantine;
   the negative UD2 fixture must be rejected. New XMM integer admission is
   restricted to qualified forms; unqualified packing and variable packed
   shifts remain fatal rather than using an inexact MMX half-lane lowering.
7. **Preserved tests and exact differential qualification**
   (`tools/recomp/tests/{harness.c,test_translate_insns.py,test_translate_vector.py}`, project
   `tests/cpu/`): adapt CPU layout/state capture without weakening executable
   assertions. The legacy test adapter preserves signaling register NaNs and
   executes a real invalid operation instead of manually setting IE.
   FNSAVE comparisons exclude only architecturally reserved word halves.
8. **No dead SSE lane temporaries** (`tools/recomp/{translate.py,simd_emit.py}`): the
   lane-wise logical/unpack/shuffle forms, PSHUFD and the SSE/SSE2 unpack forms
   declare only the
   `dN_`/`sN_` temporaries their body reads, and COMISS discards its unused result explicitly, so generated C compiles clean
   under `-Wall -Wextra -Werror` on clang (`-Wunused-variable`). A memory
   operand is still read in full and its unused lanes are discarded with an
   explicit `(void)`, so a faulting access remains observable. Emitted
   semantics are otherwise unchanged; the portability gate no longer exempts
   generated shards.

No result tolerance, undefined-flag comparison or discarded runnable upstream
test conceals a semantic mismatch.

`UPSTREAM.json` identifies the unmodified imported base at
`eb32cc42623e0e9e868b9d27423a57d5a5cba56b` by blob and SHA-256; the difference
between those files and the ones here is the local patch set. No interpreter
or Win32 HLE implementation is added by these patches.

## XLATB mnemonic alias (2026-10-16)

Capstone names the opcode `d7` `XLATB`; `tools/recomp/translate.py` only recognised `XLAT`, so
three Sacred sites (0x8509f4, 0x850a6d, 0x850a7a) were quarantined as unmodelled and trapped when the
world-start path reached them. `XLATB` now selects the same admitted `xlat(c)` helper and register
write set as `XLAT`; no new semantics were added.

## POP of a segment register (2026-10-16)

Granny's CPU-feature probe (`push es; push ds ... pop es; pop ds`, rva 0x5768c) and one Sacred site
(0x43e425) pop a segment register. The flat process model has no segment state to load (a
`MOV` to a segment register is already a no-op there), so `POP ES/DS/SS/FS/GS` now consumes the
32-bit stack slot and writes nothing; `POP CS` remains an invalid-opcode trap.

## Bulk REP MOVS/STOS and direct calls (2026-11-12)

`runtime/x86.h`: in driver-checkpoint builds, REP MOVS/STOS first copy or fill every run that stays inside one
readable source page and one writable destination page (forward, no watch armed, no forward overlap) with one
`memmove`/`memset`, advancing ESI/EDI/ECX as the element loop would; the element loop takes the rest and raises the
faults. `X86` gains `direct_depth`. `runtime/seh.h`: `RECOMP_DIRECT_*`, used by Sacred's generator to run a
CALL rel32 into the same module as a C call of the callee's region (see `tools/lift/generate.py`); the runtime
(`recomp_direct_enabled`, `recomp_transfer_offset`) decides when that is allowed and whether execution continues at
the return address.

## Own PE reader instead of pefile (2026-11-17)

`tools/recomp/pe.py` reads the memory image, HIGHLOW base relocations, the
import names and the section table of a PE32 image; `Image` uses it instead of
the pefile package, so the translator that runs on a player's machine needs no
package from PyPI for it. It reproduces pefile's memory image for the three
Sacred files (`tests/lift/test_pe.py`), and the generated C is unchanged
(`tools/lift/generated_digest.py`). Imports by ordinal stay unnamed (pefile names
a few from its own tables; the translator looks only for names).

## Own x86 decoder instead of capstone (2026-11-17)

`tools/recomp/x86dec.py` decodes the 32-bit instructions the translated
programs contain (general purpose, x87, MMX, 3DNow!, SSE, SSE2) and reports
them as capstone 5 does: mnemonics with their prefixes, operand sizes, the sign
of sign-extended immediates (unsigned for and/or/xor), `wait` as an instruction
of its own. `Image.md` and `Translator._op_text` use it; generate.py decodes
with it. It agrees with capstone on all 1,475,589 instructions of Sacred,
Granny and TinCat (`tests/lift/test_x86dec.py`) and the generated C is unchanged.
