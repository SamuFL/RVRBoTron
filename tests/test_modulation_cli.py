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


def run_benchmark_ok(renderer: Path, *arguments):
    completed = subprocess.run(
        [str(renderer), "benchmark", *map(str, arguments)],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)
    return completed


def feedback_loop_stage(resolved: dict) -> dict:
    return next(
        stage
        for stage in resolved["composition"]["stages"]
        if stage["type"] == "feedback-loop"
    )


def base_request(modulation=None, channels=2) -> dict:
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
        "formatVersion": 2,
        "seed": 7,
        "composition": {
            "stages": [
                {
                    "type": "split",
                    "channels": channels,
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
    if (
        zero_modulation["channelSeeds"]
        or zero_modulation["channelTargetsPerSample"]
        or zero_modulation["channelPhases"]
    ):
        raise AssertionError(
            f"zero-depth Modulation resolved per-Channel seeds/rates/phases "
            f"it should have bypassed: {zero_modulation}"
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
    # documented research baseline (depthMs 0.4, rateHz 0.7,
    # smoothed-random, channelFraction 1.0, lagrange3).
    baseline_resolved, _ = render("baseline", base_request({}))
    baseline_modulation = feedback_loop_stage(baseline_resolved)["modulation"]
    if (
        baseline_modulation["depthMs"] != 0.4
        or baseline_modulation["rateHz"] != 0.7
        or baseline_modulation["shape"] != "smoothed-random"
        or baseline_modulation["channelFraction"] != 1.0
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
        or len(active_modulation["channelPhases"]) != channels
    ):
        raise AssertionError(
            f"active Modulation did not resolve one seed/rate/phase per "
            f"Channel: {active_modulation}"
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

    # An exact resolved rerender of an active Feedback Loop Modulation
    # reproduces output.wav and resolved.json byte-for-byte -- the
    # resolved-configuration round-trip invariant applied to an active
    # Modulation object specifically, not just to the surrounding stage
    # fields every other resolved-rerender test already covers.
    active_result = workspace / "active-result"
    active_rerendered = workspace / "active-rerendered"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--resolved",
        active_result / "resolved.json",
        "--output",
        active_rerendered,
    )
    if (active_rerendered / "output.wav").read_bytes() != (
        active_result / "output.wav"
    ).read_bytes():
        raise AssertionError(
            "resolved rerender changed active Feedback Loop Modulation "
            "output"
        )
    if (active_rerendered / "resolved.json").read_bytes() != (
        active_result / "resolved.json"
    ).read_bytes():
        raise AssertionError(
            "resolved rerender changed active Feedback Loop Modulation "
            "resolved.json"
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
    # substituted -- lagrange3, linear, and allpass are the complete set
    # as of this milestone (#89/#92/#93).
    bad_interpolation_request = base_request({"interpolation": "sinc"})
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
    if "lagrange3, linear, or allpass" not in bad_interpolation.stderr:
        raise AssertionError(
            f"unsupported interpolation was not rejected by name: "
            f"{bad_interpolation.stderr}"
        )

    # linear resolves and renders end to end (issue #92): a deliberate
    # ablation exposing the unintended, depth-dependent lowpass a moving
    # linear-interpolated read produces inside a circulating loop.
    linear_resolved, linear_wav = render(
        "linear",
        base_request({"depthMs": 5.0, "rateHz": 3.0, "interpolation": "linear"}),
    )
    linear_loop = feedback_loop_stage(linear_resolved)
    if linear_loop["modulation"]["interpolation"] != "linear":
        raise AssertionError(
            f"requested linear interpolation did not round-trip: "
            f"{linear_loop['modulation']}"
        )
    if linear_wav == omitted_wav:
        raise AssertionError(
            "linear-interpolated Modulation rendered output identical to "
            "the unmodulated baseline"
        )
    if linear_wav == active_wav:
        raise AssertionError(
            "linear-interpolated Modulation rendered output identical to "
            "lagrange3 at the same depthMs/rateHz"
        )
    # The Interpolation margin -- and therefore every resolved buffer
    # size -- does not move when the interpolation method changes: it is
    # sized for the worst case across all methods, not the configured
    # one.
    if linear_loop["bufferSizes"] != active_loop["bufferSizes"]:
        raise AssertionError(
            f"linear interpolation resolved different buffer sizes than "
            f"lagrange3 at the same depthMs: {linear_loop['bufferSizes']} "
            f"!= {active_loop['bufferSizes']}"
        )

    # depthMs of 0 remains bit-identical to Modulation omitted under
    # linear too -- the resolved bypass is independent of interpolation
    # method.
    _, linear_zero_wav = render(
        "linear-zero-depth",
        base_request({"depthMs": 0.0, "interpolation": "linear"}),
    )
    if linear_zero_wav != omitted_wav:
        raise AssertionError(
            "zero-depth linear-interpolated Modulation rendered output "
            "was not bit-identical to Modulation omitted"
        )

    # Repeat renders of an identical linear-interpolated configuration
    # are exact.
    linear_repeat_resolved, linear_repeat_wav = render(
        "linear-repeat",
        base_request({"depthMs": 5.0, "rateHz": 3.0, "interpolation": "linear"}),
    )
    if linear_repeat_wav != linear_wav:
        raise AssertionError(
            "repeat renders of an identical linear-interpolated "
            "Modulation configuration were not exact"
        )

    # allpass resolves and renders end to end (issue #93): a deliberate
    # ablation exposing the transient artefacts a moving allpass-
    # interpolated read produces inside a circulating loop -- flat
    # magnitude at any fixed fractional delay, but the filter's own
    # persistent state is repeatedly invalidated by a moving one.
    allpass_resolved, allpass_wav = render(
        "allpass",
        base_request({"depthMs": 5.0, "rateHz": 3.0, "interpolation": "allpass"}),
    )
    allpass_loop = feedback_loop_stage(allpass_resolved)
    if allpass_loop["modulation"]["interpolation"] != "allpass":
        raise AssertionError(
            f"requested allpass interpolation did not round-trip: "
            f"{allpass_loop['modulation']}"
        )
    if allpass_wav == omitted_wav:
        raise AssertionError(
            "allpass-interpolated Modulation rendered output identical to "
            "the unmodulated baseline"
        )
    if allpass_wav == active_wav or allpass_wav == linear_wav:
        raise AssertionError(
            "allpass-interpolated Modulation rendered output identical to "
            "lagrange3 or linear at the same depthMs/rateHz"
        )
    # Resolved buffer sizes are unchanged from the other interpolation
    # methods: allpass's own per-Channel filter *state* is separate from
    # (and does not grow) the delay-line *buffer*.
    if allpass_loop["bufferSizes"] != active_loop["bufferSizes"]:
        raise AssertionError(
            f"allpass interpolation resolved different buffer sizes than "
            f"lagrange3 at the same depthMs: {allpass_loop['bufferSizes']} "
            f"!= {active_loop['bufferSizes']}"
        )

    # depthMs of 0 remains bit-identical to Modulation omitted under
    # allpass too -- this is the case that makes bypass-by-construction
    # necessary, since allpass state cannot collapse to identity
    # arithmetically the way lagrange3 and linear do.
    _, allpass_zero_wav = render(
        "allpass-zero-depth",
        base_request({"depthMs": 0.0, "interpolation": "allpass"}),
    )
    if allpass_zero_wav != omitted_wav:
        raise AssertionError(
            "zero-depth allpass-interpolated Modulation rendered output "
            "was not bit-identical to Modulation omitted"
        )

    # Repeat renders of an identical allpass-interpolated configuration
    # are exact.
    _, allpass_repeat_wav = render(
        "allpass-repeat",
        base_request({"depthMs": 5.0, "rateHz": 3.0, "interpolation": "allpass"}),
    )
    if allpass_repeat_wav != allpass_wav:
        raise AssertionError(
            "repeat renders of an identical allpass-interpolated "
            "Modulation configuration were not exact"
        )

    # Per-Channel allpass interpolator state is allocated at
    # configuration and counts toward the owning stage's DSP-owned
    # memory (via the benchmark tool's exact dspOwnedBytes accounting),
    # and bypassed Channels allocate none of it at all: a fully modulated
    # allpass Composition owns more bytes than an identically shaped
    # lagrange3 one, and a half-modulated allpass Composition owns fewer
    # bytes than a fully modulated one at the same Channel count.
    def dsp_owned_bytes(name):
        resolved_path = workspace / f"{name}-result" / "resolved.json"
        report_path = workspace / f"{name}-benchmark-report.json"
        completed = run_benchmark_ok(
            renderer,
            "--resolved",
            resolved_path,
            "--block-size",
            "64",
            "--warmup-seconds",
            "0",
            "--measure-seconds",
            "0.01",
            "--json",
            report_path,
        )
        return json.loads(completed.stdout)["dspOwnedBytes"]

    render(
        "allpass-full-fraction",
        base_request(
            {"depthMs": 1.0, "interpolation": "allpass", "channelFraction": 1.0},
            channels=8,
        ),
    )
    render(
        "allpass-half-fraction",
        base_request(
            {"depthMs": 1.0, "interpolation": "allpass", "channelFraction": 0.5},
            channels=8,
        ),
    )
    render(
        "lagrange3-full-fraction",
        base_request({"depthMs": 1.0, "channelFraction": 1.0}, channels=8),
    )
    allpass_full_bytes = dsp_owned_bytes("allpass-full-fraction")
    allpass_half_bytes = dsp_owned_bytes("allpass-half-fraction")
    lagrange3_full_bytes = dsp_owned_bytes("lagrange3-full-fraction")
    if allpass_full_bytes <= lagrange3_full_bytes:
        raise AssertionError(
            f"a fully modulated allpass Composition did not own more "
            f"bytes than an identically shaped lagrange3 one: "
            f"{allpass_full_bytes} <= {lagrange3_full_bytes}"
        )
    if allpass_half_bytes >= allpass_full_bytes:
        raise AssertionError(
            f"a half-modulated allpass Composition did not own fewer "
            f"bytes than a fully modulated one at the same Channel "
            f"count: {allpass_half_bytes} >= {allpass_full_bytes}"
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

    # sine and triangle shapes resolve and render end to end (issue #90),
    # each producing output that actually differs from the unmodulated
    # baseline.
    for shape in ("sine", "triangle"):
        shape_resolved, shape_wav = render(
            f"shape-{shape}",
            base_request({"depthMs": 5.0, "rateHz": 3.0, "shape": shape}),
        )
        shape_modulation = feedback_loop_stage(shape_resolved)["modulation"]
        if shape_modulation["shape"] != shape:
            raise AssertionError(
                f"requested shape {shape!r} did not round-trip: "
                f"{shape_modulation}"
            )
        if shape_wav == omitted_wav:
            raise AssertionError(
                f"{shape} Modulation rendered output identical to the "
                f"unmodulated baseline"
            )

    # An unsupported shape value is rejected by name.
    bad_shape_request = base_request({"shape": "square"})
    bad_shape_path = workspace / "bad-shape-request.json"
    bad_shape_path.write_text(json.dumps(bad_shape_request))
    bad_shape = run(
        renderer,
        "--input",
        fixture,
        "--config",
        bad_shape_path,
        "--output",
        workspace / "bad-shape-result",
    )
    if bad_shape.returncode == 0:
        raise AssertionError("renderer accepted an unsupported shape")
    if "smoothed-random" not in bad_shape.stderr:
        raise AssertionError(
            f"unsupported shape was not rejected by name: {bad_shape.stderr}"
        )

    # channelFraction resolves the modulated Channels as the first
    # ceil(fraction * N) entries of a positionally seeded fixed
    # permutation (issue #90): raising the fraction only adds Channels,
    # never reshuffling ones already selected.
    quarter_resolved, _ = render(
        "quarter-fraction",
        base_request(
            {"depthMs": 1.0, "channelFraction": 0.25}, channels=8
        ),
    )
    quarter_loop = feedback_loop_stage(quarter_resolved)
    quarter_mask = quarter_loop["modulation"]["channelModulated"]
    if len(quarter_mask) != quarter_loop["channels"] or sum(quarter_mask) != 2:
        raise AssertionError(
            f"channelFraction 0.25 over 8 Channels did not resolve "
            f"ceil(0.25 * 8) = 2 modulated Channels: {quarter_mask}"
        )

    half_resolved, _ = render(
        "half-fraction",
        base_request({"depthMs": 1.0, "channelFraction": 0.5}, channels=8),
    )
    half_mask = feedback_loop_stage(half_resolved)["modulation"][
        "channelModulated"
    ]
    if sum(half_mask) != 4:
        raise AssertionError(
            f"channelFraction 0.5 over 8 Channels did not resolve "
            f"ceil(0.5 * 8) = 4 modulated Channels: {half_mask}"
        )
    for channel, was_modulated in enumerate(quarter_mask):
        if was_modulated and not half_mask[channel]:
            raise AssertionError(
                f"raising channelFraction dropped Channel {channel} that "
                f"a smaller fraction had selected"
            )

    # channelFraction of 0 disables Modulation for the stage, bit-
    # identical to Modulation omitted.
    zero_fraction_resolved, zero_fraction_wav = render(
        "zero-fraction",
        base_request({"depthMs": 5.0, "channelFraction": 0.0}),
    )
    zero_fraction_modulation = feedback_loop_stage(zero_fraction_resolved)[
        "modulation"
    ]
    if zero_fraction_modulation["channelModulated"]:
        raise AssertionError(
            f"channelFraction of 0 still resolved a non-empty bypass "
            f"mask: {zero_fraction_modulation}"
        )
    if zero_fraction_wav != omitted_wav:
        raise AssertionError(
            "channelFraction of 0 rendered output was not bit-identical "
            "to Modulation omitted"
        )

    # An out-of-range channelFraction is rejected.
    bad_fraction_request = base_request({"channelFraction": 1.5})
    bad_fraction_path = workspace / "bad-fraction-request.json"
    bad_fraction_path.write_text(json.dumps(bad_fraction_request))
    bad_fraction = run(
        renderer,
        "--input",
        fixture,
        "--config",
        bad_fraction_path,
        "--output",
        workspace / "bad-fraction-result",
    )
    if bad_fraction.returncode == 0:
        raise AssertionError("renderer accepted a channelFraction above 1")
    if "channelFraction" not in bad_fraction.stderr:
        raise AssertionError(
            f"out-of-range channelFraction was not named in the "
            f"rejection: {bad_fraction.stderr}"
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
