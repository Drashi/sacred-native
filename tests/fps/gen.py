#!/usr/bin/env python3
"""gen.py FPS OUT: new Dwarf game, then held-button walks whose lengths are given in seconds of game time."""
import sys
fps = int(sys.argv[1])
out = sys.argv[2]
f = lambda seconds: max(1, round(seconds * fps))
pre = """frames 600
move 510 288
frames 100
down 1
frames 3
up 1
frames 400
move 530 430
frames 100
down 1
frames 3
up 1
frames 200
move 590 714
frames 100
down 1
frames 3
up 1
waitmark treppe.bin 4000
waitmark wea.bin 1500
"""
world = [f"frames {f(2.5)}", "dismiss 12 20", f"frames {f(1)}", "dismiss 12 20", f"frames {f(1)}",
         "move 1000 10", f"frames {f(2)}", "capture w0", "frames 4"]
legs = [(812, 384, 4.0), (212, 384, 4.0), (512, 634, 3.0), (512, 134, 3.0)]
for i, (x, y, t) in enumerate(legs):
    world += [f"move {x} {y}", f"frames {f(0.3)}", "down 1", f"frames {f(t)}", "up 1", f"frames {f(1.5)}",
              f"capture leg{i}", "frames 4"]

world += ["type z"]
open(out, "w").write(pre + "\n".join(world) + "\n")
