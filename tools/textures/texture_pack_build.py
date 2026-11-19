#!/usr/bin/env python3
"""Build a texture pack: every PNG of SOURCE (a texture_index --export or SACRED_TEXTURE_DUMP directory, files named
<w>x<h>_<FORMAT>_<hash16>.png) upscaled by an integer factor into TARGET under the same name.

The game's textures are mostly atlases: sprites, ground diamonds and UI pieces packed into one image with transparent
gutters between them. A plain resampling filter smears each piece into its neighbours and into the transparent
black around it, which shows up in game as faint lines along sprite and tile edges. Here:
  * transparent pixels first take the colour (and alpha) of the nearest visible pixel, so the filter only ever
    mixes a piece with itself;
  * the image is padded by replicating its border, so the outermost texels do not ring against black;
  * RGB and alpha are resampled separately (Lanczos), then alpha is cut back to the original coverage magnified
    with nearest-neighbour and grown by one output pixel: pieces keep their shape, the edge samples of a sprite
    quad still find the piece's own colour, and the rest of each gutter stays fully transparent;
  * fully opaque textures keep alpha 255.
Requires Pillow and numpy.

Usage: texture_pack_build.py SOURCE TARGET [--scale 2] [--jobs N] [--only LIST] [--overwrite]
       texture_pack_build.py --self-test
  --only LIST: a file with one hash16 (or file name) per line; only those are built.
"""
from __future__ import annotations

import argparse
import os
import sys
from multiprocessing import Pool
from pathlib import Path

import numpy as np
from PIL import Image

PAD = 4          # replicated border, in source pixels (Lanczos at 2x reads ~1.5 source pixels around a sample)
EXTEND = 6       # iterations of colour extension into transparent pixels


def extend(rgba: np.ndarray) -> np.ndarray:
    """Transparent pixels take the average colour/alpha of visible neighbours, growing outward EXTEND pixels; the
    rest takes the mean visible colour."""
    out = rgba.astype(np.float32)
    known = rgba[..., 3] > 0
    if known.all() or not known.any():
        return rgba

    for _ in range(EXTEND):
        total = np.zeros_like(out)
        count = np.zeros(known.shape, np.float32)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)):
            shifted_known = np.zeros_like(known)
            shifted = np.zeros_like(out)
            ys = slice(max(dy, 0), known.shape[0] + min(dy, 0))
            yd = slice(max(-dy, 0), known.shape[0] + min(-dy, 0))
            xs = slice(max(dx, 0), known.shape[1] + min(dx, 0))
            xd = slice(max(-dx, 0), known.shape[1] + min(-dx, 0))
            shifted_known[yd, xd] = known[ys, xs]
            shifted[yd, xd] = out[ys, xs]
            total += shifted * shifted_known[..., None]
            count += shifted_known

        grow = ~known & (count > 0)
        if not grow.any():
            break

        out[grow] = total[grow] / count[grow][:, None]
        known = known | grow

    if not known.all():
        out[~known] = out[known].mean(axis=0)

    return np.clip(np.rint(out), 0, 255).astype(np.uint8)


def resample(channels: np.ndarray, scale: int) -> np.ndarray:
    """Lanczos per channel with a replicated border."""
    height, width = channels.shape[:2]
    padded = np.pad(channels, ((PAD, PAD), (PAD, PAD), (0, 0)), mode="edge")
    planes = []
    for c in range(padded.shape[2]):
        image = Image.fromarray(padded[..., c], "L")
        big = image.resize((padded.shape[1] * scale, padded.shape[0] * scale), Image.LANCZOS)
        planes.append(np.asarray(big)[PAD * scale:(PAD + height) * scale, PAD * scale:(PAD + width) * scale])

    return np.stack(planes, axis=-1)


def build(rgba: np.ndarray, scale: int) -> np.ndarray:
    alpha = rgba[..., 3]
    if (alpha == 255).all():
        rgb = resample(rgba[..., :3], scale)
        return np.dstack([rgb, np.full(rgb.shape[:2], 255, np.uint8)])

    filled = extend(rgba)
    out = resample(filled, scale)
    coverage = np.repeat(np.repeat(alpha > 0, scale, axis=0), scale, axis=1)
    # Grown by one output pixel: a 2D sprite quad samples up to half a texel of the magnified image past its piece
    # (the game's UVs put sample points on texel edges at scale 1); there it must find the piece's own colour and
    # alpha, not the transparent gutter. Atlas gutters are >= 2 source pixels, so neighbouring pieces stay apart.
    grown = coverage.copy()
    grown[1:, :] |= coverage[:-1, :]
    grown[:-1, :] |= coverage[1:, :]
    grown[:, 1:] |= coverage[:, :-1]
    grown[:, :-1] |= coverage[:, 1:]
    out[..., 3] = np.where(coverage, np.maximum(out[..., 3], 1), np.where(grown, out[..., 3], 0))
    # Additive blending ignores alpha: where nothing is visible, show exactly what the original shows there.
    clear = out[..., 3] == 0
    if clear.any():
        out[..., :3][clear] = deringed(rgba[..., :3], scale)[clear]

    return out


def deringed(rgb: np.ndarray, scale: int) -> np.ndarray:
    """Lanczos upscale clamped to the range of each source pixel's 3x3 neighbourhood: no ringing into flat areas (a
    black background two pixels away from a glow stays exactly black)."""
    padded = np.pad(rgb, ((1, 1), (1, 1), (0, 0)), mode="edge")
    height, width = rgb.shape[:2]
    windows = [padded[dy:dy + height, dx:dx + width] for dy in range(3) for dx in range(3)]
    low = np.repeat(np.repeat(np.min(windows, axis=0), scale, axis=0), scale, axis=1)
    high = np.repeat(np.repeat(np.max(windows, axis=0), scale, axis=0), scale, axis=1)
    return np.clip(resample(rgb, scale), low, high)


def self_test() -> None:
    rng = np.random.default_rng(1)
    # Additive-style icon: a glowing disc on transparent black.
    yy, xx = np.mgrid[:32, :32]
    disc = (yy - 15.5) ** 2 + (xx - 15.5) ** 2 < 100
    icon = np.zeros((32, 32, 4), np.uint8)
    icon[disc] = (240, 200, 60, 255)
    out = build(icon, 2)
    far = np.repeat(np.repeat((yy - 15.5) ** 2 + (xx - 15.5) ** 2 > 150, 2, 0), 2, 1)
    assert out.shape == (64, 64, 4)
    assert (out[..., 3][far] == 0).all() and (out[..., :3][far] == 0).all(), "transparent black must stay black"
    assert (out[..., :3][out[..., 3] > 0].max(axis=1) > 0).all()
    # Alpha-blended atlas piece: extended colour still sits under the visible (alpha > 0) output, so no dark fringe.
    piece = np.zeros((16, 16, 4), np.uint8)
    piece[4:12, 4:12] = (200, 100, 50, 255)
    out = build(piece, 2)
    visible = out[..., 3] > 0
    assert np.abs(out[..., :3][visible].astype(int) - (200, 100, 50)).max() <= 2, "fringe under visible pixels"
    assert (out[:6] == 0).all() and (out[26:] == 0).all() and (out[:, :6] == 0).all() and (out[:, 26:] == 0).all()
    # Opaque additive texture (black background, no alpha): plain upscale, black stays black.
    flare = np.zeros((16, 16, 4), np.uint8)
    flare[..., 3] = 255
    flare[6:10, 6:10, :3] = 255
    out = build(flare, 2)
    assert (out[..., 3] == 255).all() and (out[:4, :, :3] == 0).all() and (out[:, :4, :3] == 0).all()
    # Random textures: transparent output equals the upscale of the original colour.
    for _ in range(20):
        image = rng.integers(0, 256, (24, 40, 4), dtype=np.uint8)
        image[rng.random((24, 40)) < 0.5, 3] = 0
        out = build(image, 3)
        assert np.array_equal(out[..., :3][out[..., 3] == 0], deringed(image[..., :3], 3)[out[..., 3] == 0])

    print("self-test ok")


def job(arguments: tuple[str, str, int, bool]) -> str | None:
    source, target, scale, overwrite = arguments
    try:
        if not overwrite and os.path.exists(target):
            return None

        rgba = np.asarray(Image.open(source).convert("RGBA"))
        Image.fromarray(build(rgba, scale), "RGBA").save(target, compress_level=6)
        return None
    except Exception as error:  # noqa: BLE001 - reported per file, the run continues
        return f"{source}: {error}"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path, nargs="?")
    parser.add_argument("target", type=Path, nargs="?")
    parser.add_argument("--scale", type=int, default=2)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--only", type=Path)
    parser.add_argument("--overwrite", action="store_true", help="rebuild files that already exist in TARGET")
    parser.add_argument("--self-test", action="store_true")
    options = parser.parse_args()
    if options.self_test:
        self_test()
        return

    if options.source is None or options.target is None:
        parser.error("SOURCE and TARGET are required")

    if not 2 <= options.scale <= 4:
        raise SystemExit("--scale must be 2..4")

    wanted = None
    if options.only:
        wanted = {line.strip()[:-4].split("_")[-1] if line.strip().endswith(".png") else line.strip()
                  for line in options.only.read_text().splitlines() if line.strip()}

    options.target.mkdir(parents=True, exist_ok=True)
    work = []
    for path in sorted(options.source.rglob("*.png")):
        if wanted is not None and path.stem.split("_")[-1] not in wanted:
            continue

        work.append((str(path), str(options.target / path.name), options.scale, options.overwrite))

    failures = []
    with Pool(options.jobs) as pool:
        for done, failure in enumerate(pool.imap_unordered(job, work, chunksize=8), 1):
            if failure:
                failures.append(failure)

            if done % 500 == 0 or done == len(work):
                print(f"{done}/{len(work)}", file=sys.stderr, flush=True)

    for failure in failures:
        print("FAILED", failure, file=sys.stderr)

    print(f"built {len(work) - len(failures)} of {len(work)} into {options.target}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
