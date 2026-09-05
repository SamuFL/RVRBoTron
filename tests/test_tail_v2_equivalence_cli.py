#!/usr/bin/env python3

import json
import shutil
import subprocess
import sys
from pathlib import Path

REQUIRED_METRIC_KEYS = {
    "nonFiniteSampleCount",
    "decayRt60Sec",
    "predictedTargetRt60Sec",
    "predictedRangeLowRt60Sec",
    "predictedRangeHighRt60Sec",
    "decayRelativeError",
    "eventualContractionSlopeDbPerSegment",
    "alignmentScore",
    "colorationPeakToPeakDb",
    "colorationRmsDb",
    "colorationSpectralFlatness",
}


def run(*arguments):
    return subprocess.run(
        list(map(str, arguments)),
        check=False,
        capture_output=True,
        text=True,
    )


def tolerance_document(same_architecture, cross_architecture, rationale):
    return {
        "byPrecision": {
            precision: {
                key: {
                    "sameArchitectureAbsoluteTolerance": same_architecture,
                    "crossArchitectureAbsoluteTolerance": cross_architecture,
                    "rationale": rationale,
                }
                for key in REQUIRED_METRIC_KEYS
            }
            for precision in ("float32", "float64")
        }
    }


def main():
    extractor = Path(sys.argv[1])
    comparator = Path(sys.argv[2])
    analyzer = Path(sys.argv[3])
    renderer = Path(sys.argv[4])
    fixture = Path(sys.argv[5])
    workspace = Path(sys.argv[6])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    tolerances_document = json.loads(
        (
            Path(__file__).resolve().parent.parent
            / "tools"
            / "tail_tolerances_v2.json"
        ).read_text()
    )
    for precision in ("float32", "float64"):
        categories = tolerances_document["byPrecision"][precision]
        if set(categories) != REQUIRED_METRIC_KEYS:
            raise AssertionError(
                f"{precision} tolerances are missing/extra metric keys: "
                f"{set(categories)}"
            )
        for name, entry in categories.items():
            for comparison_class in ("sameArchitecture", "crossArchitecture"):
                key = f"{comparison_class}AbsoluteTolerance"
                if not isinstance(entry[key], (int, float)):
                    raise AssertionError(
                        f"{precision}.{name}.{key} tolerance is not numeric"
                    )
            if not entry.get("rationale"):
                raise AssertionError(f"{precision}.{name} has no rationale")

    # N=2, delayStrategy "even": the same millisecond-scale CI tracer used
    # throughout, with a research-baseline Damping object enabled (issue
    # #78 wants the cross-platform CI tracer to actually exercise Damping).
    request = workspace / "request.json"
    request.write_text(
        json.dumps(
            {
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
                            "damping": {
                                "highRatio": 0.5,
                                "highHz": 4000,
                                "lowRatio": 1.0,
                            },
                        },
                        {"type": "downmix", "strategy": "select"},
                    ]
                },
            }
        )
    )
    render_result = workspace / "render-result"
    rendered = run(
        renderer,
        "render",
        "--input",
        fixture,
        "--config",
        request,
        "--output",
        render_result,
        "--block-size",
        "32",
    )
    if rendered.returncode != 0:
        raise AssertionError(rendered.stderr)
    analyzed = run(sys.executable, analyzer, render_result, "--source", fixture)
    if analyzed.returncode != 0:
        raise AssertionError(analyzed.stderr)

    equivalence_a = workspace / "equivalence-macos-arm64.json"
    extracted = run(
        sys.executable, extractor, render_result, "--output", equivalence_a
    )
    if extracted.returncode != 0:
        raise AssertionError(extracted.stderr)

    equivalence = json.loads(equivalence_a.read_text())
    if equivalence["formatVersion"] != 1:
        raise AssertionError(f"unexpected format version: {equivalence}")
    if {"platform", "architecture", "samplePrecision", "rendererVersion"} - set(
        equivalence
    ):
        raise AssertionError(f"missing provenance fields: {equivalence}")
    dumped = json.dumps(equivalence)
    if "twelfthOctaveCurve" in dumped or "perChannelRt60Sec" in dumped:
        raise AssertionError(
            "compact tail-v2-equivalence artifact leaked bulky per-sample "
            "evidence"
        )
    bands = equivalence["decay"]["bands"]
    if len(bands) != 9:
        raise AssertionError(f"unexpected octave-band count: {equivalence}")
    for band in bands:
        if {
            "centerHz",
            "t20Rt60Sec",
            "t30Rt60Sec",
            "predictedTargetRt60Sec",
            "predictedRangeRt60Sec",
            "withinPredictedTolerance",
        } - set(band):
            raise AssertionError(f"unexpected band shape: {band}")
    if equivalence["nonFiniteSampleCount"] != 0:
        raise AssertionError(f"unexpected non-finite samples: {equivalence}")
    if equivalence["dampingEnabled"] is not True:
        raise AssertionError(f"dampingEnabled was not carried through: {equivalence}")

    # extraction requires an already-analyzed Render Result.
    unanalyzed = workspace / "unanalyzed-result"
    shutil.copytree(render_result, unanalyzed)
    shutil.rmtree(unanalyzed / "analysis")
    missing_analysis = run(
        sys.executable,
        extractor,
        unanalyzed,
        "--output",
        workspace / "unused.json",
    )
    if missing_analysis.returncode == 0:
        raise AssertionError("extraction succeeded without tail-v2.json")

    # A second, identical extraction relabeled as another platform: an
    # exact match compares clean against the tiny, well-resolved tracer.
    equivalence_b = workspace / "equivalence-windows-x86_64.json"
    relabeled = json.loads(equivalence_a.read_text())
    relabeled["platform"] = "Windows"
    relabeled["architecture"] = "x86_64"
    equivalence_b.write_text(json.dumps(relabeled))

    identical = run(sys.executable, comparator, equivalence_a, equivalence_b)
    if identical.returncode != 0:
        raise AssertionError(
            f"identical platforms compared unequal: {identical.stdout}"
        )
    if "FAIL" in identical.stdout:
        raise AssertionError(f"unexpected FAIL in a passing comparison: {identical.stdout}")

    # A single precision group (nothing to compare against) is a skip, not
    # a failure.
    lonely = run(sys.executable, comparator, equivalence_a)
    if lonely.returncode != 0:
        raise AssertionError(f"a lone platform should not fail: {lonely.stdout}")
    if "nothing to compare" not in lonely.stdout:
        raise AssertionError(f"lone platform did not explain the skip: {lonely.stdout}")

    # A structural metric (the octave-band count, fixed by the sample rate)
    # that diverges must fail unconditionally.
    band_count_mismatch = json.loads(equivalence_b.read_text())
    band_count_mismatch["decay"]["bands"].pop()
    band_count_mismatch_path = workspace / "equivalence-band-count-mismatch.json"
    band_count_mismatch_path.write_text(json.dumps(band_count_mismatch))
    mismatched = run(
        sys.executable, comparator, equivalence_a, band_count_mismatch_path
    )
    if mismatched.returncode != 1:
        raise AssertionError(
            f"structural band-count mismatch was not reported as a failure: "
            f"{mismatched.stdout}"
        )
    if "decay.bandCount" not in mismatched.stdout:
        raise AssertionError(
            f"structural mismatch did not name the metric: {mismatched.stdout}"
        )

    # withinPredictedTolerance is likewise structural: a platform diverging
    # on it must fail unconditionally, never silently folded into a
    # tolerance-checked numeric comparison.
    tolerance_flag_mismatch = json.loads(equivalence_b.read_text())
    tolerance_flag_mismatch["decay"]["bands"][4]["withinPredictedTolerance"] = False
    tolerance_flag_mismatch_path = (
        workspace / "equivalence-tolerance-flag-mismatch.json"
    )
    tolerance_flag_mismatch_path.write_text(json.dumps(tolerance_flag_mismatch))
    tolerance_flag_mismatched = run(
        sys.executable, comparator, equivalence_a, tolerance_flag_mismatch_path
    )
    if tolerance_flag_mismatched.returncode != 1:
        raise AssertionError(
            f"withinPredictedTolerance mismatch was not reported as a "
            f"failure: {tolerance_flag_mismatched.stdout}"
        )
    if "decay.withinPredictedTolerance" not in tolerance_flag_mismatched.stdout:
        raise AssertionError(
            f"withinPredictedTolerance mismatch did not name the metric: "
            f"{tolerance_flag_mismatched.stdout}"
        )

    # The eventual-contraction trend sign is likewise structural.
    trend_mismatch = json.loads(equivalence_b.read_text())
    trend_mismatch["eventualContraction"]["negativeTrend"] = False
    trend_mismatch_path = workspace / "equivalence-trend-mismatch.json"
    trend_mismatch_path.write_text(json.dumps(trend_mismatch))
    trend_mismatched = run(
        sys.executable, comparator, equivalence_a, trend_mismatch_path
    )
    if trend_mismatched.returncode != 1:
        raise AssertionError(
            f"eventual-contraction trend mismatch was not reported as a "
            f"failure: {trend_mismatched.stdout}"
        )
    if "eventualContraction.negativeTrend" not in trend_mismatched.stdout:
        raise AssertionError(
            f"trend mismatch did not name the metric: {trend_mismatched.stdout}"
        )

    # Missing fits and an unavailable contraction trend are valid nullable
    # structural values. Matching nulls compare exactly; a null/value split
    # still fails as a structural mismatch.
    nullable_a = json.loads(equivalence_a.read_text())
    nullable_b = json.loads(equivalence_b.read_text())
    for nullable in (nullable_a, nullable_b):
        nullable["decay"]["bands"][0]["t30Rt60Sec"] = None
        nullable["decay"]["bands"][0]["withinPredictedTolerance"] = None
        nullable["decay"]["bands"][0]["predictedTargetRt60Sec"] = None
        nullable["eventualContraction"]["slopeDbPerSegment"] = None
        nullable["eventualContraction"]["negativeTrend"] = None
    nullable_a_path = workspace / "equivalence-nullable-a.json"
    nullable_b_path = workspace / "equivalence-nullable-b.json"
    nullable_a_path.write_text(json.dumps(nullable_a))
    nullable_b_path.write_text(json.dumps(nullable_b))
    matching_nulls = run(
        sys.executable, comparator, nullable_a_path, nullable_b_path
    )
    if matching_nulls.returncode != 0:
        raise AssertionError(
            "matching nullable structural evidence compared unequal: "
            f"{matching_nulls.stderr}{matching_nulls.stdout}"
        )

    nullable_b["eventualContraction"]["negativeTrend"] = False
    nullable_b_path.write_text(json.dumps(nullable_b))
    mismatched_null = run(
        sys.executable, comparator, nullable_a_path, nullable_b_path
    )
    if mismatched_null.returncode != 1:
        raise AssertionError(
            "nullable structural mismatch was not reported as a failure: "
            f"{mismatched_null.stdout}"
        )

    boolean_as_number = json.loads(equivalence_b.read_text())
    boolean_as_number["eventualContraction"]["negativeTrend"] = 1
    boolean_as_number_path = workspace / "equivalence-boolean-as-number.json"
    boolean_as_number_path.write_text(json.dumps(boolean_as_number))
    mismatched_type = run(
        sys.executable, comparator, equivalence_b, boolean_as_number_path
    )
    if mismatched_type.returncode != 1:
        raise AssertionError(
            "structural boolean accepted a numerically equal integer: "
            f"{mismatched_type.stdout}"
        )

    # A numeric metric that exceeds its committed tolerance must fail; a
    # custom tolerances document controls exactly where the pass/fail
    # boundary sits.
    custom_tolerances = workspace / "custom-tolerances.json"
    custom_tolerances.write_text(
        json.dumps(tolerance_document(1e-9, 1e-9, "tracer"))
    )
    numeric_breach = json.loads(equivalence_b.read_text())
    numeric_breach["alignmentScore"] += 1e-6
    numeric_breach_path = workspace / "equivalence-numeric-breach.json"
    numeric_breach_path.write_text(json.dumps(numeric_breach))
    breached = run(
        sys.executable,
        comparator,
        equivalence_a,
        numeric_breach_path,
        "--tolerances",
        custom_tolerances,
    )
    if breached.returncode != 1:
        raise AssertionError(
            f"numeric tolerance breach was not reported as a failure: {breached.stdout}"
        )
    if "alignmentScore" not in breached.stdout:
        raise AssertionError(
            f"numeric tolerance breach did not name the metric: {breached.stdout}"
        )

    within_tolerance = run(
        sys.executable,
        comparator,
        equivalence_a,
        equivalence_b,
        "--tolerances",
        custom_tolerances,
    )
    if within_tolerance.returncode != 0:
        raise AssertionError(
            f"identical platforms failed under a tight tolerances document: "
            f"{within_tolerance.stdout}"
        )

    # Architecture-specific tolerances preserve a tight x86_64-to-x86_64
    # comparison while allowing the separately evidenced arm64 FMA
    # contraction delta. All platform pairs must be compared: using only an
    # arm64 baseline would never exercise the tighter same-architecture
    # threshold when the group also contains two x86_64 platforms.
    architecture_tolerances = workspace / "architecture-tolerances.json"
    architecture_tolerances.write_text(
        json.dumps(
            tolerance_document(1e-9, 1e-5, "architecture-class tracer")
        )
    )
    arm64 = json.loads(equivalence_a.read_text())
    arm64["platform"] = "macOS"
    arm64["architecture"] = "arm64"
    macos_x86 = json.loads(equivalence_b.read_text())
    macos_x86["platform"] = "macOS"
    macos_x86["alignmentScore"] += 2e-6
    macos_x86_path = workspace / "equivalence-macos-x86_64.json"
    macos_x86_path.write_text(json.dumps(macos_x86))
    windows_x86 = json.loads(equivalence_b.read_text())
    windows_x86["alignmentScore"] += 4e-6
    windows_x86_path = workspace / "equivalence-windows-x86_64-offset.json"
    windows_x86_path.write_text(json.dumps(windows_x86))
    architecture_compared = run(
        sys.executable,
        comparator,
        equivalence_a,
        macos_x86_path,
        windows_x86_path,
        "--tolerances",
        architecture_tolerances,
    )
    if architecture_compared.returncode != 1:
        raise AssertionError(
            "same-architecture tolerance breach was hidden by the wider "
            f"cross-architecture tolerance: {architecture_compared.stdout}"
        )
    if (
        "macOS-x86_64 -> Windows-x86_64" not in architecture_compared.stdout
        or "comparison=sameArchitecture" not in architecture_compared.stdout
    ):
        raise AssertionError(
            "same-architecture failure did not identify its platform pair "
            f"and comparison class: {architecture_compared.stdout}"
        )
    if "arm64 ->" in architecture_compared.stdout and "[FAIL]" in "\n".join(
        line
        for line in architecture_compared.stdout.splitlines()
        if "arm64 ->" in line
    ):
        raise AssertionError(
            "cross-architecture delta did not use its wider tolerance: "
            f"{architecture_compared.stdout}"
        )

    report_path = workspace / "report.json"
    reported = run(
        sys.executable,
        comparator,
        equivalence_a,
        equivalence_b,
        "--json",
        report_path,
    )
    if reported.returncode != 0:
        raise AssertionError(f"unexpected comparison failure: {reported.stdout}")
    report = json.loads(report_path.read_text())
    if report["pass"] is not True or not report["groups"]:
        raise AssertionError(f"unexpected --json report: {report}")


if __name__ == "__main__":
    main()
