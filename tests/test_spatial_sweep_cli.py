#!/usr/bin/env python3

import html
import json
import shutil
import subprocess
import sys
from pathlib import Path


def section_html(report_html, name):
    """The report's HTML for one named point -- from its own `<h2>`
    heading up to the next point's, or end of document for the last point
    -- so an assertion about one point's evidence can't be satisfied by
    another point's section instead."""
    marker = f"<h2>{html.escape(name)}"
    start = report_html.find(marker)
    if start == -1:
        raise AssertionError(f"listening report has no section for {name!r}")
    next_marker = report_html.find("<h2>", start + len(marker))
    return report_html[start:] if next_marker == -1 else report_html[start:next_marker]


def run(*arguments):
    return subprocess.run(
        list(map(str, arguments)),
        check=False,
        capture_output=True,
        text=True,
    )


def outcome(report, name):
    return next(entry for entry in report["points"] if entry["name"] == name)


def main():
    runner = Path(sys.argv[1])
    renderer = Path(sys.argv[2])
    analyzer = Path(sys.argv[3])
    fixture = Path(sys.argv[4])
    stereo_fixture = Path(sys.argv[5])
    workspace = Path(sys.argv[6])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    # A millisecond-scale tracer catalog: N=2, a single-step Diffuser
    # (delayStrategy "even", matching test_feedback_loop_cli.py's own
    # deterministic convention) feeding a short Feedback Loop, and a
    # single Early tap at that one step -- fast enough for CI while still
    # exercising both branches, both Downmixes, and (via the
    # coherent-downmix axis) the Coherent Downmix comparator. One
    # one-value axis (early-level) plus the coherent-downmix axis itself
    # (also one value: sum-all), so the sweep produces exactly three
    # points: reference, early-level/loud, coherent-downmix/sum-all.
    reference = {
        "formatVersion": 2,
        "seed": 7,
        "composition": {
            "mainEnabled": True,
            "mainLevelDb": 0,
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
                        "delayStrategy": "even",
                        "mix": "hadamard",
                        "shuffle": True,
                        "polarity": "seeded-random",
                    },
                },
                {
                    "type": "feedback-loop",
                    "delayMinMs": 1.0,
                    "delayMaxMs": 2.0,
                    "delayStrategy": "even",
                    "rt60Sec": 0.05,
                    "mix": "householder",
                    "gainMode": "per-channel",
                },
                {
                    "type": "downmix",
                    "strategy": "orthogonal-rows",
                    "normalisation": "energy",
                    "widthDeg": 90,
                },
            ],
            "early": {
                "enabled": True,
                "levelDb": -6,
                "decayDbPerSec": 0,
                "taps": [{"stepIndex": 0, "gainDb": 0}],
                "downmix": {
                    "strategy": "select",
                    "leftChannel": 0,
                    "rightChannel": 1,
                    "widthDeg": 90,
                    "normalisation": "energy",
                },
            },
        },
    }
    tracer_catalog = {
        "formatVersion": 1,
        "reference": reference,
        "axes": [
            {
                "name": "early-level",
                "hypothesis": "tracer",
                "scope": "/composition/early/levelDb",
                "values": [
                    {
                        "label": "loud",
                        "overrides": [
                            {
                                "op": "replace",
                                "path": "/composition/early/levelDb",
                                "value": 0,
                            }
                        ],
                    }
                ],
            },
            {
                "name": "coherent-downmix",
                "hypothesis": "tracer",
                "scope": "/composition/early/downmix",
                "values": [
                    {
                        "label": "sum-all",
                        "overrides": [
                            {"op": "remove", "path": "/composition/early/downmix/leftChannel"},
                            {"op": "remove", "path": "/composition/early/downmix/rightChannel"},
                            {
                                "op": "replace",
                                "path": "/composition/early/downmix/strategy",
                                "value": "sum-all",
                            },
                        ],
                    }
                ],
            },
        ],
    }
    catalog_path = workspace / "tracer-sweep.json"
    catalog_path.write_text(json.dumps(tracer_catalog, indent=2))

    output_dir = workspace / "output"

    def run_sweep(sample, catalog=catalog_path):
        return run(
            sys.executable,
            runner,
            "--catalog",
            catalog,
            "--renderer",
            renderer,
            "--analyzer",
            analyzer,
            "--sample",
            sample,
            "--mono-impulse",
            fixture,
            "--stereo-impulse",
            stereo_fixture,
            "--output",
            output_dir,
            "--block-size",
            "32",
        )

    # A missing sample is skipped, not failed -- no output is created.
    missing = run_sweep(workspace / "does-not-exist.wav")
    if missing.returncode != 0:
        raise AssertionError(f"missing sample should not fail the sweep: {missing.stdout}")
    if "[skipped]" not in missing.stdout or "git lfs pull" not in missing.stdout:
        raise AssertionError(f"missing sample was not explained: {missing.stdout}")
    if output_dir.exists():
        raise AssertionError("a skipped sweep unexpectedly created output")

    # A malformed catalog (an axis missing a required field) fails with a
    # descriptive message naming the field, not a bare KeyError.
    malformed_catalog = dict(tracer_catalog)
    malformed_catalog["axes"] = [{"values": []}]
    malformed_catalog_path = workspace / "malformed-sweep.json"
    malformed_catalog_path.write_text(json.dumps(malformed_catalog))
    malformed = run_sweep(fixture, catalog=malformed_catalog_path)
    if malformed.returncode != 1:
        raise AssertionError(f"malformed catalog should fail cleanly: {malformed.stdout}")
    if "missing required field" not in malformed.stderr or "'name'" not in malformed.stderr:
        raise AssertionError(
            f"malformed catalog surfaced a bare KeyError instead of a "
            f"descriptive message naming the field: {malformed.stderr}"
        )

    # A catalog point whose overrides reach outside its axis's own
    # declared scope fails loudly, naming the offending path, rather than
    # silently confounding the sweep (issue #116's own isolation proof).
    # Run against a dedicated output directory: unlike the malformed-
    # catalog case above (which fails before any point renders), the
    # *other* points in this catalog (reference, coherent-downmix/sum-all)
    # still complete, and reusing `output_dir` here would let their
    # results leak into the "real sample sweeps every point" run below,
    # turning its expected "[completed]" points into "[resumed]" ones.
    bad_scope_output_dir = workspace / "bad-scope-output"
    bad_scope_catalog = json.loads(json.dumps(tracer_catalog))
    bad_scope_catalog["axes"][0]["values"][0]["overrides"].append(
        {"op": "replace", "path": "/composition/mainLevelDb", "value": -3}
    )
    bad_scope_catalog_path = workspace / "bad-scope-sweep.json"
    bad_scope_catalog_path.write_text(json.dumps(bad_scope_catalog))
    bad_scope = run(
        sys.executable,
        runner,
        "--catalog",
        bad_scope_catalog_path,
        "--renderer",
        renderer,
        "--analyzer",
        analyzer,
        "--sample",
        fixture,
        "--mono-impulse",
        fixture,
        "--stereo-impulse",
        stereo_fixture,
        "--output",
        bad_scope_output_dir,
        "--block-size",
        "32",
    )
    if bad_scope.returncode != 1:
        raise AssertionError(
            f"an out-of-scope override should fail the sweep: {bad_scope.stdout}"
        )
    if (
        "outside its declared scope" not in bad_scope.stdout
        or "/composition/mainLevelDb" not in bad_scope.stdout
    ):
        raise AssertionError(
            f"out-of-scope override was not explained: {bad_scope.stdout}"
        )
    bad_scope_point = outcome(
        json.loads(
            (bad_scope_output_dir / fixture.stem / "sweep-report.json").read_text()
        ),
        "early-level/loud",
    )
    if bad_scope_point["status"] != "failed":
        raise AssertionError(
            f"the point with the out-of-scope override should itself be "
            f"reported failed: {bad_scope_point}"
        )

    # A real (tracer) sample sweeps every point.
    first = run_sweep(fixture)
    if first.returncode != 0:
        raise AssertionError(first.stdout + first.stderr)
    for expected in (
        "[completed] reference",
        "[completed] early-level/loud",
        "[completed] coherent-downmix/sum-all",
    ):
        if expected not in first.stdout:
            raise AssertionError(f"missing {expected!r} in stdout: {first.stdout}")

    sample_root = output_dir / fixture.stem
    reference_dir = sample_root / "00-reference"
    early_level_dir = sample_root / "01-early-level" / "01-loud"
    coherent_dir = sample_root / "02-coherent-downmix" / "01-sum-all"

    # Output is laid out per sample, per axis, and per axis value, with
    # numeric prefixes ordering it for auditioning.
    for point_dir in (reference_dir, early_level_dir, coherent_dir):
        for artifact in (
            point_dir / "sample" / "render.json",
            point_dir / "impulse" / "render.json",
            point_dir / "impulse" / "analysis" / "downmix-v1.json",
            point_dir / "determinism.json",
        ):
            if not artifact.exists():
                raise AssertionError(f"expected artifact missing: {artifact}")
        # Downmix analysis runs against the impulse render, never the
        # sample (it needs Stage captures; the sample render has none).
        if (point_dir / "sample" / "analysis").exists():
            raise AssertionError(
                f"downmix analysis was published against the sample render: {point_dir}"
            )
        # Every sweep point renders both the sample and the deterministic
        # impulse under an identical Resolved Configuration.
        sample_resolved = (point_dir / "sample" / "resolved.json").read_bytes()
        impulse_resolved = (point_dir / "impulse" / "resolved.json").read_bytes()
        if sample_resolved != impulse_resolved:
            raise AssertionError(
                f"sample and impulse renders did not share a Resolved "
                f"Configuration: {point_dir}"
            )
        # The determinism check actually ran and reported success, and
        # cleaned up its own scratch repeat render rather than leaving it
        # behind.
        determinism = json.loads((point_dir / "determinism.json").read_text())
        if determinism.get("deterministic") is not True:
            raise AssertionError(f"determinism check did not pass: {point_dir}")
        if (point_dir / "impulse-repeat").exists():
            raise AssertionError(
                f"determinism check left its scratch repeat render behind: "
                f"{point_dir}"
            )

    # The early-level/loud point's Resolved Configuration actually carries
    # the overridden value, materialized from the Reference plus exactly
    # that one axis's overrides.
    early_level_request = json.loads(
        (early_level_dir / "materialized" / "sample-request.json").read_text()
    )
    expected_request = json.loads(json.dumps(reference))
    expected_request["composition"]["early"]["levelDb"] = 0
    if early_level_request != expected_request:
        raise AssertionError(
            f"early-level/loud did not materialize a single-axis override: "
            f"{early_level_request} != {expected_request}"
        )

    # The Reference point itself carries no overrides.
    reference_request = json.loads(
        (reference_dir / "materialized" / "sample-request.json").read_text()
    )
    if reference_request != reference:
        raise AssertionError(
            f"reference point was not the unmodified Reference configuration: "
            f"{reference_request}"
        )

    # The Coherent Downmix comparator ran, comparing the matched select
    # control (the Reference's own Early Downmix) against the sum-all
    # ablation, non-fatally -- the ablation point itself still completed.
    comparison_path = sample_root / "coherent-downmix-comparison.json"
    comparison = json.loads(comparison_path.read_text())
    if (
        comparison["controlPoint"] != "reference"
        or comparison["controlStrategy"] != "select"
        or comparison["ablationPoint"] != "coherent-downmix/sum-all"
        or comparison["ablationStrategy"] != "sum-all"
        or comparison["ablationTagged"] is not True
    ):
        raise AssertionError(f"unexpected Coherent Downmix comparison: {comparison}")
    if not isinstance(comparison["peakFactorDelta"], float):
        raise AssertionError(f"peakFactorDelta was not measured: {comparison}")
    if not comparison["warning"]:
        raise AssertionError(f"comparison carried no warning: {comparison}")

    report = json.loads((sample_root / "sweep-report.json").read_text())
    if report["sample"] != fixture.name:
        raise AssertionError(f"unexpected sweep report header: {report}")
    if report["coherentDownmixComparison"] != comparison:
        raise AssertionError(
            f"sweep report did not embed the Coherent Downmix comparison: {report}"
        )
    for name in ("reference", "early-level/loud", "coherent-downmix/sum-all"):
        if outcome(report, name)["status"] != "completed":
            raise AssertionError(f"unexpected sweep report: {report}")

    # A self-contained listening report is generated, presenting every
    # point's renders and Downmix evidence, plus the comparison -- readable
    # on its own, without the terminal output that produced it.
    report_html_path = sample_root / "listening-report.html"
    report_html = report_html_path.read_text()
    if "http://" in report_html or "https://" in report_html:
        raise AssertionError(
            "listening report references an external URL, is not self-contained"
        )
    if "<script" in report_html:
        raise AssertionError("listening report includes a script, is not self-contained")
    for expected in (
        "reference",
        "early-level/loud",
        "coherent-downmix/sum-all",
        "Coherent Downmix comparison",
        # Renders: relative paths only, playable alongside the report.
        "00-reference/sample/output.wav",
        "00-reference/impulse/output.wav",
    ):
        if expected not in report_html:
            raise AssertionError(f"listening report is missing {expected!r}")
    coherent_section = section_html(report_html, "coherent-downmix/sum-all")
    if "sum-all" not in coherent_section:
        raise AssertionError(
            f"coherent-downmix/sum-all's own section did not report its "
            f"strategy: {coherent_section}"
        )

    # Resumable: a second run does not re-render or re-analyze anything,
    # but the report is regenerated as part of that same run -- deleting
    # it and rerunning brings it back without touching any render.
    reference_output_before = (reference_dir / "sample" / "output.wav").read_bytes()
    report_html_path.unlink()
    second = run_sweep(fixture)
    if second.returncode != 0:
        raise AssertionError(f"resumed sweep failed: {second.stdout}{second.stderr}")
    for expected in (
        "[resumed] reference",
        "[resumed] early-level/loud",
        "[resumed] coherent-downmix/sum-all",
    ):
        if expected not in second.stdout:
            raise AssertionError(f"resumed sweep did not report {expected!r}: {second.stdout}")
    reference_output_after = (reference_dir / "sample" / "output.wav").read_bytes()
    if reference_output_before != reference_output_after:
        raise AssertionError("resuming the sweep replaced completed evidence")
    if not report_html_path.exists():
        raise AssertionError(
            "listening report was not regenerated by a fully resumed sweep "
            "(no re-rendering involved)"
        )

    # Resumability is per-step: a point missing only its determinism
    # marker retries just that step, without re-rendering.
    early_level_determinism = early_level_dir / "determinism.json"
    early_level_determinism.unlink()
    early_level_resolved_before = (
        early_level_dir / "sample" / "resolved.json"
    ).read_bytes()
    third = run_sweep(fixture)
    if "[completed] early-level/loud" not in third.stdout:
        raise AssertionError(
            f"a point missing only its determinism marker did not retry "
            f"and complete: {third.stdout}"
        )
    if not early_level_determinism.exists():
        raise AssertionError("retried determinism step did not publish its marker")
    early_level_resolved_after = (
        early_level_dir / "sample" / "resolved.json"
    ).read_bytes()
    if early_level_resolved_before != early_level_resolved_after:
        raise AssertionError("retrying the determinism step re-rendered the point")


if __name__ == "__main__":
    main()
