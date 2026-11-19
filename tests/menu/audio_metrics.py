#!/usr/bin/env python3
"""Audio checks over the QA WAV tees. Frame indices come from QA_AUDIO_* markers (tee_frames=N) in run.log.

  audio_metrics.py music EVIDENCE_DIR [--min-rms N]   a stream (default: any) starts and its window is non-silent
  audio_metrics.py sfx EVIDENCE_DIR [--min-rms N] [--after MARKER] [--before MARKER]
                                                      a sample starts (between two run.log markers) and the
                                                      samples-only tee is non-silent in the 0.25 s after it
Prints PASS/FAIL lines; exit status 1 on any FAIL.
"""
import argparse, array, math, re, struct, sys

RATE = 44100


def load(path):
    with open(path, "rb") as f:
        data = f.read()

    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit("FAIL %s is not a WAV" % path)

    channels, rate = struct.unpack_from("<HI", data, 22)
    pcm = array.array("h")
    pcm.frombytes(data[44:44 + (len(data) - 44) // 4 * 4])
    return pcm, rate, channels


def rms(pcm, start_frame, frames):
    lo, hi = start_frame * 2, min(len(pcm), (start_frame + frames) * 2)
    if hi <= lo:
        return 0.0

    return math.sqrt(sum(v * v for v in pcm[lo:hi]) / (hi - lo))


def markers(log):
    out = []
    for index, line in enumerate(open(log, errors="replace")):
        m = re.search(r"tee_frames=(\d+)", line)
        out.append((index, line.strip(), int(m.group(1)) if m else None))

    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["music", "sfx"])
    ap.add_argument("evidence")
    ap.add_argument("--min-rms", type=float, default=100.0)
    ap.add_argument("--after")
    ap.add_argument("--before")
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--file", help="stream file name substring (music mode)")
    args = ap.parse_args()
    log = args.evidence + "/run.log"
    lines = markers(log)
    ok = True
    mix, rate, channels = load(args.evidence + "/audio.wav")
    total = len(mix) // 2 / RATE
    print("INFO mix duration %.2fs rate=%d channels=%d" % (total, rate, channels))
    if args.mode == "music":
        starts = [m for m in lines if "QA_AUDIO_STREAM_START" in m[1]]
        if args.file:
            handles = [re.search(r"handle=(\d+)", m[1]).group(1) for m in lines
                       if "QA_AUDIO_STREAM_OPEN" in m[1] and args.file.lower() in m[1].lower()]
            starts = [m for m in starts if any("handle=%s " % h in m[1] for h in handles)]

        if not starts:
            print("FAIL no QA_AUDIO_STREAM_START marker for %s" % (args.file or "any stream"))
            sys.exit(1)

        start = starts[0][2]
        level = rms(mix, start, int(args.seconds * RATE))
        verdict = level >= args.min_rms
        ok &= verdict
        print("%s music window %.1fs from frame %d (%s) rms=%.1f (min %.1f)" %
              ("PASS" if verdict else "FAIL", args.seconds, start, args.file or "first stream", level, args.min_rms))
    else:
        sfx, _, _ = load(args.evidence + "/audio.sfx.wav")
        lo = 0
        hi = len(lines)
        if args.after:
            hits = [i for i, m in enumerate(lines) if args.after in m[1]]
            lo = hits[0] if hits else hi

        if args.before:
            hits = [i for i, m in enumerate(lines) if args.before in m[1] and i >= lo]
            hi = hits[0] if hits else hi

        starts = [m for m in lines[lo:hi] if "QA_AUDIO_SAMPLE_START" in m[1]]
        loud = [m for m in starts if rms(sfx, m[2], RATE // 4) >= args.min_rms]
        verdict = bool(loud)
        ok &= verdict
        print("%s sample effects in window: %d started, %d audible (samples-only rms >= %.1f in 0.25s)" %
              ("PASS" if verdict else "FAIL", len(starts), len(loud), args.min_rms))
        if loud:
            f = loud[0][2]
            print("INFO first audible effect at frame %d (%.2fs) rms=%.1f" % (f, f / RATE, rms(sfx, f, RATE // 4)))

    sys.exit(0 if ok else 1)


main()
