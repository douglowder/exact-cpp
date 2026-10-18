#!/usr/bin/env python3
"""Writes the metronome's two clicks: short decaying sine bursts as 16-bit mono WAV.

  make_clicks.py OUT_DIR

click-high.wav (beat 1, accented) and click-low.wav (the other beats). Synthesized here so
their source is this script; the same bytes come out on every run.
"""

import math
import struct
import sys
import wave
from pathlib import Path

RATE = 22050
LENGTH_S = 0.03
ATTACK_S = 0.001
DECAY_S = 0.006

CLICKS = {
    "click-high.wav": (2000.0, 0.9),
    "click-low.wav": (1200.0, 0.7),
}


def samples(freq: float, level: float) -> bytes:
    """One click's 16-bit samples: a sine with a 1 ms attack and an exponential decay."""
    out = bytearray()
    for i in range(round(RATE * LENGTH_S)):
        t = i / RATE
        envelope = min(1.0, t / ATTACK_S) * math.exp(-t / DECAY_S)
        value = level * envelope * math.sin(2 * math.pi * freq * t)
        out += struct.pack("<h", round(value * 32767))
    return bytes(out)


def main() -> int:
    """Writes both clicks into the directory named on the command line."""
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[2], file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, (freq, level) in CLICKS.items():
        with wave.open(str(out / name), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(samples(freq, level))
        print(f"{out / name}: {freq:.0f} Hz, {LENGTH_S * 1000:.0f} ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
