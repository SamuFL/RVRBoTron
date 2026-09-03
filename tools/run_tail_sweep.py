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

Every run also (re)generates <output>/<sample>/listening-report.html: one
self-contained page presenting every point's renders (playable in place,
via relative paths -- no external resource requests), measured decay
against the requested RT60, and benchmark cost, so a tuning session is
consumable without reading terminal scrollback or opening a dozen JSON
files. Regeneration is unconditional and reads only already-published
evidence, so rerunning a fully resumed sweep refreshes the report without
re-rendering anything.

Materialisation, command execution, render/benchmark invocation, and
per-step resumability are tail-agnostic and live in experiment_runner; this
module supplies only what is specific to the tail sweep: its axis-catalog
schema, the tail analyzer, the paired impulse render, and the artifact
layout each point's steps produce.
"""

import argparse
import html
import json
import sys
import wave
from pathlib import Path

from experiment_runner import (
    CatalogError,
    CaseOutcome,
    benchmark_case,
    build_benchmark_summary,
    format_ranked_entry,
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
    """Rendering the impulse via `--resolved` (see run_sweep_point) requires
    the resolved Split's inputChannels to match exactly -- a mono impulse
    against a resolved.json baked from a stereo sample fails loudly rather
    than silently mixing channel counts -- so the matching fixture is
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


def _point_evidence(point_dir):
    """Best-effort read of a point's already-published tail analysis and
    benchmark evidence for the listening report -- None for whatever a
    failed point never produced, rather than raising."""
    analysis_path = point_dir / "impulse" / "analysis" / "tail-v1.json"
    benchmark_path = point_dir / "benchmark.json"

    def load_json(path):
        if not path.exists():
            return None
        try:
            return json.loads(path.read_text())
        except (OSError, UnicodeDecodeError, json.JSONDecodeError):
            return None

    return load_json(analysis_path), load_json(benchmark_path)


def _yes_no(value):
    """True/False/None -> "yes"/"no"/"n/a" -- decayEnvelope.monotonic is
    None when a point's tail is too short to segment (see analyze_tail.py's
    decay_envelope_evidence), and collapsing that into "no" would falsely
    read as a measured non-monotonic decay rather than an unmeasured one."""
    if value is None:
        return "n/a"
    return "yes" if value else "no"


def _decay_section_html(analysis):
    decay = analysis["decay"]
    measured = decay["measuredRt60Sec"]
    measured_word = f'{measured:.3f} s' if measured is not None else "n/a"
    error_word = (
        f'{decay["relativeError"] * 100:.2f}%'
        if decay["relativeError"] is not None
        else "n/a"
    )
    band_rows = "".join(
        "<tr><td>{:.0f} Hz</td><td>{}</td></tr>".format(
            band["centerHz"],
            f'{band["t30"]["rt60Sec"]:.3f} s' if band["t30"] else "n/a",
        )
        for band in decay["bands"]
    )
    return f"""<table>
<tr><th>Requested RT60</th><th>Measured RT60 (1 kHz)</th><th>Relative error</th>
<th>Within +/-5%</th><th>Alignment score</th><th>Decay monotonic</th></tr>
<tr><td>{decay['requestedRt60Sec']:.3f} s</td><td>{measured_word}</td>
<td>{_yes_no(decay["withinAccuracyInvariant"])}</td>
<td>{analysis['alignment']['score']:.3f}</td>
<td>{_yes_no(analysis['decayEnvelope']['monotonic'])}</td></tr>
</table>
<details><summary>Per-band T30 RT60</summary>
<table><tr><th>Band</th><th>T30 RT60</th></tr>{band_rows}</table></details>"""


def _benchmark_section_html(benchmark):
    return f"""<table>
<tr><th>Median block time</th><th>p95</th><th>Worst</th></tr>
<tr><td>{benchmark['medianBlockSeconds'] * 1e6:.2f} us</td>
<td>{benchmark['p95BlockSeconds'] * 1e6:.2f} us</td>
<td>{benchmark['worstBlockSeconds'] * 1e6:.2f} us</td></tr>
</table>"""


def _point_section_html(name, directory, point_dir, outcome):
    heading = html.escape(name)
    if outcome.status == "failed":
        return (
            f"<h2>{heading}</h2>\n"
            f'<p class="status-failed">FAILED: {html.escape(outcome.detail or "")}</p>'
        )

    analysis, benchmark = _point_evidence(point_dir)
    decay_html = (
        _decay_section_html(analysis)
        if analysis is not None
        else "<p>Tail analysis unavailable.</p>"
    )
    benchmark_html = (
        _benchmark_section_html(benchmark)
        if benchmark is not None
        else "<p>Benchmark unavailable.</p>"
    )
    sample_audio = html.escape(f"{directory}/sample/output.wav")
    impulse_audio = html.escape(f"{directory}/impulse/output.wav")
    return f"""<h2>{heading} <span class="status-{outcome.status}">[{outcome.status}]</span></h2>
<p class="meta">Sample render</p>
<audio controls src="{sample_audio}"></audio>
<p class="meta">Impulse render (used for tail analysis)</p>
<audio controls src="{impulse_audio}"></audio>
{decay_html}
{benchmark_html}"""


def _benchmark_summary_html(summary):
    if summary is None:
        return "<p>No benchmark summary available.</p>"
    rows = "".join(
        "<tr><td>{}</td><td>{:.2f} us</td><td>{:.2f} us</td><td>{:.2f} us</td>"
        "<td>{:.2f}x</td></tr>".format(
            html.escape(case), median_us, p95_us, worst_us, ratio
        )
        for case, median_us, p95_us, worst_us, ratio in (
            format_ranked_entry(entry) for entry in summary["rankedBySlowestMedian"]
        )
    )
    return f"""<table>
<tr><th>Point</th><th>Median</th><th>p95</th><th>Worst</th><th>vs Reference</th></tr>
{rows}
</table>"""


_REPORT_STYLE = """
body { font-family: system-ui, sans-serif; margin: 2rem; max-width: 960px; }
h1 { font-size: 1.4rem; }
h2 { font-size: 1.1rem; margin-top: 2.5rem; border-bottom: 1px solid #ccc; padding-bottom: .25rem; }
table { border-collapse: collapse; margin: .5rem 0 1rem; }
th, td { border: 1px solid #ccc; padding: .25rem .5rem; text-align: right; font-variant-numeric: tabular-nums; }
th { text-align: center; background: #f2f2f2; }
td:first-child, th:first-child { text-align: left; }
audio { width: 100%; margin: .25rem 0 .75rem; }
.status-failed { color: #b00020; font-weight: bold; }
.status-completed, .status-resumed { color: #1a7a1a; }
.meta { color: #555; font-size: .9rem; margin-bottom: 0; }
"""


def generate_report(sample_root, sample_name, points, summary):
    """Writes one self-contained listening-report.html presenting every
    point's renders, measured decay, and benchmark cost -- no external
    resource requests, no JavaScript, and no dependency on anything but the
    evidence this sweep already published to sample_root. Unconditional and
    read-only against already-published evidence, so rerunning a fully
    resumed sweep regenerates the report without re-rendering anything."""
    sections = "\n".join(
        _point_section_html(name, directory, point_dir, outcome)
        for name, directory, point_dir, outcome in points
    )
    document = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Tail sweep: {html.escape(sample_name)}</title>
<style>{_REPORT_STYLE}</style>
</head>
<body>
<h1>Tail sweep: {html.escape(sample_name)}</h1>
<h2>Benchmark comparison</h2>
{_benchmark_summary_html(summary)}
{sections}
</body>
</html>
"""
    report_path = sample_root / "listening-report.html"
    report_path.write_text(document)
    return report_path


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
    points = []
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
        points.append((name, directory, point_dir, outcome))
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

    status_report_path = sample_root / "sweep-report.json"
    status_report_path.write_text(
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
    print(status_report_path)

    # Unconditional and read-only against already-published evidence, so a
    # rerun of a fully resumed sweep regenerates the report without
    # re-rendering anything.
    listening_report_path = generate_report(
        sample_root, arguments.sample.name, points, summary
    )
    print()
    print(listening_report_path)

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
