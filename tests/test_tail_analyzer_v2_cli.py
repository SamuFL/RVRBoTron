#!/usr/bin/env python3

import hashlib
import json
import math
import shutil
import subprocess
import sys
import time
from pathlib import Path


def run_renderer(renderer, request_path, output, fixture, block_size=32):
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


def feedback_loop_request(channels=2, damping=None, seed=7, **overrides):
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
    loop.update(overrides)
    return {
        "formatVersion": 1,
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
                {"type": "downmix", "strategy": "select"},
            ]
        },
    }


def main():
    tail_v1_analyzer = Path(sys.argv[1])
    tail_v2_analyzer = Path(sys.argv[2])
    renderer = Path(sys.argv[3])
    fixture = Path(sys.argv[4])
    workspace = Path(sys.argv[5])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    # N=2, delayStrategy "even": the same millisecond-scale CI tracer used
    # throughout the Feedback Loop/Damping/tail-v1 test suites, with a
    # research-baseline Damping object enabled.
    request_path = workspace / "request.json"
    request_path.write_text(
        json.dumps(
            feedback_loop_request(
                damping={"highRatio": 0.5, "highHz": 4000, "lowRatio": 1.0}
            )
        )
    )
    render_result = workspace / "render-result"
    run_renderer(renderer, request_path, render_result, fixture)

    v1_analyzed = run_analyzer(tail_v1_analyzer, render_result, fixture)
    if v1_analyzed.returncode != 0:
        raise AssertionError(v1_analyzed.stderr)
    v1_artifact = render_result / "analysis" / "tail-v1.json"
    v1_contents = v1_artifact.read_bytes()

    analyzed = run_analyzer(tail_v2_analyzer, render_result, fixture)
    if analyzed.returncode != 0:
        raise AssertionError(analyzed.stderr)

    # tail-v2 is append-only: it publishes beside tail-v1 without touching it.
    if v1_artifact.read_bytes() != v1_contents:
        raise AssertionError("tail-v2 analysis modified the existing tail-v1 artifact")

    artifact = render_result / "analysis" / "tail-v2.json"
    analysis = json.loads(artifact.read_text())

    if analysis["analyzer"] != "tail" or analysis["analyzerVersion"] != 2:
        raise AssertionError(f"unexpected analyzer header: {analysis}")
    if analysis["source"] != {
        "filename": fixture.name,
        "sha256": hashlib.sha256(fixture.read_bytes()).hexdigest(),
        "verified": True,
    }:
        raise AssertionError(f"unexpected source provenance: {analysis}")

    expected_tail_budget = math.ceil(1.0 * 1.5 * 48000)
    expected_frames = 32 + expected_tail_budget
    if analysis["completeResponse"] != {
        "inputFrames": 32,
        "tailBudgetFrames": expected_tail_budget,
        "expectedFrames": expected_frames,
        "outputFrames": expected_frames,
        "frameCountCheck": "exact",
        "silenceFloorEnabled": False,
    }:
        raise AssertionError(f"unexpected complete-response facts: {analysis}")
    if analysis["nonFiniteSampleCount"] != 0:
        raise AssertionError(f"unexpected non-finite samples: {analysis}")
    if analysis["dampingEnabled"] is not True:
        raise AssertionError(f"dampingEnabled was not reported: {analysis}")
    if analysis["gainMode"] != "per-channel":
        raise AssertionError(f"unexpected gainMode: {analysis}")

    decay = analysis["decay"]
    if decay["referenceBandHz"] != 1000.0 or decay["requestedRt60Sec"] != 1.0:
        raise AssertionError(f"unexpected decay header: {decay}")
    if decay["measuredRt60Sec"] is None:
        raise AssertionError(f"Reference band RT60 fit failed: {decay}")
    if not isinstance(decay["significantDeviation"], bool):
        raise AssertionError(f"significantDeviation was not a bool: {decay}")

    expected_centers = [
        63.0,
        125.0,
        250.0,
        500.0,
        1000.0,
        2000.0,
        4000.0,
        8000.0,
        16000.0,
    ]
    bands = decay["bands"]
    if [band["centerHz"] for band in bands] != expected_centers:
        raise AssertionError(f"unexpected octave-band centers: {bands}")
    for band in bands:
        predicted = band["predictedRt60Sec"]
        if len(predicted["perChannelRt60Sec"]) != 2:
            raise AssertionError(f"unexpected predicted per-Channel count: {band}")
        low, high = predicted["rangeRt60Sec"]
        if not (low <= predicted["targetRt60Sec"] <= high):
            raise AssertionError(
                f"predicted target did not fall within its own range: {band}"
            )
        if "withinPredictedTolerance" not in band:
            raise AssertionError(f"missing withinPredictedTolerance: {band}")

    canonical = decay["canonicalRatios"]
    if {"low", "reference", "high"} != set(canonical):
        raise AssertionError(f"unexpected canonicalRatios shape: {canonical}")
    if canonical["low"]["centerHz"] != 63.0 or canonical["high"]["centerHz"] != 16000.0:
        raise AssertionError(f"unexpected canonicalRatios band selection: {canonical}")
    if canonical["reference"]["centerHz"] != 1000.0:
        raise AssertionError(f"unexpected canonicalRatios reference band: {canonical}")

    eventual = analysis["eventualContraction"]
    if eventual["negativeTrend"] is not True:
        raise AssertionError(
            f"eventual-contraction evidence did not show a negative "
            f"late-tail trend: {eventual}"
        )
    if eventual["segmentCount"] != 16:
        raise AssertionError(f"unexpected eventual-contraction segment count: {eventual}")

    if not analysis["coloration"].get("twelfthOctaveCurve"):
        raise AssertionError(f"Coloration curve was empty: {analysis['coloration']}")

    # Idempotent: repeating the analysis does not rewrite the artifact.
    original = artifact.read_bytes()
    modified = artifact.stat().st_mtime_ns
    time.sleep(0.01)
    repeated = run_analyzer(tail_v2_analyzer, render_result, fixture)
    if repeated.returncode != 0:
        raise AssertionError(repeated.stderr)
    if artifact.read_bytes() != original or artifact.stat().st_mtime_ns != modified:
        raise AssertionError("idempotent tail-v2 analysis rewrote its artifact")

    # A source that does not match render.json's recorded SHA-256 is
    # rejected without changing the published artifact -- the same
    # provenance logic tail-v1 already exercises thoroughly, checked here
    # once for confidence this module's own copy of it behaves the same.
    wrong_source = workspace / "wrong.wav"
    wrong_source.write_bytes(fixture.read_bytes() + b"\x00")
    rejected = run_analyzer(tail_v2_analyzer, render_result, fixture, source=wrong_source)
    if rejected.returncode == 0:
        raise AssertionError("tail-v2 analyzer accepted wrong source")
    if "source SHA-256 does not match render metadata" not in rejected.stderr:
        raise AssertionError(f"unexpected provenance failure: {rejected.stderr}")
    if artifact.read_bytes() != original:
        raise AssertionError("failed provenance check changed analysis")

    # Inconsistent resolved Damping evidence (a shelf coefficient array
    # missing a Channel's worth of values) is rejected with a descriptive
    # failure, before any prediction math runs.
    corrupted_result = workspace / "corrupted-damping-result"
    shutil.copytree(render_result, corrupted_result)
    (corrupted_result / "analysis" / "tail-v2.json").unlink()
    corrupted_path = corrupted_result / "resolved.json"
    corrupted = json.loads(corrupted_path.read_text())
    corrupted_loop = next(
        stage
        for stage in corrupted["composition"]["stages"]
        if stage["type"] == "feedback-loop"
    )
    corrupted_loop["damping"]["highShelfB0"].pop()
    corrupted_path.write_text(json.dumps(corrupted))
    corrupted_analyzed = run_analyzer(tail_v2_analyzer, corrupted_result, fixture)
    if corrupted_analyzed.returncode == 0:
        raise AssertionError("tail-v2 analyzer accepted inconsistent Damping evidence")
    if "resolved Damping evidence is inconsistent" not in corrupted_analyzed.stderr:
        raise AssertionError(
            f"unexpected inconsistent-Damping failure: {corrupted_analyzed.stderr}"
        )
    if (corrupted_result / "analysis" / "tail-v2.json").exists():
        raise AssertionError("rejected Damping evidence published a tail-v2 analysis")

    # A Composition without a Feedback Loop is rejected, mirroring tail-v1.
    diffuser_only_request = {
        "formatVersion": 1,
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
                {"type": "downmix", "strategy": "select"},
            ]
        },
    }
    diffuser_only_path = workspace / "diffuser-only-request.json"
    diffuser_only_path.write_text(json.dumps(diffuser_only_request))
    diffuser_only_result = workspace / "diffuser-only-result"
    run_renderer(renderer, diffuser_only_path, diffuser_only_result, fixture)
    rejected_composition = run_analyzer(
        tail_v2_analyzer, diffuser_only_result, fixture
    )
    if rejected_composition.returncode == 0:
        raise AssertionError(
            "tail-v2 analyzer accepted a Composition without a Feedback Loop"
        )
    if "tail analysis requires a Feedback Loop" not in rejected_composition.stderr:
        raise AssertionError(
            f"unexpected composition rejection: {rejected_composition.stderr}"
        )

    # Undamped: predicted RT60 falls back to the plain per-Channel implied
    # decay (no shelf response), matching tail-v1's own undamped semantics.
    undamped_path = workspace / "undamped-request.json"
    undamped_path.write_text(json.dumps(feedback_loop_request()))
    undamped_result = workspace / "undamped-result"
    run_renderer(renderer, undamped_path, undamped_result, fixture)
    undamped_analyzed = run_analyzer(tail_v2_analyzer, undamped_result, fixture)
    if undamped_analyzed.returncode != 0:
        raise AssertionError(undamped_analyzed.stderr)
    undamped_analysis = json.loads(
        (undamped_result / "analysis" / "tail-v2.json").read_text()
    )
    if undamped_analysis["dampingEnabled"] is not False:
        raise AssertionError(
            f"dampingEnabled was not False for an undamped render: "
            f"{undamped_analysis}"
        )
    for band in undamped_analysis["decay"]["bands"]:
        target = band["predictedRt60Sec"]["targetRt60Sec"]
        if abs(target - 1.0) > 1e-6:
            raise AssertionError(
                f"undamped predicted RT60 did not converge on rt60Sec: {band}"
            )

    # gainMode: uniform vs. per-channel, at N=8 with a wide delay spread
    # (see test_tail_analyzer_cli.py's own gainMode divergence test):
    # per-channel gives a near-common predicted target across Channels;
    # uniform spreads the same Damping request into a materially wider
    # predicted per-Channel range.
    def predicted_reference_band(name, gain_mode):
        request = workspace / f"{name}-request.json"
        request.write_text(
            json.dumps(
                feedback_loop_request(
                    channels=8,
                    delayMaxMs=32.0,
                    gainMode=gain_mode,
                    damping={
                        "highRatio": 0.5,
                        "highHz": 4000,
                        "lowRatio": 0.6,
                        "lowHz": 200,
                    },
                )
            )
        )
        result = workspace / f"{name}-result"
        run_renderer(renderer, request, result, fixture)
        analyzed = run_analyzer(tail_v2_analyzer, result, fixture)
        if analyzed.returncode != 0:
            raise AssertionError(analyzed.stderr)
        analysis = json.loads((result / "analysis" / "tail-v2.json").read_text())
        reference = next(
            band
            for band in analysis["decay"]["bands"]
            if band["centerHz"] == 1000.0
        )
        if reference["t30"] is None:
            raise AssertionError(f"{name} T30 fit failed: {reference}")
        low, high = reference["predictedRt60Sec"]["rangeRt60Sec"]
        return high - low, reference["withinPredictedTolerance"]

    per_channel_spread, per_channel_within = predicted_reference_band(
        "wide-per-channel", "per-channel"
    )
    uniform_spread, uniform_within = predicted_reference_band(
        "wide-uniform", "uniform"
    )
    if not per_channel_within:
        raise AssertionError(
            "gainMode: per-channel measured T30 did not fall within its "
            "near-common predicted target"
        )
    if not uniform_within:
        raise AssertionError(
            "gainMode: uniform measured T30 did not fall within its "
            "predicted per-Channel range"
        )
    if uniform_spread <= 10 * per_channel_spread:
        raise AssertionError(
            "gainMode: uniform did not spread the predicted Reference-band "
            f"range materially wider than gainMode: per-channel: "
            f"uniform={uniform_spread} vs per-channel={per_channel_spread}"
        )

    # A corner placed on top of the Reference band with a large ratio
    # produces a significant (>10%) deviation from rt60Sec, flagged but
    # never rejected (ADR-0004, docs/adr/0004-validate-structure-not-
    # acoustics.md).
    on_reference_path = workspace / "on-reference-request.json"
    on_reference_path.write_text(
        json.dumps(
            feedback_loop_request(damping={"highRatio": 0.2, "highHz": 1000})
        )
    )
    on_reference_result = workspace / "on-reference-result"
    run_renderer(renderer, on_reference_path, on_reference_result, fixture)
    on_reference_analyzed = run_analyzer(
        tail_v2_analyzer, on_reference_result, fixture
    )
    if on_reference_analyzed.returncode != 0:
        raise AssertionError(on_reference_analyzed.stderr)
    on_reference_analysis = json.loads(
        (on_reference_result / "analysis" / "tail-v2.json").read_text()
    )
    on_reference_decay = on_reference_analysis["decay"]
    if on_reference_decay["significantDeviation"] is not True:
        raise AssertionError(
            "test fixture did not actually exercise a significant "
            f"Reference-band deviation: {on_reference_decay}"
        )


if __name__ == "__main__":
    main()
