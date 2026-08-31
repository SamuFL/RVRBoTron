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
    # still exercising a Feedback Loop composition. One two-value axis
    # (delay-range) plus one one-value axis (gain-mode), so the sweep
    # produces exactly three points: reference, delay-range/large,
    # gain-mode/uniform.
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
                "name": "delay-range",
                "hypothesis": "tracer",
                "values": [
                    {
                        "label": "large",
                        "overrides": [
                            {
                                "op": "replace",
                                "path": "/composition/stages/1/delayMaxMs",
                                "value": 4.0,
                            }
                        ],
                    }
                ],
            },
            {
                "name": "gain-mode",
                "hypothesis": "tracer",
                "values": [
                    {
                        "label": "uniform",
                        "overrides": [
                            {
                                "op": "replace",
                                "path": "/composition/stages/1/gainMode",
                                "value": "uniform",
                            }
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

    # A real (tracer) sample sweeps every point.
    first = run_sweep(fixture)
    if first.returncode != 0:
        raise AssertionError(first.stdout + first.stderr)
    for expected in (
        "[completed] reference",
        "[completed] delay-range/large",
        "[completed] gain-mode/uniform",
    ):
        if expected not in first.stdout:
            raise AssertionError(f"missing {expected!r} in stdout: {first.stdout}")

    sample_root = output_dir / fixture.stem
    reference_dir = sample_root / "00-reference"
    delay_range_dir = sample_root / "01-delay-range" / "01-large"
    gain_mode_dir = sample_root / "02-gain-mode" / "01-uniform"

    # Output is laid out per sample, per axis, and per axis value, with
    # numeric prefixes ordering it for auditioning.
    for point_dir in (reference_dir, delay_range_dir, gain_mode_dir):
        for artifact in (
            point_dir / "sample" / "render.json",
            point_dir / "impulse" / "render.json",
            point_dir / "impulse" / "analysis" / "tail-v1.json",
            point_dir / "benchmark.json",
        ):
            if not artifact.exists():
                raise AssertionError(f"expected artifact missing: {artifact}")
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

    # The delay-range/large point's Resolved Configuration actually carries
    # the overridden value, materialized from the Reference plus exactly
    # that one axis's overrides.
    delay_range_request = json.loads(
        (delay_range_dir / "materialized" / "sample-request.json").read_text()
    )
    expected_request = json.loads(json.dumps(reference))
    expected_request["composition"]["stages"][1]["delayMaxMs"] = 4.0
    if delay_range_request != expected_request:
        raise AssertionError(
            f"delay-range/large did not materialize a single-axis override: "
            f"{delay_range_request} != {expected_request}"
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
    for name in ("reference", "delay-range/large", "gain-mode/uniform"):
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
        "delay-range/large",
        "gain-mode/uniform",
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

    # Resumable: a second run does not re-render or re-analyze anything.
    reference_output_before = (
        reference_dir / "sample" / "output.wav"
    ).read_bytes()
    second = run_sweep(fixture)
    if second.returncode != 0:
        raise AssertionError(f"resumed sweep failed: {second.stdout}{second.stderr}")
    for expected in (
        "[resumed] reference",
        "[resumed] delay-range/large",
        "[resumed] gain-mode/uniform",
    ):
        if expected not in second.stdout:
            raise AssertionError(f"resumed sweep did not report {expected!r}: {second.stdout}")
    reference_output_after = (
        reference_dir / "sample" / "output.wav"
    ).read_bytes()
    if reference_output_before != reference_output_after:
        raise AssertionError("resuming the sweep replaced completed evidence")

    # Resumability is per-step: a point missing only its benchmark retries
    # just that step, without re-rendering.
    gain_mode_benchmark = gain_mode_dir / "benchmark.json"
    gain_mode_benchmark.unlink()
    gain_mode_resolved_before = (
        gain_mode_dir / "sample" / "resolved.json"
    ).read_bytes()
    third = run_sweep(fixture)
    if "[completed] gain-mode/uniform" not in third.stdout:
        raise AssertionError(
            f"a point missing only its benchmark did not retry and complete: "
            f"{third.stdout}"
        )
    if not gain_mode_benchmark.exists():
        raise AssertionError("retried benchmark step did not publish its report")
    gain_mode_resolved_after = (
        gain_mode_dir / "sample" / "resolved.json"
    ).read_bytes()
    if gain_mode_resolved_before != gain_mode_resolved_after:
        raise AssertionError("retrying the benchmark step re-rendered the point")


if __name__ == "__main__":
    main()
