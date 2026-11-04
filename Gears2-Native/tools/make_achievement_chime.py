"""Makes COGBox's placeholder achievement sound: assets/sounds/achievement.wav.

Two short sine notes (E6 then B6), 44.1 kHz, 16-bit mono, about 0.6 s, peak -12 dBFS. Our own sound, made from
nothing but maths, so it can ship. Replace the .wav to change the sound (see assets/sounds/README.txt).

Usage: python -I tools/make_achievement_chime.py [output.wav]
"""
import math
import struct
import sys
import wave
from pathlib import Path

RATE = 44100
PEAK = 10 ** (-12 / 20)  # -12 dBFS
NOTES = [(1318.51, 0.0), (1975.53, 0.25)]  # (Hz, start seconds): E6, B6
NOTE_SECONDS = 0.35
TOTAL_SECONDS = 0.6
ATTACK = 0.01


def sample(t: float) -> float:
    v = 0.0
    for freq, start in NOTES:
        u = t - start
        if 0.0 <= u < NOTE_SECONDS:
            env = min(1.0, u / ATTACK) * math.exp(-u * 9.0)
            v += env * (math.sin(2 * math.pi * freq * u) + 0.25 * math.sin(4 * math.pi * freq * u))
    return v


def main() -> None:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "assets" / "sounds" / "achievement.wav"
    out.parent.mkdir(parents=True, exist_ok=True)
    n = int(RATE * TOTAL_SECONDS)
    raw = [sample(i / RATE) for i in range(n)]
    top = max(abs(x) for x in raw) or 1.0
    frames = b"".join(struct.pack("<h", int(x / top * PEAK * 32767)) for x in raw)
    with wave.open(str(out), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(frames)
    print(f"wrote {out} ({n} samples)")


if __name__ == "__main__":
    main()
