#!/usr/bin/env python3

import json
import shutil
import subprocess
import sys
from pathlib import Path

REQUIRED_METRIC_KEYS = {
    "energyRelativeError",
    "orthogonalityMaximumAbsoluteError",
    "orthogonalityRmsError",
    "correlationMeanAbsoluteOffDiagonal",
    "correlationMaxAbsoluteOffDiagonal",
    "alignmentMinimum",
    "alignmentMean",
    "colorationPeakToPeakDb",
    "colorationRmsDb",
    "colorationSpectralFlatness",
    "densityCount",
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
            / "diffusion_tolerances_v1.json"
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

    request = workspace / "request.json"
    request.write_text(
        json.dumps(
            {
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
        "--capture-stages",
        "all",
        "--output",
        render_result,
    )
    if rendered.returncode != 0:
        raise AssertionError(rendered.stderr)
    analyzed = run(
        sys.executable, analyzer, render_result, "--source", fixture
    )
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
    if "twelfthOctaveCurve" in json.dumps(equivalence):
        raise AssertionError(
            "compact equivalence artifact leaked the full Coloration curve"
        )
    if equivalence["density"] != {"echoPaths": 8, "totalDistinctArrivals": 8, "bins": [8]}:
        raise AssertionError(f"unexpected density evidence: {equivalence}")
    if equivalence["orthogonality"] != [
        {
            "stepIndex": 0,
            "maximumAbsoluteError": 2.220446049250313e-16,
            "rmsError": 7.850462293418876e-17,
        }
    ]:
        raise AssertionError(f"unexpected orthogonality evidence: {equivalence}")

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
        raise AssertionError("extraction succeeded without diffusion-v1.json")

    # A second, identical extraction relabeled as another platform: an
    # exact match compares clean against the tiny, well-separated tracer.
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

    # A structural metric (the N^k Echo-path count, a deterministic
    # function of the Resolved Configuration) that diverges must fail
    # unconditionally -- it is never looked up in a tolerances document.
    exact_mismatch = json.loads(equivalence_b.read_text())
    exact_mismatch["density"]["echoPaths"] = 7
    exact_mismatch_path = workspace / "equivalence-exact-mismatch.json"
    exact_mismatch_path.write_text(json.dumps(exact_mismatch))
    mismatched = run(sys.executable, comparator, equivalence_a, exact_mismatch_path)
    if mismatched.returncode != 1:
        raise AssertionError(
            f"structural-metric mismatch was not reported as a failure: {mismatched.stdout}"
        )
    if "density.echoPaths" not in mismatched.stdout:
        raise AssertionError(
            f"structural-metric mismatch did not name the metric: {mismatched.stdout}"
        )

    # Measured Distinct-arrival evidence goes through the same
    # tolerance-committed mechanism as every other measured metric --
    # against the real committed tolerances file, whose densityCount entry
    # is currently zero, so any divergence still fails.
    density_mismatch = json.loads(equivalence_b.read_text())
    density_mismatch["density"]["totalDistinctArrivals"] = 7
    density_mismatch_path = workspace / "equivalence-density-mismatch.json"
    density_mismatch_path.write_text(json.dumps(density_mismatch))
    density_mismatched = run(
        sys.executable, comparator, equivalence_a, density_mismatch_path
    )
    if density_mismatched.returncode != 1:
        raise AssertionError(
            f"density count mismatch was not reported as a failure: "
            f"{density_mismatched.stdout}"
        )
    if "densityCount[metric=totalDistinctArrivals]" not in density_mismatched.stdout:
        raise AssertionError(
            f"density count mismatch did not name the metric: "
            f"{density_mismatched.stdout}"
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
    numeric_breach["orthogonality"][0]["maximumAbsoluteError"] += 1e-6
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
    if "orthogonalityMaximumAbsoluteError" not in breached.stdout:
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
