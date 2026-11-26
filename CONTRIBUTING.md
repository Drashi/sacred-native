# Contributing

## What never goes into the repository

No game code or data: no `Sacred.exe`, DLLs, paks, screenshots or captures of the game, no Ghidra export, no
generated C. `.gitignore` keeps `build/`, `analysis/` and `ghidra-proj/` out. Code comments may name addresses of the
game's functions (`0x62ae90`), never contain decompiled code.

## Build

Requirements: a C/C++ toolchain (gcc or clang), CMake 3.20+, python3 with venv, `curl`, and an installed Sacred Gold
2.28. Linux also needs pkg-config and the SDL2, OpenGL and FreeType development packages (or
`tools/build_linux_deps.sh`); on macOS (Apple Silicon only) `tools/build_macos_deps.sh` builds pkg-config, SDL2 and
FreeType into `build/macos-deps/prefix`, and nothing is taken from Homebrew.

```sh
export SACRED_ASSETS="/path/to/Sacred Gold"        # read only
tools/build_macos_deps.sh                          # macOS, once
tools/bootstrap.sh                                 # build/venv, pinned LGPL FFmpeg, generated C in build/game/generated
tools/gates.sh                                     # builds build/game and runs every check
```

`tools/bootstrap.sh` takes steps (`--venv --ffmpeg --export --generate`) and skips those whose product exists.
CMake refuses a system FFmpeg unless `-DSACRED_ALLOW_SYSTEM_FFMPEG=ON` (it may be GPL-configured; never distribute such
a build). `SACRED_QA` (default ON) compiles the QA hooks (`--qa-script`, `SACRED_QA_*`); release bundles are built
with `-DSACRED_QA=OFF`.

`tools/dist/make_bundle.sh` (or `cmake --build build/game --target bundle`) builds the release bundle for the host
system: the runtime without game code (`-DSACRED_GAME_MODULE=ON`), the launcher, the translator with the recipe, a
Python with its standard library only, Zig as a fallback compiler and the bundled libraries with their licenses.
Users read `tools/dist/BUNDLE_README.md` (Linux) or `tools/dist/macos/BUNDLE_README.md`. On macOS the result is
`dist/Sacred Native.app`, packed into `dist/sacred-native-macos-arm64.zip`. The logo is
`data/icons/sacred-native-1024.png`; the bundles make the app icon from it and carry it for the windows and the Linux
desktop entry (`data/icons/README.md`).
The bundle ships the committed tree: the scripts refuse a working tree that differs from `HEAD`
(`BUNDLE_ALLOW_DIRTY=1` for a local trial), take the translator sources and menu pictures from the commit, and
check the finished archive for uncommitted files and for paths of the build machine.

## Tests

`ctest --test-dir build/game --output-on-failure` runs the unit tests and, with `SACRED_ASSETS` set, the tests that
read the installation. On macOS the GL tests open hidden Cocoa windows, so run them in a logged-in GUI session.
`lift_generated_identical` (label `slow`, about a minute) translates the whole game and compares it with
`data/generated-digest.json`; update that file with `tools/lift/generated_digest.py` when a translator change is meant
to change the generated C.

QA builds can drive the real game from a script: `sacred-native --assets DIR --qa-script tests/menu/qa/NAME.script`
(commands in `runtime/modules/qa_script.cpp`). `tests/menu/hd_compare.py` and `tests/menu/audio_metrics.py` check the
captures of such runs; frame rate checks are in `tests/fps/`.

`docs/notes/` describes parts of the port, `docs/internals/` what the original game does where the port had to match
it.

## Code style

Four spaces, no tabs, lines up to 120 columns (`.editorconfig`). In every language: one statement per line, the
body of an `if`, `for` or `while` on its own lines (in C, C++ and Java always in braces), and a blank line after a
block that is followed by more code at the same level. C, C++ and Java follow `.clang-format`, Python follows PEP 8
otherwise.

```sh
python3 -m venv build/fmt-venv && build/fmt-venv/bin/pip install clang-format==19.1.7   # once
build/fmt-venv/bin/clang-format -i FILE...      # C, C++, Java
tools/format_python.py FILE...                  # Python
tools/check_format.sh                           # everything, as CI checks it
```

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request: the layout check, the build and `ctest` on Linux
ARM64 and macOS Apple Silicon (without the game, so the tests that read an installation are not registered), and
both release bundles with their checks. Run the tests with your installation locally before asking for a review.

## Releases

Versions follow Semantic Versioning (`0.x` while things still change). A release is an annotated tag on `main` whose
message is the release's summary, in Markdown:

```sh
git tag -a v0.2.0 -F notes.md  # notes.md: highlights, known problems (-F keeps "## " headings, the editor drops them)
git push origin v0.2.0
```

CI then builds both bundles from the tag and, once every job has passed, publishes them with `SHA256SUMS` as a
GitHub release. Its notes are the tag's summary followed by the pull requests merged since the previous release,
grouped by their label (`.github/release.yml`); `.github/workflows/labels.yml` labels each pull request from its
Conventional Commits title. A tag with a suffix (`v0.2.0-rc.1`) becomes a pre-release.

## Commits

Conventional Commits (`feat(menu): ...`, `fix(hd): ...`, `perf(granny): ...`), one topic per commit, with the tests
passing.

## For maintainers: the translation recipe

The C is generated from the translation recipe `data/recipe.json.gz` (about 0.5 MB) and the player's own files. The
recipe holds each module's SHA-256, the order and length of every instruction and the basic-block leaders, but no byte
of the game. It is made from a Ghidra export of the three modules:

```sh
export GHIDRA_HOME=/path/to/ghidra_12.1.4_PUBLIC   # Ghidra 12.1.4 headless, JDK 21
SACRED_RECIPE= tools/bootstrap.sh --export         # ghidra-proj/ and analysis/export/ (both ignored)
build/venv/bin/python tools/lift/recipe.py make    # analysis/export -> data/recipe.json.gz
build/venv/bin/python tools/lift/recipe.py check data/recipe.json.gz --assets "$SACRED_ASSETS" --export analysis/export
```

`tools/ghidra_scripts/ExportRecompileIR.java` writes the listing; `tools/lift/export_*.sh` drive it, and
`tools/lift/check_export.sh` regenerates the export and compares it with `tools/lift/export-sacred.sha256`. Entry
points the analysis misses are admitted from `tools/lift/code-pointer-entries-*.txt` and `data/entry-targets.json`.
With `SACRED_RECIPE=` (empty), `tools/bootstrap.sh --generate` reads the export instead of the recipe; both give
identical C.
