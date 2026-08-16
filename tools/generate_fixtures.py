#!/usr/bin/env python3

import struct
import wave
from pathlib import Path


def main():
    destination = (
        Path(__file__).resolve().parents[1]
        / "tests"
        / "fixtures"
        / "audio"
        / "impulse-mono-pcm16-48000.wav"
    )
    destination.parent.mkdir(parents=True, exist_ok=True)

    samples = [16384] + [0] * 31
    with wave.open(str(destination), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(48000)
        output.writeframes(struct.pack("<" + "h" * len(samples), *samples))


if __name__ == "__main__":
    main()
