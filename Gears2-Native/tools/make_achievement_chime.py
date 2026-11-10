"""Makes COGBox's achievement sound: assets/sounds/achievement.wav.

A bell chime made from nothing but maths (0.4.9, replaces the ElevenLabs sound): three rising bell notes (G5, D6, G6)
then a soft high shimmer. Each note is a few inharmonic bell partials with exponential decay. The format matches the
sound it replaces: 48 kHz, 16-bit stereo, 80640 frames (1.68 s), RMS about -26 dBFS like the old sound (peak -10 dBFS). Deterministic: the same file every run.

Licence: part of COGBox, MIT (see LICENSE). Replace the .wav to change the sound (see assets/sounds/README.txt).

Usage: python -I tools/make_achievement_chime.py [output.wav]
"""
import math
import struct
import sys
import wave
from pathlib import Path

RATE = 48000
FRAMES = 80640  # 1.68 s
PEAK = 10 ** (-10 / 20)  # -10 dBFS: same loudness (RMS about -26 dBFS) as the sound it replaces
ATTACK = 0.004
# (frequency Hz, start s, gain, decay per second, pan -1..1)
NOTES = [
    (783.99, 0.00, 1.00, 5.0, -0.25),   # G5
    (1174.66, 0.11, 0.90, 5.0, 0.0),    # D6
    (1567.98, 0.22, 1.00, 3.2, 0.25),   # G6, held longest
    (3135.96, 0.34, 0.18, 4.0, 0.0),    # G7 shimmer
]
# bell partials: (ratio to the note, relative gain, extra decay factor)
PARTIALS = [(1.0, 1.0, 1.0), (2.0, 0.35, 1.6), (2.76, 0.18, 2.2), (5.40, 0.07, 3.5)]


def note(t: float, freq: float, gain: float, decay: float) -> float:
    if t < 0.0:
        return 0.0
    env = min(1.0, t / ATTACK)
    v = 0.0
    for ratio, g, dk in PARTIALS:
        v += g * math.exp(-t * decay * dk) * math.sin(2 * math.pi * freq * ratio * t)
    return gain * env * v


def main() -> None:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "assets" / "sounds" / "achievement.wav"
    out.parent.mkdir(parents=True, exist_ok=True)
    left, right = [], []
    for i in range(FRAMES):
        t = i / RATE
        l = r = 0.0
        for freq, start, gain, decay, pan in NOTES:
            v = note(t - start, freq, gain, decay)
            l += v * math.sqrt((1 - pan) / 2)
            r += v * math.sqrt((1 + pan) / 2)
        fade = min(1.0, (FRAMES - i) / (0.05 * RATE))  # last 50 ms fade to silence
        left.append(l * fade)
        right.append(r * fade)
    top = max(max(abs(x) for x in left), max(abs(x) for x in right)) or 1.0
    k = PEAK * 32767 / top
    frames = b"".join(struct.pack("<hh", int(round(a * k)), int(round(b * k))) for a, b in zip(left, right))
    with wave.open(str(out), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(frames)
    print(f"wrote {out} ({FRAMES} frames)")


if __name__ == "__main__":
    main()
