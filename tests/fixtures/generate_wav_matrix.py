#!/usr/bin/env python3

import hashlib
import json
import struct
import sys
from pathlib import Path


SAMPLE_RATES = (44100, 48000, 96000)
CHANNEL_COUNTS = (1, 2)
ENCODINGS = {
    "pcm16": (1, 16),
    "pcm24": (1, 24),
    "pcm32": (1, 32),
    "float32": (3, 32),
    "float64": (3, 64),
}
Q31_MAX = (1 << 31) - 1
Q31_MIN = -(1 << 31)


def deterministic_noise(seed: int, count: int):
    values = []
    state = seed
    for _ in range(count):
        state = (1664525 * state + 1013904223) & 0xFFFFFFFF
        values.append(state - (1 << 32) if state >= (1 << 31) else state)
    return values


def channel_samples(channel: int):
    sine = (0, 1518500249, Q31_MAX, 1518500249, 0, -1518500249, Q31_MIN, -1518500249)
    if channel == 0:
        return (
            0,
            Q31_MAX,
            Q31_MIN,
            1610612736,
            0,
            *sine,
            *deterministic_noise(0x12345678, 4),
        )
    return (
        0,
        Q31_MIN,
        Q31_MAX,
        0,
        -1073741824,
        *sine[2:],
        *sine[:2],
        *deterministic_noise(0x87654321, 4),
    )


def interleaved_samples(channels: int):
    per_channel = [channel_samples(channel) for channel in range(channels)]
    return tuple(
        per_channel[channel][frame]
        for frame in range(len(per_channel[0]))
        for channel in range(channels)
    )


def encode_sample(sample: int, encoding: str):
    if encoding == "pcm16":
        return struct.pack("<h", sample >> 16)
    if encoding == "pcm24":
        return (sample >> 8).to_bytes(3, byteorder="little", signed=True)
    if encoding == "pcm32":
        return struct.pack("<i", sample)

    normalized = 1.0 if sample == Q31_MAX else sample / float(1 << 31)
    if encoding == "float32":
        return struct.pack("<f", normalized)
    return struct.pack("<d", normalized)


def make_wav(sample_rate: int, channels: int, encoding: str):
    format_tag, bits_per_sample = ENCODINGS[encoding]
    bytes_per_sample = bits_per_sample // 8
    block_align = channels * bytes_per_sample
    byte_rate = sample_rate * block_align
    samples = interleaved_samples(channels)
    audio = b"".join(encode_sample(sample, encoding) for sample in samples)
    fmt = struct.pack(
        "<HHIIHH",
        format_tag,
        channels,
        sample_rate,
        byte_rate,
        block_align,
        bits_per_sample,
    )
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(audio)) + audio
    if len(audio) % 2:
        chunks += b"\x00"
    return b"RIFF" + struct.pack("<I", len(chunks) + 4) + b"WAVE" + chunks


def main():
    if len(sys.argv) > 2:
        raise SystemExit("usage: generate_wav_matrix.py [output-directory]")
    output_directory = (
        Path(sys.argv[1])
        if len(sys.argv) == 2
        else Path(__file__).parent / "audio" / "matrix"
    )
    output_directory.mkdir(parents=True, exist_ok=True)
    manifest = {}

    for channels in CHANNEL_COUNTS:
        channel_name = "mono" if channels == 1 else "stereo"
        for sample_rate in SAMPLE_RATES:
            for encoding in ENCODINGS:
                filename = f"identity-{channel_name}-{sample_rate}-{encoding}.wav"
                contents = make_wav(sample_rate, channels, encoding)
                (output_directory / filename).write_bytes(contents)
                manifest[filename] = {
                    "channels": channels,
                    "encoding": encoding,
                    "frames": 17,
                    "sampleRate": sample_rate,
                    "sha256": hashlib.sha256(contents).hexdigest(),
                }

    (output_directory / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )


if __name__ == "__main__":
    main()
