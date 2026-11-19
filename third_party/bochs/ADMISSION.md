# Software numerical source subset

This is the pinned numerical source for the software ext80 x87 and the SIMD
arithmetic the game reaches. Which operations take their results from it is
described in `docs/ARCHITECTURE.md` (CPU semantics) and in
`third_party/recomp-kit/PATCHES.md`.

The exact source revision and per-file identities are in `UPSTREAM.json`.
Files in `fpu/` and `softfloat3e/` retain their upstream notices. `COPYING`
preserves the Bochs distribution's LGPL 2.1 text; SoftFloat's license is also
retained at `softfloat3e/COPYING.txt`. `config.h` is local standalone compiler
configuration. The numerical source files are otherwise unmodified.

No emulator execution loop, decoder, guest process model or runtime fallback
is present. Berkeley SoftFloat 3e's integer-based ext80 and IEEE helpers are
used through Bochs' per-operation status API, so rounding/exception state is
owned by the guest CPU rather than process-global SoftFloat variables.

## License compatibility and separation

The recompiler's admitted source retains its MIT license. SoftFloat 3e's
BSD-style three-clause terms remain at `softfloat3e/COPYING.txt`. Bochs'
`fpu_trans.h` carries LGPL 2-or-later notices; `COPYING` preserves its supplied
LGPL 2.1 text. Numerical transcendental files also retain the older SoftFloat
extension's four-paragraph permission/disclaimer and derivative-work terms
verbatim. They are not relabeled MIT.

These sources are combined as a separate numerical library, not copied into
the MIT instruction emitter. The permissive emitter license imposes no
conflicting downstream restrictions; using the numerical library does not
remove its original source, notice, modification or relinking obligations.
The build links them as the shared library `libsacred_softfloat` (a static
archive of the same sources exists only for tests), so the library can be
rebuilt or replaced without relinking the rest. Release bundles carry the
distinct license texts, and this repository is the corresponding source
(`THIRD_PARTY_NOTICES.md`). No game
binary or asset is redistributed by this source admission.

Primary license texts were checked directly. This records the source/linkage
decision, not a blanket MIT license claim for the final port.
