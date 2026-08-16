#!/usr/bin/env python3

import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path


def read_float_wav(path: Path):
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise AssertionError("output is not a RIFF/WAVE file")

    offset = 12
    audio_format = channels = sample_rate = bits_per_sample = None
    samples = None
    while offset + 8 <= len(data):
        chunk_id = data[offset : offset + 4]
        chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
        chunk = data[offset + 8 : offset + 8 + chunk_size]
        if chunk_id == b"fmt ":
            audio_format, channels, sample_rate = struct.unpack_from("<HHI", chunk)
            bits_per_sample = struct.unpack_from("<H", chunk, 14)[0]
        elif chunk_id == b"data":
            samples = struct.unpack("<" + "f" * (chunk_size // 4), chunk)
        offset += 8 + chunk_size + (chunk_size % 2)

    return audio_format, channels, sample_rate, bits_per_sample, samples


def main():
    renderer = Path(sys.argv[1])
    fixture = Path(sys.argv[2])
    result = Path(sys.argv[3])
    shutil.rmtree(result, ignore_errors=True)

    completed = subprocess.run(
        [
            str(renderer),
            "render",
            "--input",
            str(fixture),
            "--output",
            str(result),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)

    expected_files = {"output.wav", "resolved.json", "render.json"}
    if {path.name for path in result.iterdir()} != expected_files:
        raise AssertionError("Render Result does not contain the expected evidence")

    wav = read_float_wav(result / "output.wav")
    if wav[:4] != (3, 1, 48000, 32):
        raise AssertionError(f"unexpected output WAV format: {wav[:4]}")
    if wav[4] != (0.5,) + (0.0,) * 31:
        raise AssertionError("identity render changed decoded samples")

    resolved = json.loads((result / "resolved.json").read_text())
    if resolved != {
        "formatVersion": 1,
        "seed": 0,
        "sampleRate": 48000,
        "composition": {"stages": []},
    }:
        raise AssertionError(f"unexpected Resolved Configuration: {resolved}")

    render = json.loads((result / "render.json").read_text())
    expected_render = {
        "formatVersion": 1,
        "rendererVersion": "0.1.0",
        "samplePrecision": "float32",
        "sampleRate": 48000,
        "channels": 1,
        "frames": 32,
        "blockSize": 512,
    }
    if render != expected_render:
        raise AssertionError(f"unexpected render metadata: {render}")


if __name__ == "__main__":
    main()
