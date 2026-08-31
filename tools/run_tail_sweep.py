#!/usr/bin/env python3

"""Runs the versioned tail axis catalog (see tail_sweep_v1.json): sweeps one
curated listening sample across the Feedback Loop research axes -- delay
range, RT60, matrix, delay strategy, gain mode -- one axis at a time from a
single Reference configuration, never combinatorially.

Every sweep point renders both the selected listening sample and the
deterministic impulse under an identical Resolved Configuration (the sample
render's resolved.json, reused via `--resolved` rather than re-resolving
from the impulse), because Schroeder integration assumes an impulse
response and programme material would contaminate the decay curve with its
own envelope. Tail analysis and benchmarking then both run against that
paired impulse/Resolved Configuration.

Output is laid out per sample, per axis, and per axis value, with numeric
prefixes so a folder plays back in sweep order: <output>/<sample>/
00-reference/, 01-<axis>/01-<value>/, 01-<axis>/02-<value>/, and so on.

Materialisation, command execution, render/benchmark invocation, and
per-step resumability are tail-agnostic and live in experiment_runner; this
module supplies only what is specific to the tail sweep: its axis-catalog
schema, the tail analyzer, the paired impulse render, and the artifact
layout each point's steps produce.
"""

import argparse
import json
import sys
import wave
from pathlib import Path

from experiment_runner import (
    CatalogError,
    CaseOutcome,
    benchmark_case,
    build_benchmark_summary,
    materialize,
    print_human_table,
    render_case,
    render_from_resolved,
    run_command,
    run_resumable_steps,
    write_materialized,
)

CANONICAL_WARMUP_SECONDS = 1.0
CANONICAL_MEASURE_SECONDS = 5.0
CANONICAL_BENCHMARK_BLOCK_SIZE = 128

# The first four bytes of a Git LFS pointer file, checked out in place of
# real content whenever a clone has not run `git lfs pull` -- distinct from
# the sample being absent entirely, which `Path.exists()` alone catches.
_LFS_POINTER_PREFIX = b"version https://git-lfs.github.com/spec/v1"


def load_catalog(path: Path):
    document = json.loads(path.read_text())
    if document.get("formatVersion") != 1:
        raise CatalogError(f"unsupported catalog formatVersion: {document.get('formatVersion')}")
    for field in ("reference", "axes"):
        if field not in document:
            raise CatalogError(f"catalog is missing required field: {field}")
    names = [axis["name"] for axis in document["axes"]]
    if len(names) != len(set(names)):
        raise CatalogError("catalog contains duplicate axis names")
    for axis in document["axes"]:
        labels = [value["label"] for value in axis["values"]]
        if len(labels) != len(set(labels)):
            raise CatalogError(
                f"axis {axis['name']!r} contains duplicate value labels"
            )
        if not labels:
            raise CatalogError(f"axis {axis['name']!r} has no values to sweep")
    return document


def sample_unavailable_reason(sample: Path):
    """None when the sample is real, local, curated material; otherwise the
    reason it cannot be swept -- either genuinely absent, or present only as
    an unpulled Git LFS pointer (the file exists, but is a few dozen bytes
    of pointer text rather than audio)."""
    if not sample.exists():
        return f"listening sample not present locally (needs `git lfs pull`): {sample}"
    if sample.read_bytes()[: len(_LFS_POINTER_PREFIX)] == _LFS_POINTER_PREFIX:
        return (
            f"listening sample is an unpulled Git LFS pointer, not audio "
            f"(needs `git lfs pull`): {sample}"
        )
    return None


def matching_impulse(sample, mono_impulse, stereo_impulse):
    """Rendering the impulse via `--resolved` (see run_sweep_point) requires
    the resolved Split's inputChannels to match exactly -- a mono impulse
    against a resolved.json baked from a stereo sample fails loudly rather
    than silently mixing channel counts -- so the matching fixture is
    selected from the sample's own Channel count, mirroring
    run_diffusion_catalog.py's mono/stereo source pairing."""
    with wave.open(str(sample), "rb") as wav:
        channels = wav.getnchannels()
    if channels == 1:
        return mono_impulse
    if channels == 2:
        return stereo_impulse
    raise CatalogError(
        f"listening sample has {channels} Channels; only mono or stereo "
        f"source material is supported: {sample}"
    )


def sweep_points(catalog):
    """Yields (point_name, directory_name, overrides) for the Reference
    point followed by every axis's values in catalog order -- one axis at a
    time, never combinatorially: each point overrides only its own single
    axis from the Reference, and the Reference point itself carries no
    overrides at all."""
    yield "reference", "00-reference", []
    for axis_index, axis in enumerate(catalog["axes"], start=1):
        axis_directory = f"{axis_index:02d}-{axis['name']}"
        for value_index, value in enumerate(axis["values"], start=1):
            point_name = f"{axis['name']}/{value['label']}"
            directory = f"{axis_directory}/{value_index:02d}-{value['label']}"
            yield point_name, directory, value["overrides"]


def run_sweep_point(
    name,
    point_dir,
    overrides,
    reference,
    sample,
    impulse,
    renderer,
    analyzer,
    warmup_seconds,
    measure_seconds,
    block_size,
):
    """Renders, tail-analyzes, and benchmarks one sweep point. Each of the
    four steps below is independently resumable, matching
    run_diffusion_catalog.py's established per-case shape: a case that
    failed partway through finishes on the next run instead of failing the
    same way forever."""
    sample_dir = point_dir / "sample"
    impulse_dir = point_dir / "impulse"
    resolved_path = sample_dir / "resolved.json"
    benchmark_path = point_dir / "benchmark.json"

    def render_sample_step():
        materialized = materialize(reference, overrides)
        materialized_path = write_materialized(
            point_dir, "sample-request.json", materialized
        )
        return render_case(
            renderer,
            sample,
            materialized_path,
            sample_dir,
            capture_stages=False,
            block_size=block_size,
        )

    steps = [
        (sample_dir / "render.json", "render sample", render_sample_step),
        (
            impulse_dir / "render.json",
            "render impulse",
            lambda: render_from_resolved(
                renderer, impulse, resolved_path, impulse_dir, block_size=block_size
            ),
        ),
        (
            impulse_dir / "analysis" / "tail-v1.json",
            "analyze tail",
            lambda: run_command(
                sys.executable, analyzer, impulse_dir, "--source", impulse
            ),
        ),
        (
            benchmark_path,
            "benchmark",
            lambda: benchmark_case(
                renderer,
                resolved_path,
                block_size or CANONICAL_BENCHMARK_BLOCK_SIZE,
                warmup_seconds,
                measure_seconds,
                benchmark_path,
            ),
        ),
    ]

    did_new_work, failure = run_resumable_steps(steps)
    if failure is not None:
        return CaseOutcome(name, "failed", failure)
    status = "completed" if did_new_work else "resumed"
    return CaseOutcome(
        name, status, benchmark=json.loads(benchmark_path.read_text())
    )


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Sweep one curated listening sample across the versioned tail "
            "axis catalog."
        )
    )
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--analyzer", required=True, type=Path)
    parser.add_argument("--sample", required=True, type=Path)
    parser.add_argument("--mono-impulse", required=True, type=Path)
    parser.add_argument("--stereo-impulse", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--warmup-seconds", type=float, default=CANONICAL_WARMUP_SECONDS
    )
    parser.add_argument(
        "--measure-seconds", type=float, default=CANONICAL_MEASURE_SECONDS
    )
    parser.add_argument(
        "--block-size",
        type=int,
        default=None,
        help=(
            "override both render and benchmark block size; the Reference "
            "axis catalog's smallest delay comfortably exceeds the "
            "renderer's own default, so this is normally only needed for a "
            "millisecond-scale tracer catalog"
        ),
    )
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    try:
        catalog = load_catalog(arguments.catalog)
    except (CatalogError, OSError, json.JSONDecodeError, KeyError) as error:
        print(f"tail sweep failed: {error}", file=sys.stderr)
        return 1

    reason = sample_unavailable_reason(arguments.sample)
    if reason is not None:
        print(f"[skipped] {reason}")
        return 0

    try:
        impulse = matching_impulse(
            arguments.sample, arguments.mono_impulse, arguments.stereo_impulse
        )
    except CatalogError as error:
        print(f"tail sweep failed: {error}", file=sys.stderr)
        return 1

    sample_root = arguments.output / arguments.sample.stem
    sample_root.mkdir(parents=True, exist_ok=True)

    outcomes = []
    for name, directory, overrides in sweep_points(catalog):
        point_dir = sample_root / directory
        try:
            outcome = run_sweep_point(
                name,
                point_dir,
                overrides,
                catalog["reference"],
                arguments.sample,
                impulse,
                arguments.renderer,
                arguments.analyzer,
                arguments.warmup_seconds,
                arguments.measure_seconds,
                arguments.block_size,
            )
        except CatalogError as error:
            outcome = CaseOutcome(name, "failed", str(error))
        outcomes.append(outcome)
        print(f'[{outcome.status}] {outcome.name}' + (
            f': {outcome.detail}' if outcome.detail else ''
        ))

    summary = build_benchmark_summary(
        outcomes,
        "reference",
        arguments.block_size or CANONICAL_BENCHMARK_BLOCK_SIZE,
    )
    if summary is not None:
        summary_path = sample_root / "benchmark-summary.json"
        summary_path.write_text(
            json.dumps(summary, allow_nan=False, indent=2, sort_keys=False) + "\n"
        )
        print()
        print_human_table(summary)
        print()
        print(summary_path)

    report_path = sample_root / "sweep-report.json"
    report_path.write_text(
        json.dumps(
            {
                "formatVersion": 1,
                "sample": arguments.sample.name,
                "points": [outcome.to_json() for outcome in outcomes],
            },
            allow_nan=False,
            indent=2,
        )
        + "\n"
    )
    print()
    print(report_path)

    failures = [outcome for outcome in outcomes if outcome.status == "failed"]
    if failures:
        print(file=sys.stderr)
        print("Tail sweep had failures:", file=sys.stderr)
        for outcome in failures:
            print(f"  {outcome.name}: {outcome.detail}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
