## Jump-table lift gap (fixed 2026-10-16)

Symptoms: `missing compiled guest target 0x530c9f` (source 0x530665, `jmp [ecx*4+0x536ccc]`) and
`0x531dc8` (source 0x52ec4f) because case labels reachable only through a jump table were never instruction starts.

Findings
- The committed recipe (`tools/lift/export_exe.sh`) already reproduced the shipped export
  (blocks_ir_sha256 8006a84c..., 1,289,727 instructions). The earlier "20,226 fewer instructions" came from
  adding `table:` specs: `AdmitObservedEntry` threw on the first target (Ghidra had typed code bytes as a
  data dword, so `disassemble()` silently did nothing), and `analyzeHeadless` still exits 0 and exports after
  a pre-script error, which dropped the later `vtables`/`immediates` passes.
- Fixes: the script clears mis-typed data at an admitted target; `export_exe.sh` fails with
  `EXPORT_SCRIPT_FAILED` on any `SCRIPT ERROR`; `tools/lift/check_export.sh` regenerates from a clean run
  and compares with `tools/lift/export-sacred.sha256`.
- `tools/lift/sweep_jump_tables.py` sweeps every `[reg*4+imm]` code-pointer table (864 aligned tables; unaligned
  runs are coincidental data). The shipped export had 9 tables (23 targets) with non-start targets; admitting
  them exposed 3 nested second-level tables, admitted in the same recipe (12 `table:` specs, fixpoint reached).
  Result: 0 missing targets, 7,817 table targets all compiled.

Hashes: old blocks_ir_sha256 8006a84ccaa2af58e504c795d9a5101a2a53a3e2e02141fc75b7c46c24652b1c (1,289,727
instructions); new 83baa0c80a47f6bca8fc28e6dc800804bd73fa3f424faeff48c2ed041ade3653 (1,293,705).

Static proof: `python3 -I tools/lift/sweep_jump_tables.py --dispatch build/game/generated/sacred --prove 0x531dc8 0x530c9f`
prints `targets_not_compiled=0` and, for both targets, `instruction_start=True compiled_dispatch=True`.
Not covered: tables filled at run time and unaligned or non-`*4` index forms.
