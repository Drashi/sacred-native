# Texture packs

The engine can replace any game texture with a larger image (`TEXTURE_PACK`, see `tools/dist/BUNDLE_README.md`).
Wide pictures for the 1024x768 menu screens are a separate option, `MENU_BACKGROUNDS`, documented there too.
A replacement is found by the content hash of the original texture, so a pack is just a folder of PNGs named
`<w>x<h>_<FORMAT>_<hash16>.png` (or `<hash16>.png`), each an integer multiple of the original size.

These tools build a pack for every texture of the game, offline, from the installation:

* `texture_index` (C++, built with the engine): reads `PAK/texture.pak` and `PAK/texture03.pak`, turns every entry
  into the exact bytes the game puts into its texture (format 4: zlib A4R4G4B4, format 6: raw B,G,R,A =
  A8R8G8B8), hashes them like the engine and, with `--export`, writes each distinct texture once as a PNG with the
  pack's file name. Model skins (e.g. `DWARF.TGA`) are in these paks too. Not covered: textures the game makes at run
  time (font glyph caches, a few generated 64x64 images); `SACRED_TEXTURE_DUMP=DIR` while playing writes those.
* `texture_pack_build.py` (Python 3 with Pillow and numpy): upscales such a folder into a pack (Lanczos, atlas-safe:
  pieces of an atlas do not bleed into each other or into the transparent gutters; fully transparent output keeps
  the original colour, normally black, because glows and markers are drawn additively and ignore alpha).

Numbers for Sacred Gold (texture.pak + texture03.pak): 25,531 entries, 23,591 distinct textures (10.6k `MIX*`
ground, 6.0k `MINIMAP*`, 5.0k `ISO*` objects/vegetation, the rest UI, effects and skins). The last three entries of
the shipped texture.pak (`MIX9997..9999`) are cut off in the file and are skipped.

## Build the full 2x pack (macOS)

From the repository root, with the engine already built (`tools/gates.sh` or `cmake --build build/game`):

    cmake --build build/game --target texture_index
    GAME="/path/to/Sacred Gold"
    OUT=~/SacredPacks                                   # needs ~11 GB free for everything below

    # 1. Originals as PNG (~1.7 GB, ~2-3 minutes). Optional: --skip MINIMAP (minimap tiles, ~6k files, ~-25 %).
    build/game/texture_index --export "$OUT/originals" "$GAME/PAK/texture.pak" "$GAME/PAK/texture03.pak" \
        > "$OUT/texture_index.tsv"

    # 2. Python with Pillow and numpy in a private venv (no Homebrew needed; pip downloads the wheels).
    python3 -m venv ~/.venvs/sacred-textures
    ~/.venvs/sacred-textures/bin/pip install pillow numpy

    # 3. The 2x pack (~9 GB, ~23.6k files; minutes on all cores; --jobs N to limit). Re-running skips finished files.
    ~/.venvs/sacred-textures/bin/python tools/textures/texture_pack_build.py "$OUT/originals" "$OUT/pack-2x"

    # The originals are not needed by the game afterwards: rm -r "$OUT/originals" frees 1.7 GB.

`texture_pack_build.py SOURCE TARGET [--scale 2..4] [--jobs N] [--only LIST] [--overwrite]`: `--only` takes a file
of hashes (or file names) to build just those; `--overwrite` rebuilds files already in TARGET (needed after a builder
change). `texture_pack_build.py --self-test` checks the builder's edge rules. Any other upscaler can be used instead: keep the file names and an exact integer
multiple of the original size; replacements may be edited by hand (any RGBA PNG).

The engine can do the same without a pack: `TEXTURE_UPSCALE : 2` upscales each texture of the paks when the game
first uses it (`runtime/ddraw/texture_upscale.cpp`, the same algorithm and arithmetic as `texture_pack_build.py`;
`texture_upscale_tests SOURCE EXPECTED` compares it byte for byte with the script's output: 1966 of 1966 sampled
textures identical, about 9 ms each on one core). A pack is still the way to ship hand-edited or AI-upscaled
textures; its files win over the engine's upscale.

## Play with it

    SACRED_TEXTURE_PACK=~/SacredPacks/pack-2x SACRED_RENDER_SCALE=2 ./sacred-gold "$GAME"

or `TEXTURE_PACK : /Users/.../pack-2x` and `RENDER_SCALE : 2` in the profile's `Settings.cfg` (the environment
wins). At start the engine only indexes the file names (`SACRED_TEXTURE_PACK dir=... textures=N`); a replacement is
decoded on a background thread when its texture is first used and shown from the next frame on, so loading never
waits for PNG decoding (about 3 ms per 512x512 PNG). Decoded images stay in a cache of `SACRED_TEXTURE_CACHE_MB`
(default 512); the GPU keeps its own copy. Every 300 frames the log shows `SACRED_HD ... replaced=... deferred=...
rejected=...` and `SACRED_TEXTURE_PACK decoded=... avg_ms=... max_ms=...`.

Known limitation: a sprite the game draws from several atlas pieces (some trees) can show a faint line where the
pieces meet, because each piece is upscaled on its own.
