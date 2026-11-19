# Source admission

Pinned source: veritr1x/recomp-kit at
`eb32cc42623e0e9e868b9d27423a57d5a5cba56b`.
`UPSTREAM.json` enumerates the imported base files with their Git blobs and
SHA-256; the local changes to them are described in `PATCHES.md`.
LICENSE and NOTICE are preserved verbatim. NOTICE also names upstream
components (Lua, TinySoundFont, stb_truetype, Open Sans, GeneralUser GS, miniz,
SDL3, FFmpeg 7.1.1, volk, the Vulkan headers) that are not part of this subset:
none of them is in this repository or in a release bundle. No upstream
third-party dependency is copied.

The admitted subset is the instruction-to-C translator, its configuration
parser, the original exporter (the reference baseline for
`tools/ghidra_scripts/ExportRecompileIR.java`), the CPU state/helper header,
the SEH checkpoint implementation and declarations, the intrinsic declarations,
and the translator, configuration, CPU and SEH tests. The monolithic
`runtime_tests.cpp` is kept intact because it also covers CPU behaviour; its
other HLE tests do not admit the HLE implementation they exercise.

Excluded: the entire upstream host, dx, platform, mods, launcher, mobile/web
packagers, renderers, root build system, product/game scaffolding, unrelated
tools and dependencies. Also excluded are runtime/interp.*, runtime/thunks.*
(runtime decoding), runtime/cpu.cpp (interpreter and zero-result fallback),
the imports and the Win32/HLE success stubs. The port's own `runtime/` and
`platform/` take their place; nothing here decodes or interprets x86 at run
time.

The x87 state is software ext80 instead of upstream's double storage, and the
translator's flag-store elision is disabled; these and the other corrections
are in `PATCHES.md`. `tools/recomp/pe.py`, `x86dec.py`, `x87_emit.py` and
`simd_emit.py` are not among the imported files (`UPSTREAM.json`): they
belong to the local patch set. Upstream test fixtures that were not imported
are listed in `PATCHES.md`.
