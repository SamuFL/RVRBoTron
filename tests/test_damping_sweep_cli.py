#!/usr/bin/env python3

import json
import shutil
import subprocess
import sys
from pathlib import Path


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

    # A millisecond-scale tracer catalog: N=2, delayStrategy "even" (see
    # test_feedback_loop_cli.py), delays 1/2 ms -- fast enough for CI while
    # still exercising a Damping-enabled Feedback Loop composition. One
    # two-value axis (high-ratio) plus one one-value axis (low-corner, which
    # overrides two fields together the way damping_sweep_v1.json's own
    # low-corner axis does), so the sweep produces exactly three points:
    # reference, high-ratio/strong, low-corner/100Hz.
    reference = {
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
                    "gainMode": "per-channel",
                    "damping": {
                        "highRatio": 0.5,
                        "highHz": 4000,
                        "lowRatio": 1.0,
                        "lowHz": 200,
                    },
                },
                {"type": "downmix", "strategy": "select"},
            ]
        },
    }
    tracer_catalog = {
        "formatVersion": 1,
        "reference": reference,
        "axes": [
            {
                "name": "high-ratio",
                "hypothesis": "tracer",
                "values": [
                    {
                        "label": "strong",
                        "overrides": [
                            {
                                "op": "replace",
                                "path": "/composition/stages/1/damping/highRatio",
                                "value": 0.2,
                            }
                        ],
                    }
                ],
            },
            {
                "name": "low-corner",
                "hypothesis": "tracer",
                "values": [
                    {
                        "label": "100Hz",
                        "overrides": [
                            {
                                "op": "replace",
                                "path": "/composition/stages/1/damping/lowRatio",
                                "value": 0.4,
                            },
                            {
                                "op": "replace",
                                "path": "/composition/stages/1/damping/lowHz",
                                "value": 100,
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

    def run_sweep(sample):
        return run(
            sys.executable,
            runner,
            "--catalog",
            catalog_path,
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
            "--warmup-seconds",
            "0.02",
            "--measure-seconds",
            "0.02",
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

    # An unpulled Git LFS pointer (present on disk, but pointer text rather
    # than audio) is likewise skipped, not failed, with its own distinct
    # explanation.
    lfs_pointer = workspace / "lfs-pointer.wav"
    lfs_pointer.write_bytes(
        b"version https://git-lfs.github.com/spec/v1\n"
        b"oid sha256:0000000000000000000000000000000000000000000000000000000000000000\n"
        b"size 12345\n"
    )
    pointer_result = run_sweep(lfs_pointer)
    if pointer_result.returncode != 0:
        raise AssertionError(
            f"an unpulled LFS pointer should not fail the sweep: {pointer_result.stdout}"
        )
    if "git lfs pull" not in pointer_result.stdout or "pointer" not in pointer_result.stdout:
        raise AssertionError(
            f"LFS pointer sample was not explained: {pointer_result.stdout}"
        )
    if output_dir.exists():
        raise AssertionError("a skipped (LFS pointer) sweep unexpectedly created output")

    # A sample that exists locally but is not a readable WAV file fails
    # cleanly with an explanatory message, not an unhandled traceback.
    invalid_wav = workspace / "invalid.wav"
    invalid_wav.write_bytes(b"not a wav file at all")
    invalid = run_sweep(invalid_wav)
    if invalid.returncode != 1:
        raise AssertionError(f"invalid WAV sample should fail cleanly: {invalid.stdout}")
    if "Traceback" in invalid.stderr:
        raise AssertionError(f"invalid WAV sample crashed instead of failing cleanly: {invalid.stderr}")
    if "not a readable WAV file" not in invalid.stderr:
        raise AssertionError(f"invalid WAV sample was not explained: {invalid.stderr}")

    # A malformed catalog (an axis missing a required field) fails with a
    # descriptive message naming the field, not a bare KeyError.
    malformed_catalog = dict(tracer_catalog)
    malformed_catalog["axes"] = [{"values": []}]
    malformed_catalog_path = workspace / "malformed-sweep.json"
    malformed_catalog_path.write_text(json.dumps(malformed_catalog))
    malformed = run(
        sys.executable,
        runner,
        "--catalog",
        malformed_catalog_path,
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
        output_dir,
    )
    if malformed.returncode != 1:
        raise AssertionError(f"malformed catalog should fail cleanly: {malformed.stdout}")
    if "missing required field" not in malformed.stderr or "'name'" not in malformed.stderr:
        raise AssertionError(
            f"malformed catalog surfaced a bare KeyError instead of a "
            f"descriptive message naming the field: {malformed.stderr}"
        )

    # A real (tracer) sample sweeps every point.
    first = run_sweep(fixture)
    if first.returncode != 0:
        raise AssertionError(first.stdout + first.stderr)
    for expected in (
        "[completed] reference",
        "[completed] high-ratio/strong",
        "[completed] low-corner/100Hz",
    ):
        if expected not in first.stdout:
            raise AssertionError(f"missing {expected!r} in stdout: {first.stdout}")

    sample_root = output_dir / fixture.stem
    reference_dir = sample_root / "00-reference"
    high_ratio_dir = sample_root / "01-high-ratio" / "01-strong"
    low_corner_dir = sample_root / "02-low-corner" / "01-100Hz"

    # Output is laid out per sample, per axis, and per axis value, with
    # numeric prefixes ordering it for auditioning; every point publishes
    # tail-v2 (not tail-v1) evidence.
    for point_dir in (reference_dir, high_ratio_dir, low_corner_dir):
        for artifact in (
            point_dir / "sample" / "render.json",
            point_dir / "impulse" / "render.json",
            point_dir / "impulse" / "analysis" / "tail-v2.json",
            point_dir / "benchmark.json",
        ):
            if not artifact.exists():
                raise AssertionError(f"expected artifact missing: {artifact}")
        if (point_dir / "impulse" / "analysis" / "tail-v1.json").exists():
            raise AssertionError(
                f"damping sweep unexpectedly published tail-v1 evidence: {point_dir}"
            )
        # Tail analysis runs against the impulse render, never the sample.
        if (point_dir / "sample" / "analysis").exists():
            raise AssertionError(
                f"tail analysis was published against the sample render: {point_dir}"
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

    # The high-ratio/strong point's Resolved Configuration actually carries
    # the overridden value, materialized from the Reference plus exactly
    # that one axis's overrides.
    high_ratio_request = json.loads(
        (high_ratio_dir / "materialized" / "sample-request.json").read_text()
    )
    expected_request = json.loads(json.dumps(reference))
    expected_request["composition"]["stages"][1]["damping"]["highRatio"] = 0.2
    if high_ratio_request != expected_request:
        raise AssertionError(
            f"high-ratio/strong did not materialize a single-axis override: "
            f"{high_ratio_request} != {expected_request}"
        )

    # The low-corner/100Hz point overrides both lowRatio and lowHz together
    # (one conceptual axis, two related fields -- see damping_sweep_v1.json's
    # own low-corner axis, whose Reference lowRatio is bypassed at 1.0).
    low_corner_request = json.loads(
        (low_corner_dir / "materialized" / "sample-request.json").read_text()
    )
    expected_low_corner = json.loads(json.dumps(reference))
    expected_low_corner["composition"]["stages"][1]["damping"]["lowRatio"] = 0.4
    expected_low_corner["composition"]["stages"][1]["damping"]["lowHz"] = 100
    if low_corner_request != expected_low_corner:
        raise AssertionError(
            f"low-corner/100Hz did not materialize its two-field axis "
            f"override: {low_corner_request} != {expected_low_corner}"
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

    report = json.loads((sample_root / "sweep-report.json").read_text())
    if report["sample"] != fixture.name:
        raise AssertionError(f"unexpected sweep report header: {report}")
    for name in ("reference", "high-ratio/strong", "low-corner/100Hz"):
        if outcome(report, name)["status"] != "completed":
            raise AssertionError(f"unexpected sweep report: {report}")

    # Every point is benchmarked, and the benchmarks aggregate into a
    # ranked cross-point comparison, so acoustic preference and processing
    # cost are visible together -- not just one point's cost at a time.
    summary = json.loads((sample_root / "benchmark-summary.json").read_text())
    if summary["referenceCase"] != "reference" or summary["blockSize"] != 32:
        raise AssertionError(f"unexpected benchmark summary header: {summary}")
    ranked = summary["rankedBySlowestMedian"]
    if {entry["case"] for entry in ranked} != {
        "reference",
        "high-ratio/strong",
        "low-corner/100Hz",
    }:
        raise AssertionError(f"unexpected ranked points: {summary}")
    medians = [entry["medianBlockSeconds"] for entry in ranked]
    if medians != sorted(medians, reverse=True):
        raise AssertionError(f"ranking was not descending by median: {summary}")
    reference_entry = next(e for e in ranked if e["case"] == "reference")
    if (
        reference_entry["deltaFromReferenceMedianSeconds"] != 0
        or reference_entry["ratioToReferenceMedian"] != 1.0
    ):
        raise AssertionError(f"Reference did not compare exactly to itself: {summary}")

    # A self-contained listening report is generated, presenting every
    # point's renders, requested/measured Damping ratios, the octave-band
    # decay curve, complete-response/contraction status, and benchmark cost
    # -- readable on its own, without the terminal output that produced it.
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
        "high-ratio/strong",
        "low-corner/100Hz",
        # Renders: relative paths only, playable alongside the report.
        "00-reference/sample/output.wav",
        "00-reference/impulse/output.wav",
        # Requested Damping ratios/corners.
        "highRatio",
        "lowHz",
        # Measured decay against the requested RT60 and the canonical
        # low/Reference/high ratio summary.
        "Requested RT60",
        "Measured RT60",
        "Requested ratio",
        "Measured ratio",
        # The full octave-band predicted-vs-measured curve.
        "Full octave-band decay curve",
        "Within +/-10%",
        # Complete-response and eventual-contraction status.
        "Frame count check",
        "Eventual contraction",
        # Benchmark cost.
        "Median block time",
    ):
        if expected not in report_html:
            raise AssertionError(f"listening report is missing {expected!r}")

    # Resumable: a second run does not re-render or re-analyze anything, but
    # the report is regenerated as part of that same run -- deleting it and
    # rerunning brings it back without touching any render.
    reference_output_before = (
        reference_dir / "sample" / "output.wav"
    ).read_bytes()
    report_html_path.unlink()
    second = run_sweep(fixture)
    if second.returncode != 0:
        raise AssertionError(f"resumed sweep failed: {second.stdout}{second.stderr}")
    for expected in (
        "[resumed] reference",
        "[resumed] high-ratio/strong",
        "[resumed] low-corner/100Hz",
    ):
        if expected not in second.stdout:
            raise AssertionError(f"resumed sweep did not report {expected!r}: {second.stdout}")
    reference_output_after = (
        reference_dir / "sample" / "output.wav"
    ).read_bytes()
    if reference_output_before != reference_output_after:
        raise AssertionError("resuming the sweep replaced completed evidence")
    if not report_html_path.exists():
        raise AssertionError(
            "listening report was not regenerated by a fully resumed sweep "
            "(no re-rendering involved)"
        )

    # Resumability is per-step: a point missing only its benchmark retries
    # just that step, without re-rendering.
    low_corner_benchmark = low_corner_dir / "benchmark.json"
    low_corner_benchmark.unlink()
    low_corner_resolved_before = (
        low_corner_dir / "sample" / "resolved.json"
    ).read_bytes()
    third = run_sweep(fixture)
    if "[completed] low-corner/100Hz" not in third.stdout:
        raise AssertionError(
            f"a point missing only its benchmark did not retry and complete: "
            f"{third.stdout}"
        )
    if not low_corner_benchmark.exists():
        raise AssertionError("retried benchmark step did not publish its report")
    low_corner_resolved_after = (
        low_corner_dir / "sample" / "resolved.json"
    ).read_bytes()
    if low_corner_resolved_before != low_corner_resolved_after:
        raise AssertionError("retrying the benchmark step re-rendered the point")


if __name__ == "__main__":
    main()
