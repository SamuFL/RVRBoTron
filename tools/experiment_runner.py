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
import wave
from pathlib import Path

# The first four bytes of a Git LFS pointer file, checked out in place of
# real content whenever a clone has not run `git lfs pull` -- distinct from
# the sample being absent entirely, which `Path.exists()` alone catches.
_LFS_POINTER_PREFIX = b"version https://git-lfs.github.com/spec/v1"


class CatalogError(ValueError):
    pass


def load_catalog(path: Path):
    """Loads and validates a versioned one-axis-at-a-time catalog: a
    `formatVersion`, one complete `reference` Requested Configuration, and
    `axes` -- each a named list of `{label, overrides}` values, `overrides`
    being JSON-Pointer operations applied to the Reference (see
    `materialize`). Shared by every catalog whose sample is a runtime
    argument rather than catalog-embedded (tail_sweep_v1.json,
    damping_sweep_v1.json): both use this exact schema, so a catalog-
    authoring mistake -- a missing field, a duplicate axis name, a duplicate
    value label within an axis, an axis with no values -- fails identically
    and descriptively regardless of which sweep is loading it."""
    document = json.loads(path.read_text())
    if document.get("formatVersion") != 1:
        raise CatalogError(f"unsupported catalog formatVersion: {document.get('formatVersion')}")
    for field in ("reference", "axes"):
        if field not in document:
            raise CatalogError(f"catalog is missing required field: {field}")

    names = []
    for axis in document["axes"]:
        for field in ("name", "values"):
            if field not in axis:
                raise CatalogError(f"axis is missing required field {field!r}: {axis}")
        names.append(axis["name"])
    if len(names) != len(set(names)):
        raise CatalogError("catalog contains duplicate axis names")

    for axis in document["axes"]:
        labels = []
        for value in axis["values"]:
            for field in ("label", "overrides"):
                if field not in value:
                    raise CatalogError(
                        f"axis {axis['name']!r} has a value missing required "
                        f"field {field!r}: {value}"
                    )
            labels.append(value["label"])
        if len(labels) != len(set(labels)):
            raise CatalogError(
                f"axis {axis['name']!r} contains duplicate value labels"
            )
        if not labels:
            raise CatalogError(f"axis {axis['name']!r} has no values to sweep")
    return document


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


def sample_unavailable_reason(sample: Path):
    """None when the sample is real, local, curated material; otherwise the
    reason it cannot be swept -- either genuinely absent, or present only as
    an unpulled Git LFS pointer (the file exists, but is a few dozen bytes
    of pointer text rather than audio)."""
    if not sample.exists():
        return f"listening sample not present locally (needs `git lfs pull`): {sample}"
    with sample.open("rb") as handle:
        prefix = handle.read(len(_LFS_POINTER_PREFIX))
    if prefix == _LFS_POINTER_PREFIX:
        return (
            f"listening sample is an unpulled Git LFS pointer, not audio "
            f"(needs `git lfs pull`): {sample}"
        )
    return None


def matching_impulse(sample, mono_impulse, stereo_impulse):
    """Rendering the impulse via `--resolved` (see render_from_resolved)
    requires the resolved Split's inputChannels to match exactly -- a mono
    impulse against a resolved.json baked from a stereo sample fails loudly
    rather than silently mixing channel counts -- so the matching fixture is
    selected from the sample's own Channel count, mirroring
    run_diffusion_catalog.py's mono/stereo source pairing."""
    try:
        with wave.open(str(sample), "rb") as wav:
            channels = wav.getnchannels()
    except (wave.Error, EOFError) as error:
        raise CatalogError(
            f"listening sample is not a readable WAV file: {sample} ({error})"
        ) from error
    if channels == 1:
        return mono_impulse
    if channels == 2:
        return stereo_impulse
    raise CatalogError(
        f"listening sample has {channels} Channels; only mono or stereo "
        f"source material is supported: {sample}"
    )


def yes_no(value):
    """True/False/None -> "yes"/"no"/"n/a" for an HTML report -- several
    optional booleans across tail analysis versions (tail-v1's
    decayEnvelope.monotonic, tail-v2's withinAccuracyInvariant/
    withinPredictedTolerance/eventualContraction.negativeTrend) are None
    when the tail is too short to measure, and collapsing that into "no"
    would falsely read as a measured negative result rather than an
    unmeasured one."""
    if value is None:
        return "n/a"
    return "yes" if value else "no"


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
    differing content -- the pattern a catalog uses to pair a deterministic
    impulse with the sample it accompanies. The new source's Channel count
    must still match the resolved Split's inputChannels exactly; the caller
    is responsible for picking a source with the right Channel count (see
    matching_impulse below)."""
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


def format_ranked_entry(entry):
    """One ranked benchmark row's (case, median us, p95 us, worst us, ratio
    to Reference), shared by every rendering of build_benchmark_summary's
    output -- the terminal table below and run_tail_sweep.py's HTML
    report -- so the field selection and microsecond conversion live in one
    place rather than being re-derived per presentation."""
    return (
        entry["case"],
        entry["medianBlockSeconds"] * 1e6,
        entry["p95BlockSeconds"] * 1e6,
        entry["worstBlockSeconds"] * 1e6,
        entry["ratioToReferenceMedian"],
    )


def print_human_table(summary):
    if summary is None:
        print("No Reference benchmark available; skipping ranked table.")
        return
    print(
        f'{"case":<28} {"median (us)":>12} {"p95 (us)":>12} '
        f'{"worst (us)":>12} {"vs reference":>14}'
    )
    for entry in summary["rankedBySlowestMedian"]:
        case, median_us, p95_us, worst_us, ratio = format_ranked_entry(entry)
        print(
            f'{case:<28} {median_us:>12.2f} {p95_us:>12.2f} {worst_us:>12.2f} '
            f'{ratio:>13.2f}x'
        )
