#!/usr/bin/env python3

import json
import shutil
import subprocess
import sys
from pathlib import Path

REQUIRED_METRIC_KEYS = {
    "nonFiniteSampleCount",
    "decayRt60Sec",
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


def main():
    extractor = Path(sys.argv[1])
    comparator = Path(sys.argv[2])
    analyzer = Path(sys.argv[3])
    renderer = Path(sys.argv[4])
    fixture = Path(sys.argv[5])
    workspace = Path(sys.argv[6])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    # Committed tolerances document: schema-complete for both precisions,
    # independent of whatever specific numbers get tightened over time.
    tolerances_document = json.loads(
        (
            Path(__file__).resolve().parent.parent
            / "tools"
            / "tail_tolerances_v1.json"
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
            if not isinstance(entry["absoluteTolerance"], (int, float)):
                raise AssertionError(f"{precision}.{name} tolerance is not numeric")
            if not entry.get("rationale"):
                raise AssertionError(f"{precision}.{name} has no rationale")

    # N=2, delayStrategy "even" (see test_feedback_loop_cli.py and
    # test_tail_analyzer_cli.py): the same millisecond-scale CI tracer
    # already proven fast and well-resolved for octave-band analysis.
    request = workspace / "request.json"
    request.write_text(
        json.dumps(
            {
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
    if "twelfthOctaveCurve" in dumped or "segmentEnergies" in dumped:
        raise AssertionError(
            "compact tail-equivalence artifact leaked bulky per-sample evidence"
        )
    bands = equivalence["decay"]["bands"]
    if len(bands) != 9:
        raise AssertionError(f"unexpected octave-band count: {equivalence}")
    for band in bands:
        if {"centerHz", "t20Rt60Sec", "t30Rt60Sec"} - set(band):
            raise AssertionError(f"unexpected band shape: {band}")
    if equivalence["nonFiniteSampleCount"] != 0:
        raise AssertionError(f"unexpected non-finite samples: {equivalence}")
    if equivalence["decayEnvelopeMonotonic"] is not True:
        raise AssertionError(f"unexpected decay-envelope evidence: {equivalence}")

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
        raise AssertionError("extraction succeeded without tail-v1.json")

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
    # that diverges must fail unconditionally -- it is never looked up in a
    # tolerances document.
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

    # Whether a band's T20/T30 fit exists at all is likewise structural: a
    # platform that fails to fit where the baseline succeeded must fail
    # unconditionally, never silently dropped from the numeric comparison.
    fit_presence_mismatch = json.loads(equivalence_b.read_text())
    fit_presence_mismatch["decay"]["bands"][4]["t30Rt60Sec"] = None
    fit_presence_mismatch_path = workspace / "equivalence-fit-presence-mismatch.json"
    fit_presence_mismatch_path.write_text(json.dumps(fit_presence_mismatch))
    fit_presence_mismatched = run(
        sys.executable, comparator, equivalence_a, fit_presence_mismatch_path
    )
    if fit_presence_mismatched.returncode != 1:
        raise AssertionError(
            f"T20/T30 fit-presence mismatch was not reported as a failure: "
            f"{fit_presence_mismatched.stdout}"
        )
    if "decay.t30Present" not in fit_presence_mismatched.stdout:
        raise AssertionError(
            f"fit-presence mismatch did not name the metric: "
            f"{fit_presence_mismatched.stdout}"
        )

    # The decay-envelope monotonicity flag is likewise structural.
    monotonic_mismatch = json.loads(equivalence_b.read_text())
    monotonic_mismatch["decayEnvelopeMonotonic"] = False
    monotonic_mismatch_path = workspace / "equivalence-monotonic-mismatch.json"
    monotonic_mismatch_path.write_text(json.dumps(monotonic_mismatch))
    monotonic_mismatched = run(
        sys.executable, comparator, equivalence_a, monotonic_mismatch_path
    )
    if monotonic_mismatched.returncode != 1:
        raise AssertionError(
            f"decay-envelope monotonicity mismatch was not reported as a "
            f"failure: {monotonic_mismatched.stdout}"
        )
    if "decayEnvelopeMonotonic" not in monotonic_mismatched.stdout:
        raise AssertionError(
            f"monotonicity mismatch did not name the metric: "
            f"{monotonic_mismatched.stdout}"
        )

    # A numeric metric that exceeds its committed tolerance must fail; a
    # custom tolerances document (independent of the real committed one)
    # controls exactly where the pass/fail boundary sits.
    custom_tolerances = workspace / "custom-tolerances.json"
    custom_tolerances.write_text(
        json.dumps(
            {
                "byPrecision": {
                    precision: {
                        key: {"absoluteTolerance": 1e-9, "rationale": "tracer"}
                        for key in REQUIRED_METRIC_KEYS
                    }
                    for precision in ("float32", "float64")
                }
            }
        )
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
