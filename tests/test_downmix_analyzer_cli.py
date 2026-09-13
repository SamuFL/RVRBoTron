#!/usr/bin/env python3

import json
import math
import shutil
import subprocess
import sys
from pathlib import Path


def run_renderer(renderer, fixture, request, output, capture_stages=True):
    arguments = [
        str(renderer),
        "render",
        "--input",
        str(fixture),
        "--config",
        str(request),
    ]
    if capture_stages:
        arguments += ["--capture-stages", "all"]
    arguments += ["--output", str(output)]
    return subprocess.run(
        arguments, check=False, capture_output=True, text=True
    )


def run_analyzer(analyzer, render_result, source):
    return subprocess.run(
        [sys.executable, str(analyzer), str(render_result), "--source", str(source)],
        check=False,
        capture_output=True,
        text=True,
    )


def split_diffuser_stages(channels, mix="hadamard"):
    return [
        {
            "type": "split",
            "channels": channels,
            "strategy": "duplicate",
            "normalisation": "energy",
        },
        {
            "type": "diffuser",
            "steps": 2,
            "totalMs": 2,
            "distribution": "even",
            "step": {
                "delayStrategy": "segmented-random",
                "mix": mix,
                "shuffle": True,
                "polarity": "seeded-random",
            },
        },
    ]


def downmix_config(strategy, channels):
    if strategy == "select":
        right = None if channels == 1 else 1
        config = {"type": "downmix", "strategy": "select", "leftChannel": 0}
        if right is not None:
            config["rightChannel"] = right
        config["normalisation"] = "energy"
        return config
    return {"type": "downmix", "strategy": strategy, "normalisation": "energy"}


def aligned_document(strategy, channels, mix="hadamard", with_early=False):
    document = {
        "formatVersion": 2,
        "seed": 42,
        "composition": {
            "stages": [
                *split_diffuser_stages(channels, mix),
                downmix_config(strategy, channels),
            ],
        },
    }
    if with_early:
        document["composition"]["early"] = {"taps": [{"stepIndex": 0}]}
    return document


def unaligned_document(strategy, channels):
    return {
        "formatVersion": 2,
        "seed": 42,
        "composition": {
            "stages": [
                {
                    "type": "split",
                    "channels": channels,
                    "strategy": "duplicate",
                    "normalisation": "energy",
                },
                {"type": "feedback-loop"},
                downmix_config(strategy, channels),
            ],
        },
    }


def render_and_analyze(renderer, analyzer, fixture, workspace, name, document):
    request = workspace / f"{name}-request.json"
    request.write_text(json.dumps(document))
    result = workspace / f"{name}-result"
    rendered = run_renderer(renderer, fixture, request, result)
    if rendered.returncode != 0:
        raise AssertionError(f"{name}: render failed: {rendered.stderr}")
    analyzed = run_analyzer(analyzer, result, fixture)
    if analyzed.returncode != 0:
        raise AssertionError(f"{name}: analysis failed: {analyzed.stderr}")
    analysis = json.loads((result / "analysis" / "downmix-v1.json").read_text())
    return result, analysis


def check_finite_tree(value, path="analysis"):
    if isinstance(value, dict):
        for key, child in value.items():
            check_finite_tree(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            check_finite_tree(child, f"{path}[{index}]")
    elif isinstance(value, float):
        if not math.isfinite(value):
            raise AssertionError(f"non-finite value at {path}: {value}")


def check_branch_reconciliation(analysis, tolerance=1e-6):
    reconciliation = analysis["branchEnergyReconciliation"]
    combined = reconciliation["combinedEnergy"]
    expected = reconciliation["expectedCombinedEnergy"]
    if abs(combined - expected) > tolerance * max(1.0, combined):
        raise AssertionError(
            f"combined energy did not reconcile with branch energies plus "
            f"their cross term: {reconciliation}"
        )


def main():
    analyzer = Path(sys.argv[1])
    renderer = Path(sys.argv[2])
    fixture = Path(sys.argv[3])
    stereo_fixture = (
        fixture.parent / "matrix" / "identity-stereo-48000-float32.wav"
    )
    workspace = Path(sys.argv[4])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    # Every strategy, aligned (Diffuser-only) at N=4 and unaligned
    # (Feedback Loop) at the same N: covers both Alignment expectations
    # for all five strategies (issue #115's own AC). Aligned evidence
    # requires a Diffusion Step immediately before the Downmix to be
    # available (Alignment score, spectral deviation against source,
    # branch energy ratio); unaligned evidence must instead report those
    # three as unavailable, never silently computed against the wrong
    # signal.
    strategies = ["select", "orthogonal-rows", "halves", "alternating", "sum-all"]
    for strategy in strategies:
        aligned_result, aligned_analysis = render_and_analyze(
            renderer,
            analyzer,
            fixture,
            workspace,
            f"{strategy}-aligned-n4",
            aligned_document(strategy, 4),
        )
        check_finite_tree(aligned_analysis)
        check_branch_reconciliation(aligned_analysis)
        main_evidence = aligned_analysis["main"]
        if main_evidence["strategy"] != strategy:
            raise AssertionError(f"{strategy}: unexpected strategy: {main_evidence}")
        if main_evidence["alignmentExpectation"] != "aligned":
            raise AssertionError(
                f"{strategy}: aligned fixture did not resolve aligned: "
                f"{main_evidence}"
            )
        expected_tag = strategy == "sum-all"
        if main_evidence["coherentDownmixAblation"] != expected_tag:
            raise AssertionError(
                f"{strategy}: unexpected coherentDownmixAblation on an "
                f"aligned fixture: {main_evidence}"
            )
        if main_evidence["alignmentScore"] is None:
            raise AssertionError(
                f"{strategy}: aligned fixture reported no Alignment score: "
                f"{main_evidence}"
            )
        if not (0.0 <= main_evidence["alignmentScore"]["mean"] <= 1.0):
            raise AssertionError(
                f"{strategy}: Alignment score mean out of range: "
                f"{main_evidence['alignmentScore']}"
            )
        if not main_evidence["spectralDeviation"]["available"]:
            raise AssertionError(
                f"{strategy}: aligned fixture reported spectral deviation "
                f"unavailable: {main_evidence}"
            )
        if not main_evidence["branchEnergyRatio"]["available"]:
            raise AssertionError(
                f"{strategy}: aligned fixture reported branch energy ratio "
                f"unavailable: {main_evidence}"
            )
        if main_evidence["branchEnergy"] <= 0.0:
            raise AssertionError(
                f"{strategy}: aligned fixture's own branch energy was "
                f"not positive: {main_evidence}"
            )
        if aligned_analysis["early"] is not None:
            raise AssertionError(
                f"{strategy}: no Early Reflections branch was configured "
                f"but analysis reported one: {aligned_analysis}"
            )

        unaligned_result, unaligned_analysis = render_and_analyze(
            renderer,
            analyzer,
            fixture,
            workspace,
            f"{strategy}-unaligned-n4",
            unaligned_document(strategy, 4),
        )
        check_finite_tree(unaligned_analysis)
        check_branch_reconciliation(unaligned_analysis)
        unaligned_main = unaligned_analysis["main"]
        if unaligned_main["alignmentExpectation"] != "unaligned":
            raise AssertionError(
                f"{strategy}: Feedback Loop fixture did not resolve "
                f"unaligned: {unaligned_main}"
            )
        if unaligned_main["coherentDownmixAblation"] is not False:
            raise AssertionError(
                f"{strategy}: unaligned fixture was tagged as a Coherent "
                f"Downmix ablation: {unaligned_main}"
            )
        if unaligned_main["alignmentScore"] is not None:
            raise AssertionError(
                f"{strategy}: unaligned fixture reported a measured "
                f"Alignment score (no Diffusion Step is this Downmix's "
                f"own immediate input): {unaligned_main}"
            )
        if unaligned_main["spectralDeviation"]["available"]:
            raise AssertionError(
                f"{strategy}: unaligned fixture reported spectral "
                f"deviation available: {unaligned_main}"
            )
        if unaligned_main["branchEnergyRatio"]["available"]:
            raise AssertionError(
                f"{strategy}: unaligned fixture reported branch energy ratio "
                f"available: {unaligned_main}"
            )

    # select/sum-all support N=1 (issues #107/#114); the other three
    # strategies require N>=2 and are exercised above at N=4 and below at
    # odd N=5 instead.
    for strategy in ("select", "sum-all"):
        _, analysis = render_and_analyze(
            renderer,
            analyzer,
            fixture,
            workspace,
            f"{strategy}-aligned-n1",
            aligned_document(strategy, 1),
        )
        check_finite_tree(analysis)
        check_branch_reconciliation(analysis)
        if analysis["main"]["alignmentScore"]["mean"] != 1.0:
            raise AssertionError(
                f"{strategy} at N=1: a single Channel trivially agrees "
                f"with itself, so Alignment score mean must be 1.0: "
                f"{analysis['main']}"
            )

    # Odd N=5 (Householder, unlike the default Hadamard, is valid there)
    # for every strategy that requires N>=2.
    for strategy in ("orthogonal-rows", "halves", "alternating", "sum-all"):
        _, analysis = render_and_analyze(
            renderer,
            analyzer,
            fixture,
            workspace,
            f"{strategy}-aligned-n5",
            aligned_document(strategy, 5, mix="householder"),
        )
        check_finite_tree(analysis)
        check_branch_reconciliation(analysis)
        if analysis["main"]["strategy"] != strategy:
            raise AssertionError(f"{strategy} at odd N=5: {analysis['main']}")

    # An Early Reflections branch: both branches contribute nonzero
    # energy and a nonzero cross term, and the analyzer reports a
    # populated `early` block alongside `main`. A single-step Diffuser
    # (rather than the two-step fixture above) makes Main's own Downmix
    # and Early's only possible tap (stepIndex 0) read the *same*
    # Diffusion Step output, so their contributions genuinely overlap in
    # time instead of arriving in disjoint windows.
    with_early_document = aligned_document("select", 4, with_early=True)
    with_early_document["composition"]["stages"][1]["steps"] = 1
    with_early_result, with_early_analysis = render_and_analyze(
        renderer,
        analyzer,
        fixture,
        workspace,
        "select-aligned-with-early",
        with_early_document,
    )
    check_finite_tree(with_early_analysis)
    check_branch_reconciliation(with_early_analysis)
    if with_early_analysis["early"] is None:
        raise AssertionError(
            "a configured Early Reflections branch was not reported"
        )
    if with_early_analysis["early"]["alignmentExpectation"] != "aligned":
        raise AssertionError(
            f"Early Reflections did not resolve aligned: "
            f"{with_early_analysis['early']}"
        )
    if with_early_analysis["early"]["branchEnergy"] <= 0.0:
        raise AssertionError(
            f"Early Reflections branch energy was not positive: "
            f"{with_early_analysis['early']}"
        )
    # Early's own tap(s) always draw from a Diffusion Step by
    # construction, so its own Alignment score/spectral deviation/
    # branch energy ratio must be available exactly like Main's are on
    # an aligned Diffuser-only fixture -- reconstructed from the
    # gain-weighted sum of its own taps' captures, not omitted.
    early_evidence = with_early_analysis["early"]
    if early_evidence["alignmentScore"] is None:
        raise AssertionError(
            f"Early Reflections reported no Alignment score: "
            f"{early_evidence}"
        )
    if early_evidence["alignmentScore"]["stepIndices"] != [0]:
        raise AssertionError(
            f"Early Reflections Alignment score did not identify its own "
            f"tap's Diffusion Step: {early_evidence}"
        )
    if not early_evidence["spectralDeviation"]["available"]:
        raise AssertionError(
            f"Early Reflections reported spectral deviation unavailable: "
            f"{early_evidence}"
        )
    if not early_evidence["branchEnergyRatio"]["available"]:
        raise AssertionError(
            f"Early Reflections reported branch energy ratio "
            f"unavailable: {early_evidence}"
        )
    reconciliation = with_early_analysis["branchEnergyReconciliation"]
    if reconciliation["crossTerm"] == 0.0:
        raise AssertionError(
            f"Main and Early both reading the same Channels was expected "
            f"to produce a nonzero cross term: {reconciliation}"
        )

    # Republishing over the same Render Result is idempotent, matching
    # every other analyzer's own immutable-artifact contract.
    republished = run_analyzer(analyzer, with_early_result, fixture)
    if republished.returncode != 0:
        raise AssertionError(republished.stderr)
    republished_analysis = json.loads(
        (with_early_result / "analysis" / "downmix-v1.json").read_text()
    )
    if republished_analysis != with_early_analysis:
        raise AssertionError("republishing changed the analysis artifact")

    # A disabled Main branch skips its own Downmix/Width/level processing
    # entirely (issue #109), so its own branch energy ratio and spectral
    # deviation must be unavailable -- comparing its exact-zero capture
    # against a nonzero source would otherwise report measurements for
    # processing that never ran. Alignment score stays available
    # regardless: it characterizes the source, not Main's own (skipped)
    # processing.
    main_disabled_document = aligned_document("select", 4)
    main_disabled_document["composition"]["mainEnabled"] = False
    _, main_disabled_analysis = render_and_analyze(
        renderer,
        analyzer,
        fixture,
        workspace,
        "select-main-disabled",
        main_disabled_document,
    )
    check_finite_tree(main_disabled_analysis)
    check_branch_reconciliation(main_disabled_analysis)
    main_disabled_evidence = main_disabled_analysis["main"]
    if main_disabled_evidence["branchEnergy"] != 0.0:
        raise AssertionError(
            f"a disabled Main branch's own energy was not exact zero: "
            f"{main_disabled_evidence}"
        )
    if main_disabled_evidence["alignmentScore"] is None:
        raise AssertionError(
            f"a disabled Main branch reported no Alignment score, even "
            f"though its own source is still measurable: "
            f"{main_disabled_evidence}"
        )
    if main_disabled_evidence["branchEnergyRatio"]["available"]:
        raise AssertionError(
            f"a disabled Main branch reported a branch energy ratio: "
            f"{main_disabled_evidence}"
        )
    if main_disabled_evidence["spectralDeviation"]["available"]:
        raise AssertionError(
            f"a disabled Main branch reported spectral deviation "
            f"available: {main_disabled_evidence}"
        )

    # branchEnergyRatio is the branch's *combined* Downmix+Width+level
    # energy ratio, not an isolated Width effect -- there is no capture
    # between Downmix and Width to separate them (adding one would need
    # its own versioned capture boundary per ADR-0005). Demonstrate that
    # both a non-90-degree Width and a nonzero mainLevelDb move the
    # ratio, so it is never mistaken for a pure Width metric.
    width_reference_document = aligned_document("select", 4)
    _, width_reference_analysis = render_and_analyze(
        renderer,
        analyzer,
        fixture,
        workspace,
        "select-width-reference",
        width_reference_document,
    )
    reference_ratio = width_reference_analysis["main"]["branchEnergyRatio"]["ratio"]

    width_level_document = aligned_document("select", 4)
    width_level_document["composition"]["stages"][2]["widthDeg"] = 45.0
    width_level_document["composition"]["mainLevelDb"] = -6.0
    _, width_level_analysis = render_and_analyze(
        renderer,
        analyzer,
        fixture,
        workspace,
        "select-width-level",
        width_level_document,
    )
    width_level_ratio = width_level_analysis["main"]["branchEnergyRatio"]["ratio"]
    if width_level_ratio == reference_ratio:
        raise AssertionError(
            f"a non-90-degree Width plus a nonzero mainLevelDb was "
            f"expected to move the branch energy ratio: reference="
            f"{reference_ratio}, width/level={width_level_ratio}"
        )

    # The Composition's own dry/wet envelope (issue #131): output.wav is
    # no longer always the raw Main-stereo/Early-stereo capture sum once
    # a non-neutral wetGain or an enabled dry path is configured, so the
    # reconstruction the analyzer checks output.wav against must itself
    # account for the envelope, not merely the branch captures.
    wet_gain_document = aligned_document("select", 4)
    wet_gain_document["composition"]["wetDb"] = -3.0
    _, wet_gain_analysis = render_and_analyze(
        renderer,
        analyzer,
        fixture,
        workspace,
        "select-envelope-wet-gain",
        wet_gain_document,
    )
    check_finite_tree(wet_gain_analysis)
    check_branch_reconciliation(wet_gain_analysis)

    insert_document = aligned_document("select", 4)
    insert_document["composition"]["wetOnly"] = False
    insert_document["composition"]["dryDb"] = -6.0
    insert_document["composition"]["wetDb"] = -3.0
    _, insert_analysis = render_and_analyze(
        renderer,
        analyzer,
        fixture,
        workspace,
        "select-envelope-insert",
        insert_document,
    )
    check_finite_tree(insert_analysis)
    check_branch_reconciliation(insert_analysis)

    # wetOnly: true (the default) preserves a configured dryDb without
    # letting it affect output.wav -- the analyzer's own reconstruction
    # must gate dry exactly like Reverb.cpp does, not merely because
    # dryGain happens to be neutral.
    gated_document = aligned_document("select", 4)
    gated_document["composition"]["dryDb"] = -6.0
    _, gated_analysis = render_and_analyze(
        renderer,
        analyzer,
        fixture,
        workspace,
        "select-envelope-gated-dry",
        gated_document,
    )
    check_finite_tree(gated_analysis)
    check_branch_reconciliation(gated_analysis)

    # A stereo dry source through insert-style dry+wet: the reconstructed
    # dry contribution must map channel-for-channel, not merely survive
    # the mono case above.
    stereo_insert_document = aligned_document("select", 4)
    stereo_insert_document["composition"]["wetOnly"] = False
    stereo_insert_document["composition"]["dryDb"] = -6.0
    stereo_insert_document["composition"]["wetDb"] = -3.0
    stereo_insert_request = workspace / "select-envelope-insert-stereo-request.json"
    stereo_insert_request.write_text(json.dumps(stereo_insert_document))
    stereo_insert_result = workspace / "select-envelope-insert-stereo-result"
    stereo_insert_rendered = run_renderer(
        renderer, stereo_fixture, stereo_insert_request, stereo_insert_result
    )
    if stereo_insert_rendered.returncode != 0:
        raise AssertionError(stereo_insert_rendered.stderr)
    stereo_insert_analyzed = run_analyzer(
        analyzer, stereo_insert_result, stereo_fixture
    )
    if stereo_insert_analyzed.returncode != 0:
        raise AssertionError(
            f"stereo insert-style dry+wet analysis failed: "
            f"{stereo_insert_analyzed.stderr}"
        )
    check_finite_tree(
        json.loads(
            (stereo_insert_result / "analysis" / "downmix-v1.json").read_text()
        )
    )

    # --capture-stages all is required: the analyzer needs Main-stereo
    # (and, when configured, Early-stereo and Diffusion Step) captures.
    no_capture_request = workspace / "no-capture-request.json"
    no_capture_request.write_text(json.dumps(aligned_document("select", 4)))
    no_capture_result = workspace / "no-capture-result"
    no_capture_rendered = run_renderer(
        renderer, fixture, no_capture_request, no_capture_result, capture_stages=False
    )
    if no_capture_rendered.returncode != 0:
        raise AssertionError(no_capture_rendered.stderr)
    no_capture_analyzed = run_analyzer(analyzer, no_capture_result, fixture)
    if no_capture_analyzed.returncode == 0:
        raise AssertionError(
            "analyzer accepted a Render Result captured without "
            "--capture-stages all"
        )

    # A Feedback-Loop-only Composition (no Diffuser at all, so Early
    # Reflections are not even structurally possible) still analyzes
    # Main-stereo correctly.
    loop_only_document = {
        "formatVersion": 2,
        "seed": 42,
        "composition": {
            "stages": [
                {
                    "type": "split",
                    "channels": 4,
                    "strategy": "duplicate",
                    "normalisation": "energy",
                },
                {"type": "feedback-loop"},
                downmix_config("select", 4),
            ]
        },
    }
    _, loop_only_analysis = render_and_analyze(
        renderer, analyzer, fixture, workspace, "loop-only", loop_only_document
    )
    check_finite_tree(loop_only_analysis)
    if loop_only_analysis["main"]["spectralDeviation"]["available"]:
        raise AssertionError(
            f"a Feedback-Loop-only Composition reported spectral "
            f"deviation available: {loop_only_analysis['main']}"
        )

    # A source SHA-256 mismatch is rejected, matching every other
    # analyzer's own provenance check.
    mismatched_source = workspace / "mismatched-source.wav"
    mismatched_source.write_bytes(fixture.read_bytes()[:-2])
    mismatched_analyzed = run_analyzer(
        analyzer, with_early_result, mismatched_source
    )
    if mismatched_analyzed.returncode == 0:
        raise AssertionError(
            "analyzer accepted a source that does not match render.json's "
            "own recorded SHA-256"
        )


if __name__ == "__main__":
    main()
