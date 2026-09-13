#!/usr/bin/env python3

import struct
import wave
from pathlib import Path


def write_mono_impulse(destination: Path):
    samples = [16384] + [0] * 31
    with wave.open(str(destination), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(48000)
        output.writeframes(struct.pack("<" + "h" * len(samples), *samples))


def write_stereo_impulse(destination: Path, *, left: int, right: int):
    frames = [(left, right)] + [(0, 0)] * 31
    with wave.open(str(destination), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(48000)
        output.writeframes(
            struct.pack(
                "<" + "h" * (len(frames) * 2),
                *(sample for frame in frames for sample in frame),
            )
        )


def main():
    audio_directory = (
        Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "audio"
    )
    audio_directory.mkdir(parents=True, exist_ok=True)

    write_mono_impulse(audio_directory / "impulse-mono-pcm16-48000.wav")
    # Left-only and right-only stereo impulses characterize both source
    # dimensions independently, so a Split strategy that preserves stereo
    # position (stereo-halves, stereo-interleave) can be told apart from
    # one that discards it (duplicate).
    write_stereo_impulse(
        audio_directory / "impulse-stereo-left-pcm16-48000.wav",
        left=16384,
        right=0,
    )
    write_stereo_impulse(
        audio_directory / "impulse-stereo-right-pcm16-48000.wav",
        left=0,
        right=16384,
    )


if __name__ == "__main__":
    main()
