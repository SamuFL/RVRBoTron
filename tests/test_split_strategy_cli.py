#!/usr/bin/env python3

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
    channels = bits = None
    samples = None
    while offset + 8 <= len(data):
        chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
        chunk = data[offset + 8 : offset + 8 + chunk_size]
        if data[offset : offset + 4] == b"fmt ":
            _, channels, _ = struct.unpack_from("<HHI", chunk)
            bits = struct.unpack_from("<H", chunk, 14)[0]
        elif data[offset : offset + 4] == b"data":
            sample_format = "f" if bits == 32 else "d"
            samples = struct.unpack(
                "<" + sample_format * (len(chunk) // (bits // 8)), chunk
            )
        offset += 8 + chunk_size + chunk_size % 2
    if samples is None:
        raise AssertionError(f"{path} has no audio data")
    return channels, samples


def run_renderer(renderer: Path, *arguments):
    return subprocess.run(
        [str(renderer), "render", *map(str, arguments)],
        check=False,
        capture_output=True,
        text=True,
    )


def require_success(completed):
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)


def require_failure(completed, expected_error: str, output: Path):
    if completed.returncode == 0:
        raise AssertionError("renderer unexpectedly succeeded")
    if expected_error not in completed.stderr:
        raise AssertionError(
            f"expected error {expected_error!r}, got {completed.stderr!r}"
        )
    if output.exists():
        raise AssertionError("configuration failure created a Render Result")


def stereo_request(
    strategy: str, channels: int = 4, normalisation: str = "energy"
):
    return {
        "formatVersion": 1,
        "seed": 1,
        "composition": {
            "stages": [
                {
                    "type": "split",
                    "channels": channels,
                    "strategy": strategy,
                    "normalisation": normalisation,
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
                {"type": "downmix", "strategy": "select"},
            ]
        },
    }


def render(renderer: Path, fixture: Path, request_path: Path, output: Path):
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            request_path,
            "--capture-stages",
            "all",
            "--output",
            output,
        )
    )


def analyze_diffusion(analyzer: Path, result: Path, source: Path):
    completed = subprocess.run(
        [
            sys.executable,
            str(analyzer),
            str(result),
            "--source",
            str(source),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(
            f"analyze_diffusion.py failed: {completed.stderr}"
        )
    return json.loads((result / "analysis" / "diffusion-v1.json").read_text())


def captured_split_first_frame(result: Path, channels: int):
    resolved = json.loads((result / "resolved.json").read_text())
    manifest = json.loads((result / "render.json").read_text())
    for capture in manifest["stageCaptures"]:
        if capture["boundary"] == "split":
            split_channels, samples = read_float_wav(result / capture["path"])
            if split_channels != channels:
                raise AssertionError("Split capture channel count is wrong")
            return resolved["composition"]["stages"][0], samples[:channels]
    raise AssertionError("Stage capture manifest is missing Split")


def main():
    renderer = Path(sys.argv[1])
    analyzer = Path(sys.argv[2])
    left_fixture = Path(sys.argv[3])
    right_fixture = Path(sys.argv[4])
    mono_fixture = Path(sys.argv[5])
    workspace = Path(sys.argv[6])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    channel_gain = math.sqrt(2.0 / 4.0)
    impulse_amplitude = 16384 / 32768.0

    # `pattern` records, per Channel, which source Channel ("L" or "R") it
    # is fed from; both the energy-normalized and diagnostic "none" gains
    # are derived from the same shape so each strategy's mapping is
    # expressed once.
    for strategy, pattern in (
        ("stereo-halves", ("L", "L", "R", "R")),
        ("stereo-interleave", ("L", "R", "L", "R")),
    ):
        left_frame = tuple(
            channel_gain * impulse_amplitude if side == "L" else 0.0
            for side in pattern
        )
        right_frame = tuple(
            channel_gain * impulse_amplitude if side == "R" else 0.0
            for side in pattern
        )
        none_frame = tuple(
            impulse_amplitude if side == "L" else 0.0 for side in pattern
        )

        request_path = workspace / f"{strategy}-request.json"
        request_path.write_text(json.dumps(stereo_request(strategy)))

        left_result = workspace / f"{strategy}-left-result"
        render(renderer, left_fixture, request_path, left_result)
        split, left_samples = captured_split_first_frame(left_result, 4)
        if split["strategy"] != strategy:
            raise AssertionError(f"resolved Split strategy is wrong: {split}")
        if split["sourceGain"] != 1.0:
            raise AssertionError(
                f"stereo-preserving Split should not scale by sourceGain: {split}"
            )
        if abs(split["channelGain"] - channel_gain) > 1e-12:
            raise AssertionError(f"unexpected Split channelGain: {split}")
        if any(
            abs(actual - expected) > 1e-6
            for actual, expected in zip(left_samples, left_frame)
        ):
            raise AssertionError(
                f"{strategy} left-only Split mapping is wrong: {left_samples}"
            )

        # analyze_diffusion.py's energy report must reflect the
        # strategy-selected Split energy accurately, not just the raw
        # capture samples asserted on above.
        diffusion_analysis = analyze_diffusion(analyzer, left_result, left_fixture)
        split_energy = diffusion_analysis["energy"]["split"]
        if split_energy["channelCount"] != 4:
            raise AssertionError(
                f"{strategy} diffusion analysis Channel count is wrong: "
                f"{split_energy}"
            )
        if abs(split_energy["sumOfSquares"] - impulse_amplitude**2) > 1e-6:
            raise AssertionError(
                f"{strategy} diffusion analysis did not report energy-"
                f"preserving Split energy: {split_energy}"
            )

        right_result = workspace / f"{strategy}-right-result"
        render(renderer, right_fixture, request_path, right_result)
        _, right_samples = captured_split_first_frame(right_result, 4)
        if any(
            abs(actual - expected) > 1e-6
            for actual, expected in zip(right_samples, right_frame)
        ):
            raise AssertionError(
                f"{strategy} right-only Split mapping is wrong: {right_samples}"
            )

        # The diagnostic "none" normalisation is the unnormalized ablation:
        # Channel gain stays 1.0 regardless of N, so Split energy scales
        # with the raw sample amplitude instead of being held constant.
        none_request_path = workspace / f"{strategy}-none-request.json"
        none_request_path.write_text(
            json.dumps(stereo_request(strategy, normalisation="none"))
        )
        none_result = workspace / f"{strategy}-none-result"
        render(renderer, left_fixture, none_request_path, none_result)
        none_split, none_samples = captured_split_first_frame(none_result, 4)
        if none_split["channelGain"] != 1.0:
            raise AssertionError(
                f"unnormalized {strategy} Split channelGain is wrong: "
                f"{none_split}"
            )
        if any(
            abs(actual - expected) > 1e-6
            for actual, expected in zip(none_samples, none_frame)
        ):
            raise AssertionError(
                f"unnormalized {strategy} Split mapping is wrong: "
                f"{none_samples}"
            )

    # Mono input has no left/right dimension, so every requested strategy
    # resolves to the same mono duplication mapping as "duplicate".
    mono_request_path = workspace / "mono-stereo-halves-request.json"
    mono_request_path.write_text(json.dumps(stereo_request("stereo-halves")))
    mono_result = workspace / "mono-stereo-halves-result"
    render(renderer, mono_fixture, mono_request_path, mono_result)
    mono_split, mono_samples = captured_split_first_frame(mono_result, 4)
    expected_mono_gain = 1.0 / math.sqrt(4.0)
    if mono_split["sourceGain"] != 1.0:
        raise AssertionError(f"mono Split source gain is wrong: {mono_split}")
    if abs(mono_split["channelGain"] - expected_mono_gain) > 1e-12:
        raise AssertionError(f"mono Split channel gain is wrong: {mono_split}")
    if any(abs(sample - mono_samples[0]) > 1e-9 for sample in mono_samples):
        raise AssertionError(
            f"mono input did not duplicate uniformly: {mono_samples}"
        )

    # An odd Channel count is incompatible with stereo-halves/interleave when
    # the source is stereo; the error must name the Split stage precisely.
    odd_request = stereo_request("stereo-halves", channels=3)
    odd_request_path = workspace / "odd-request.json"
    odd_request_path.write_text(json.dumps(odd_request))
    odd_output = workspace / "odd-result"
    require_failure(
        run_renderer(
            renderer,
            "--input",
            left_fixture,
            "--config",
            odd_request_path,
            "--output",
            odd_output,
        ),
        "invalid_configuration at /composition/stages/0/channels: "
        "stereo-halves and stereo-interleave require an even Channel count "
        "for stereo input",
        odd_output,
    )

    unknown_strategy_request = stereo_request("stereo-halves")
    unknown_strategy_request["composition"]["stages"][0]["strategy"] = "unknown"
    unknown_strategy_path = workspace / "unknown-strategy-request.json"
    unknown_strategy_path.write_text(json.dumps(unknown_strategy_request))
    unknown_strategy_output = workspace / "unknown-strategy-result"
    require_failure(
        run_renderer(
            renderer,
            "--input",
            left_fixture,
            "--config",
            unknown_strategy_path,
            "--output",
            unknown_strategy_output,
        ),
        "invalid_configuration at /composition/stages/0/strategy: "
        "expected duplicate, stereo-halves, or stereo-interleave",
        unknown_strategy_output,
    )

    return 0


if __name__ == "__main__":
    sys.exit(main())
