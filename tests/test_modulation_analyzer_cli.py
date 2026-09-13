#!/usr/bin/env python3

import hashlib
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path


def run_renderer(renderer, request_path, output, fixture, block_size=8):
    completed = subprocess.run(
        [
            str(renderer),
            "render",
            "--input",
            str(fixture),
            "--config",
            str(request_path),
            "--output",
            str(output),
            "--block-size",
            str(block_size),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)
    return completed


def run_analyzer(analyzer, render_result, fixture, source=None):
    return subprocess.run(
        [
            sys.executable,
            str(analyzer),
            str(render_result),
            "--source",
            str(source or fixture),
        ],
        check=False,
        capture_output=True,
        text=True,
    )


def feedback_loop_request(
    channels=2, damping=None, modulation=None, seed=7, **overrides
):
    loop = {
        "type": "feedback-loop",
        "delayMinMs": 1.0,
        "delayMaxMs": 2.0,
        "delayStrategy": "even",
        "rt60Sec": 1.0,
        "mix": "householder",
    }
    if damping is not None:
        loop["damping"] = damping
    if modulation is not None:
        loop["modulation"] = modulation
    loop.update(overrides)
    return {
        "formatVersion": 2,
        "seed": seed,
        "composition": {
            "stages": [
                {
                    "type": "split",
                    "channels": channels,
                    "strategy": "duplicate",
                    "normalisation": "energy",
                },
                loop,
                {"type": "downmix", "strategy": "select", "leftChannel": 0, "rightChannel": 1},
            ]
        },
    }


def main():
    tail_analyzer = Path(sys.argv[1])
    tail_v2_analyzer = Path(sys.argv[2])
    modulation_analyzer = Path(sys.argv[3])
    renderer = Path(sys.argv[4])
    fixture = Path(sys.argv[5])
    workspace = Path(sys.argv[6])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    # N=2, delayStrategy "even": the same millisecond-scale CI tracer used
    # throughout, with a research-baseline Damping object and an active
    # Feedback Loop Modulation.
    request_path = workspace / "request.json"
    request_path.write_text(
        json.dumps(
            feedback_loop_request(
                damping={"highRatio": 0.5, "highHz": 4000, "lowRatio": 1.0},
                modulation={
                    "depthMs": 0.4,
                    "rateHz": 0.7,
                    "shape": "sine",
                    "interpolation": "linear",
                },
            )
        )
    )
    render_result = workspace / "render-result"
    run_renderer(renderer, request_path, render_result, fixture)

    tail_analyzed = run_analyzer(tail_analyzer, render_result, fixture)
    if tail_analyzed.returncode != 0:
        raise AssertionError(tail_analyzed.stderr)
    tail_v1_artifact = render_result / "analysis" / "tail-v1.json"
    tail_v1_contents = tail_v1_artifact.read_bytes()

    tail_v2_analyzed = run_analyzer(tail_v2_analyzer, render_result, fixture)
    if tail_v2_analyzed.returncode != 0:
        raise AssertionError(tail_v2_analyzed.stderr)
    tail_v2_artifact = render_result / "analysis" / "tail-v2.json"
    tail_v2_contents = tail_v2_artifact.read_bytes()

    analyzed = run_analyzer(modulation_analyzer, render_result, fixture)
    if analyzed.returncode != 0:
        raise AssertionError(analyzed.stderr)

    # modulation-v1 is append-only: it publishes beside tail-v1/tail-v2
    # without touching either.
    if tail_v1_artifact.read_bytes() != tail_v1_contents:
        raise AssertionError("modulation-v1 analysis modified the tail-v1 artifact")
    if tail_v2_artifact.read_bytes() != tail_v2_contents:
        raise AssertionError("modulation-v1 analysis modified the tail-v2 artifact")

    artifact = render_result / "analysis" / "modulation-v1.json"
    analysis = json.loads(artifact.read_text())

    if analysis["analyzer"] != "modulation" or analysis["analyzerVersion"] != 1:
        raise AssertionError(f"unexpected analyzer header: {analysis}")
    if analysis["source"] != {
        "filename": fixture.name,
        "sha256": hashlib.sha256(fixture.read_bytes()).hexdigest(),
        "verified": True,
    }:
        raise AssertionError(f"unexpected source provenance: {analysis}")
    if analysis["nonFiniteSampleCount"] != 0:
        raise AssertionError(f"unexpected non-finite samples: {analysis}")

    if analysis["modulationPresent"] is not True:
        raise AssertionError(f"modulationPresent was not True: {analysis}")
    if analysis["activeModulations"] != [
        {
            "owner": "feedback-loop",
            "depthMs": 0.4,
            "rateHz": 0.7,
            "shape": "sine",
            "channelFraction": 1.0,
            "interpolation": "linear",
        }
    ]:
        raise AssertionError(f"unexpected activeModulations: {analysis}")

    bounded_energy = analysis["boundedEnergy"]
    if bounded_energy["negativeTrend"] is not True:
        raise AssertionError(
            f"bounded-energy evidence did not show a negative late-tail "
            f"trend: {bounded_energy}"
        )
    if bounded_energy["segmentCount"] != 16:
        raise AssertionError(f"unexpected bounded-energy segment count: {bounded_energy}")

    decay = analysis["decay"]
    expected_centers = [
        63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0,
    ]
    if [band["centerHz"] for band in decay["bands"]] != expected_centers:
        raise AssertionError(f"unexpected octave-band centers: {decay['bands']}")
    for band in decay["bands"]:
        predicted = band["predictedRt60Sec"]
        if len(predicted["perChannelRt60Sec"]) != 2:
            raise AssertionError(f"unexpected predicted per-Channel count: {band}")
        if "withinPredictedTolerance" not in band:
            raise AssertionError(f"missing withinPredictedTolerance: {band}")

    decay_tilt = analysis["decayTilt"]
    if decay_tilt["bandCount"] < 2:
        raise AssertionError(f"decay tilt fit had too few bands: {decay_tilt}")
    if not isinstance(decay_tilt["slopeRatioPerOctave"], float):
        raise AssertionError(f"decay tilt slope was not a float: {decay_tilt}")
    if not isinstance(decay_tilt["significant"], bool):
        raise AssertionError(f"decay tilt significance was not a bool: {decay_tilt}")

    coherent_pitch_movement = analysis["coherentPitchMovement"]
    rates = coherent_pitch_movement["rates"]
    if len(rates) != 1 or rates[0]["owner"] != "feedback-loop" or rates[0]["rateHz"] != 0.7:
        raise AssertionError(f"unexpected coherent-pitch-movement rates: {rates}")
    if rates[0]["relativeMagnitude"] is None or rates[0]["relativeMagnitude"] <= 1.0:
        raise AssertionError(
            "active Modulation did not show elevated instantaneous-frequency "
            f"magnitude at its own rate: {rates[0]}"
        )
    if rates[0]["significant"] is not True:
        raise AssertionError(
            f"a clearly elevated rate was not flagged as significant: {rates[0]}"
        )

    output_correlation = analysis["outputCorrelation"]
    if len(output_correlation["matrix"]) != 2:
        raise AssertionError(f"unexpected outputCorrelation shape: {output_correlation}")
    if not isinstance(output_correlation["significant"], bool):
        raise AssertionError(
            f"outputCorrelation significance was not a bool: {output_correlation}"
        )

    # Idempotent: repeating the analysis does not rewrite the artifact.
    original = artifact.read_bytes()
    modified = artifact.stat().st_mtime_ns
    time.sleep(0.01)
    repeated = run_analyzer(modulation_analyzer, render_result, fixture)
    if repeated.returncode != 0:
        raise AssertionError(repeated.stderr)
    if artifact.read_bytes() != original or artifact.stat().st_mtime_ns != modified:
        raise AssertionError("idempotent modulation analysis rewrote its artifact")

    # A source that does not match render.json's recorded SHA-256 is
    # rejected without changing the published artifact.
    wrong_source = workspace / "wrong.wav"
    wrong_source.write_bytes(fixture.read_bytes() + b"\x00")
    rejected = run_analyzer(
        modulation_analyzer, render_result, fixture, source=wrong_source
    )
    if rejected.returncode == 0:
        raise AssertionError("modulation analyzer accepted wrong source")
    if "source SHA-256 does not match render metadata" not in rejected.stderr:
        raise AssertionError(f"unexpected provenance failure: {rejected.stderr}")
    if artifact.read_bytes() != original:
        raise AssertionError("failed provenance check changed analysis")

    # Inconsistent resolved Modulation evidence is rejected descriptively,
    # before any measurement math runs.
    def reject_corrupted_modulation(name, corrupt):
        corrupted_result = workspace / f"corrupted-modulation-{name}-result"
        shutil.copytree(render_result, corrupted_result)
        (corrupted_result / "analysis" / "modulation-v1.json").unlink()
        corrupted_path = corrupted_result / "resolved.json"
        corrupted = json.loads(corrupted_path.read_text())
        corrupted_loop = next(
            stage
            for stage in corrupted["composition"]["stages"]
            if stage["type"] == "feedback-loop"
        )
        corrupt(corrupted_loop)
        corrupted_path.write_text(json.dumps(corrupted))
        corrupted_analyzed = run_analyzer(
            modulation_analyzer, corrupted_result, fixture
        )
        if corrupted_analyzed.returncode == 0:
            raise AssertionError(
                f"modulation analyzer accepted inconsistent evidence: {name}"
            )
        if (
            "resolved Modulation evidence is inconsistent"
            not in corrupted_analyzed.stderr
        ):
            raise AssertionError(
                f"unexpected inconsistent-Modulation failure ({name}): "
                f"{corrupted_analyzed.stderr}"
            )
        if (corrupted_result / "analysis" / "modulation-v1.json").exists():
            raise AssertionError(
                f"rejected Modulation evidence published a modulation-v1 "
                f"analysis: {name}"
            )

    reject_corrupted_modulation(
        "non-finite-rate",
        lambda loop: loop["modulation"].__setitem__("rateHz", float("nan")),
    )
    reject_corrupted_modulation(
        "unrecognized-shape",
        lambda loop: loop["modulation"].__setitem__("shape", "square"),
    )
    reject_corrupted_modulation(
        "short-channel-modulated",
        lambda loop: loop["modulation"]["channelModulated"].pop(),
    )
    reject_corrupted_modulation(
        "non-object", lambda loop: loop.update(modulation=[])
    )

    # A Composition without a Feedback Loop is rejected.
    diffuser_only_request = {
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
    }
    diffuser_only_path = workspace / "diffuser-only-request.json"
    diffuser_only_path.write_text(json.dumps(diffuser_only_request))
    diffuser_only_result = workspace / "diffuser-only-result"
    run_renderer(renderer, diffuser_only_path, diffuser_only_result, fixture)
    rejected_composition = run_analyzer(
        modulation_analyzer, diffuser_only_result, fixture
    )
    if rejected_composition.returncode == 0:
        raise AssertionError(
            "modulation analyzer accepted a Composition without a Feedback Loop"
        )
    if "modulation analysis requires a Feedback Loop" not in rejected_composition.stderr:
        raise AssertionError(
            f"unexpected composition rejection: {rejected_composition.stderr}"
        )

    # An unmodulated render analyzes successfully and reports Modulation
    # as absent; the movement-agnostic evidence (bounded energy, decay
    # tilt, Output correlation) is still measured against the still-static
    # nominal prediction.
    undamped_path = workspace / "unmodulated-request.json"
    undamped_path.write_text(json.dumps(feedback_loop_request()))
    unmodulated_result = workspace / "unmodulated-result"
    run_renderer(renderer, undamped_path, unmodulated_result, fixture, block_size=32)
    unmodulated_analyzed = run_analyzer(
        modulation_analyzer, unmodulated_result, fixture
    )
    if unmodulated_analyzed.returncode != 0:
        raise AssertionError(unmodulated_analyzed.stderr)
    unmodulated_analysis = json.loads(
        (unmodulated_result / "analysis" / "modulation-v1.json").read_text()
    )
    if unmodulated_analysis["modulationPresent"] is not False:
        raise AssertionError(
            f"modulationPresent was not False for an unmodulated render: "
            f"{unmodulated_analysis}"
        )
    if unmodulated_analysis["activeModulations"] != []:
        raise AssertionError(
            f"an unmodulated render reported active Modulations: "
            f"{unmodulated_analysis}"
        )
    if unmodulated_analysis["coherentPitchMovement"]["rates"] != []:
        raise AssertionError(
            f"an unmodulated render reported configured rates: "
            f"{unmodulated_analysis}"
        )

    # A Diffusion Step's own Modulation (Diffuser + Feedback Loop
    # Composition) is reported under its own distinct owner, alongside the
    # Feedback Loop's.
    combined_request = {
        "formatVersion": 2,
        "seed": 11,
        "composition": {
            "stages": [
                {
                    "type": "split",
                    "channels": 2,
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
                        "modulation": {
                            "depthMs": 0.2,
                            "rateHz": 1.1,
                            "shape": "triangle",
                            "interpolation": "lagrange3",
                        },
                    },
                },
                {
                    "type": "feedback-loop",
                    "delayMinMs": 1.0,
                    "delayMaxMs": 2.0,
                    "delayStrategy": "even",
                    "rt60Sec": 1.0,
                    "mix": "householder",
                    "modulation": {
                        "depthMs": 0.4,
                        "rateHz": 0.7,
                        "shape": "sine",
                        "interpolation": "linear",
                    },
                },
                {"type": "downmix", "strategy": "select", "leftChannel": 0, "rightChannel": 1},
            ]
        },
    }
    combined_path = workspace / "combined-request.json"
    combined_path.write_text(json.dumps(combined_request))
    combined_result = workspace / "combined-result"
    run_renderer(renderer, combined_path, combined_result, fixture)
    combined_analyzed = run_analyzer(modulation_analyzer, combined_result, fixture)
    if combined_analyzed.returncode != 0:
        raise AssertionError(combined_analyzed.stderr)
    combined_analysis = json.loads(
        (combined_result / "analysis" / "modulation-v1.json").read_text()
    )
    owners = {entry["owner"] for entry in combined_analysis["activeModulations"]}
    if owners != {"feedback-loop", "diffusion-step[0]"}:
        raise AssertionError(f"unexpected combined owners: {owners}")
    rate_owners = {
        rate["owner"] for rate in combined_analysis["coherentPitchMovement"]["rates"]
    }
    if rate_owners != {"feedback-loop", "diffusion-step[0]"}:
        raise AssertionError(
            f"unexpected combined coherent-pitch-movement owners: {rate_owners}"
        )

    # depthMs: 0 and channelFraction: 0 are both resolved bypasses
    # (src/config/ResolveConfig.cpp): the Modulation object stays present
    # in resolved.json but its channelModulated is empty rather than one
    # flag per Channel. Both explicit-zero forms must analyze successfully
    # and report Modulation as absent, not crash on the shape mismatch.
    def assert_bypass_analyzes_as_absent(name, request):
        request_path = workspace / f"{name}-request.json"
        request_path.write_text(json.dumps(request))
        result = workspace / f"{name}-result"
        run_renderer(renderer, request_path, result, fixture, block_size=32)
        analyzed = run_analyzer(modulation_analyzer, result, fixture)
        if analyzed.returncode != 0:
            raise AssertionError(f"{name}: {analyzed.stderr}")
        analysis = json.loads((result / "analysis" / "modulation-v1.json").read_text())
        if analysis["modulationPresent"] is not False:
            raise AssertionError(
                f"{name}: modulationPresent was not False for a resolved "
                f"bypass: {analysis}"
            )
        if analysis["activeModulations"] != []:
            raise AssertionError(
                f"{name}: a resolved bypass reported active Modulations: "
                f"{analysis}"
            )

    assert_bypass_analyzes_as_absent(
        "zero-depth", feedback_loop_request(modulation={"depthMs": 0.0})
    )
    assert_bypass_analyzes_as_absent(
        "zero-channel-fraction",
        feedback_loop_request(
            modulation={"depthMs": 0.4, "rateHz": 0.7, "channelFraction": 0.0}
        ),
    )

    # A bypassed Diffusion Step Modulation alongside an active Feedback
    # Loop Modulation: only the Feedback Loop counts as active.
    diffuser_bypass_request = {
        "formatVersion": 2,
        "seed": 13,
        "composition": {
            "stages": [
                {
                    "type": "split",
                    "channels": 2,
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
                        "modulation": {"depthMs": 0.0},
                    },
                },
                {
                    "type": "feedback-loop",
                    "delayMinMs": 1.0,
                    "delayMaxMs": 2.0,
                    "delayStrategy": "even",
                    "rt60Sec": 1.0,
                    "mix": "householder",
                    "modulation": {
                        "depthMs": 0.4,
                        "rateHz": 0.7,
                        "shape": "sine",
                        "interpolation": "linear",
                    },
                },
                {"type": "downmix", "strategy": "select", "leftChannel": 0, "rightChannel": 1},
            ]
        },
    }
    diffuser_bypass_path = workspace / "diffuser-bypass-request.json"
    diffuser_bypass_path.write_text(json.dumps(diffuser_bypass_request))
    diffuser_bypass_result = workspace / "diffuser-bypass-result"
    run_renderer(renderer, diffuser_bypass_path, diffuser_bypass_result, fixture)
    diffuser_bypass_analyzed = run_analyzer(
        modulation_analyzer, diffuser_bypass_result, fixture
    )
    if diffuser_bypass_analyzed.returncode != 0:
        raise AssertionError(diffuser_bypass_analyzed.stderr)
    diffuser_bypass_analysis = json.loads(
        (diffuser_bypass_result / "analysis" / "modulation-v1.json").read_text()
    )
    bypass_owners = {
        entry["owner"] for entry in diffuser_bypass_analysis["activeModulations"]
    }
    if bypass_owners != {"feedback-loop"}:
        raise AssertionError(
            f"a bypassed Diffusion Step Modulation was reported as active: "
            f"{bypass_owners}"
        )

    # A nonzero Pre-delay (issue #133) extends the render's own total
    # drain past what tailBudgetFrames alone authorises; the analyzer's
    # own expected-frames check, and its bounded-energy tail window,
    # must both account for preDelayFrames rather than rejecting the
    # renderer's own valid, longer output or measuring "post-input"
    # decay while delayed source material is still arriving at Split.
    pre_delay_document = feedback_loop_request(
        damping={"highRatio": 0.5, "highHz": 4000, "lowRatio": 1.0},
        modulation={
            "depthMs": 0.4,
            "rateHz": 0.7,
            "shape": "sine",
            "interpolation": "linear",
        },
    )
    pre_delay_document["composition"]["preDelayMs"] = 10.0
    pre_delay_request_path = workspace / "pre-delay-request.json"
    pre_delay_request_path.write_text(json.dumps(pre_delay_document))
    pre_delay_render_result = workspace / "pre-delay-render-result"
    run_renderer(
        renderer, pre_delay_request_path, pre_delay_render_result, fixture
    )
    pre_delay_analyzed = run_analyzer(
        modulation_analyzer, pre_delay_render_result, fixture
    )
    if pre_delay_analyzed.returncode != 0:
        raise AssertionError(
            f"modulation analyzer rejected a valid nonzero Pre-delay "
            f"render: {pre_delay_analyzed.stderr}"
        )
    pre_delay_metadata = json.loads(
        (pre_delay_render_result / "render.json").read_text()
    )
    if pre_delay_metadata["preDelayFrames"] != 480:  # 10ms @ 48kHz, exact
        raise AssertionError(f"unexpected preDelayFrames: {pre_delay_metadata}")
    pre_delay_analysis = json.loads(
        (pre_delay_render_result / "analysis" / "modulation-v1.json").read_text()
    )
    expected_pre_delay_frames = (
        pre_delay_metadata["inputFrames"]
        + pre_delay_metadata["preDelayFrames"]
        + pre_delay_metadata["tailBudgetFrames"]
    )
    if (
        pre_delay_analysis["completeResponse"]["expectedFrames"]
        != expected_pre_delay_frames
        or pre_delay_analysis["completeResponse"]["frameCountCheck"] != "exact"
    ):
        raise AssertionError(
            f"modulation-v1's own expectedFrames did not include "
            f"preDelayFrames: {pre_delay_analysis['completeResponse']}"
        )
    if pre_delay_analysis["boundedEnergy"]["segmentCount"] == 0:
        raise AssertionError(
            "the bounded-energy tail window collapsed to nothing under "
            f"a nonzero Pre-delay: {pre_delay_analysis}"
        )


if __name__ == "__main__":
    main()
