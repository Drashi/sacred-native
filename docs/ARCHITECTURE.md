# Architecture

Sacred Native runs Sacred Gold natively on Linux ARM64 and macOS Apple Silicon. The game's own code is kept: `Sacred.exe`,
`granny.dll` and `tincat2.dll` are translated from x86 to C ahead of time, instruction by instruction, and the result
runs on a native runtime that replaces Windows, DirectDraw/Direct3D 7, Miles Sound System and the COM/DirectShow movie
path. No emulator, interpreter, JIT, Wine or QEMU is part of the shipped program. The player's installation is read
only; everything derived from it (the Ghidra export, generated C, screenshots) is built locally and never committed.

## Pipeline

```text
player's Sacred.exe, granny.dll, tincat2.dll (checked by SHA-256)
        |
Ghidra 12.1.4 headless export          tools/ghidra_scripts/, tools/lift/export_*.sh  ->  analysis/export/
        |
translator                              tools/lift/generate.py (+ third_party/recomp-kit)
        |
generated C shards                      build/game/generated/{sacred,granny,tincat}/*.c
        |
native compiler and linker  +  runtime/, platform/  ->  sacred-native
```

`tools/bootstrap.sh` runs the export and generation steps; `tools/gates.sh` builds and runs the tests.

### Export

`ExportRecompileIR.java` writes the complete instruction listing (not only Ghidra's function bodies), basic blocks,
cross references, jump tables, imports, exports, relocations and a coverage report as gzipped JSON lines. Ghidra's
decompiler output is never a compilation input. The export is reproducible: `tools/lift/check_export.sh` regenerates it
and compares it with the hashes in `tools/lift/export-sacred.sha256`. Entry points that Ghidra's analysis misses
(code reached only through vtables, jump tables or code pointers in data) are admitted from
`tools/lift/code-pointer-entries-*.txt` and `data/entry-targets.json`; `tools/lift/sweep_jump_tables.py` and
`sweep_code_pointers.py` find them.

### Translation

The translator is a maintained fork of [recomp-kit](https://github.com/veritr1x/recomp-kit) (pinned at
`eb32cc4`, local changes in `third_party/recomp-kit/PATCHES.md`). Each x86 instruction becomes a short C sequence over
an explicit guest CPU state. Code is cut into *regions* of about 128 instructions at basic-block leaders, not at
function boundaries; a region is one C function, and its code lives in `region_XXXXXXXX`. A table maps every resumable
guest address to its region and block.

- **Control flow.** A guest `CALL` pushes the real guest return address; `RET` pops whatever is on the guest stack.
  Jumps inside a region are C `goto`s; everything else goes through the block table. A target that was not translated
  stops the program with its address and the source instruction; there is no fallback decoder.
- **Registers and flags** live in C locals inside a region (`tools/lift/promote.py`) and are spilled to the guest CPU
  state at region exits and before calls that can observe them. Flags are computed eagerly; a flag store is dropped only
  when every path in the region overwrites it before any read (`tests/lift/test_flag_liveness.py`).
- **Native stand-ins.** `NATIVE_CALLS` in `generate.py` routes a few hot functions to hand-written C
  (`runtime/sacred/native.c`, `runtime/granny/native.c`). A stand-in is bound to the exact code bytes it was written
  against and produces the same guest memory writes and integer registers. `SACRED_GRANNY_NATIVE=verify` (granny.dll)
  and `SACRED_EXE_NATIVE=verify` (Sacred.exe) run both versions and compare them; `SACRED_GRANNY_NATIVE=0` turns the
  granny.dll stand-ins off.
- **Resolution hooks.** `tools/lift/resolution.py` and `resolution_sites.json` patch the places where the game
  hard-codes 1024x768, for the optional HD mode.

Generated shards are compiled with `-O1` (the runtime with `-O2`), without fast-math or FP contraction, by gcc or clang
with `-Werror`.

## CPU semantics

- **x87** keeps real 80-bit registers, the register stack, tag, control and status words, as in hardware. Basic
  arithmetic, conversions and comparisons go through Berkeley SoftFloat; FSIN, FCOS, FSINCOS, FPTAN, FPATAN, F2XM1,
  FYL2X and FYL2XP1 use the Bochs FPU code (`third_party/bochs/`, built as a separate LGPL shared library). Results are
  then rounded to the guest's precision and rounding control.
- **Fast path.** In the state the game actually runs in (precision 53 or 24 bits, round to nearest) additions,
  subtractions, multiplications and divisions of ordinary operands use host doubles, with exact status bits derived
  from the rounding error (`runtime/cpu/x87.h`, tested against the SoftFloat path in `tests/cpu/x87_fast_tests.c`).
  Anything else takes the SoftFloat path. `tools/lift/x87_fast.py` lowers whole runs of x87 instructions; a run takes
  a direct call of the C runtime's `_ftol` in as one operation and goes on behind it (`SACRED_FTOL_INLINE=verify`
  compares that with the routine while the game runs, `=0` keeps the call).
- **MMX** aliases the x87 registers; **SSE/3DNow** forms that the game executes are implemented lane by lane
  (`runtime/cpu/simd.cpp`) with MXCSR rounding and status.
- **CPUID** answers from a fixed profile (`data/entry-cpuid.json`), never from the host.

The CPU semantics were checked instruction by instruction against reference implementations during development;
`tests/cpu/` keeps the state invariants and the x87 fast path tests.

## Guest process

- **Memory.** A 4 GiB host reservation holds the whole guest address space; a guest address `a` is `base + a`. Only
  committed guest memory is mapped, so generated code uses plain loads and stores and the host MMU catches invalid
  accesses; a fault handler grows thread stacks and reports everything else as a guest fault. Guest page permissions are
  tracked separately in 4 KiB pages, independent of the host page size (16 KiB on Apple Silicon). The runtime's own
  accesses to guest memory are always checked.
- **Loading** (`runtime/process/loader.cpp`). The PE images are mapped at a layout fixed before generation (granny and
  tincat2 both prefer `0x10000000`, so tincat2 is placed at `0x11000000` and its relocations applied). The program starts at the EXE
  entry point and runs the original CRT start-up and DLL initialisation. Replaced DLLs (kernel32, user32, ddraw, mss32,
  ...) are synthetic PE images in guest memory whose exports point at host-function tokens, so `GetProcAddress` and the
  game's own export walker see the same thing.
- **Calls into the host.** Each import has a declared calling convention, argument layout and stack cleanup
  (`runtime/process/control.cpp`, `boundary.c`). Guest pointers are never cast to host function pointers, and variadic
  functions such as `wsprintf` parse the guest stack themselves. An import without an implementation stops the program
  with its name and caller.
- **Exceptions.** The guest's SEH chain, MSVC C++ exception metadata and unwinding run as translated guest code. A
  guest exception is dispatched through guest structures; host `longjmp` happens only in the C driver that owns the
  checkpoint, after native resources are released.
- **Threads** (`runtime/process/scheduler.h`). Every guest thread has its own CPU state, TEB, TLS and stack on its own
  host thread (16 MiB stack, `platform/host_thread.h`), but only one thread at a time runs guest code. Blocking calls
  release that baton, and a per-block budget makes busy loops yield.

## Windows and middleware

| Area | Implementation |
|---|---|
| Files, registry | `runtime/win32/`: a case-insensitive Windows path view over the installation, read only, plus a writable profile (`SDL_GetPrefPath`) that receives every write, including `Settings.cfg` and saves. The registry is an in-memory store with case-insensitive keys; nothing is written back. |
| Windows, input | `runtime/win32/menu.cpp`, `input.cpp`: SDL2 events become guest window messages for the game's window procedure. Mouse rules are in `docs/notes/mouse-input.md`, keyboard in `docs/internals/esc-key.md`. |
| GDI, fonts | `runtime/win32/gdi.cpp`, `platform/font.cpp`: CPU surfaces and FreeType text; `platform/font_match.cpp` picks the font file (game fonts, then system fonts, metric-compatible substitutes for Windows families; `SACRED_FONT_DIRS` overrides the system directories). |
| DirectDraw / Direct3D 7 | `runtime/ddraw/`: guest COM objects with host-side state; surfaces have a guest-format CPU copy and a GL texture. D3D render states become a small set of GLSL shader variants on an OpenGL 3.3 core context (`platform/renderer_gl.cpp`, at most 4.1 features so macOS works). Lock rules: `docs/internals/ddlock.md`. |
| Miles (mss32) | `runtime/miles/`: replaced at the `AIL_*` API with a native mixer on SDL2 audio; MP3 through minimp3, WAV/ADPCM through dr_wav. NULL-handle behaviour follows the original: `docs/internals/miles-null-handles.md`. |
| Movies | `runtime/ole/`: the game's DirectShow multimedia-stream calls are served by an FFmpeg decoder (pinned LGPL build, `tools/build_ffmpeg.sh`): `docs/notes/movie-playback.md`, `docs/internals/directshow-surface.md`. |
| libxml2 | `runtime/xml/`: our own XML reader and writer (`platform/xml_native.cpp`, same trees and bytes as libxml2) parse and serialise; the tree the game walks is built in guest memory with the original structure layout. |
| Networking | `runtime/win32/winsock.cpp`, `platform/net_socket.cpp`: POSIX sockets behind the Winsock API. |
| granny.dll, tincat2.dll | translated like the EXE. |

## Portability rules

These keep Linux and macOS on one code base; `tools/check_portability.sh` and `tools/scan_host_apis.sh` check them.

1. OpenGL 3.3 core, nothing beyond 4.1 core (macOS's ceiling), no GLES-only paths. Context creation tries core 3.3 and
   4.1; on Linux without a working context the process re-executes once with Mesa llvmpipe. One `SACRED_GL path=...`
   log line says which path was taken.
2. The host page size is queried at run time; guest permissions stay in 4 KiB pages.
3. Generated C and the runtime compile with both gcc and clang with warnings as errors, without GNU extensions.
4. OS-specific APIs (`/proc`, `memfd_create`, `mremap`, `epoll`, ...) live only in `platform/`. Checkpoints use
   `_setjmp`/`_longjmp`, never a signal-mask-saving `setjmp`.
5. Apple ARM64 ABI: x18 is reserved and variadic arguments are passed on the stack, so host functions are never
   called through mismatched function-pointer types and guest varargs are never passed as a host `va_list`.
6. Profile and save directories come from `SDL_GetPrefPath`; Windows paths resolve case-insensitively on both systems.
7. Audio, video and input go through SDL2 and portable libraries; no direct X11, Wayland, ALSA or PulseAudio calls.

## Optional HD mode

All off by default; see `tools/dist/BUNDLE_README.md` for the user-facing options.

- `HD_WIDTH` / `HD_HEIGHT` (or a fullscreen start) replace the game's 1024x768 layout through the resolution hooks;
  `runtime/modules/resolution.cpp` holds the table of patched values.
- `RENDER_SCALE` renders 3D at an integer multiple of the window size.
- `TEXTURE_PACK` replaces textures by content hash (`runtime/ddraw/texture_pack.h`); `tools/textures/` builds packs.
- Screens designed for 1024x768 (menus, loading, world map, save window) are drawn into the frame's top-left
  1024x768 when a resolution hook raises the inset word; `DirectDraw::inset_view` shows that rectangle scaled to the
  window height (`platform/letterbox.h` `fit_inset`, the resampling pass in `platform/renderer_gl.cpp`) and the
  pointer is mapped through the same box. Only the present that ends a raising frame shows the inset, and a repeat
  present of that frame with nothing drawn in between (`runtime/ddraw/inset_schedule.h`): the first world frame after
  such a screen is shown whole. `SACRED_INSET_TRACE=1` logs every present's decision (`INSET_TRACE`), and the QA
  script command `record N [LABEL]` captures the next N presents to `<evidence>/rec/`. `MENU_BACKGROUNDS` (`runtime/ddraw/menu_backdrops.h`) adds the user's wide
  pictures: a screen's background tiles, recognised by the content of their `texture.pak` entries, are drawn from the
  picture's centre and the presentation draws the picture's sides around the inset; a loading bitmap with a picture
  loads the picture's centre (`LoadImageA`). The save game window stands centred in the frame (inset flag 0x10000,
  shown from the frame's centre, its version label moved with it) and takes its picture the same way (`saveload`).
- The main menu and the character select use a menu canvas in a frame wider than 4:3: `D` of
  `tools/lift/resolution.py` (= `menu_canvas_extension`) moves their windows, the projections of the water and the
  heroes, the version label and the character select's absolutely placed parts by `D` (its bottom panel, the hovered
  hero's description, the multiplayer header line and the eight hero slots with their hit tests and name labels;
  message boxes over these screens are centred on the canvas: the shown-inset word after the inset word is 3 for a
  canvas frame), and widens the menu water grid from 64x64 to 128x128 cells of the same size
  (its random drops remapped onto the same cells, its waves allowed out to the canvas sides), so the canvas is
  1024 + 2D wide with the original in its middle. The host extends the sky quad and the picture's outer tile columns
  to the canvas edges (drawn one picture pixel per canvas pixel) and draws the water from textures twice as wide
  (`wide_water_texture`): the original in the middle, then the picture's own painted water projected back onto the
  floor when it matches the live water (the Sacred main menu), otherwise patches of the floor's object-free part.
  The network windows (local network and internet lobby, account, password) are 1024x768 screens drawn at the
  frame's left inside main menu frames: their window draw raises screen flag 0x2000, which shows them as a still
  screen (shown-inset word 2, so message boxes keep their place) with the `network` picture around it: made at start
  from their stone tiles, mirrored out to the sides. Creating a network game starts `GameServer.exe`, which the port
  does not run: `CreateProcessA` fails as for a missing file and the game reports "Initialization failed".

## Testing

- `tests/cpu/`: x87 state invariants and the fast path against SoftFloat.
- `tests/control/`, `tests/process/`: control transfer, SEH, the loader and the resolution table.
- `tests/lift/`: the translator (flag liveness, recipe, PE reader, x86 decoder, resolution sites).
- `tests/menu/`, `tests/movie/`: window host, renderer, surfaces, audio and movie decoding; scripted runs of the real
  game (`tests/menu/qa/*.script`, `--qa-script`), frame rate checks in `tests/fps/`.
- `tools/gates.sh` runs the build, `ctest`, the portability check and the export check.

## Distribution

`tools/dist/make_bundle.sh` builds a relocatable Release binary and copies the non-system libraries (FFmpeg, SDL2,
FreeType and the SoftFloat library) into `lib/`. On Linux, glibc, the GL/X11/Wayland stack and audio
client libraries stay external so they match the user's drivers; on macOS everything outside `/usr/lib` and
`/System/Library` is bundled and ad-hoc signed. The `sacred-launcher` window (and the `sacred-gold` script) takes the installation path, keeps the
profile in the `SDL_GetPrefPath` directory and logs there.
On macOS the bundle is `Sacred Native.app` (in `sacred-native-macos-arm64.zip`): the executables in `Contents/MacOS`,
the libraries in `Contents/Frameworks`, the read-only pieces in `Contents/Resources`, where the launcher and the
script look for them when they run from `Contents/MacOS`; everything they write goes to the profile. The app carries
the project's logo as its icon, and the launcher and the game load the same logo from the bundle for their windows
and the Dock (`platform/app_icon.h`). The Linux bundle carries it beside a desktop entry that
`install-desktop-entry.sh` installs for the current user; the windows' X11 class and Wayland app id
(`sacred-native`) match the entry's `StartupWMClass`, so they group under it.
