#!/usr/bin/env python3

"""Runs the versioned diffusion experiment catalog (see
diffusion_catalog_v1.json): materializes each case's complete Requested
Configuration from the Reference configuration plus named JSON-Pointer
overrides, renders and analyzes quantitative cases against the
deterministic impulse basis, renders listening cases against curated LFS
material sharing their paired quantitative case's exact configuration and
impulse evidence, benchmarks every case (plus a block-size sweep for
Reference), and aggregates the benchmark reports into a ranked human table
and benchmark-summary.json.

Render Results are immutable: a case whose output directory already
contains render.json is never re-rendered. A failed or interrupted run can
be resumed by rerunning the same command -- already-rendered cases skip
straight to whichever of analysis/benchmarking they are still missing,
without replacing existing evidence.

Materialisation, command execution, render/benchmark invocation, per-step
resumability, and benchmark aggregation are diffusion-agnostic and live in
experiment_runner; this module supplies only what is specific to the
diffusion catalog: its schema, the diffusion analyzer, and the artifact
paths each case's steps produce.
"""

import argparse
import json
import sys
from pathlib import Path

from experiment_runner import (
    CatalogError,
    CaseOutcome,
    benchmark_case,
    build_benchmark_summary,
    materialize,
    print_human_table,
    render_case,
    run_command,
    run_resumable_steps,
    write_materialized,
)

CANONICAL_WARMUP_SECONDS = 1.0
CANONICAL_MEASURE_SECONDS = 5.0
CANONICAL_BLOCK_SIZE = 128
REFERENCE_BLOCK_SIZE_SWEEP = (32, 64, 128, 256, 512)


def load_catalog(path: Path):
    document = json.loads(path.read_text())
    if document.get("formatVersion") != 1:
        raise CatalogError(f"unsupported catalog formatVersion: {document.get('formatVersion')}")
    for field in ("reference", "cases", "listeningCases"):
        if field not in document:
            raise CatalogError(f"catalog is missing required field: {field}")
    names = [case["name"] for case in document["cases"]]
    if len(names) != len(set(names)):
        raise CatalogError("catalog contains duplicate case names")
    if "reference" not in names:
        raise CatalogError("catalog is missing required quantitative case: 'reference'")
    listening_names = [case["name"] for case in document["listeningCases"]]
    if len(listening_names) != len(set(listening_names)):
        raise CatalogError("catalog contains duplicate listening case names")
    for listening_case in document["listeningCases"]:
        paired = listening_case["pairedQuantitativeCase"]
        if paired not in names:
            raise CatalogError(
                f"listening case {listening_case['name']!r} pairs with unknown "
                f"quantitative case {paired!r}"
            )
    return document


def run_quantitative_case(
    case,
    reference,
    mono_source,
    stereo_source,
    renderer,
    analyzer,
    output_root,
    warmup_seconds,
    measure_seconds,
):
    """Renders, analyzes, and benchmarks one catalog case. Each of those
    three steps is independently resumable: a step whose expected output
    already exists is not repeated, so a case that failed partway through
    (for example a transient benchmark failure after a successful render)
    finishes on the next run instead of failing the same way forever."""
    name = case["name"]
    case_dir = output_root / "cases" / name
    resolved_path = case_dir / "resolved.json"
    benchmark_path = case_dir / f"benchmark-{CANONICAL_BLOCK_SIZE}.json"
    source = stereo_source if case.get("source") == "stereo" else mono_source

    def render_step():
        materialized = materialize(reference, case["overrides"])
        materialized_path = write_materialized(
            output_root, f"{name}.json", materialized
        )
        return render_case(renderer, source, materialized_path, case_dir)

    steps = [
        (case_dir / "render.json", "render", render_step),
        (
            case_dir / "analysis" / "diffusion-v1.json",
            "analyze",
            lambda: run_command(sys.executable, analyzer, case_dir, "--source", source),
        ),
        (
            benchmark_path,
            "benchmark",
            lambda: benchmark_case(
                renderer,
                resolved_path,
                CANONICAL_BLOCK_SIZE,
                warmup_seconds,
                measure_seconds,
                benchmark_path,
            ),
        ),
    ]

    if name == "reference":
        for block_size in REFERENCE_BLOCK_SIZE_SWEEP:
            if block_size == CANONICAL_BLOCK_SIZE:
                continue
            sweep_path = case_dir / f"benchmark-{block_size}.json"
            steps.append(
                (
                    sweep_path,
                    f"benchmark (block size {block_size})",
                    lambda bs=block_size, sp=sweep_path: benchmark_case(
                        renderer, resolved_path, bs, warmup_seconds, measure_seconds, sp
                    ),
                )
            )

    did_new_work, failure = run_resumable_steps(steps)
    if failure is not None:
        return CaseOutcome(name, "failed", failure)

    status = "completed" if did_new_work else "resumed"
    return CaseOutcome(
        name, status, benchmark=json.loads(benchmark_path.read_text())
    )


def run_listening_case(
    listening_case,
    catalog,
    listening_dir,
    renderer,
    output_root,
):
    name = listening_case["name"]
    sample_path = listening_dir / listening_case["sample"]
    case_dir = output_root / "listening" / name

    if not sample_path.exists():
        return CaseOutcome(
            name,
            "skipped",
            f"listening sample not present locally (needs `git lfs pull`): "
            f"{sample_path}",
        )
    if (case_dir / "render.json").exists():
        return CaseOutcome(name, "resumed")

    paired = next(
        case
        for case in catalog["cases"]
        if case["name"] == listening_case["pairedQuantitativeCase"]
    )
    materialized = materialize(catalog["reference"], paired["overrides"])
    materialized_path = write_materialized(
        output_root, f"{name}-listening.json", materialized
    )

    rendered = render_case(
        renderer, sample_path, materialized_path, case_dir, capture_stages=False
    )
    if rendered.returncode != 0:
        return CaseOutcome(name, "failed", f"render: {rendered.stderr.strip()}")
    return CaseOutcome(name, "completed")


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Run the versioned diffusion experiment catalog."
    )
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument("--analyzer", required=True, type=Path)
    parser.add_argument("--mono-source", required=True, type=Path)
    parser.add_argument("--stereo-source", required=True, type=Path)
    parser.add_argument("--listening-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--warmup-seconds", type=float, default=CANONICAL_WARMUP_SECONDS
    )
    parser.add_argument(
        "--measure-seconds", type=float, default=CANONICAL_MEASURE_SECONDS
    )
    parser.add_argument(
        "--skip-listening",
        action="store_true",
        help="skip listening cases entirely (quantitative cases only)",
    )
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    try:
        catalog = load_catalog(arguments.catalog)
    except (CatalogError, OSError, json.JSONDecodeError, KeyError) as error:
        print(f"diffusion catalog failed: {error}", file=sys.stderr)
        return 1

    arguments.output.mkdir(parents=True, exist_ok=True)

    outcomes = []
    for case in catalog["cases"]:
        try:
            outcome = run_quantitative_case(
                case,
                catalog["reference"],
                arguments.mono_source,
                arguments.stereo_source,
                arguments.renderer,
                arguments.analyzer,
                arguments.output,
                arguments.warmup_seconds,
                arguments.measure_seconds,
            )
        except CatalogError as error:
            outcome = CaseOutcome(case["name"], "failed", str(error))
        outcomes.append(outcome)
        print(f'[{outcome.status}] {outcome.name}' + (
            f': {outcome.detail}' if outcome.detail else ''
        ))

    listening_outcomes = []
    if not arguments.skip_listening:
        for listening_case in catalog["listeningCases"]:
            try:
                outcome = run_listening_case(
                    listening_case,
                    catalog,
                    arguments.listening_dir,
                    arguments.renderer,
                    arguments.output,
                )
            except CatalogError as error:
                outcome = CaseOutcome(listening_case["name"], "failed", str(error))
            listening_outcomes.append(outcome)
            print(f'[{outcome.status}] {outcome.name}' + (
                f': {outcome.detail}' if outcome.detail else ''
            ))

    summary = build_benchmark_summary(outcomes, "reference", CANONICAL_BLOCK_SIZE)
    if summary is not None:
        summary_path = arguments.output / "benchmark-summary.json"
        summary_path.write_text(
            json.dumps(summary, allow_nan=False, indent=2, sort_keys=False) + "\n"
        )
        print()
        print_human_table(summary)
        print()
        print(summary_path)

    manifest_path = arguments.output / "catalog-report.json"
    manifest_path.write_text(
        json.dumps(
            {
                "formatVersion": 1,
                "cases": [outcome.to_json() for outcome in outcomes],
                "listeningCases": [
                    outcome.to_json() for outcome in listening_outcomes
                ],
            },
            allow_nan=False,
            indent=2,
        )
        + "\n"
    )

    failures = [
        outcome
        for outcome in outcomes + listening_outcomes
        if outcome.status == "failed"
    ]
    if failures:
        print(file=sys.stderr)
        print("Catalog run had failures:", file=sys.stderr)
        for outcome in failures:
            print(f"  {outcome.name}: {outcome.detail}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
