# Third-party notices

The project's own code is licensed under the GNU General Public License, version 3 or later (`LICENSE`). It grants
nothing in Sacred Gold: no game code or data is part of this repository or of any release. Vendored sources keep
their license next to the code; all of them are compatible with GPL-3.0.

| Component | Path | License |
|---|---|---|
| recomp-kit (instruction translator and runtime headers) | `third_party/recomp-kit` | MIT (`LICENSE`, `NOTICE`) |
| Berkeley SoftFloat 3e (as shipped inside Bochs) | `third_party/bochs/softfloat3e` | BSD 3-clause (`softfloat3e/COPYING.txt`) |
| Bochs x87/FPU numerics | `third_party/bochs/fpu` | LGPL-2.1-or-later (`third_party/bochs/COPYING`) |
| minimp3 | `third_party/minimp3` | CC0 |
| dr_libs (dr_wav) | `third_party/dr_libs` | public domain / MIT-0 |
| stb_image, stb_image_write | `third_party/stb` | public domain / MIT |

Libraries linked dynamically and shipped in release bundles: FFmpeg (LGPL-2.1-or-later), SDL2 (zlib), FreeType (FTL).
Every bundle carries their license texts under `licenses/`; the bundle scripts fail if one is missing.

`third_party/recomp-kit/NOTICE` is upstream's file, kept verbatim. It also names components of the complete kit (Lua,
TinySoundFont, stb_truetype, Open Sans, GeneralUser GS, miniz, SDL3, FFmpeg 7.1.1, volk, the Vulkan headers) that are
not part of the subset vendored here: none of them is in this repository or in a release bundle
(`third_party/recomp-kit/ADMISSION.md`).

## Bochs numerics: LGPL relinking

The Bochs FPU sources and SoftFloat are built as the separate shared library `libsacred_softfloat` (`.so` on Linux,
`.dylib` on macOS), not linked statically into `sacred-native`. It can be rebuilt from `third_party/bochs`, or replaced
by any build with the same exported symbols, without relinking the rest (LGPL section 6). The corresponding source is
this repository at the tagged release.

## FFmpeg

`sacred-native` links libavformat, libavcodec, libavutil, libswscale and libswresample dynamically for movie playback.

- Version: FFmpeg 8.0.1 (`https://ffmpeg.org/releases/ffmpeg-8.0.1.tar.xz`, SHA-256
  `05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41`), copyright the FFmpeg developers.
- License: LGPL-2.1-or-later. `tools/build_ffmpeg.sh` configures it without `--enable-gpl` and `--enable-nonfree` and
  fails if the resulting `config.h` is not LGPL; the configure line there and the tarball above are the corresponding
  source. Every release bundle carries both (`licenses/ffmpeg-8.0.1.tar.xz`, `licenses/ffmpeg-build.sh`), and the
  bundle scripts fail when the bundled libraries are not that version. The libraries can be replaced by another build
  with the same soname.
- Do not distribute a build linked against a GPL-configured FFmpeg (for example a distribution or Homebrew package).

## Tools shipped in release bundles

For the first-start build of the game module a bundle carries two unmodified third-party tools, without the files
that build does not need:

- CPython 3.13 (PSF-2.0, `licenses/python-LICENSE.txt`), the `install_only_stripped` build of
  [python-build-standalone](https://github.com/astral-sh/python-build-standalone). That build links the libraries of
  its standard extension modules statically (OpenSSL, SQLite, libffi, bzip2, xz, zlib, expat, mpdecimal and others);
  their licenses are listed in that project's documentation.
- Zig (MIT, `licenses/zig-LICENSE`) as a fallback C compiler. Its `lib/` directory contains code of other projects
  under their own licenses: clang's headers and libunwind from LLVM (Apache-2.0 with LLVM exceptions) and the C
  library headers of the target (glibc on Linux, Apple's on macOS).
