# Wide menu backgrounds (split)

Pictures for `MENU_BACKGROUNDS` (see `tools/dist/BUNDLE_README.md`): the 1024x768 menu screens extended to
1836x768, made for this project. They are stored split by `tools/menu/split_backgrounds.py`, so that no unchanged
pixel of the game is here:

* `<screen>.sides.png`: our pixels (the new sides, paint over the game's transparent holes); over the game's
  visible pixels (the blend band near the old 4:3 border, retouches) only the change: `(ours - game) mod 256` per
  channel; `(0,0,0,0)` where the game's own pixel is used
* `<screen>.mask.png`: 255 where `.sides.png` holds the pixel, 64 where it holds a change to the game's pixel, 0
  where the game's is taken
* `composed.sha256`: the SHA-256 of each composed RGBA picture; `menu_split_backgrounds` checks the engine's result
  against it

The engine (`runtime/ddraw/menu_backdrops.cpp`) puts each picture back together at start from the player's
`PAK/texture.pak` tiles or loading bitmap (the installation's own files: a profile's `PAK/texture03.pak` from
`tools/legacy_char_pak.py` replaces the character select's tiles and is not used as the base; a picture that does not
compose to its hash in `composed.sha256` is left out with a note) (`menu_split_backgrounds` test; the result equals the unsplit originals
up to the noise rule below). A full `<screen>.png` in the same directory wins over a pair.

Pixels within 3 levels of the game's are taken as the game's (an invisible difference; it drops a third to three
quarters of the blend band's pixels). The blend band itself (27 pixels inside each old 4:3 border) is kept: without
it (`--drop-band`, our side colour-matched to the unchanged screen instead) the seam measured 1.3 to 2.1 times the
picture's usual column-to-column change and showed on the save game window's arch.

The new parts were made from the game's screens (extended and retouched), so they are derived from its art: they
are not the game's files, but they are not free of it either. `charselect-sacred` carries a repainted middle (places
for the two Underworld classes).
