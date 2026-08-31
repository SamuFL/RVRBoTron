#!/usr/bin/env python3

"""Runner internals shared by every catalog-style experiment CLI:
JSON-Pointer materialisation of a Requested Configuration from a Reference
configuration plus named overrides, subprocess command execution, render
and benchmark invocation, per-step resumability, and benchmark aggregation
into a ranked table.

This module knows nothing about diffusion, tails, or any other stage; a
catalog runner owns its own schema, its own choice of analyzer, and the
artifact paths that make each of its steps resumable.
"""

import copy
import json
import subprocess


class CatalogError(ValueError):
    pass


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


def _render(renderer, source, output_dir, config_flag, config_value, extra_flags):
    arguments = [
        renderer,
        "render",
        "--input",
        source,
        config_flag,
        config_value,
        *extra_flags,
        "--output",
        output_dir,
    ]
    return run_command(*arguments)


def render_case(
    renderer, source, request_path, output_dir, capture_stages=True, block_size=None
):
    extra_flags = []
    if capture_stages:
        extra_flags += ["--capture-stages", "all"]
    if block_size is not None:
        extra_flags += ["--block-size", block_size]
    return _render(
        renderer, source, output_dir, "--config", request_path, extra_flags
    )


def render_from_resolved(renderer, source, resolved_path, output_dir, block_size=None):
    """Renders a different source against an already-resolved Configuration
    rather than a Requested Configuration, guaranteeing byte-identical DSP
    parameters (delays, gains, matrices) regardless of the two sources'
    differing content or Channel count -- the pattern a catalog uses to pair
    a deterministic impulse with the sample it accompanies."""
    extra_flags = [] if block_size is None else ["--block-size", block_size]
    return _render(
        renderer, source, output_dir, "--resolved", resolved_path, extra_flags
    )


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


def run_resumable_steps(steps):
    """Runs each `(expected_path, name, action)` step in order, skipping any
    step whose `expected_path` already exists. Stops at the first step whose
    `action()` returns a failing `CompletedProcess`, reporting `name` beside
    its stderr. A case that fails partway through therefore retries only its
    remaining steps on the next run, without repeating or corrupting
    whatever earlier steps already produced.

    Returns `(did_new_work, failure_detail)`; `failure_detail` is `None` on
    success."""
    did_new_work = False
    for expected_path, name, action in steps:
        if expected_path.exists():
            continue
        result = action()
        if result.returncode != 0:
            return did_new_work, f"{name}: {result.stderr.strip()}"
        did_new_work = True
    return did_new_work, None


def build_benchmark_summary(outcomes, reference_name, block_size):
    completed = {
        outcome.name: outcome.benchmark
        for outcome in outcomes
        if outcome.benchmark is not None
    }
    if reference_name not in completed:
        return None
    reference_median = completed[reference_name]["medianBlockSeconds"]

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
        "referenceCase": reference_name,
        "blockSize": block_size,
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
