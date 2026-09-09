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


def outcome(report, section, name):
    return next(entry for entry in report[section] if entry["name"] == name)


def main():
    renderer = Path(sys.argv[1])
    analyzer = Path(sys.argv[2])
    runner = Path(sys.argv[3])
    fixture_dir = Path(sys.argv[4])
    workspace = Path(sys.argv[5])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    mono_source = fixture_dir / "impulse-mono-pcm16-48000.wav"
    stereo_source = fixture_dir / "impulse-stereo-left-pcm16-48000.wav"

    # A small tracer catalog: two configurations that must succeed, two
    # that must fail cleanly (one at materialization time via an unknown
    # JSON Pointer, one at render time via an invalid resolved value), and
    # two listening cases -- one whose sample is present, one that is not.
    reference = {
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
                {
                    "type": "downmix",
                    "strategy": "select",
                    "leftChannel": 0,
                    "rightChannel": 1,
                    "normalisation": "energy",
                },
            ]
        },
    }
    tracer_catalog = {
        "formatVersion": 1,
        "reference": reference,
        "cases": [
            {"name": "reference", "source": "mono", "overrides": []},
            {
                "name": "n-4",
                "source": "mono",
                "overrides": [
                    {
                        "op": "replace",
                        "path": "/composition/stages/0/channels",
                        "value": 4,
                    }
                ],
            },
            {
                "name": "lengths-explicit",
                "source": "mono",
                "overrides": [
                    {"op": "remove", "path": "/composition/stages/1/steps"},
                    {"op": "remove", "path": "/composition/stages/1/totalMs"},
                    {"op": "remove", "path": "/composition/stages/1/distribution"},
                    {
                        "op": "add",
                        "path": "/composition/stages/1/lengthsMs",
                        "value": [1],
                    },
                ],
            },
            {
                "name": "invalid-override",
                "source": "mono",
                "overrides": [
                    {
                        "op": "replace",
                        "path": "/composition/stages/5/bogus",
                        "value": 1,
                    }
                ],
            },
            {
                "name": "invalid-config",
                "source": "mono",
                "overrides": [
                    {
                        "op": "replace",
                        "path": "/composition/stages/0/channels",
                        "value": 3,
                    }
                ],
            },
        ],
        "listeningCases": [
            {
                "name": "listening-found",
                "sample": "impulse-mono-pcm16-48000.wav",
                "hypothesis": "tracer",
                "pairedQuantitativeCase": "n-4",
            },
            {
                "name": "listening-missing",
                "sample": "does-not-exist.wav",
                "hypothesis": "tracer",
                "pairedQuantitativeCase": "reference",
            },
        ],
    }
    catalog_path = workspace / "tracer-catalog.json"
    catalog_path.write_text(json.dumps(tracer_catalog, indent=2))

    output_dir = workspace / "output"

    def run_catalog():
        return run(
            sys.executable,
            runner,
            "--catalog",
            catalog_path,
            "--renderer",
            renderer,
            "--analyzer",
            analyzer,
            "--mono-source",
            mono_source,
            "--stereo-source",
            stereo_source,
            "--listening-dir",
            fixture_dir,
            "--output",
            output_dir,
            "--warmup-seconds",
            "0.02",
            "--measure-seconds",
            "0.02",
        )

    first = run_catalog()
    if first.returncode != 1:
        raise AssertionError(
            f"expected catalog run with failing cases to exit 1: "
            f"{first.returncode}\nstdout: {first.stdout}\nstderr: {first.stderr}"
        )
    for expected in (
        "[completed] reference",
        "[completed] n-4",
        "[completed] lengths-explicit",
        "[failed] invalid-override",
        "[failed] invalid-config",
        "[completed] listening-found",
        "[skipped] listening-missing",
    ):
        if expected not in first.stdout:
            raise AssertionError(f"missing {expected!r} in stdout: {first.stdout}")
    if "git lfs pull" not in first.stdout:
        raise AssertionError(
            f"missing-sample listening case did not explain how to fetch it: "
            f"{first.stdout}"
        )

    report = json.loads((output_dir / "catalog-report.json").read_text())
    if outcome(report, "cases", "reference")["status"] != "completed":
        raise AssertionError(f"unexpected report: {report}")
    if outcome(report, "cases", "invalid-override")["status"] != "failed":
        raise AssertionError(f"unexpected report: {report}")
    if "out of range" not in outcome(report, "cases", "invalid-override")["detail"]:
        raise AssertionError(
            f"invalid-override failure was not explained clearly: {report}"
        )
    if outcome(report, "cases", "invalid-config")["status"] != "failed":
        raise AssertionError(f"unexpected report: {report}")
    if "power-of-two" not in outcome(report, "cases", "invalid-config")["detail"]:
        raise AssertionError(
            f"invalid-config failure did not propagate the renderer's reason: "
            f"{report}"
        )

    # JSON-Pointer materialization is correct, independently re-derived
    # (not by trusting the runner's own materialize() implementation) and
    # the complete Requested Configuration is preserved immutably in the
    # Render Result.
    expected_n4_request = json.loads(json.dumps(reference))
    expected_n4_request["composition"]["stages"][0]["channels"] = 4
    actual_n4_request = json.loads(
        (output_dir / "cases" / "n-4" / "request.json").read_text()
    )
    if actual_n4_request != expected_n4_request:
        raise AssertionError(
            f"n-4 materialization was wrong: {actual_n4_request} != "
            f"{expected_n4_request}"
        )
    n4_resolved = json.loads(
        (output_dir / "cases" / "n-4" / "resolved.json").read_text()
    )
    if n4_resolved["composition"]["stages"][0]["channels"] != 4:
        raise AssertionError(f"n-4 override did not reach resolution: {n4_resolved}")

    expected_lengths_request = json.loads(json.dumps(reference))
    diffuser_stage = expected_lengths_request["composition"]["stages"][1]
    del diffuser_stage["steps"]
    del diffuser_stage["totalMs"]
    del diffuser_stage["distribution"]
    diffuser_stage["lengthsMs"] = [1]
    actual_lengths_request = json.loads(
        (output_dir / "cases" / "lengths-explicit" / "request.json").read_text()
    )
    if actual_lengths_request != expected_lengths_request:
        raise AssertionError(
            f"lengths-explicit materialization was wrong: "
            f"{actual_lengths_request} != {expected_lengths_request}"
        )

    # Diffusion analysis was published for every completed quantitative case.
    for name in ("reference", "n-4", "lengths-explicit"):
        if not (
            output_dir / "cases" / name / "analysis" / "diffusion-v1.json"
        ).exists():
            raise AssertionError(f"{name} did not publish diffusion analysis")

    # Benchmarks: canonical block size for every completed case, plus the
    # Reference block-size sweep.
    for name in ("reference", "n-4", "lengths-explicit"):
        if not (output_dir / "cases" / name / "benchmark-128.json").exists():
            raise AssertionError(f"{name} did not publish a benchmark report")
    for block_size in (32, 64, 256, 512):
        if not (
            output_dir / "cases" / "reference" / f"benchmark-{block_size}.json"
        ).exists():
            raise AssertionError(
                f"Reference did not benchmark block size {block_size}"
            )
    if (output_dir / "cases" / "n-4" / "benchmark-32.json").exists():
        raise AssertionError("non-Reference case benchmarked outside the sweep")

    # Listening cases: the found sample renders with the paired
    # quantitative case's exact Requested Configuration; the missing
    # sample is skipped rather than failing the run.
    listening_request = json.loads(
        (output_dir / "listening" / "listening-found" / "request.json").read_text()
    )
    if listening_request != expected_n4_request:
        raise AssertionError(
            f"listening-found did not reuse its paired case's Requested "
            f"Configuration: {listening_request}"
        )
    if (output_dir / "listening" / "listening-missing").exists():
        raise AssertionError("missing listening sample unexpectedly created output")

    # Benchmark aggregation: ranked descending by median, Reference has a
    # zero delta/unity ratio against itself.
    summary = json.loads((output_dir / "benchmark-summary.json").read_text())
    if summary["referenceCase"] != "reference" or summary["blockSize"] != 128:
        raise AssertionError(f"unexpected benchmark summary header: {summary}")
    ranked = summary["rankedBySlowestMedian"]
    if {entry["case"] for entry in ranked} != {"reference", "n-4", "lengths-explicit"}:
        raise AssertionError(f"unexpected ranked cases: {summary}")
    medians = [entry["medianBlockSeconds"] for entry in ranked]
    if medians != sorted(medians, reverse=True):
        raise AssertionError(f"ranking was not descending by median: {summary}")
    reference_entry = next(e for e in ranked if e["case"] == "reference")
    if (
        reference_entry["deltaFromReferenceMedianSeconds"] != 0
        or reference_entry["ratioToReferenceMedian"] != 1.0
    ):
        raise AssertionError(f"Reference did not compare exactly to itself: {summary}")

    # Resumable: completed and skipped cases are not re-attempted; failed
    # cases (which never produced a Render Result) retry and fail the same
    # way, without corrupting or replacing existing evidence.
    n4_output_before = (output_dir / "cases" / "n-4" / "output.wav").read_bytes()
    second = run_catalog()
    if second.returncode != 1:
        raise AssertionError(f"resumed run changed overall outcome: {second.stdout}")
    for expected in (
        "[resumed] reference",
        "[resumed] n-4",
        "[resumed] lengths-explicit",
        "[failed] invalid-override",
        "[failed] invalid-config",
        "[resumed] listening-found",
    ):
        if expected not in second.stdout:
            raise AssertionError(
                f"resumed run did not report {expected!r}: {second.stdout}"
            )
    n4_output_after = (output_dir / "cases" / "n-4" / "output.wav").read_bytes()
    if n4_output_before != n4_output_after:
        raise AssertionError("resuming replaced completed evidence")

    # Resumability is per-step, not all-or-nothing: a case whose render
    # succeeded but whose benchmark did not (simulated here by deleting an
    # already-published benchmark report) retries only the missing step on
    # the next run, rather than failing the same way forever.
    n4_benchmark_path = output_dir / "cases" / "n-4" / "benchmark-128.json"
    n4_benchmark_path.unlink()
    n4_resolved_before = (output_dir / "cases" / "n-4" / "resolved.json").read_bytes()
    third = run_catalog()
    if "[completed] n-4" not in third.stdout:
        raise AssertionError(
            f"a case missing only its benchmark did not retry and complete: "
            f"{third.stdout}"
        )
    if not n4_benchmark_path.exists():
        raise AssertionError("retried benchmark step did not publish its report")
    n4_resolved_after = (output_dir / "cases" / "n-4" / "resolved.json").read_bytes()
    if n4_resolved_before != n4_resolved_after:
        raise AssertionError("retrying the benchmark step re-rendered the case")


if __name__ == "__main__":
    main()
