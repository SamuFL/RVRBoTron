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
"""

import argparse
import copy
import json
import subprocess
import sys
from pathlib import Path

CANONICAL_WARMUP_SECONDS = 1.0
CANONICAL_MEASURE_SECONDS = 5.0
CANONICAL_BLOCK_SIZE = 128
REFERENCE_BLOCK_SIZE_SWEEP = (32, 64, 128, 256, 512)


class CatalogError(ValueError):
    pass


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


def _unescape_token(token: str) -> str:
    return token.replace("~1", "/").replace("~0", "~")


def _split_pointer(pointer: str):
    if pointer == "":
        raise CatalogError("JSON Pointer must not be the document root")
    if not pointer.startswith("/"):
        raise CatalogError(f"JSON Pointer must start with '/': {pointer}")
    return [_unescape_token(token) for token in pointer.split("/")[1:]]


def _navigate(document, tokens, pointer):
    node = document
    for token in tokens:
        if isinstance(node, list):
            try:
                index = int(token)
            except ValueError as error:
                raise CatalogError(
                    f"JSON Pointer array index is not an integer: {pointer}"
                ) from error
            if not 0 <= index < len(node):
                raise CatalogError(f"JSON Pointer index out of range: {pointer}")
            node = node[index]
        elif isinstance(node, dict):
            if token not in node:
                raise CatalogError(f"JSON Pointer references a missing field: {pointer}")
            node = node[token]
        else:
            raise CatalogError(f"JSON Pointer traverses a non-container value: {pointer}")
    return node


def _apply_operation(document, operation):
    op = operation["op"]
    pointer = operation["path"]
    tokens = _split_pointer(pointer)
    parent = _navigate(document, tokens[:-1], pointer)
    last = tokens[-1]

    if isinstance(parent, list):
        try:
            index = len(parent) if last == "-" else int(last)
        except ValueError as error:
            raise CatalogError(
                f"JSON Pointer array index is not an integer: {pointer}"
            ) from error
        if op == "remove":
            if not 0 <= index < len(parent):
                raise CatalogError(f"JSON Pointer index out of range: {pointer}")
            del parent[index]
        elif op == "add":
            if not 0 <= index <= len(parent):
                raise CatalogError(f"JSON Pointer index out of range: {pointer}")
            parent.insert(index, operation["value"])
        elif op == "replace":
            if not 0 <= index < len(parent):
                raise CatalogError(f"JSON Pointer index out of range: {pointer}")
            parent[index] = operation["value"]
        else:
            raise CatalogError(f"unsupported override op: {op}")
    elif isinstance(parent, dict):
        if op == "remove":
            if last not in parent:
                raise CatalogError(f"JSON Pointer references a missing field: {pointer}")
            del parent[last]
        elif op == "add":
            parent[last] = operation["value"]
        elif op == "replace":
            if last not in parent:
                raise CatalogError(f"JSON Pointer references a missing field: {pointer}")
            parent[last] = operation["value"]
        else:
            raise CatalogError(f"unsupported override op: {op}")
    else:
        raise CatalogError(f"JSON Pointer traverses a non-container value: {pointer}")


def materialize(reference: dict, overrides):
    """Applies named JSON-Pointer overrides to a deep copy of the complete
    Reference configuration, returning a new complete Requested
    Configuration. Every override must resolve against an existing field
    (replace/remove) or an existing parent container (add): a catalog case
    that references a path the Reference configuration does not have is a
    catalog-authoring error and fails loudly rather than silently creating
    unrelated structure."""
    materialized = copy.deepcopy(reference)
    for operation in overrides:
        _apply_operation(materialized, operation)
    return materialized


def run_command(*arguments):
    return subprocess.run(
        list(map(str, arguments)),
        check=False,
        capture_output=True,
        text=True,
    )


class CaseOutcome:
    def __init__(self, name, status, detail=None, benchmark=None):
        self.name = name
        self.status = status  # "completed" | "resumed" | "skipped" | "failed"
        self.detail = detail
        self.benchmark = benchmark

    def to_json(self):
        return {"name": self.name, "status": self.status, "detail": self.detail}


def render_case(renderer, source, request_path, output_dir, capture_stages=True):
    arguments = [
        renderer,
        "render",
        "--input",
        source,
        "--config",
        request_path,
        "--output",
        output_dir,
    ]
    if capture_stages:
        arguments[-2:-2] = ["--capture-stages", "all"]
    return run_command(*arguments)


def benchmark_case(
    renderer, resolved_path, block_size, warmup_seconds, measure_seconds, json_path
):
    return run_command(
        renderer,
        "benchmark",
        "--resolved",
        resolved_path,
        "--block-size",
        block_size,
        "--warmup-seconds",
        warmup_seconds,
        "--measure-seconds",
        measure_seconds,
        "--json",
        json_path,
    )


def write_materialized(output_root, filename, materialized):
    path = output_root / "materialized" / filename
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(materialized, indent=2) + "\n")
    return path


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
    did_new_work = False

    if not (case_dir / "render.json").exists():
        source = stereo_source if case.get("source") == "stereo" else mono_source
        materialized = materialize(reference, case["overrides"])
        materialized_path = write_materialized(
            output_root, f"{name}.json", materialized
        )
        rendered = render_case(renderer, source, materialized_path, case_dir)
        if rendered.returncode != 0:
            return CaseOutcome(name, "failed", f"render: {rendered.stderr.strip()}")
        did_new_work = True
    else:
        source = stereo_source if case.get("source") == "stereo" else mono_source

    if not (case_dir / "analysis" / "diffusion-v1.json").exists():
        analyzed = run_command(sys.executable, analyzer, case_dir, "--source", source)
        if analyzed.returncode != 0:
            return CaseOutcome(name, "failed", f"analyze: {analyzed.stderr.strip()}")
        did_new_work = True

    if not benchmark_path.exists():
        benchmarked = benchmark_case(
            renderer,
            resolved_path,
            CANONICAL_BLOCK_SIZE,
            warmup_seconds,
            measure_seconds,
            benchmark_path,
        )
        if benchmarked.returncode != 0:
            return CaseOutcome(
                name, "failed", f"benchmark: {benchmarked.stderr.strip()}"
            )
        did_new_work = True

    if name == "reference":
        for block_size in REFERENCE_BLOCK_SIZE_SWEEP:
            if block_size == CANONICAL_BLOCK_SIZE:
                continue
            sweep_path = case_dir / f"benchmark-{block_size}.json"
            if sweep_path.exists():
                continue
            swept = benchmark_case(
                renderer,
                resolved_path,
                block_size,
                warmup_seconds,
                measure_seconds,
                sweep_path,
            )
            if swept.returncode != 0:
                return CaseOutcome(
                    name,
                    "failed",
                    f"benchmark (block size {block_size}): {swept.stderr.strip()}",
                )
            did_new_work = True

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


def build_benchmark_summary(outcomes):
    completed = {
        outcome.name: outcome.benchmark
        for outcome in outcomes
        if outcome.benchmark is not None
    }
    if "reference" not in completed:
        return None
    reference_median = completed["reference"]["medianBlockSeconds"]

    ranked = []
    for name, benchmark in completed.items():
        median = benchmark["medianBlockSeconds"]
        ranked.append(
            {
                "case": name,
                "medianBlockSeconds": median,
                "p95BlockSeconds": benchmark["p95BlockSeconds"],
                "worstBlockSeconds": benchmark["worstBlockSeconds"],
                "deltaFromReferenceMedianSeconds": median - reference_median,
                "ratioToReferenceMedian": (
                    median / reference_median if reference_median > 0 else 0.0
                ),
            }
        )
    ranked.sort(key=lambda entry: entry["medianBlockSeconds"], reverse=True)

    return {
        "formatVersion": 1,
        "referenceCase": "reference",
        "blockSize": CANONICAL_BLOCK_SIZE,
        "rankedBySlowestMedian": ranked,
    }


def print_human_table(summary):
    if summary is None:
        print("No Reference benchmark available; skipping ranked table.")
        return
    print(
        f'{"case":<28} {"median (us)":>12} {"p95 (us)":>12} '
        f'{"worst (us)":>12} {"vs reference":>14}'
    )
    for entry in summary["rankedBySlowestMedian"]:
        print(
            f'{entry["case"]:<28} '
            f'{entry["medianBlockSeconds"] * 1e6:>12.2f} '
            f'{entry["p95BlockSeconds"] * 1e6:>12.2f} '
            f'{entry["worstBlockSeconds"] * 1e6:>12.2f} '
            f'{entry["ratioToReferenceMedian"]:>13.2f}x'
        )


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

    summary = build_benchmark_summary(outcomes)
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
