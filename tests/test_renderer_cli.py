#!/usr/bin/env python3

import hashlib
import json
import platform
import shutil
import struct
import subprocess
import sys
import wave
from pathlib import Path


def read_float_wav(path: Path):
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise AssertionError("output is not a RIFF/WAVE file")

    offset = 12
    audio_format = channels = sample_rate = bits_per_sample = None
    data_chunk = None
    while offset + 8 <= len(data):
        chunk_id = data[offset : offset + 4]
        chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
        chunk = data[offset + 8 : offset + 8 + chunk_size]
        if chunk_id == b"fmt ":
            audio_format, channels, sample_rate = struct.unpack_from("<HHI", chunk)
            bits_per_sample = struct.unpack_from("<H", chunk, 14)[0]
        elif chunk_id == b"data":
            data_chunk = chunk
        offset += 8 + chunk_size + (chunk_size % 2)

    if data_chunk is None:
        raise AssertionError("output WAV is missing a data chunk")

    if bits_per_sample == 32:
        samples = struct.unpack("<" + "f" * (len(data_chunk) // 4), data_chunk)
    elif bits_per_sample == 64:
        samples = struct.unpack("<" + "d" * (len(data_chunk) // 8), data_chunk)
    else:
        raise AssertionError(f"unexpected bits per sample: {bits_per_sample}")

    return audio_format, channels, sample_rate, bits_per_sample, samples


def write_pcm16_wav(path: Path, sample_rate: int, channels: int, samples):
    with wave.open(str(path), "wb") as output:
        output.setnchannels(channels)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(struct.pack("<" + "h" * len(samples), *samples))


def run_renderer(renderer: Path, fixture: Path, result: Path, block_size=None):
    shutil.rmtree(result, ignore_errors=True)
    arguments = [
        str(renderer),
        "render",
        "--input",
        str(fixture),
    ]
    if block_size is not None:
        arguments.extend(("--block-size", str(block_size)))
    arguments.extend(("--output", str(result)))

    completed = subprocess.run(
        arguments,
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)


def main():
    renderer = Path(sys.argv[1])
    fixture = Path(sys.argv[2])
    result = Path(sys.argv[3])
    sample_bits = int(sys.argv[4])
    if sample_bits not in (32, 64):
        raise AssertionError(f"unexpected configured sample bits: {sample_bits}")
    sample_precision = f"float{sample_bits}"
    run_renderer(renderer, fixture, result)

    expected_files = {"output.wav", "resolved.json", "render.json"}
    if {path.name for path in result.iterdir()} != expected_files:
        raise AssertionError("Render Result does not contain the expected evidence")

    wav = read_float_wav(result / "output.wav")
    if wav[:4] != (3, 1, 48000, sample_bits):
        raise AssertionError(f"unexpected output WAV format: {wav[:4]}")
    if wav[4] != (0.5,) + (0.0,) * 31:
        raise AssertionError("identity render changed decoded samples")

    mono_block_result = result.parent / "mono-result-3"
    run_renderer(renderer, fixture, mono_block_result, block_size=3)
    if read_float_wav(mono_block_result / "output.wav")[4] != wav[4]:
        raise AssertionError("block size changed decoded mono identity output")

    resolved = json.loads((result / "resolved.json").read_text())
    if resolved != {
        "formatVersion": 1,
        "seed": 0,
        "sampleRate": 48000,
        "composition": {"stages": []},
    }:
        raise AssertionError(f"unexpected Resolved Configuration: {resolved}")

    render = json.loads((result / "render.json").read_text())
    machine = platform.machine().lower()
    expected_render = {
        "architecture": {
            "amd64": "x86_64",
            "x86_64": "x86_64",
            "arm64": "arm64",
            "aarch64": "arm64",
        }.get(machine, machine),
        "formatVersion": 1,
        "rendererVersion": "0.1.0",
        "platform": {
            "Darwin": "macos",
            "Windows": "windows",
            "Linux": "linux",
        }.get(platform.system(), platform.system().lower()),
        "samplePrecision": sample_precision,
        "configurationInput": "defaults",
        "inputFilename": fixture.name,
        "inputSha256": hashlib.sha256(fixture.read_bytes()).hexdigest(),
        "sampleRate": 48000,
        "channels": 1,
        "frames": 32,
        "inputFrames": 32,
        "blockSize": 512,
    }
    if render != expected_render:
        raise AssertionError(f"unexpected render metadata: {render}")

    stereo_samples = (
        16384,
        -16384,
        0,
        32767,
        -32768,
        0,
        8192,
        -8192,
        1,
        -1,
        12345,
        -23456,
        0,
        0,
    )
    stereo_fixture = result.parent / "odd-stereo-pcm16-44100.wav"
    write_pcm16_wav(stereo_fixture, 44100, 2, stereo_samples)
    expected_stereo = tuple(sample / 32768.0 for sample in stereo_samples)
    decoded_outputs = []

    for block_size in (2, 8):
        stereo_result = result.parent / f"stereo-result-{block_size}"
        run_renderer(
            renderer,
            stereo_fixture,
            stereo_result,
            block_size=block_size,
        )

        stereo_wav = read_float_wav(stereo_result / "output.wav")
        if stereo_wav[:4] != (3, 2, 44100, sample_bits):
            raise AssertionError(
                f"unexpected stereo output WAV format: {stereo_wav[:4]}"
            )
        if stereo_wav[4] != expected_stereo:
            raise AssertionError("stereo identity render changed decoded samples")
        decoded_outputs.append(stereo_wav[4])

        stereo_render = json.loads((stereo_result / "render.json").read_text())
        if stereo_render["channels"] != 2:
            raise AssertionError("Render Result did not preserve channel count")
        if stereo_render["sampleRate"] != 44100:
            raise AssertionError("Render Result did not preserve sample rate")
        if stereo_render["frames"] != 7:
            raise AssertionError("Render Result did not preserve odd frame count")
        if stereo_render["inputFrames"] != 7:
            raise AssertionError("Render Result did not record input frame count")
        if stereo_render["blockSize"] != block_size:
            raise AssertionError("Render Result did not record the block size")

    if decoded_outputs[0] != decoded_outputs[1]:
        raise AssertionError("block size changed decoded identity output")


if __name__ == "__main__":
    main()
