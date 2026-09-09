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
    return subprocess.run(
        [str(renderer), "render", *map(str, arguments)],
        check=False,
        capture_output=True,
        text=True,
    )


def run_ok(renderer: Path, *arguments):
    completed = run(renderer, *arguments)
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)
    return completed


def main():
    renderer = Path(sys.argv[1])
    fixture = Path(sys.argv[2])
    workspace = Path(sys.argv[3])
    sample_bits = int(sys.argv[4])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    # N=2, delayStrategy "even" (deterministic, no positional-random draws)
    # so channel delays are hand-derivable: evenDelay(lastPosition=48, N=2,
    # channel) is 0 for channel 0 and lastPosition for channel 1, so the two
    # resolved delays land exactly on delayMinMs and delayMaxMs.
    request = {
        "formatVersion": 2,
        "seed": 7,
        "composition": {
            "stages": [
                {
                    "type": "split",
                    "channels": 2,
                    "strategy": "duplicate",
                    "normalisation": "energy",
                },
                {
                    "type": "feedback-loop",
                    "delayMinMs": 1.0,
                    "delayMaxMs": 2.0,
                    "delayStrategy": "even",
                    "rt60Sec": 1.0,
                    "mix": "householder",
                },
                {"type": "downmix", "strategy": "select"},
            ]
        },
    }
    request_path = workspace / "request.json"
    request_path.write_text(json.dumps(request, indent=2) + "\n")

    # Resolved delays here are 48/96 samples (see below), below the default
    # 512-frame block size, so a legal --block-size (#53) must be requested
    # explicitly.
    result = workspace / "result"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--config",
        request_path,
        "--output",
        result,
        "--block-size",
        "32",
    )

    channels, sample_rate, bits, samples = read_float_wav(result / "output.wav")
    if (channels, sample_rate, bits) != (2, 48000, sample_bits):
        raise AssertionError("Feedback Loop render did not emit canonical stereo")

    metadata = json.loads((result / "render.json").read_text())
    expected_tail_budget = math.ceil(1.0 * 1.5 * 48000)
    if metadata["tailBudgetFrames"] != expected_tail_budget:
        raise AssertionError(
            f"unexpected tailBudgetFrames: {metadata['tailBudgetFrames']} != "
            f"{expected_tail_budget}"
        )
    if metadata["inputFrames"] != 32:
        raise AssertionError(f"unexpected inputFrames: {metadata}")
    if metadata["frames"] != metadata["inputFrames"] + metadata["tailBudgetFrames"]:
        raise AssertionError(
            f"renderer did not drain exactly the Tail budget: {metadata}"
        )
    if len(samples) != metadata["frames"] * 2:
        raise AssertionError("output.wav frame count does not match render.json")

    resolved = json.loads((result / "resolved.json").read_text())
    loop = next(
        stage
        for stage in resolved["composition"]["stages"]
        if stage["type"] == "feedback-loop"
    )
    if loop["channels"] != 2:
        raise AssertionError(f"unexpected resolved Channel count: {loop}")
    if loop["delayStrategy"] != "even":
        raise AssertionError(f"unexpected resolved delayStrategy: {loop}")
    if (loop["delayMinSamples"], loop["delayMaxSamples"]) != (48, 96):
        raise AssertionError(
            f"unexpected resolved delay range in samples: {loop}"
        )
    if loop["delaysSamples"] != [48, 96]:
        raise AssertionError(
            f"even delayStrategy did not land on delayMinMs/delayMaxMs: {loop}"
        )
    if loop["delaysMs"] != [1.0, 2.0]:
        raise AssertionError(f"unexpected resolved delaysMs: {loop}")
    if loop["bufferSizes"] != [48, 96]:
        raise AssertionError(f"unexpected resolved bufferSizes: {loop}")
    if loop["rt60Sec"] != 1.0 or loop["decayMargin"] != 1.5:
        raise AssertionError(f"unexpected resolved rt60Sec/decayMargin: {loop}")
    if loop["tailBudgetSamples"] != expected_tail_budget:
        raise AssertionError(f"unexpected resolved tailBudgetSamples: {loop}")
    if loop["blockSizeBoundSamples"] != 48:
        raise AssertionError(f"unexpected resolved blockSizeBoundSamples: {loop}")
    if loop["gainMode"] != "per-channel":
        raise AssertionError(f"unexpected default resolved gainMode: {loop}")
    if "silenceFloorDb" not in loop or loop["silenceFloorDb"] is not None:
        raise AssertionError(
            f"silenceFloorDb was not present and disabled by default: {loop}"
        )

    expected_gains = [10 ** (-3 * (48 / 48000) / 1.0), 10 ** (-3 * (96 / 48000) / 1.0)]
    for actual, expected in zip(loop["gains"], expected_gains):
        if abs(actual - expected) > 1e-9:
            raise AssertionError(
                f"resolved gain did not match the RT60 solve: {loop['gains']} != "
                f"{expected_gains}"
            )

    # N=2 Householder mixing is a pure swap-and-negate: diagonal
    # 1 + (-2/N) = 0, off-diagonal -2/N = -1.
    if loop["mix"] != "householder" or loop["matrix"] != [[0.0, -1.0], [-1.0, 0.0]]:
        raise AssertionError(f"unexpected resolved Householder matrix: {loop}")

    # An exact resolved rerender reproduces output.wav bit-identically.
    rerendered = workspace / "rerendered"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--resolved",
        result / "resolved.json",
        "--output",
        rerendered,
        "--block-size",
        "32",
    )
    if (rerendered / "output.wav").read_bytes() != (result / "output.wav").read_bytes():
        raise AssertionError("resolved rerender changed Feedback Loop output")
    if (rerendered / "resolved.json").read_bytes() != (
        result / "resolved.json"
    ).read_bytes():
        raise AssertionError("resolved rerender changed resolved.json")

    # An explicitly disabled silence floor (#54) is bit-identical to an
    # omitted one, so existing evidence stays reproducible once the field
    # exists in configuration.
    explicit_disabled_request = json.loads(json.dumps(request))
    explicit_disabled_request["composition"]["stages"][1]["silenceFloorDb"] = None
    explicit_disabled_path = workspace / "explicit-disabled-request.json"
    explicit_disabled_path.write_text(json.dumps(explicit_disabled_request))
    explicit_disabled_result = workspace / "explicit-disabled-result"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--config",
        explicit_disabled_path,
        "--output",
        explicit_disabled_result,
        "--block-size",
        "32",
    )
    if (explicit_disabled_result / "output.wav").read_bytes() != (
        result / "output.wav"
    ).read_bytes():
        raise AssertionError(
            "an explicit silenceFloorDb: null changed output.wav relative "
            "to omitting the field"
        )
    if (explicit_disabled_result / "resolved.json").read_bytes() != (
        result / "resolved.json"
    ).read_bytes():
        raise AssertionError(
            "an explicit silenceFloorDb: null changed resolved.json "
            "relative to omitting the field"
        )

    # silenceFloorDb round-trips through a resolved rerender as a plain
    # numeric value -- the seam is structurally versioned even though
    # nothing in this milestone yet acts on it while enabled.
    enabled_resolved = json.loads((result / "resolved.json").read_text())
    enabled_loop = next(
        stage
        for stage in enabled_resolved["composition"]["stages"]
        if stage["type"] == "feedback-loop"
    )
    enabled_loop["silenceFloorDb"] = -90.0
    enabled_resolved_path = workspace / "enabled-resolved.json"
    enabled_resolved_path.write_text(json.dumps(enabled_resolved))
    enabled_result = workspace / "enabled-result"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--resolved",
        enabled_resolved_path,
        "--output",
        enabled_result,
        "--block-size",
        "32",
    )
    enabled_output_resolved = json.loads(
        (enabled_result / "resolved.json").read_text()
    )
    enabled_output_loop = next(
        stage
        for stage in enabled_output_resolved["composition"]["stages"]
        if stage["type"] == "feedback-loop"
    )
    if enabled_output_loop["silenceFloorDb"] != -90.0:
        raise AssertionError(
            f"resolved silenceFloorDb did not round-trip: {enabled_output_loop}"
        )

    # An unreasonable delay range is rejected against the DSP memory budget
    # rather than silently allocating hundreds of megabytes of delay lines.
    oversized_request = json.loads(json.dumps(request))
    oversized_request["composition"]["stages"][1]["delayMaxMs"] = 700000.0
    oversized_path = workspace / "oversized-request.json"
    oversized_path.write_text(json.dumps(oversized_request))
    oversized_result = workspace / "oversized-result"
    oversized = run(
        renderer,
        "--input",
        fixture,
        "--config",
        oversized_path,
        "--output",
        oversized_result,
    )
    if oversized.returncode == 0:
        raise AssertionError("renderer accepted an unreasonable delay range")
    if "memory budget" not in oversized.stderr:
        raise AssertionError(
            f"oversized delay range was not rejected as a memory-budget "
            f"failure: {oversized.stderr}"
        )
    if oversized_result.exists():
        raise AssertionError("rejected configuration created a Render Result")

    # A --block-size above the resolved Feedback Loop's block-size bound
    # (48 frames: min(48, 96), see above) is rejected under
    # invalid_arguments, naming both the requested size and the derived
    # bound (#53); a block size exactly at the bound is accepted.
    oversized_block_result = workspace / "oversized-block-result"
    oversized_block = run(
        renderer,
        "--input",
        fixture,
        "--config",
        request_path,
        "--output",
        oversized_block_result,
        "--block-size",
        "49",
    )
    if oversized_block.returncode != 7:
        raise AssertionError(
            f"expected exit code 7 (invalid_arguments) for a block size "
            f"above the bound: {oversized_block.returncode}\n"
            f"{oversized_block.stderr}"
        )
    if "invalid_arguments" not in oversized_block.stderr:
        raise AssertionError(
            f"oversized block size was not rejected under "
            f"invalid_arguments: {oversized_block.stderr}"
        )
    if "49" not in oversized_block.stderr or "48" not in oversized_block.stderr:
        raise AssertionError(
            f"oversized block size rejection did not name both the "
            f"requested size and the derived bound: "
            f"{oversized_block.stderr}"
        )
    if oversized_block_result.exists():
        raise AssertionError("rejected block size created a Render Result")

    exact_bound_result = workspace / "exact-bound-result"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--config",
        request_path,
        "--output",
        exact_bound_result,
        "--block-size",
        "48",
    )
    if not (exact_bound_result / "output.wav").exists():
        raise AssertionError(
            "a --block-size exactly at the resolved bound was rejected"
        )

    # gainMode: uniform (#56) solves one shared gain from the mean loop
    # time across Channels, rather than each Channel's own.
    uniform_request = json.loads(json.dumps(request))
    uniform_request["composition"]["stages"][1]["gainMode"] = "uniform"
    uniform_path = workspace / "uniform-request.json"
    uniform_path.write_text(json.dumps(uniform_request))
    uniform_result = workspace / "uniform-result"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--config",
        uniform_path,
        "--output",
        uniform_result,
        "--block-size",
        "32",
    )
    uniform_resolved = json.loads((uniform_result / "resolved.json").read_text())
    uniform_loop = next(
        stage
        for stage in uniform_resolved["composition"]["stages"]
        if stage["type"] == "feedback-loop"
    )
    if uniform_loop["gainMode"] != "uniform":
        raise AssertionError(f"gainMode did not resolve to uniform: {uniform_loop}")
    if len(set(uniform_loop["gains"])) != 1:
        raise AssertionError(
            f"gainMode uniform did not solve one shared gain across "
            f"Channels: {uniform_loop['gains']}"
        )
    mean_loop_time_sec = (
        sum(uniform_loop["delaysSamples"]) / len(uniform_loop["delaysSamples"]) / 48000
    )
    expected_uniform_gain = 10 ** (-3 * mean_loop_time_sec / uniform_loop["rt60Sec"])
    if abs(uniform_loop["gains"][0] - expected_uniform_gain) > 1e-9:
        raise AssertionError(
            f"gainMode uniform's shared gain did not match the mean-loop-time "
            f"solve: {uniform_loop['gains'][0]} != {expected_uniform_gain}"
        )
    if uniform_loop["gains"][0] == expected_gains[0]:
        raise AssertionError(
            "gainMode uniform's shared gain coincided with gainMode "
            "per-channel's Channel-0 gain -- fixture does not distinguish "
            "the two modes"
        )

    # mix: hadamard, reusing the existing matrix-resolution seam unchanged
    # (#56). N=2 Hadamard is a known closed form: 1/sqrt(2) * [[1, 1],
    # [1, -1]].
    hadamard_request = json.loads(json.dumps(request))
    hadamard_request["composition"]["stages"][1]["mix"] = "hadamard"
    hadamard_path = workspace / "hadamard-request.json"
    hadamard_path.write_text(json.dumps(hadamard_request))
    hadamard_result = workspace / "hadamard-result"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--config",
        hadamard_path,
        "--output",
        hadamard_result,
        "--block-size",
        "32",
    )
    hadamard_resolved = json.loads((hadamard_result / "resolved.json").read_text())
    hadamard_loop = next(
        stage
        for stage in hadamard_resolved["composition"]["stages"]
        if stage["type"] == "feedback-loop"
    )
    hadamard_scale = 0.7071067811865475
    expected_hadamard_matrix = [
        [hadamard_scale, hadamard_scale],
        [hadamard_scale, -hadamard_scale],
    ]
    if hadamard_loop["mix"] != "hadamard" or hadamard_loop["matrix"] != (
        expected_hadamard_matrix
    ):
        raise AssertionError(
            f"unexpected resolved N=2 Hadamard matrix: {hadamard_loop}"
        )

    # mix: hadamard at a non-power-of-two Channel count is a hard error,
    # never a silent fallback (#56) -- the same requirement already proven
    # for the Diffuser, now covered for the Feedback Loop's own mix
    # resolution.
    non_power_of_two_request = json.loads(json.dumps(request))
    non_power_of_two_request["composition"]["stages"][0]["channels"] = 3
    non_power_of_two_request["composition"]["stages"][1]["mix"] = "hadamard"
    non_power_of_two_path = workspace / "non-power-of-two-request.json"
    non_power_of_two_path.write_text(json.dumps(non_power_of_two_request))
    non_power_of_two_result = workspace / "non-power-of-two-result"
    non_power_of_two = run(
        renderer,
        "--input",
        fixture,
        "--config",
        non_power_of_two_path,
        "--output",
        non_power_of_two_result,
        "--block-size",
        "32",
    )
    if non_power_of_two.returncode == 0:
        raise AssertionError(
            "renderer accepted hadamard at a non-power-of-two Channel count"
        )
    if (
        "hadamard requires a power-of-two Channel count"
        not in non_power_of_two.stderr
    ):
        raise AssertionError(
            f"hadamard at a non-power-of-two Channel count was not rejected "
            f"with its own specific reason: {non_power_of_two.stderr}"
        )
    if "/composition/stages/1/mix" not in non_power_of_two.stderr:
        raise AssertionError(
            f"hadamard rejection did not name the mix field: "
            f"{non_power_of_two.stderr}"
        )
    if non_power_of_two_result.exists():
        raise AssertionError("rejected configuration created a Render Result")

    # mix: random-orthogonal, delayStrategy: uniform-random, and
    # gainMode: uniform together (#56) -- every resolved combination is
    # recorded in the Resolved Configuration and rerenders exactly, not
    # just the Reference combination exercised above.
    combined_request = json.loads(json.dumps(request))
    combined_request["composition"]["stages"][1]["mix"] = "random-orthogonal"
    combined_request["composition"]["stages"][1]["delayStrategy"] = "uniform-random"
    combined_request["composition"]["stages"][1]["gainMode"] = "uniform"
    combined_path = workspace / "combined-request.json"
    combined_path.write_text(json.dumps(combined_request))
    combined_result = workspace / "combined-result"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--config",
        combined_path,
        "--output",
        combined_result,
        "--block-size",
        "32",
    )
    combined_resolved = json.loads((combined_result / "resolved.json").read_text())
    combined_loop = next(
        stage
        for stage in combined_resolved["composition"]["stages"]
        if stage["type"] == "feedback-loop"
    )
    if (
        combined_loop["mix"] != "random-orthogonal"
        or combined_loop["delayStrategy"] != "uniform-random"
        or combined_loop["gainMode"] != "uniform"
    ):
        raise AssertionError(
            f"combined mix/delayStrategy/gainMode did not resolve as "
            f"requested: {combined_loop}"
        )
    if len(combined_loop["matrix"]) != 2 or any(
        len(row) != 2 for row in combined_loop["matrix"]
    ):
        raise AssertionError(
            f"unexpected resolved random-orthogonal matrix shape: {combined_loop}"
        )

    combined_rerendered = workspace / "combined-rerendered"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--resolved",
        combined_result / "resolved.json",
        "--output",
        combined_rerendered,
        "--block-size",
        "32",
    )
    if (combined_rerendered / "output.wav").read_bytes() != (
        combined_result / "output.wav"
    ).read_bytes():
        raise AssertionError(
            "resolved rerender changed output.wav for the combined "
            "mix/delayStrategy/gainMode configuration"
        )
    if (combined_rerendered / "resolved.json").read_bytes() != (
        combined_result / "resolved.json"
    ).read_bytes():
        raise AssertionError(
            "resolved rerender changed resolved.json for the combined "
            "mix/delayStrategy/gainMode configuration"
        )

    # --- Damping (#75/#76): the Two-shelf damping path ---------------------

    def expected_shelf_gain(channel_gain, ratio):
        loss_db = 20.0 * math.log10(channel_gain)
        shelf_db = loss_db * (1.0 / ratio - 1.0)
        return 10.0 ** (shelf_db / 20.0)

    def expected_high_shelf_coefficients(gain, corner_hz, sample_rate):
        warped = 2.0 * sample_rate * math.tan(math.pi * corner_hz / sample_rate)
        k = 2.0 * sample_rate
        pole = warped * math.sqrt(gain)
        denominator = k + pole
        b0 = (gain * k + pole) / denominator
        b1 = (pole - gain * k) / denominator
        a1 = (pole - k) / denominator
        return b0, b1, a1

    def expected_low_shelf_coefficients(gain, corner_hz, sample_rate):
        warped = 2.0 * sample_rate * math.tan(math.pi * corner_hz / sample_rate)
        k = 2.0 * sample_rate
        pole = warped / math.sqrt(gain)
        denominator = k + pole
        b0 = (k + gain * pole) / denominator
        b1 = (gain * pole - k) / denominator
        a1 = (pole - k) / denominator
        return b0, b1, a1

    def shelf_magnitude(coefficients, frequency_hz, sample_rate):
        b0, b1, a1 = coefficients
        omega = 2.0 * math.pi * frequency_hz / sample_rate
        cos_omega, sin_omega = math.cos(omega), math.sin(omega)
        numerator_real, numerator_imag = b0 + b1 * cos_omega, -b1 * sin_omega
        denominator_real, denominator_imag = 1.0 + a1 * cos_omega, -a1 * sin_omega
        return math.sqrt(
            (numerator_real**2 + numerator_imag**2)
            / (denominator_real**2 + denominator_imag**2)
        )

    def check_shelf(damping, channel, channel_gain, ratio, corner_hz, prefix, high):
        expected_gain = expected_shelf_gain(channel_gain, ratio)
        actual_gain = damping[f"{prefix}Gains"][channel]
        if abs(actual_gain - expected_gain) > 1e-9 * max(1.0, expected_gain):
            raise AssertionError(
                f"{prefix} gain mismatch on channel {channel}: "
                f"{actual_gain} != {expected_gain}"
            )
        coefficients = (
            expected_high_shelf_coefficients(expected_gain, corner_hz, 48000)
            if high
            else expected_low_shelf_coefficients(expected_gain, corner_hz, 48000)
        )
        expected_b0, expected_b1, expected_a1 = coefficients
        actual = (
            damping[f"{prefix}B0"][channel],
            damping[f"{prefix}B1"][channel],
            damping[f"{prefix}A1"][channel],
        )
        if (
            abs(actual[0] - expected_b0) > 1e-9 * max(1.0, abs(expected_b0))
            or abs(actual[1] - expected_b1) > 1e-9 * max(1.0, abs(expected_b1))
            or abs(actual[2] - expected_a1) > 1e-9 * max(1.0, abs(expected_a1))
        ):
            raise AssertionError(
                f"{prefix} coefficient mismatch on channel {channel}: "
                f"{actual} != {coefficients}"
            )
        # Half-gain-in-dB corner convention: |H(j*wc)|^2 == gain exactly at
        # the prewarped corner. High: H(s) = (gain*s+p)/(s+p), p = wc*sqrt(
        # gain). Low (mirrored): H(s) = (s+gain*p)/(s+p), p = wc/sqrt(gain).
        warped = 2 * 48000 * math.tan(math.pi * corner_hz / 48000)
        if high:
            pole = warped * math.sqrt(expected_gain)
            magnitude_squared = (pole**2 + (expected_gain * warped) ** 2) / (
                pole**2 + warped**2
            )
        else:
            pole = warped / math.sqrt(expected_gain)
            magnitude_squared = (warped**2 + (expected_gain * pole) ** 2) / (
                warped**2 + pole**2
            )
        if abs(magnitude_squared - expected_gain) > 1e-9 * max(1.0, expected_gain):
            raise AssertionError(
                f"{prefix} did not land at half-gain-in-dB at its corner: "
                f"|H|^2={magnitude_squared} != gain={expected_gain}"
            )
        return expected_gain, coefficients

    # Exact powers of two: the IEEE 754 machine epsilon for each supported
    # sample precision (see rvrbotron::config::resolveMatrixContractionBound).
    eps_float32 = 2.0**-23
    eps_float64 = 2.0**-52

    def settling_time_sec(a1, sample_rate=48000):
        if a1 == 0.0:
            return 0.0
        samples = 60.0 / (-20.0 * math.log10(abs(a1)))
        return samples / sample_rate

    def check_damping_evidence(loop, damping):
        channels = len(loop["gains"])
        matrix_bound_32 = 1.0 + math.sqrt(channels) * eps_float32
        matrix_bound_64 = 1.0 + math.sqrt(channels) * eps_float64
        expected_slowest = loop["rt60Sec"]
        for channel in range(channels):
            channel_gain = loop["gains"][channel]
            loop_time_sec = loop["delaysSamples"][channel] / 48000
            high_gain, high_coefficients = check_shelf(
                damping,
                channel,
                channel_gain,
                damping["highRatio"],
                damping["highHz"],
                "highShelf",
                True,
            )
            low_gain, low_coefficients = check_shelf(
                damping,
                channel,
                channel_gain,
                damping["lowRatio"],
                damping["lowHz"],
                "lowShelf",
                False,
            )
            loss_db = 20.0 * math.log10(channel_gain)
            implied_undamped_rt60 = -60.0 * loop_time_sec / loss_db
            reference_magnitude = shelf_magnitude(
                low_coefficients, 1000.0, 48000
            ) * shelf_magnitude(high_coefficients, 1000.0, 48000)
            reference_loss_db = loss_db + 20.0 * math.log10(reference_magnitude)
            expected_low_rt60 = damping["lowRatio"] * implied_undamped_rt60
            expected_high_rt60 = damping["highRatio"] * implied_undamped_rt60
            shelf_factor = max(1.0, low_gain) * max(1.0, high_gain)
            expected_bound_32 = channel_gain * shelf_factor * matrix_bound_32
            expected_bound_64 = channel_gain * shelf_factor * matrix_bound_64
            expected = {
                "expectedLowRt60Sec": expected_low_rt60,
                "expectedHighRt60Sec": expected_high_rt60,
                "expectedReferenceRt60Sec": -60.0 * loop_time_sec / reference_loss_db,
                "contractionBoundFloat32": expected_bound_32,
                "contractionMarginFloat32": 1.0 - expected_bound_32,
                "contractionBoundFloat64": expected_bound_64,
                "contractionMarginFloat64": 1.0 - expected_bound_64,
            }
            for key, expected_value in expected.items():
                actual_value = damping[key][channel]
                if abs(actual_value - expected_value) > 1e-6 * max(
                    1.0, abs(expected_value)
                ):
                    raise AssertionError(
                        f"{key} mismatch on channel {channel}: "
                        f"{actual_value} != {expected_value} ({damping})"
                    )
            if not (damping["contractionBoundFloat32"][channel] < 1.0) or not (
                damping["contractionBoundFloat64"][channel] < 1.0
            ):
                raise AssertionError(
                    f"channel {channel} was accepted with a contraction "
                    f"bound not strictly below unity: {damping}"
                )
            if damping["highRatio"] > 1.0:
                expected_slowest = max(expected_slowest, expected_high_rt60)
                expected_slowest = max(
                    expected_slowest, settling_time_sec(high_coefficients[2])
                )
            if damping["lowRatio"] > 1.0:
                expected_slowest = max(expected_slowest, expected_low_rt60)
                expected_slowest = max(
                    expected_slowest, settling_time_sec(low_coefficients[2])
                )
        if abs(damping["slowestResolvedRt60Sec"] - expected_slowest) > 1e-6 * max(
            1.0, expected_slowest
        ):
            raise AssertionError(
                f"slowestResolvedRt60Sec mismatch: "
                f"{damping['slowestResolvedRt60Sec']} != {expected_slowest} "
                f"({damping})"
            )
        return expected_slowest

    def render_damping(name, damping_fields, expect_ok=True):
        req = json.loads(json.dumps(request))
        req["composition"]["stages"][1]["damping"] = damping_fields
        path = workspace / f"damping-{name}-request.json"
        path.write_text(json.dumps(req))
        result_dir = workspace / f"damping-{name}-result"
        if expect_ok:
            run_ok(
                renderer,
                "--input",
                fixture,
                "--config",
                path,
                "--output",
                result_dir,
                "--block-size",
                "32",
            )
            return result_dir
        return run(
            renderer,
            "--input",
            fixture,
            "--config",
            path,
            "--output",
            result_dir,
        ), result_dir

    def loop_and_damping(result_dir):
        resolved = json.loads((result_dir / "resolved.json").read_text())
        loop = next(
            stage
            for stage in resolved["composition"]["stages"]
            if stage["type"] == "feedback-loop"
        )
        return loop, loop.get("damping")

    # Omission preserves existing undamped output: `result` above already
    # carries the undamped render, and its resolved.json carries no
    # "damping" key at all.
    baseline_loop, baseline_damping = loop_and_damping(result)
    if baseline_damping is not None:
        raise AssertionError(
            f"an omitted Damping object appeared in resolved.json: {baseline_loop}"
        )

    # An included empty Damping object resolves the research baseline and
    # changes the render; its resolved evidence (both shelves, every
    # per-Channel expected decay) matches the ratio/coefficient solve.
    default_result = render_damping("default", {})
    if (default_result / "output.wav").read_bytes() == (
        result / "output.wav"
    ).read_bytes():
        raise AssertionError(
            "an included empty Damping object did not change output.wav"
        )
    default_loop, default_damping = loop_and_damping(default_result)
    if default_damping is None:
        raise AssertionError("an included empty Damping object did not resolve")
    if (
        default_damping["highRatio"],
        default_damping["highHz"],
        default_damping["lowRatio"],
        default_damping["lowHz"],
    ) != (0.5, 4000.0, 1.0, 200.0):
        raise AssertionError(
            f"empty Damping object did not resolve the research baseline: "
            f"{default_damping}"
        )
    check_damping_evidence(default_loop, default_damping)

    # Resolved rerendering reproduces audio and resolved evidence exactly
    # for high-only (the baseline default above already covers this), and
    # for low-only and combined Two-shelf damping below.
    def check_exact_rerender(name, result_dir):
        rerendered = workspace / f"damping-{name}-rerendered"
        run_ok(
            renderer,
            "--input",
            fixture,
            "--resolved",
            result_dir / "resolved.json",
            "--output",
            rerendered,
            "--block-size",
            "32",
        )
        if (rerendered / "output.wav").read_bytes() != (
            result_dir / "output.wav"
        ).read_bytes():
            raise AssertionError(f"resolved rerender changed {name} output.wav")
        if (rerendered / "resolved.json").read_bytes() != (
            result_dir / "resolved.json"
        ).read_bytes():
            raise AssertionError(f"resolved rerender changed {name} resolved.json")

    check_exact_rerender("high-only", default_result)

    # Low-only damping (#76): the low shelf now actually shapes the tail.
    low_only_result = render_damping(
        "low-only", {"highRatio": 1.0, "lowRatio": 0.6, "lowHz": 300}
    )
    low_only_loop, low_only_damping = loop_and_damping(low_only_result)
    check_damping_evidence(low_only_loop, low_only_damping)
    check_exact_rerender("low-only", low_only_result)

    # Combined Two-shelf damping: both sections active simultaneously.
    combined_result = render_damping(
        "combined", {"highRatio": 0.5, "highHz": 4000, "lowRatio": 0.6, "lowHz": 300}
    )
    combined_loop, combined_damping = loop_and_damping(combined_result)
    check_damping_evidence(combined_loop, combined_damping)
    check_exact_rerender("combined", combined_result)

    # Deterministic impulse evidence distinguishes low-frequency cleanup,
    # high-frequency darkening, the combination, and the undamped Reference
    # case from one another -- four pairwise-distinct renders.
    renders = {
        "undamped": (result / "output.wav").read_bytes(),
        "high-only": (default_result / "output.wav").read_bytes(),
        "low-only": (low_only_result / "output.wav").read_bytes(),
        "combined": (combined_result / "output.wav").read_bytes(),
    }
    names = list(renders)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            if renders[names[i]] == renders[names[j]]:
                raise AssertionError(
                    f"{names[i]} and {names[j]} renders were not distinguishable"
                )

    # Crossed/overlapping corner layouts (lowHz above highHz) are permitted
    # -- the response contract, not a conventional corner order, decides
    # validity.
    crossed_result = render_damping(
        "crossed", {"highRatio": 0.6, "highHz": 2000, "lowRatio": 0.6, "lowHz": 8000}
    )
    crossed_loop, crossed_damping = loop_and_damping(crossed_result)
    check_damping_evidence(crossed_loop, crossed_damping)

    # Explicit unity ratios for both shelves render bit-identically to
    # Damping disabled, and each section's bypass is independent: a unity
    # high ratio alongside an active low ratio only skips the high shelf
    # (low-only above already differs from undamped; high-only already
    # differs from undamped; here both-unity must match undamped exactly).
    unity_result = render_damping(
        "unity",
        {"highRatio": 1.0, "highHz": 4000, "lowRatio": 1.0, "lowHz": 200},
    )
    if (unity_result / "output.wav").read_bytes() != (result / "output.wav").read_bytes():
        raise AssertionError(
            "explicit unity ratios for both shelves did not render "
            "bit-identically to Damping disabled"
        )

    # gainMode narrow-common-target vs. per-Channel range (#76): under
    # per-channel gain mode (used throughout above), every Channel's own
    # expected decay converges on the same target since gain is solved from
    # that Channel's own loop time. Under uniform gain mode, the shared gain
    # combined with each Channel's own (unequal) loop time spreads expected
    # decay into a real per-Channel range instead.
    per_channel_high = default_damping["expectedHighRt60Sec"]
    if abs(per_channel_high[0] - per_channel_high[1]) > 1e-6:
        raise AssertionError(
            f"gainMode per-channel did not converge on a narrow common "
            f"expected-decay target across Channels: {per_channel_high}"
        )
    uniform_request = json.loads(json.dumps(request))
    uniform_request["composition"]["stages"][1]["gainMode"] = "uniform"
    uniform_request["composition"]["stages"][1]["damping"] = {}
    uniform_path = workspace / "damping-uniform-gain-mode-request.json"
    uniform_path.write_text(json.dumps(uniform_request))
    uniform_result = workspace / "damping-uniform-gain-mode-result"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--config",
        uniform_path,
        "--output",
        uniform_result,
        "--block-size",
        "32",
    )
    uniform_loop, uniform_damping = loop_and_damping(uniform_result)
    check_damping_evidence(uniform_loop, uniform_damping)
    uniform_high = uniform_damping["expectedHighRt60Sec"]
    if abs(uniform_high[0] - uniform_high[1]) < 1e-3:
        raise AssertionError(
            f"gainMode uniform did not spread expected decay into a "
            f"per-Channel range across Channels of unequal loop time: "
            f"{uniform_high}"
        )

    # Safe boosting (#77): a ratio above 1.0 is accepted when the
    # conservative contraction certificate passes, with resolved evidence
    # (per-Channel contraction bound/margin at both float32 and float64,
    # and the slowest resolved RT60) recorded and internally consistent.
    safe_boost_result = render_damping("safe-boost", {"highRatio": 1.5})
    safe_boost_loop, safe_boost_damping = loop_and_damping(safe_boost_result)
    expected_slowest = check_damping_evidence(safe_boost_loop, safe_boost_damping)
    if expected_slowest <= safe_boost_loop["rt60Sec"]:
        raise AssertionError(
            "test fixture did not actually exercise a boosted Tail budget: "
            f"{expected_slowest} vs rt60Sec={safe_boost_loop['rt60Sec']}"
        )

    # The Tail budget equals the slowest resolved RT60 times decayMargin,
    # rounded up to frames; render metadata agrees, and the renderer
    # writes the complete authorized response.
    expected_tail_budget_samples = math.ceil(
        expected_slowest * safe_boost_loop["decayMargin"] * 48000
    )
    if safe_boost_loop["tailBudgetSamples"] != expected_tail_budget_samples:
        raise AssertionError(
            f"boosted tailBudgetSamples did not match the slowest resolved "
            f"RT60 solve: {safe_boost_loop['tailBudgetSamples']} != "
            f"{expected_tail_budget_samples}"
        )
    safe_boost_metadata = json.loads(
        (safe_boost_result / "render.json").read_text()
    )
    if safe_boost_metadata["tailBudgetFrames"] != expected_tail_budget_samples:
        raise AssertionError(
            f"render metadata did not agree with the extended Tail budget: "
            f"{safe_boost_metadata}"
        )
    if (
        safe_boost_metadata["frames"]
        != safe_boost_metadata["inputFrames"]
        + safe_boost_metadata["tailBudgetFrames"]
    ):
        raise AssertionError(
            f"renderer did not drain the complete authorized response: "
            f"{safe_boost_metadata}"
        )
    check_exact_rerender("safe-boost", safe_boost_result)

    # A long deterministic burst response (the boosted tail above) contains
    # no non-finite samples and has a negative late-energy trend, without
    # requiring every local energy window to decrease -- local modal
    # beating does not fail this evidence.
    _burst_channels, _burst_rate, _burst_bits, burst_samples = read_float_wav(
        safe_boost_result / "output.wav"
    )
    if not all(math.isfinite(sample) for sample in burst_samples):
        raise AssertionError("boosted burst response contained a non-finite sample")
    tail_samples = burst_samples[len(burst_samples) // 4 :]
    window_count = 10
    window_size = max(1, len(tail_samples) // window_count)
    window_energies = [
        math.sqrt(
            sum(s * s for s in tail_samples[i * window_size : (i + 1) * window_size])
            / window_size
        )
        for i in range(window_count)
    ]
    if window_energies[-1] >= max(window_energies) * 0.1:
        raise AssertionError(
            f"boosted burst response did not show a negative late-energy "
            f"trend: {window_energies}"
        )

    # A nearby request beyond the conservative bound is rejected with an
    # explanation naming the Damping object, the responsible Channel, and
    # the contraction bound, while a safely contractive one right below it
    # is accepted -- both shelves boosted together at the same ratio
    # (found empirically for this fixture: exactly ratio 2.0 already
    # fails, so 1.8/2.2 straddle it with comfortable margin either side).
    boundary_accept_result = render_damping(
        "boundary-accept", {"highRatio": 1.8, "lowRatio": 1.8}
    )
    boundary_accept_loop, boundary_accept_damping = loop_and_damping(
        boundary_accept_result
    )
    check_damping_evidence(boundary_accept_loop, boundary_accept_damping)
    check_exact_rerender("boundary-accept", boundary_accept_result)

    boundary_reject, boundary_reject_result = render_damping(
        "boundary-reject", {"highRatio": 2.2, "lowRatio": 2.2}, expect_ok=False
    )
    if boundary_reject.returncode == 0:
        raise AssertionError(
            "renderer accepted a boost combination beyond the conservative "
            "contraction bound"
        )
    if (
        "/composition/stages/1/damping" not in boundary_reject.stderr
        or "contraction" not in boundary_reject.stderr
    ):
        raise AssertionError(
            f"boost-beyond-bound rejection did not name the Damping object "
            f"and the contraction bound: {boundary_reject.stderr}"
        )
    if boundary_reject_result.exists():
        raise AssertionError(
            "rejected boost-beyond-bound configuration created a Render "
            "Result"
        )

    # Deliberately corrupted resolved Damping data (an impossible |a1| >= 1
    # shelf pole) is rejected before processing, not merely producing
    # unstable or garbage output.
    corrupted_resolved = json.loads(
        (safe_boost_result / "resolved.json").read_text()
    )
    corrupted_loop = next(
        stage
        for stage in corrupted_resolved["composition"]["stages"]
        if stage["type"] == "feedback-loop"
    )
    corrupted_loop["damping"]["highShelfA1"][0] = 1.5
    corrupted_path = workspace / "damping-corrupted-resolved.json"
    corrupted_path.write_text(json.dumps(corrupted_resolved))
    corrupted_result = workspace / "damping-corrupted-result"
    corrupted_run = run(
        renderer,
        "--input",
        fixture,
        "--resolved",
        corrupted_path,
        "--output",
        corrupted_result,
    )
    if corrupted_run.returncode == 0:
        raise AssertionError(
            "renderer accepted a corrupted (|a1| >= 1) resolved Damping "
            "coefficient"
        )
    if corrupted_result.exists():
        raise AssertionError(
            "rejected corrupted resolved Damping configuration created a "
            "Render Result"
        )

    # Both ratios must be finite and greater than zero.
    for field in ("highRatio", "lowRatio"):
        for invalid_ratio, label in ((0.0, "zero"), (-0.5, "negative")):
            invalid_run, _ = render_damping(
                f"invalid-{field}-{label}", {field: invalid_ratio}, expect_ok=False
            )
            if invalid_run.returncode == 0:
                raise AssertionError(f"renderer accepted a {label} {field}")
            if f"/composition/stages/1/damping/{field}" not in invalid_run.stderr:
                raise AssertionError(
                    f"{label} {field} rejection did not name the field: "
                    f"{invalid_run.stderr}"
                )

    # Both corners must be strictly between 0 Hz and Nyquist.
    for field in ("highHz", "lowHz"):
        for invalid_hz, label in (
            (0.0, "zero"),
            (24000.0, "at-nyquist"),
            (30000.0, "above-nyquist"),
        ):
            invalid_run, _ = render_damping(
                f"invalid-{field}-{label}", {field: invalid_hz}, expect_ok=False
            )
            if invalid_run.returncode == 0:
                raise AssertionError(f"renderer accepted a {label} {field}")
            if f"/composition/stages/1/damping/{field}" not in invalid_run.stderr:
                raise AssertionError(
                    f"{label} {field} rejection did not name the field: "
                    f"{invalid_run.stderr}"
                )

    # A Reference-band deviation (a corner/ratio combination whose solved
    # 1 kHz response implies an RT60 far from rt60Sec) is not rejected --
    # see ADR-0004: the render is stable and does exactly what was
    # requested, so only the resolved evidence's own internal consistency
    # is checked, not its distance from rt60Sec. A corner placed on top of
    # the Reference band with a large ratio is exactly such a case.
    on_reference_result = render_damping(
        "on-reference", {"highRatio": 0.2, "highHz": 1000}
    )
    on_reference_loop, on_reference_damping = loop_and_damping(on_reference_result)
    check_damping_evidence(on_reference_loop, on_reference_damping)
    reference_rt60 = on_reference_damping["expectedReferenceRt60Sec"][0]
    if abs(reference_rt60 - on_reference_loop["rt60Sec"]) < 0.05 * on_reference_loop[
        "rt60Sec"
    ]:
        raise AssertionError(
            "test fixture did not actually exercise a Reference-band "
            f"deviation: {reference_rt60} vs rt60Sec="
            f"{on_reference_loop['rt60Sec']}"
        )

    # Unknown fields inside the Damping object are rejected, matching every
    # other configuration object in this schema.
    unknown_run, _ = render_damping("unknown-field", {"bogus": 1}, expect_ok=False)
    if unknown_run.returncode == 0:
        raise AssertionError("renderer accepted an unknown Damping field")
    if "/composition/stages/1/damping/bogus" not in unknown_run.stderr:
        raise AssertionError(
            f"unknown Damping field rejection did not name it: "
            f"{unknown_run.stderr}"
        )


if __name__ == "__main__":
    main()
