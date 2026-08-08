#!/usr/bin/env python3
"""Find and timestamp individual pops in a recording.

    tools/find_pops.py <file.wav> [more.wav ...]

Averaged metrics can't see a pop — they smear it into a mean. This looks for the
thing a pop actually is: a shift in the local mean that PERSISTS. A square wave's
own edges move the signal violently but leave the mean over several cycles alone,
which is why "biggest sample step" flags every note and finds nothing.

The discriminator is centre versus amplitude. A pop moves the waveform's centre
while the oscillation carries on unchanged; a note change moves the centre AND
the amplitude. So a candidate needs all three:

  * the centre shifts, measured against the wave's own height  (a level shift)
  * the amplitude does NOT change much                         (not a new note)
  * the shift is still there 100 ms later                       (it persists)

A count on its own proves nothing — run a reference emulator through it too.

Every reported event carries a timestamp, so it can be cut out and listened to.
Run it on a reference emulator too: the count only means something compared with
something else.

Stdlib only — no ffmpeg, no numpy.
"""
import array
import math
import sys
import wave

RATE = 48000
W = 960        # 20 ms: averages out anything at or above ~100 Hz
PERSIST = 3    # the shift must survive 3 windows (60 ms)
HOP = 96       # test every 2 ms


def read(path):
    with wave.open(path) as w:
        rate = w.getframerate()
        ch = w.getnchannels()
        a = array.array("h")
        a.frombytes(w.readframes(w.getnframes()))
    return [v / 32768.0 for v in (a[0::ch] if ch > 1 else a)], rate


def centre_amp(x, a, b):
    """Midpoint and half-height of the window — the wave's centre and its size."""
    lo = hi = x[a]
    for i in range(a, b):
        v = x[i]
        if v < lo:
            lo = v
        elif v > hi:
            hi = v
    return (hi + lo) / 2.0, (hi - lo) / 2.0


def find_pops(x, rate):
    m = sum(x) / len(x)
    rms = math.sqrt(sum((v - m) ** 2 for v in x) / len(x)) or 1e-9

    events = []
    t = W
    while t + W * (PERSIST + 1) < len(x):
        cb, ab = centre_amp(x, t - W, t)
        ca, aa = centre_amp(x, t, t + W)
        step = ca - cb
        # Floored: dividing by a near-silent window's amplitude reports nonsense.
        scale = max(ab, aa, 0.1 * rms)
        # Against the wave's own height, not full scale: a 0.01 shift is nothing
        # under a loud passage and a bang under a quiet one.
        if abs(step) > 0.35 * scale and abs(aa - ab) < 0.5 * abs(step):
            cl, _al = centre_amp(x, t + W * PERSIST, t + W * (PERSIST + 1))
            if abs(cl - cb) > 0.5 * abs(step):
                # A pop lands in ~1 sample; a drift of the same size has no edge.
                edge = 0.0
                for i in range(max(1, t - 48), min(len(x), t + 48)):
                    d = abs(x[i] - x[i - 1])
                    if d > edge:
                        edge = d
                if edge > 0.5 * abs(step):
                    events.append((abs(step) / scale, t, step, edge))
        t += HOP

    events.sort(key=lambda e: e[1])
    merged = []
    for e in events:
        if merged and e[1] - merged[-1][1] < W * 2:
            if e[0] > merged[-1][0]:
                merged[-1] = e
        else:
            merged.append(e)
    merged.sort(key=lambda e: -e[0])
    return merged, rms


def main(paths):
    for path in paths:
        x, rate = read(path)
        if rate != RATE:
            print(f"{path}: {rate} Hz, expected {RATE}")
            continue
        pops, rms = find_pops(x, rate)
        name = path.split("/")[-1]
        print(f"\n{name}")
        print(f"  passage RMS {rms:.5f} · {len(pops)} centre shifts without a note change")
        for rel, t, step, edge in pops[:12]:
            print(f"    {t / rate:7.3f}s  centre {step:+.5f}  "
                  f"({rel * 100:4.0f}% of wave height)  sharpest edge {edge:.4f}")
        if len(pops) > 12:
            print(f"    … {len(pops) - 12} more")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1:])
