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


def diffuser_stage(resolved: dict) -> dict:
    return next(
        stage
        for stage in resolved["composition"]["stages"]
        if stage["type"] == "diffuser"
    )


def feedback_loop_stage(resolved: dict) -> dict:
    return next(
        stage
        for stage in resolved["composition"]["stages"]
        if stage["type"] == "feedback-loop"
    )


def base_request(
    step_defaults=None,
    step_overrides=None,
    channels=8,
    total_ms=60.0,
    steps=2,
    loop_modulation=None,
) -> dict:
    step = {
        "delayStrategy": "segmented-random",
        "mix": "hadamard",
        "shuffle": True,
        "polarity": "seeded-random",
    }
    if step_defaults is not None:
        step["modulation"] = step_defaults
    diffuser = {
        "type": "diffuser",
        "steps": steps,
        "totalMs": total_ms,
        "distribution": "even",
        "step": step,
    }
    if step_overrides is not None:
        diffuser["stepOverrides"] = step_overrides
    loop = {
        "type": "feedback-loop",
        "delayMinMs": 40.0,
        "delayMaxMs": 60.0,
        "delayStrategy": "even",
        "rt60Sec": 1.0,
        "mix": "householder",
    }
    if loop_modulation is not None:
        loop["modulation"] = loop_modulation
    return {
        "formatVersion": 1,
        "seed": 11,
        "composition": {
            "stages": [
                {
                    "type": "split",
                    "channels": channels,
                    "strategy": "duplicate",
                    "normalisation": "energy",
                },
                diffuser,
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

    # Modulation omitted from both the shared step defaults and every
    # override: resolved.json carries no "modulation" key on any step (see
    # docs/design/reverb/stages/06-modulation.md's "modulation is optional
    # wherever it appears").
    omitted_resolved, omitted_wav = render("omitted", base_request())
    omitted_diffuser = diffuser_stage(omitted_resolved)
    for step in omitted_diffuser["steps"]:
        if "modulation" in step:
            raise AssertionError(
                f"omitted Modulation still resolved a modulation object on "
                f"step {step['index']}: {step}"
            )

    # Modulation configured in the shared step defaults applies to every
    # step (acceptance criterion: accepted in both shared defaults and
    # per-step overrides, exactly as every other per-step field).
    defaults_resolved, defaults_wav = render(
        "defaults", base_request(step_defaults={"depthMs": 0.3, "rateHz": 4.0})
    )
    defaults_diffuser = diffuser_stage(defaults_resolved)
    for step in defaults_diffuser["steps"]:
        if "modulation" not in step:
            raise AssertionError(
                f"shared step default Modulation did not apply to step "
                f"{step['index']}: {step}"
            )
    if defaults_wav == omitted_wav:
        raise AssertionError(
            "Modulation configured in the shared step defaults rendered "
            "output identical to Modulation omitted"
        )

    # A per-step override's own Modulation wins over the shared defaults,
    # and a step that specifies no override at all still inherits the
    # shared default.
    override_resolved, _ = render(
        "override",
        base_request(
            step_defaults={"depthMs": 0.3, "rateHz": 4.0},
            step_overrides=[
                {"index": 0, "modulation": {"depthMs": 1.0, "rateHz": 2.0}},
            ],
        ),
    )
    override_diffuser = diffuser_stage(override_resolved)
    step0 = override_diffuser["steps"][0]
    step1 = override_diffuser["steps"][1]
    if step0["modulation"]["depthMs"] != 1.0 or step0["modulation"]["rateHz"] != 2.0:
        raise AssertionError(
            f"per-step override Modulation did not win over the shared "
            f"defaults: {step0['modulation']}"
        )
    if step1["modulation"]["depthMs"] != 0.3 or step1["modulation"]["rateHz"] != 4.0:
        raise AssertionError(
            f"a step without its own override did not inherit the shared "
            f"default Modulation: {step1['modulation']}"
        )

    # An included but otherwise empty Modulation object resolves the
    # documented research baseline, exactly as on the Feedback Loop.
    baseline_resolved, _ = render(
        "baseline",
        base_request(step_overrides=[{"index": 0, "modulation": {}}]),
    )
    baseline_modulation = diffuser_stage(baseline_resolved)["steps"][0][
        "modulation"
    ]
    if (
        baseline_modulation["depthMs"] != 0.4
        or baseline_modulation["rateHz"] != 0.7
        or baseline_modulation["shape"] != "smoothed-random"
        or baseline_modulation["channelFraction"] != 1.0
        or baseline_modulation["interpolation"] != "lagrange3"
    ):
        raise AssertionError(
            f"an empty Modulation object on a Diffusion Step did not "
            f"resolve the documented research baseline: "
            f"{baseline_modulation}"
        )

    # Explicit zero depth on a step is the resolved bypass: it records
    # evidence that Modulation was configured but renders bit-identical to
    # Modulation omitted from that step.
    zero_resolved, zero_wav = render(
        "zero-depth",
        base_request(step_overrides=[{"index": 0, "modulation": {"depthMs": 0.0}}]),
    )
    zero_step = diffuser_stage(zero_resolved)["steps"][0]
    if "modulation" not in zero_step:
        raise AssertionError("explicit zero depth did not resolve Modulation")
    if (
        zero_step["modulation"]["channelSeeds"]
        or zero_step["modulation"]["channelTargetsPerSample"]
        or zero_step["modulation"]["channelPhases"]
    ):
        raise AssertionError(
            f"zero-depth step Modulation resolved per-Channel seeds/rates/"
            f"phases it should have bypassed: {zero_step['modulation']}"
        )
    if zero_step["bufferSizes"] != omitted_diffuser["steps"][0]["bufferSizes"]:
        raise AssertionError(
            "zero-depth step Modulation grew the resolved buffer sizes "
            "over Modulation omitted"
        )
    if zero_wav != omitted_wav:
        raise AssertionError(
            "zero-depth step Modulation rendered output was not "
            "bit-identical to Modulation omitted"
        )

    # Active Modulation on a Diffusion Step moves that step's delays and
    # renders end to end: buffer headroom grows by Excursion plus the
    # fixed Interpolation margin only on the modulated step, one seed per
    # Channel resolves, and the rendered output differs from the
    # unmodulated baseline.
    active_resolved, active_wav = render(
        "active",
        base_request(
            step_overrides=[
                {"index": 0, "modulation": {"depthMs": 0.4, "rateHz": 5.0}},
            ]
        ),
    )
    active_diffuser = diffuser_stage(active_resolved)
    active_step0 = active_diffuser["steps"][0]
    active_step1 = active_diffuser["steps"][1]
    channels = len(active_step0["delaysSamples"])
    active_modulation = active_step0["modulation"]
    if (
        len(active_modulation["channelSeeds"]) != channels
        or len(active_modulation["channelTargetsPerSample"]) != channels
        or len(active_modulation["channelPhases"]) != channels
    ):
        raise AssertionError(
            f"active step Modulation did not resolve one seed/rate/phase "
            f"per Channel: {active_modulation}"
        )
    sample_rate = 48000
    expected_excursion = 0.4 * sample_rate / 1000.0
    margin = active_modulation["interpolationMarginSamples"]
    for delay, buffer_size in zip(
        active_step0["delaysSamples"], active_step0["bufferSizes"]
    ):
        expected_buffer_size = delay + math.ceil(expected_excursion) + margin
        if buffer_size != expected_buffer_size:
            raise AssertionError(
                f"active step Modulation did not reserve Excursion plus "
                f"the fixed Interpolation margin: {active_step0}"
            )
    # Buffer headroom is added only to the step actually modulated (issue
    # #91's acceptance criteria) -- the unmodulated step 1 keeps its
    # buffer sizes exactly equal to its delays.
    if "modulation" in active_step1:
        raise AssertionError(
            f"step 1 resolved Modulation when none was configured for it: "
            f"{active_step1}"
        )
    if active_step1["bufferSizes"] != active_step1["delaysSamples"]:
        raise AssertionError(
            "an unmodulated step's buffer sizes grew even though it has "
            "no Modulation configured"
        )
    if active_wav == omitted_wav:
        raise AssertionError(
            "active step Modulation rendered output identical to the "
            "unmodulated baseline"
        )

    # Diffusion Step trajectories are seeded per step and per Channel:
    # two steps modulated with identical parameters never share a
    # trajectory, and neither does a modulated step share one with the
    # Feedback Loop's own Modulation even when both resolve itemIndex 0
    # internally.
    both_steps_resolved, _ = render(
        "both-steps",
        base_request(
            step_defaults={"depthMs": 0.4, "rateHz": 5.0},
            loop_modulation={"depthMs": 0.4, "rateHz": 0.7},
        ),
    )
    both_steps_diffuser = diffuser_stage(both_steps_resolved)
    seeds_step0 = both_steps_diffuser["steps"][0]["modulation"]["channelSeeds"]
    seeds_step1 = both_steps_diffuser["steps"][1]["modulation"]["channelSeeds"]
    seeds_loop = feedback_loop_stage(both_steps_resolved)["modulation"][
        "channelSeeds"
    ]
    if seeds_step0 == seeds_step1:
        raise AssertionError(
            "two modulated Diffusion Steps with identical parameters "
            "shared the exact same per-Channel trajectory seeds"
        )
    if seeds_step0 == seeds_loop or seeds_step1 == seeds_loop:
        raise AssertionError(
            "a modulated Diffusion Step shared the exact same per-Channel "
            "trajectory seeds as the Feedback Loop's own Modulation"
        )

    # Feedback Loop Modulation and Diffusion Step Modulation resolve
    # independently and may carry different values in the same
    # Composition -- this is what makes comparing one-shot detuning
    # against compounding detuning possible in a single render.
    independent_resolved, independent_wav = render(
        "independent",
        base_request(
            step_overrides=[
                {"index": 0, "modulation": {"depthMs": 0.4, "rateHz": 9.0}},
            ],
            loop_modulation={"depthMs": 5.0, "rateHz": 3.0},
        ),
    )
    independent_step_modulation = diffuser_stage(independent_resolved)["steps"][
        0
    ]["modulation"]
    independent_loop_modulation = feedback_loop_stage(independent_resolved)[
        "modulation"
    ]
    if (
        independent_step_modulation["depthMs"] == independent_loop_modulation["depthMs"]
        or independent_step_modulation["rateHz"]
        == independent_loop_modulation["rateHz"]
    ):
        raise AssertionError(
            "Diffusion Step and Feedback Loop Modulation did not resolve "
            "independent values in the same Composition"
        )
    if independent_wav == omitted_wav:
        raise AssertionError(
            "a Composition with both Modulations active rendered output "
            "identical to Modulation omitted entirely"
        )

    # The Excursion rejection rule applies identically to a Diffusion
    # Step, naming the responsible step's own Modulation parameter path.
    too_short_request = base_request(
        step_overrides=[{"index": 0, "modulation": {"depthMs": 50.0}}],
        total_ms=4.0,
    )
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
            "renderer accepted a Diffusion Step delay too short for the "
            "requested Excursion and Interpolation margin"
        )
    if "/steps/0/modulation/depthMs" not in too_short.stderr:
        raise AssertionError(
            f"short-delay rejection did not name the responsible step's "
            f"own Modulation parameter path: {too_short.stderr}"
        )
    if too_short_result.exists():
        raise AssertionError("rejected configuration created a Render Result")

    # Repeat renders of an identical active-Modulation configuration are
    # exact.
    repeat_resolved, repeat_wav = render(
        "active-repeat",
        base_request(
            step_overrides=[
                {"index": 0, "modulation": {"depthMs": 0.4, "rateHz": 5.0}},
            ]
        ),
    )
    if repeat_wav != active_wav:
        raise AssertionError(
            "repeat renders of an identical Diffusion Step Modulation "
            "configuration were not exact"
        )


if __name__ == "__main__":
    main()
