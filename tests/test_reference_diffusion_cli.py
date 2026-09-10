#!/usr/bin/env python3

import hashlib
import json
import math
import shutil
import struct
import subprocess
import sys
from pathlib import Path


def read_float_wav(path: Path):
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise AssertionError(f"{path} is not a RIFF/WAVE file")
    offset = 12
    channels = sample_rate = bits = None
    samples = None
    while offset + 8 <= len(data):
        chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
        chunk = data[offset + 8 : offset + 8 + chunk_size]
        if data[offset : offset + 4] == b"fmt ":
            _, channels, sample_rate = struct.unpack_from("<HHI", chunk)
            bits = struct.unpack_from("<H", chunk, 14)[0]
        elif data[offset : offset + 4] == b"data":
            sample_format = "f" if bits == 32 else "d"
            samples = struct.unpack(
                "<" + sample_format * (len(chunk) // (bits // 8)), chunk
            )
        offset += 8 + chunk_size + chunk_size % 2
    if samples is None:
        raise AssertionError(f"{path} has no audio data")
    return channels, sample_rate, bits, samples


def run(renderer: Path, *arguments):
    completed = subprocess.run(
        [str(renderer), "render", *map(str, arguments)],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)


def main():
    renderer = Path(sys.argv[1])
    fixture = Path(sys.argv[2])
    workspace = Path(sys.argv[3])
    sample_bits = int(sys.argv[4])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    request = workspace / "request.json"
    request.write_text(
        json.dumps(
            {
                "formatVersion": 2,
                "seed": 42,
                "composition": {
                    "stages": [
                        {
                            "type": "split",
                            "channels": 8,
                            "strategy": "duplicate",
                            "normalisation": "energy",
                        },
                        {
                            "type": "diffuser",
                            "steps": 1,
                            "totalMs": 1,
                            "distribution": "even",
                            "step": {
                                "delayStrategy": "segmented-random",
                                "mix": "hadamard",
                                "shuffle": True,
                                "polarity": "seeded-random",
                            },
                        },
                        {"type": "downmix", "strategy": "select", "leftChannel": 0, "rightChannel": 1},
                    ]
                },
            },
            indent=2,
        )
        + "\n"
    )

    result = workspace / "result"
    run(
        renderer,
        "--input",
        fixture,
        "--config",
        request,
        "--block-size",
        7,
        "--output",
        result,
    )
    channels, sample_rate, bits, samples = read_float_wav(
        result / "output.wav"
    )
    if (channels, sample_rate, bits) != (2, 48000, sample_bits):
        raise AssertionError("reference Diffuser did not emit canonical stereo")
    if len(samples) != 80 * 2:
        raise AssertionError("renderer did not drain the full finite response")

    expected_arrivals = {
        5: (-0.125, -0.125),
        8: (0.125, 0.125),
        16: (0.125, 0.125),
        22: (0.125, -0.125),
        27: (0.125, -0.125),
        32: (0.125, -0.125),
        41: (-0.125, 0.125),
        44: (-0.125, -0.125),
    }
    for frame in range(80):
        actual = samples[frame * 2 : frame * 2 + 2]
        expected = expected_arrivals.get(frame, (0.0, 0.0))
        if any(abs(a - e) > 1e-6 for a, e in zip(actual, expected)):
            raise AssertionError(
                f"unexpected wet output at frame {frame}: {actual}"
            )

    metadata = json.loads((result / "render.json").read_text())
    if metadata["inputFrames"] != 32 or metadata["frames"] != 80:
        raise AssertionError(f"unexpected finite frame counts: {metadata}")
    if metadata["channels"] != 2:
        raise AssertionError("render metadata did not record stereo output")
    if "stageCaptures" in metadata or "stageCaptureProfile" in metadata:
        raise AssertionError("ordinary render unexpectedly published captures")

    captured = workspace / "captured"
    run(
        renderer,
        "--input",
        fixture,
        "--config",
        request,
        "--block-size",
        7,
        "--capture-stages",
        "all",
        "--output",
        captured,
    )
    if (captured / "output.wav").read_bytes() != (
        result / "output.wav"
    ).read_bytes():
        raise AssertionError("Stage capture changed output.wav")

    captured_metadata = json.loads((captured / "render.json").read_text())
    if captured_metadata["stageCaptureProfile"] != "all-v1":
        raise AssertionError("render metadata did not record capture profile")
    # No Early Reflections branch is configured, so only the N-Channel
    # Split/Diffusion-Step captures and the always-present Main-stereo
    # capture (issue #113) are published -- no early-stereo entry.
    expected_boundaries = ["split", "diffusion-step", "main-stereo"]
    captures = captured_metadata["stageCaptures"]
    if [capture["boundary"] for capture in captures] != expected_boundaries:
        raise AssertionError(f"unexpected capture manifest: {captures}")
    if [capture["path"] for capture in captures] != [
        "captures/00-split.wav",
        "captures/01-diffusion-step-0.wav",
        "captures/02-main-stereo.wav",
    ]:
        raise AssertionError(f"unstable capture paths: {captures}")

    for capture in captures:
        path = captured / capture["path"]
        if hashlib.sha256(path.read_bytes()).hexdigest() != capture["sha256"]:
            raise AssertionError("Stage capture SHA-256 does not match manifest")
        capture_channels, rate, capture_bits, captured_samples = read_float_wav(
            path
        )
        expected_channels = 2 if capture["boundary"] == "main-stereo" else 8
        if (capture_channels, rate, capture_bits) != (
            expected_channels,
            48000,
            sample_bits,
        ):
            raise AssertionError("Stage capture is not canonical N-Channel WAV")
        if len(captured_samples) != 80 * expected_channels:
            raise AssertionError("Stage capture does not share output timeline")
        if capture["channels"] != expected_channels or capture["frames"] != 80:
            raise AssertionError("capture manifest audio facts are wrong")
        if capture["disabled"]:
            raise AssertionError("an enabled branch's capture was manifested disabled")

    split_samples = read_float_wav(
        captured / "captures" / "00-split.wav"
    )[3]
    expected_split = 0.5 / math.sqrt(8)
    if any(abs(value - expected_split) > 1e-6 for value in split_samples[:8]):
        raise AssertionError("captured Split did not use energy normalization")
    if any(value != 0.0 for value in split_samples[8:]):
        raise AssertionError("captured Split contains unexpected tail energy")

    diffusion_samples = read_float_wav(
        captured / "captures" / "01-diffusion-step-0.wav"
    )[3]
    energy = math.fsum(value * value for value in diffusion_samples)
    if abs(energy - 0.25) > 1e-5:
        raise AssertionError(
            f"captured Diffusion Step did not preserve energy: {energy}"
        )

    rerendered = workspace / "resolved-captured"
    run(
        renderer,
        "--input",
        fixture,
        "--resolved",
        captured / "resolved.json",
        "--block-size",
        7,
        "--capture-stages",
        "all",
        "--output",
        rerendered,
    )
    for relative in (
        "output.wav",
        "resolved.json",
        "captures/00-split.wav",
        "captures/01-diffusion-step-0.wav",
    ):
        if (rerendered / relative).read_bytes() != (
            captured / relative
        ).read_bytes():
            raise AssertionError(
                f"resolved rerender changed deterministic evidence: {relative}"
            )


if __name__ == "__main__":
    main()
