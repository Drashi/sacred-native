#!/usr/bin/env python3
"""Compare a 1024x768 capture with a wider HD capture of the same moment (tests/menu/qa/hd-dwarf-*.script).

The HD view must show the same world shifted right by (W-1024)/2 pixels at the same scale. For each probe rectangle
of the 1024 image, the best normalised-correlation match is searched in the HD image around the expected shift;
the probe passes when it is found exactly there with a good score.

Usage: hd_compare.py REFERENCE.bmp HD.bmp [x,y,w,h ...]   (needs Pillow and NumPy)
"""
import sys

import numpy as np
from PIL import Image


def gray(path):
    return np.asarray(Image.open(path).convert("L"), dtype=np.float32)


def best_match(patch, image, x0, y0, radius=24):
    ph, pw = patch.shape
    p = (patch - patch.mean()) / (patch.std() + 1e-6)
    best = (-2.0, x0, y0)
    for y in range(max(0, y0 - radius), min(image.shape[0] - ph, y0 + radius) + 1):
        for x in range(max(0, x0 - radius), min(image.shape[1] - pw, x0 + radius) + 1):
            w = image[y:y + ph, x:x + pw]
            score = float(((w - w.mean()) / (w.std() + 1e-6) * p).mean())
            if score > best[0]:
                best = (score, x, y)

    return best


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    ref, hd = gray(sys.argv[1]), gray(sys.argv[2])
    shift = (hd.shape[1] - ref.shape[1]) // 2
    probes = [tuple(int(v) for v in a.split(",")) for a in sys.argv[3:]] or [
        (180, 230, 200, 150), (460, 140, 70, 60), (650, 280, 60, 110), (880, 330, 120, 200), (560, 70, 60, 60)]
    failed = 0
    for x, y, w, h in probes:
        score, mx, my = best_match(ref[y:y + h, x:x + w], hd, x + shift, y)
        ok = mx - x == shift and my == y and score >= 0.9
        failed += not ok
        print(f"HD_COMPARE probe={x},{y},{w},{h} dx={mx - x} dy={my - y} expected_dx={shift} score={score:.2f} "
              f"{'ok' if ok else 'FAIL'}")

    print("HD_COMPARE_PASS" if not failed else f"HD_COMPARE_FAIL {failed}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
