#!/usr/bin/env python3

import json
import math
import shutil
import subprocess
import sys
from pathlib import Path


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


def feedback_loop_stage(resolved: dict) -> dict:
    return next(
        stage
        for stage in resolved["composition"]["stages"]
        if stage["type"] == "feedback-loop"
    )


def base_request(modulation=None) -> dict:
    loop = {
        "type": "feedback-loop",
        "delayMinMs": 40.0,
        "delayMaxMs": 60.0,
        "delayStrategy": "even",
        "rt60Sec": 1.0,
        "mix": "householder",
    }
    if modulation is not None:
        loop["modulation"] = modulation
    return {
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
                loop,
                {"type": "downmix", "strategy": "select"},
            ]
        },
    }


def main():
    renderer = Path(sys.argv[1])
    fixture = Path(sys.argv[2])
    workspace = Path(sys.argv[3])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    def render(name: str, request: dict):
        request_path = workspace / f"{name}-request.json"
        request_path.write_text(json.dumps(request, indent=2) + "\n")
        result = workspace / f"{name}-result"
        run_ok(
            renderer,
            "--input",
            fixture,
            "--config",
            request_path,
            "--output",
            result,
        )
        resolved = json.loads((result / "resolved.json").read_text())
        wav = (result / "output.wav").read_bytes()
        return resolved, wav

    # Modulation omitted: resolved.json carries no "modulation" key at all
    # (see docs/design/reverb/stages/06-modulation.md's "modulation is
    # optional wherever it appears").
    omitted_resolved, omitted_wav = render("omitted", base_request())
    omitted_loop = feedback_loop_stage(omitted_resolved)
    if "modulation" in omitted_loop:
        raise AssertionError(
            f"omitted Modulation still resolved a modulation object: "
            f"{omitted_loop}"
        )

    # Explicit zero depth: resolved.json now records a modulation object
    # (evidence that the field was actually configured), but the rendered
    # output must be byte-identical to Modulation omitted -- the resolved
    # bypass rather than arithmetic collapsing to identity.
    zero_resolved, zero_wav = render(
        "zero-depth", base_request({"depthMs": 0.0})
    )
    zero_loop = feedback_loop_stage(zero_resolved)
    if "modulation" not in zero_loop:
        raise AssertionError("explicit zero depth did not resolve Modulation")
    zero_modulation = zero_loop["modulation"]
    if zero_modulation["depthMs"] != 0.0:
        raise AssertionError(f"unexpected resolved depthMs: {zero_modulation}")
    if zero_modulation["channelSeeds"] or zero_modulation["channelTargetsPerSample"]:
        raise AssertionError(
            f"zero-depth Modulation resolved per-Channel seeds/rates it "
            f"should have bypassed: {zero_modulation}"
        )
    if zero_loop["bufferSizes"] != omitted_loop["bufferSizes"]:
        raise AssertionError(
            "zero-depth Modulation grew the resolved buffer sizes over "
            "Modulation omitted"
        )
    if zero_wav != omitted_wav:
        raise AssertionError(
            "zero-depth Modulation rendered output was not bit-identical "
            "to Modulation omitted"
        )

    # An included but otherwise empty Modulation object resolves the
    # documented research baseline (depthMs 0.4, rateHz 0.7, lagrange3).
    baseline_resolved, _ = render("baseline", base_request({}))
    baseline_modulation = feedback_loop_stage(baseline_resolved)["modulation"]
    if (
        baseline_modulation["depthMs"] != 0.4
        or baseline_modulation["rateHz"] != 0.7
        or baseline_modulation["interpolation"] != "lagrange3"
    ):
        raise AssertionError(
            f"an empty Modulation object did not resolve the documented "
            f"research baseline: {baseline_modulation}"
        )

    # Active Modulation: buffer headroom grows by Excursion plus the fixed
    # Interpolation margin, per-Channel seeds/rates are resolved (one per
    # Channel), and the rendered output differs from the unmodulated
    # baseline -- Modulation actually did something audible.
    active_resolved, active_wav = render(
        "active", base_request({"depthMs": 5.0, "rateHz": 3.0})
    )
    active_loop = feedback_loop_stage(active_resolved)
    active_modulation = active_loop["modulation"]
    channels = active_loop["channels"]
    if (
        len(active_modulation["channelSeeds"]) != channels
        or len(active_modulation["channelTargetsPerSample"]) != channels
    ):
        raise AssertionError(
            f"active Modulation did not resolve one seed/rate per Channel: "
            f"{active_modulation}"
        )
    expected_excursion = 5.0 * 48000 / 1000.0
    if abs(active_modulation["excursionSamples"] - expected_excursion) > 1e-9:
        raise AssertionError(
            f"unexpected resolved excursionSamples: {active_modulation}"
        )
    margin = active_modulation["interpolationMarginSamples"]
    for delay, buffer_size in zip(
        active_loop["delaysSamples"], active_loop["bufferSizes"]
    ):
        expected_buffer_size = delay + math.ceil(expected_excursion) + margin
        if buffer_size != expected_buffer_size:
            raise AssertionError(
                f"active Modulation did not reserve Excursion plus the "
                f"fixed Interpolation margin: {active_loop}"
            )
    if active_wav == omitted_wav:
        raise AssertionError(
            "active Modulation rendered output identical to the "
            "unmodulated baseline"
        )

    # A resolved delay too short to serve the requested Excursion plus the
    # fixed Interpolation margin is rejected before any audio is
    # processed, naming the responsible Modulation parameter path, rather
    # than overrunning intermittently at the modulation peak.
    too_short_request = base_request({"depthMs": 50.0})
    too_short_request["composition"]["stages"][1]["delayMinMs"] = 1.0
    too_short_request["composition"]["stages"][1]["delayMaxMs"] = 2.0
    too_short_path = workspace / "too-short-request.json"
    too_short_path.write_text(json.dumps(too_short_request))
    too_short_result = workspace / "too-short-result"
    too_short = run(
        renderer,
        "--input",
        fixture,
        "--config",
        too_short_path,
        "--output",
        too_short_result,
    )
    if too_short.returncode == 0:
        raise AssertionError(
            "renderer accepted a delay too short for the requested "
            "Excursion and Interpolation margin"
        )
    if "modulation/depthMs" not in too_short.stderr:
        raise AssertionError(
            f"short-delay rejection did not name the responsible "
            f"Modulation parameter path: {too_short.stderr}"
        )
    if too_short_result.exists():
        raise AssertionError("rejected configuration created a Render Result")

    # An unsupported interpolation value is rejected by name, not silently
    # substituted -- only lagrange3 ships in this milestone (#89); linear
    # and allpass are added by later tickets.
    bad_interpolation_request = base_request({"interpolation": "linear"})
    bad_interpolation_path = workspace / "bad-interpolation-request.json"
    bad_interpolation_path.write_text(json.dumps(bad_interpolation_request))
    bad_interpolation = run(
        renderer,
        "--input",
        fixture,
        "--config",
        bad_interpolation_path,
        "--output",
        workspace / "bad-interpolation-result",
    )
    if bad_interpolation.returncode == 0:
        raise AssertionError("renderer accepted an unsupported interpolation")
    if "lagrange3" not in bad_interpolation.stderr:
        raise AssertionError(
            f"unsupported interpolation was not rejected by name: "
            f"{bad_interpolation.stderr}"
        )

    # An unknown field inside modulation is rejected like every other
    # object in this schema.
    unknown_field_request = base_request({"depthMs": 0.4, "bogus": 1})
    unknown_field_path = workspace / "unknown-field-request.json"
    unknown_field_path.write_text(json.dumps(unknown_field_request))
    unknown_field = run(
        renderer,
        "--input",
        fixture,
        "--config",
        unknown_field_path,
        "--output",
        workspace / "unknown-field-result",
    )
    if unknown_field.returncode == 0:
        raise AssertionError("renderer accepted an unknown Modulation field")
    if "modulation/bogus" not in unknown_field.stderr:
        raise AssertionError(
            f"unknown Modulation field was not named in the rejection: "
            f"{unknown_field.stderr}"
        )

    # Repeat renders of an identical active-Modulation configuration are
    # exact.
    repeat_resolved, repeat_wav = render(
        "active-repeat", base_request({"depthMs": 5.0, "rateHz": 3.0})
    )
    if repeat_wav != active_wav:
        raise AssertionError(
            "repeat renders of an identical Modulation configuration were "
            "not exact"
        )


if __name__ == "__main__":
    main()
