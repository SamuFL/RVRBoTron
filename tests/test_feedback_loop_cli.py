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
        "formatVersion": 1,
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

    # --- Damping (#75): the first audible Damping tracer ------------------

    def expected_high_shelf_gain(channel_gain, high_ratio):
        loss_db = 20.0 * math.log10(channel_gain)
        shelf_db = loss_db * (1.0 / high_ratio - 1.0)
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

    # Omission preserves existing undamped output: `result` above already
    # carries the undamped render, and its resolved.json carries no
    # "damping" key at all.
    baseline_loop = next(
        stage
        for stage in json.loads((result / "resolved.json").read_text())[
            "composition"
        ]["stages"]
        if stage["type"] == "feedback-loop"
    )
    if "damping" in baseline_loop:
        raise AssertionError(
            f"an omitted Damping object appeared in resolved.json: {baseline_loop}"
        )

    # An included empty Damping object resolves the research baseline and
    # changes the render.
    damped_request = json.loads(json.dumps(request))
    damped_request["composition"]["stages"][1]["damping"] = {}
    damped_path = workspace / "damping-default-request.json"
    damped_path.write_text(json.dumps(damped_request))
    damped_result = workspace / "damping-default-result"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--config",
        damped_path,
        "--output",
        damped_result,
        "--block-size",
        "32",
    )
    if (damped_result / "output.wav").read_bytes() == (
        result / "output.wav"
    ).read_bytes():
        raise AssertionError(
            "an included empty Damping object did not change output.wav"
        )

    damped_resolved = json.loads((damped_result / "resolved.json").read_text())
    damped_loop = next(
        stage
        for stage in damped_resolved["composition"]["stages"]
        if stage["type"] == "feedback-loop"
    )
    damping = damped_loop.get("damping")
    if damping is None:
        raise AssertionError(
            f"an included empty Damping object did not resolve: {damped_loop}"
        )
    if (
        damping["highRatio"],
        damping["highHz"],
        damping["lowRatio"],
        damping["lowHz"],
    ) != (0.5, 4000.0, 1.0, 200.0):
        raise AssertionError(
            f"empty Damping object did not resolve the research baseline: "
            f"{damping}"
        )

    for channel in range(2):
        channel_gain = damped_loop["gains"][channel]
        expected_gain = expected_high_shelf_gain(channel_gain, damping["highRatio"])
        if abs(damping["highShelfGains"][channel] - expected_gain) > 1e-9:
            raise AssertionError(
                f"resolved high-shelf gain did not match the ratio solve: "
                f"{damping['highShelfGains']} != expected {expected_gain}"
            )
        expected_b0, expected_b1, expected_a1 = expected_high_shelf_coefficients(
            expected_gain, damping["highHz"], 48000
        )
        if (
            abs(damping["highShelfB0"][channel] - expected_b0) > 1e-9
            or abs(damping["highShelfB1"][channel] - expected_b1) > 1e-9
            or abs(damping["highShelfA1"][channel] - expected_a1) > 1e-9
        ):
            raise AssertionError(
                f"resolved high-shelf coefficients did not match the "
                f"canonical prewarped one-pole solve for channel {channel}: "
                f"{damping}"
            )
        # Half-gain-in-dB corner convention: |H(j*wc)|^2 == gain exactly at
        # the prewarped corner, for the analog prototype
        # H(s) = (gain*s + p) / (s + p), p = wc*sqrt(gain).
        warped = 2 * 48000 * math.tan(math.pi * damping["highHz"] / 48000)
        pole = warped * math.sqrt(expected_gain)
        magnitude_squared = (pole**2 + (expected_gain * warped) ** 2) / (
            pole**2 + warped**2
        )
        if abs(magnitude_squared - expected_gain) > 1e-9 * max(1.0, expected_gain):
            raise AssertionError(
                f"high shelf did not land at half-gain-in-dB at its corner: "
                f"|H|^2={magnitude_squared} != gain={expected_gain}"
            )

    # Resolved rerendering reproduces audio and resolved evidence exactly.
    damped_rerendered = workspace / "damping-default-rerendered"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--resolved",
        damped_result / "resolved.json",
        "--output",
        damped_rerendered,
        "--block-size",
        "32",
    )
    if (damped_rerendered / "output.wav").read_bytes() != (
        damped_result / "output.wav"
    ).read_bytes():
        raise AssertionError("resolved rerender changed Damping output.wav")
    if (damped_rerendered / "resolved.json").read_bytes() != (
        damped_result / "resolved.json"
    ).read_bytes():
        raise AssertionError("resolved rerender changed Damping resolved.json")

    # Explicit unity ratios render bit-identically to Damping disabled.
    unity_request = json.loads(json.dumps(request))
    unity_request["composition"]["stages"][1]["damping"] = {
        "highRatio": 1.0,
        "highHz": 4000,
        "lowRatio": 1.0,
        "lowHz": 200,
    }
    unity_path = workspace / "damping-unity-request.json"
    unity_path.write_text(json.dumps(unity_request))
    unity_result = workspace / "damping-unity-result"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--config",
        unity_path,
        "--output",
        unity_result,
        "--block-size",
        "32",
    )
    if (unity_result / "output.wav").read_bytes() != (result / "output.wav").read_bytes():
        raise AssertionError(
            "an explicit unity Damping ratio did not render bit-identically "
            "to Damping disabled"
        )

    # A boost request above 1.0 is rejected with a parameter-specific
    # explanation, never clamped -- safe boosting arrives in a later slice
    # (#77).
    boost_request = json.loads(json.dumps(request))
    boost_request["composition"]["stages"][1]["damping"] = {"highRatio": 1.5}
    boost_path = workspace / "damping-boost-request.json"
    boost_path.write_text(json.dumps(boost_request))
    boost_result = workspace / "damping-boost-result"
    boost = run(
        renderer,
        "--input",
        fixture,
        "--config",
        boost_path,
        "--output",
        boost_result,
    )
    if boost.returncode == 0:
        raise AssertionError("renderer accepted a highRatio boost above 1.0")
    if "/composition/stages/1/damping/highRatio" not in boost.stderr:
        raise AssertionError(
            f"boost rejection did not name the highRatio field: {boost.stderr}"
        )
    if boost_result.exists():
        raise AssertionError("rejected boost configuration created a Render Result")

    # highRatio must be finite and greater than zero.
    for invalid_ratio, label in ((0.0, "zero"), (-0.5, "negative")):
        invalid_ratio_request = json.loads(json.dumps(request))
        invalid_ratio_request["composition"]["stages"][1]["damping"] = {
            "highRatio": invalid_ratio
        }
        invalid_ratio_path = (
            workspace / f"damping-invalid-highratio-{label}-request.json"
        )
        invalid_ratio_path.write_text(json.dumps(invalid_ratio_request))
        invalid_ratio_result = workspace / f"damping-invalid-highratio-{label}-result"
        invalid_ratio_run = run(
            renderer,
            "--input",
            fixture,
            "--config",
            invalid_ratio_path,
            "--output",
            invalid_ratio_result,
        )
        if invalid_ratio_run.returncode == 0:
            raise AssertionError(f"renderer accepted a {label} highRatio")
        if "/composition/stages/1/damping/highRatio" not in invalid_ratio_run.stderr:
            raise AssertionError(
                f"{label} highRatio rejection did not name the field: "
                f"{invalid_ratio_run.stderr}"
            )

    # lowRatio departing from 1.0 is rejected: low-frequency cleanup is not
    # implemented until a later slice (#76), and rendered audio must never
    # differ silently from the requested experiment.
    low_ratio_request = json.loads(json.dumps(request))
    low_ratio_request["composition"]["stages"][1]["damping"] = {"lowRatio": 0.5}
    low_ratio_path = workspace / "damping-lowratio-request.json"
    low_ratio_path.write_text(json.dumps(low_ratio_request))
    low_ratio_result = workspace / "damping-lowratio-result"
    low_ratio_run = run(
        renderer,
        "--input",
        fixture,
        "--config",
        low_ratio_path,
        "--output",
        low_ratio_result,
    )
    if low_ratio_run.returncode == 0:
        raise AssertionError("renderer accepted a lowRatio other than 1.0")
    if "/composition/stages/1/damping/lowRatio" not in low_ratio_run.stderr:
        raise AssertionError(
            f"lowRatio rejection did not name the field: {low_ratio_run.stderr}"
        )

    # highHz (and, by the same check, lowHz) must be strictly between 0 Hz
    # and Nyquist.
    for invalid_hz, label in (
        (0.0, "zero"),
        (24000.0, "at-nyquist"),
        (30000.0, "above-nyquist"),
    ):
        invalid_hz_request = json.loads(json.dumps(request))
        invalid_hz_request["composition"]["stages"][1]["damping"] = {
            "highHz": invalid_hz
        }
        invalid_hz_path = workspace / f"damping-invalid-highhz-{label}-request.json"
        invalid_hz_path.write_text(json.dumps(invalid_hz_request))
        invalid_hz_result = workspace / f"damping-invalid-highhz-{label}-result"
        invalid_hz_run = run(
            renderer,
            "--input",
            fixture,
            "--config",
            invalid_hz_path,
            "--output",
            invalid_hz_result,
        )
        if invalid_hz_run.returncode == 0:
            raise AssertionError(f"renderer accepted a {label} highHz")
        if "/composition/stages/1/damping/highHz" not in invalid_hz_run.stderr:
            raise AssertionError(
                f"{label} highHz rejection did not name the field: "
                f"{invalid_hz_run.stderr}"
            )

    # Unknown fields inside the Damping object are rejected, matching every
    # other configuration object in this schema.
    unknown_field_request = json.loads(json.dumps(request))
    unknown_field_request["composition"]["stages"][1]["damping"] = {"bogus": 1}
    unknown_field_path = workspace / "damping-unknown-field-request.json"
    unknown_field_path.write_text(json.dumps(unknown_field_request))
    unknown_field_result = workspace / "damping-unknown-field-result"
    unknown_field_run = run(
        renderer,
        "--input",
        fixture,
        "--config",
        unknown_field_path,
        "--output",
        unknown_field_result,
    )
    if unknown_field_run.returncode == 0:
        raise AssertionError("renderer accepted an unknown Damping field")
    if "/composition/stages/1/damping/bogus" not in unknown_field_run.stderr:
        raise AssertionError(
            f"unknown Damping field rejection did not name it: "
            f"{unknown_field_run.stderr}"
        )


if __name__ == "__main__":
    main()
