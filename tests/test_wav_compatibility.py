#!/usr/bin/env python3

import hashlib
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path


def read_canonical_wav(path: Path):
    contents = path.read_bytes()
    if contents[:4] != b"RIFF" or contents[8:12] != b"WAVE":
        raise AssertionError(f"{path.name} is not a RIFF/WAVE file")

    offset = 12
    format_info = None
    audio = None
    while offset + 8 <= len(contents):
        chunk_id = contents[offset : offset + 4]
        chunk_size = struct.unpack_from("<I", contents, offset + 4)[0]
        chunk = contents[offset + 8 : offset + 8 + chunk_size]
        if chunk_id == b"fmt ":
            format_info = struct.unpack_from("<HHIIHH", chunk)
        elif chunk_id == b"data":
            audio = chunk
        offset += 8 + chunk_size + chunk_size % 2

    if format_info is None or audio is None:
        raise AssertionError(f"{path.name} is missing canonical WAV chunks")

    (
        format_tag,
        channels,
        sample_rate,
        byte_rate,
        block_align,
        sample_bits,
    ) = format_info
    sample_size = sample_bits // 8
    if block_align != channels * sample_size:
        raise AssertionError(f"{path.name} has non-canonical block alignment")
    if byte_rate != sample_rate * block_align:
        raise AssertionError(f"{path.name} has non-canonical byte rate")

    if format_tag == 1 and sample_bits in (16, 24, 32):
        integer_limit = float(1 << (sample_bits - 1))
        samples = tuple(
            int.from_bytes(
                audio[offset : offset + sample_size],
                byteorder="little",
                signed=True,
            )
            / integer_limit
            for offset in range(0, len(audio), sample_size)
        )
    elif format_tag == 3 and sample_bits in (32, 64):
        sample_format = "f" if sample_bits == 32 else "d"
        samples = struct.unpack(
            "<" + sample_format * (len(audio) // sample_size),
            audio,
        )
    else:
        raise AssertionError(f"{path.name} is not a canonical supported WAV")

    return channels, sample_rate, sample_bits, samples


def selected_precision(samples, sample_bits: int):
    if sample_bits == 64:
        return samples
    return tuple(
        struct.unpack("<f", struct.pack("<f", sample))[0] for sample in samples
    )


def make_wav(format_tag: int, channels: int, sample_bits: int, audio: bytes):
    sample_rate = 48000
    bytes_per_sample = sample_bits // 8
    block_align = channels * bytes_per_sample
    fmt = struct.pack(
        "<HHIIHH",
        format_tag,
        channels,
        sample_rate,
        sample_rate * block_align,
        block_align,
        sample_bits,
    )
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(audio)) + audio
    if len(audio) % 2:
        chunks += b"\x00"
    return b"RIFF" + struct.pack("<I", len(chunks) + 4) + b"WAVE" + chunks


def require_rejection(
    renderer: Path,
    fixture: Path,
    result: Path,
    expected_error: str,
):
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
    if completed.returncode == 0:
        raise AssertionError(f"{fixture.name}: renderer unexpectedly succeeded")
    if expected_error not in completed.stderr:
        raise AssertionError(
            f"{fixture.name}: expected {expected_error!r}, got {completed.stderr!r}"
        )


def main():
    renderer = Path(sys.argv[1])
    fixture_directory = Path(sys.argv[2])
    generator = Path(sys.argv[3])
    workspace = Path(sys.argv[4])
    sample_bits = int(sys.argv[5])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    expected_names = {
        f"identity-{channel_name}-{sample_rate}-{encoding}.wav"
        for channel_name in ("mono", "stereo")
        for sample_rate in (44100, 48000, 96000)
        for encoding in ("pcm16", "pcm24", "pcm32", "float32", "float64")
    }
    manifest = json.loads((fixture_directory / "manifest.json").read_text())
    if set(manifest) != expected_names:
        raise AssertionError("committed fixture manifest does not cover the matrix")

    generated_directory = workspace / "generated"
    subprocess.run(
        [sys.executable, str(generator), str(generated_directory)],
        check=True,
    )
    for name in expected_names | {"manifest.json"}:
        if (generated_directory / name).read_bytes() != (
            fixture_directory / name
        ).read_bytes():
            raise AssertionError(f"committed fixture is stale: {name}")

    corpus = read_canonical_wav(
        fixture_directory / "identity-stereo-48000-pcm32.wav"
    )[3]
    q31_frames = tuple(
        (
            round(corpus[index] * (1 << 31)),
            round(corpus[index + 1] * (1 << 31)),
        )
        for index in range(0, len(corpus), 2)
    )
    if q31_frames[:5] != (
        (0, 0),
        (2147483647, -2147483648),
        (-2147483648, 2147483647),
        (1610612736, 0),
        (0, -1073741824),
    ):
        raise AssertionError("fixture corpus lost silence, boundaries, or impulses")
    if tuple(frame[0] for frame in q31_frames[5:13]) != (
        0,
        1518500249,
        2147483647,
        1518500249,
        0,
        -1518500249,
        -2147483648,
        -1518500249,
    ):
        raise AssertionError("fixture corpus lost its deterministic sine")
    if q31_frames[13:] != (
        (1967335287, 59474444),
        (-852468118, -1283369477),
        (635173569, 1204114206),
        (1264358700, -1635770395),
    ):
        raise AssertionError("fixture corpus lost its deterministic noise")

    for name in sorted(expected_names):
        fixture = fixture_directory / name
        fixture_metadata = manifest[name]
        if hashlib.sha256(fixture.read_bytes()).hexdigest() != fixture_metadata["sha256"]:
            raise AssertionError(f"fixture hash does not match manifest: {name}")

        result = workspace / Path(name).stem
        completed = subprocess.run(
            [
                str(renderer),
                "render",
                "--input",
                str(fixture),
                "--block-size",
                "5",
                "--output",
                str(result),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"{name}: {completed.stderr}")

        input_wav = read_canonical_wav(fixture)
        output_wav = read_canonical_wav(result / "output.wav")
        expected_output_format = (
            fixture_metadata["channels"],
            fixture_metadata["sampleRate"],
            sample_bits,
        )
        if output_wav[:3] != expected_output_format:
            raise AssertionError(f"{name}: unexpected output {output_wav[:3]}")
        if output_wav[3] != selected_precision(input_wav[3], sample_bits):
            raise AssertionError(f"{name}: identity changed decoded samples")

        metadata = json.loads((result / "render.json").read_text())
        if metadata["channels"] != fixture_metadata["channels"]:
            raise AssertionError(f"{name}: changed channel count")
        if metadata["sampleRate"] != fixture_metadata["sampleRate"]:
            raise AssertionError(f"{name}: changed sample rate")
        if metadata["frames"] != fixture_metadata["frames"]:
            raise AssertionError(f"{name}: changed frame count")

    invalid_directory = workspace / "invalid"
    invalid_directory.mkdir()
    invalid_cases = (
        ("malformed.wav", b"not a WAV file", "malformed input WAV"),
        (
            "truncated.wav",
            (
                fixture_directory / "identity-mono-48000-pcm16.wav"
            ).read_bytes()[:-1],
            "truncated input WAV",
        ),
        (
            "compressed.wav",
            make_wav(6, 1, 8, b"\x00"),
            "unsupported WAV encoding",
        ),
        (
            "non-finite.wav",
            make_wav(3, 1, 32, struct.pack("<f", float("nan"))),
            "non-finite input sample",
        ),
        (
            "zero-channel.wav",
            make_wav(1, 0, 16, b""),
            "WAV channel count must be mono or stereo",
        ),
        (
            "three-channel.wav",
            make_wav(1, 3, 16, b"\x00" * 6),
            "WAV channel count must be mono or stereo",
        ),
    )
    for filename, contents, expected_error in invalid_cases:
        invalid_fixture = invalid_directory / filename
        invalid_fixture.write_bytes(contents)
        require_rejection(
            renderer,
            invalid_fixture,
            invalid_directory / f"{filename}-result",
            expected_error,
        )


if __name__ == "__main__":
    main()
