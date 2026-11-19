#!/usr/bin/env python3
"""Split wide menu pictures (MENU_BACKGROUNDS, 1836x768 or k times that) into what is ours and what is the game's.

A picture shows the game's own 1024x768 screen in its middle (the 4x3 tiles of texture.pak, or the loading bitmap)
and our extension at the sides, with a blend band and retouches reaching into the middle. Only the pixels that
differ from the game's screen are kept, so no unchanged pixel of the game is stored:
    <key>.sides.png   RGBA: our pixels; (0,0,0,0) where the game's own pixel is taken
    <key>.mask.png    8-bit grey: 255 where <key>.sides.png holds the pixel, 0 where the game's is taken,
                      64 where it holds a correction of the game's visible pixel: (ours - game) mod 256 per channel
                      (RGBA), added back to the player's pixel modulo 256
Pixels of ours over the game's visible (not fully transparent) pixels are such corrections, so the retouches and the
blend band are stored as changes to the game's art, not as its pixels; ours over the game's transparent holes and
the new sides are stored as they are. The composed picture is the same either way: composed.sha256 holds the
SHA-256 of each composed RGBA image (menu_split_tests checks the engine against it).
Pixels within NOISE levels of the game's count as the game's. The blend band (BAND pixels inside each old 4:3 border,
where the new side was faded into the game's screen) is kept as it is: --drop-band leaves the game's screen
unchanged there instead and matches our side's colour to it (the colour step along the border, smoothed, fading out
over BAND pixels away from it). Measured on the eight pictures the seam is then 1.3 to 2.1 times the image's usual
column-to-column change (1.0 with the band) and visible on the save game window's arch, so the band stays.
The engine (runtime/ddraw/menu_backdrops.cpp) puts the two back together with the player's texture.pak and loading
bitmaps when MENU_BACKGROUNDS holds such a pair instead of <key>.png; --preview
writes what the engine will show (to compare with the input).

    split_backgrounds.py GAME PICTURES OUT [--preview DIR]
Requires Pillow and numpy.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

BAND = 27    # blend band at each old 4:3 border, in 768-high pixels
SMOOTH = 6   # standard deviation (pixels) of the colour step along the border
NOISE = 3    # largest per-channel difference still taken as the game's own pixel

LOADING_BITMAPS = {"loadinguw00": "LoadingUW00.bmp", "loadinguw01": "LoadingUW01.bmp",
                   "loading0": "loading0.bmp", "loading1": "loading1.bmp"}


def numbered(pattern: str) -> list[str]:
    return [pattern % (1 + i) for i in range(12)]


# runtime/ddraw/menu_backdrops.cpp backdrop_screens(): the tiles of each screen, row by row.
SCREENS = {
    "main-sacred": numbered("GUI_OPTION%d.TGA"),
    "main-underworld": numbered("GUI_UW_OPTION%d.TGA"),
    "charselect-sacred": numbered("GUI_CHAR_%02d.TGA"),
    "charselect-underworld": numbered("GUI_UW_CHAR_%02d.TGA"),
    "options-sacred": [f"GUI_OPTION_2_{r}{c}.TGA" for r in range(3) for c in range(4)],
    "options-underworld": numbered("UW_OPTION_1_%02d.TGA"),
    "saveload": numbered("UW_OPTION_%02d.TGA"),
}


def find(directory: Path, name: str) -> Path:
    for path in directory.iterdir():
        if path.name.lower() == name.lower():
            return path

    raise SystemExit(f"{name} not found in {directory}")


def pak_tiles(pak: Path) -> dict[str, np.ndarray]:
    """Format-6 (B,G,R,A) entries of a TEX\\3 pak as RGBA arrays, by upper-case name."""
    data = pak.read_bytes()
    if data[:4] != b"TEX\x03":
        raise SystemExit(f"{pak}: not a texture pak")

    count = struct.unpack_from("<I", data, 4)[0]
    tiles = {}
    for index in range(count):
        _, offset, size = struct.unpack_from("<III", data, 0x100 + index * 12)
        if not size or offset + 0x50 + size > len(data):
            continue

        name = data[offset:offset + 32].split(b"\0")[0].decode("latin-1").upper()
        width, height, format_ = struct.unpack_from("<HHI", data, offset + 32)
        if format_ & 0xFF != 6 or size != width * height * 4:
            continue

        bgra = np.frombuffer(data, np.uint8, size, offset + 0x50).reshape(height, width, 4)
        tiles[name] = bgra[..., [2, 1, 0, 3]]

    return tiles


def original(key: str, game: Path, tiles: dict[str, np.ndarray]) -> np.ndarray:
    if key.startswith("loading-"):
        bitmap = LOADING_BITMAPS.get(key[len("loading-"):])
        if not bitmap:
            raise SystemExit(f"{key}: unknown loading bitmap")

        return np.asarray(Image.open(find(game / find(game, "PAK").name, bitmap)).convert("RGBA"))

    screen = np.zeros((768, 1024, 4), np.uint8)
    for i, name in enumerate(SCREENS[key]):
        row, column = divmod(i, 4)
        screen[row * 256:row * 256 + 256, column * 256:column * 256 + 256] = tiles[name.upper()]

    return screen


def middle_of(picture: np.ndarray, screen: np.ndarray) -> tuple[slice, slice, np.ndarray]:
    """The picture's region of the original screen, and the screen at the picture's scale (nearest)."""
    k = picture.shape[0] // 768
    if picture.shape[0] != 768 * k:
        raise SystemExit("picture height must be 768 or a multiple")

    left = (picture.shape[1] - 1024 * k) // 2
    scaled = np.repeat(np.repeat(screen, k, axis=0), k, axis=1)
    return slice(0, 768 * k), slice(left, left + 1024 * k), scaled


def compose(ours: np.ndarray, mask: np.ndarray, rows: slice, columns: slice, screen: np.ndarray) -> np.ndarray:
    """What the engine shows: the game's pixel where the mask is 0, ours where it is 255, the game's pixel plus our
    correction (mod 256) where it is 64."""
    out = ours.copy()
    region = out[rows, columns]
    keep = mask[rows, columns]
    region[keep == 0] = screen[keep == 0]
    corrected = keep == 64
    region[corrected] = (screen[corrected].astype(np.uint16) + region[corrected]).astype(np.uint8)  # mod 256
    out[rows, columns] = region
    return out


def matched_sides(picture: np.ndarray, columns: slice, screen: np.ndarray) -> np.ndarray:
    """The picture with its sides shifted in colour to meet the game's screen at both borders (see the docstring)."""
    out = picture.astype(np.float64)
    k = screen.shape[0] // 768
    band = BAND * k
    radius = 3 * SMOOTH * k
    kernel = np.exp(-0.5 * (np.arange(-radius, radius + 1) / (SMOOTH * k)) ** 2)
    kernel /= kernel.sum()
    for inside, outward in ((0, -1), (screen.shape[1] - 1, 1)):
        edge = columns.start - 1 if outward < 0 else columns.stop
        game = screen[:, inside, :3].astype(np.float64)
        side = out[:, edge, :3]
        visible = (screen[:, inside, 3] > 0) & (picture[:, edge, 3] > 0)
        step = np.where(visible[:, None], game - side, 0.0)
        weight = np.convolve(visible.astype(np.float64), kernel, mode="same")
        smooth = np.stack([np.convolve(step[:, c], kernel, mode="same") for c in range(3)], axis=1)
        smooth = np.where(weight[:, None] > 1e-6, smooth / np.maximum(weight, 1e-6)[:, None], 0.0)
        for d in range(band):
            t = (d + 0.5) / band
            fade = 1 - t * t * (3 - 2 * t)
            x = edge + outward * d
            out[:, x, :3] += smooth * fade

    return np.clip(np.floor(out + 0.5), 0, 255).astype(np.uint8)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("game", type=Path)
    parser.add_argument("pictures", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--drop-band", action="store_true", help="show the game's screen in the blend band")
    parser.add_argument("--preview", type=Path, help="write what the engine will show here, and check the sides")
    args = parser.parse_args()
    tiles = pak_tiles(find(args.game / find(args.game, "PAK").name, "texture.pak"))
    args.out.mkdir(parents=True, exist_ok=True)
    failed = 0
    hashes: dict[str, str | None] = {}
    for path in sorted(args.pictures.glob("*.png")):
        key = path.stem
        if key not in SCREENS and not key.startswith("loading-"):
            print(f"{path.name}: not a screen with a known original, skipped")
            continue

        picture = np.asarray(Image.open(path).convert("RGBA")).copy()
        rows, columns, screen = middle_of(picture, original(key, args.game, tiles))
        same = np.zeros(picture.shape[:2], bool)
        difference = np.abs(picture[rows, columns].astype(int) - screen.astype(int)).max(axis=2)
        same[rows, columns] = difference <= NOISE
        band = np.zeros(picture.shape[:2], bool)
        k = picture.shape[0] // 768
        band[:, columns.start:columns.start + BAND * k] = True
        band[:, columns.stop - BAND * k:columns.stop] = True
        # Where the game's screen is fully transparent (holes for the live sky and water) the picture's paint is ours.
        empty = np.zeros(picture.shape[:2], bool)
        empty[rows, columns] = screen[..., 3] == 0
        band &= ~empty
        if not args.drop_band:
            band[:] = False

        ours = matched_sides(picture, columns, screen) if args.drop_band else picture.copy()
        ours[same | band] = 0
        mask = np.where(same | band, 0, 255).astype(np.uint8)
        # Ours over the game's visible pixels: the change, not the pixel.
        over_game = np.zeros(picture.shape[:2], bool)
        over_game[rows, columns] = screen[..., 3] > 0
        delta = over_game & (mask == 255)
        scaled = np.zeros(picture.shape, np.uint8)
        scaled[rows, columns] = screen
        ours[delta] = (ours[delta].astype(np.int16) - scaled[delta]).astype(np.uint8)  # mod 256
        mask[delta] = 64
        Image.fromarray(ours, "RGBA").save(args.out / f"{key}.sides.png", optimize=True)
        Image.fromarray(mask, "L").save(args.out / f"{key}.mask.png", optimize=True)
        print(f"{key}: {int((mask[rows, columns] == 64).sum())} corrections of the game's visible pixels, "
              f"{int((mask[rows, columns] == 255).sum())} pixels of ours over its transparent holes")
        hashes[key] = None
        if args.preview:
            back = np.asarray(Image.open(args.out / f"{key}.sides.png").convert("RGBA")).copy()
            stored = np.asarray(Image.open(args.out / f"{key}.mask.png"))
            shown = compose(back, stored, rows, columns, screen)
            hashes[key] = hashlib.sha256(shown.tobytes()).hexdigest()
            args.preview.mkdir(parents=True, exist_ok=True)
            Image.fromarray(shown, "RGBA").save(args.preview / f"{key}.png")
            error = np.abs(shown.astype(int) - picture.astype(int)).max(axis=2)
            inner = np.zeros(error.shape, bool)
            inner[:, columns.start + BAND * k:columns.stop - BAND * k] = True
            if error[inner].max() > NOISE:
                print(f"{key}: CHECK FAILED (the middle changed)")
                failed += 1

            print(f"  preview: largest difference to the input {error.max()}"
                  + (f", mean in the band {error[band].mean():.1f}" if band.any() else ""))

    if args.preview:
        sums = "".join(f"{digest}  {key}\n" for key, digest in sorted(hashes.items()))
        (args.out / "composed.sha256").write_text(sums)

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
