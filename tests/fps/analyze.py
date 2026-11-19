#!/usr/bin/env python3
"""analyze.py LABEL...: per run, the walking speed of the local player (position units per second of guest time).
Exits 1 when the straight-line speeds of the first two walks (unobstructed in gen.py's script; the later ones
end at obstacles) differ between the runs by more than 1%."""
import csv, sys, math, os
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "perf", "fps")
speeds = {}
for label in sys.argv[1:]:
    csv_path = f"{ROOT}/profile-{label}/evidence/ppos.csv"
    rows = [r for r in csv.DictReader(open(csv_path)) if r.get("layer") not in (None, "")]
    t = [float(r["guest_time"]) for r in rows]
    x = [int(r["x"]) for r in rows]
    y = [int(r["y"]) for r in rows]
    s = [int(r["sector"]) for r in rows]
    dts = [b - a for a, b in zip(t, t[1:]) if b > a]
    dts.sort()
    med = dts[len(dts) // 2] if dts else 0
    # moving segments: consecutive frames within one sector where the position changes
    segs = []
    cur = None
    for i in range(1, len(rows)):
        moved = s[i] == s[i-1] and (x[i] != x[i-1] or y[i] != y[i-1])
        if moved:
            if cur is None:
                cur = [i-1, i]
            else:
                cur[1] = i
        else:
            # allow gaps of a few identical frames inside a walk (position updates may be slower than frames)
            if cur is not None and i - cur[1] > max(3, int(0.25 / med) if med else 3):
                segs.append(cur)
                cur = None

    if cur:
        segs.append(cur)

    print(f"{label}: frames={len(rows)} median_dt={med*1000:.3f} ms ({1/med if med else 0:.1f} FPS) "
          f"guest_span={t[-1]-t[0]:.2f}s")
    for a, b in segs:
        dur = t[b] - t[a]
        if dur < 1.0:
            continue

        path = sum(math.hypot(x[i]-x[i-1], y[i]-y[i-1]) for i in range(a+1, b+1) if s[i] == s[i-1])
        # steady part: skip the first and last 0.5 s
        ia = next(i for i in range(a, b) if t[i] - t[a] >= 0.5)
        ib = next(i for i in range(b, a, -1) if t[b] - t[i] >= 0.5)
        steady = sum(math.hypot(x[i]-x[i-1], y[i]-y[i-1]) for i in range(ia+1, ib+1) if s[i] == s[i-1])
        sd = t[ib] - t[ia]
        steps = sum(1 for i in range(a+1, b+1) if (x[i], y[i]) != (x[i-1], y[i-1]))
        disp = math.hypot(x[b]-x[a], y[b]-y[a])
        if len(speeds.setdefault(label, [])) < 2:
            speeds[label].append(disp / dur)

        print(f"   walk {dur:5.2f}s path={path:9.0f} speed={path/dur:8.1f}/s "
              f"steady={steady/sd if sd>0 else 0:8.1f}/s disp={disp:7.0f} ({disp/dur:6.1f}/s) "
              f"pos_updates={steps} ({steps/dur:.1f}/s) from=({s[a]},{x[a]},{y[a]}) to=({s[b]},{x[b]},{y[b]})")

means = {label: sum(v) / len(v) for label, v in speeds.items()}
if len(means) > 1:
    spread = (max(means.values()) - min(means.values())) / min(means.values())
    speeds = ", ".join(f"{k}={v:.1f}" for k, v in means.items())
    print(f"straight-line speed of the first two walks: {speeds}; spread {spread*100:.2f}%")
    sys.exit(1 if spread > 0.01 else 0)
