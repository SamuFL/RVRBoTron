#!/usr/bin/env python3

"""Runs the versioned Damping axis catalog (see damping_sweep_v1.json):
sweeps one curated listening sample across the Two-shelf Damping research
axes -- high-ratio, low-ratio, high-corner, low-corner -- one axis at a time
from a single Reference configuration, never combinatorially (issue #79).

Every sweep point renders both the selected listening sample and the
matching deterministic impulse under an identical Resolved Configuration
(the sample render's resolved.json, reused via `--resolved` rather than
re-resolving from the impulse), because Schroeder integration assumes an
impulse response and programme material would contaminate the decay curve
with its own envelope. Tail analysis version 2 (analyze_tail_v2.py) and
benchmarking then both run against that paired impulse/Resolved
Configuration, publishing Damping-aware octave-band evidence -- predicted
and measured low/Reference/high/transition decay -- rather than tail-v1's
single broadband RT60.

Output is laid out per sample, per axis, and per axis value, with numeric
prefixes so a folder plays back in sweep order: <output>/<sample>/
00-reference/, 01-<axis>/01-<value>/, 01-<axis>/02-<value>/, and so on.

Every run also (re)generates <output>/<sample>/listening-report.html: one
self-contained page presenting every point's renders (playable in place,
via relative paths -- no external resource requests, no JavaScript), the
requested and measured low/Reference/high ratios, the full octave-band
decay curve, complete-response and eventual-contraction status, and
benchmark cost -- so a tuning session is consumable without reading
terminal scrollback or opening a dozen JSON files. Per the note on issue
#79 (and ADR-0004), a Reference-band deviation past 10% is presented as a
flagged, not failing, observation. Regeneration is unconditional and reads
only already-published evidence, so rerunning a fully resumed sweep
refreshes the report without re-rendering anything.

Materialisation, command execution, render/benchmark invocation,
per-step resumability, one-axis-at-a-time catalog loading, and Git-LFS-aware
sample matching are catalog-agnostic and live in experiment_runner (shared
with run_tail_sweep.py, the other axis-catalog sweep); this module supplies
only what is specific to the Damping sweep: the tail-v2 analyzer, and the
listening-report layout its Damping-aware evidence produces.
"""

import argparse
import html
import json
import sys
from pathlib import Path

import analyze_tail

from experiment_runner import (
    CatalogError,
    CaseOutcome,
    benchmark_case,
    build_benchmark_summary,
    format_ranked_entry,
    load_catalog,
    materialize,
    matching_impulse,
    print_human_table,
    render_case,
    render_from_resolved,
    REPORT_STYLE,
    run_command,
    run_resumable_steps,
    sample_unavailable_reason,
    sweep_points,
    write_materialized,
    yes_no,
)

CANONICAL_WARMUP_SECONDS = 1.0
CANONICAL_MEASURE_SECONDS = 5.0
CANONICAL_BENCHMARK_BLOCK_SIZE = 128

ANALYSIS_ARTIFACT_NAME = "tail-v2.json"


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
    """Renders, tail-v2-analyzes, and benchmarks one sweep point. Each of
    the four steps below is independently resumable, matching
    run_tail_sweep.py's established per-case shape: a case that failed
    partway through finishes on the next run instead of failing the same
    way forever."""
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
            impulse_dir / "analysis" / ANALYSIS_ARTIFACT_NAME,
            "analyze tail (v2)",
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
    """Best-effort read of a point's already-published tail-v2 analysis,
    Resolved Configuration, and benchmark evidence for the listening
    report -- None for whatever a failed point never produced, rather than
    raising."""
    analysis_path = point_dir / "impulse" / "analysis" / ANALYSIS_ARTIFACT_NAME
    resolved_path = point_dir / "sample" / "resolved.json"
    benchmark_path = point_dir / "benchmark.json"

    def load_json(path):
        if not path.exists():
            return None
        try:
            return json.loads(path.read_text())
        except (OSError, UnicodeDecodeError, json.JSONDecodeError):
            return None

    return (
        load_json(analysis_path),
        load_json(resolved_path),
        load_json(benchmark_path),
    )


def _requested_damping(resolved):
    """The resolved Damping object (highRatio/highHz/lowRatio/lowHz/...) for
    this point, or None when Damping is disabled or the Resolved
    Configuration doesn't parse as one -- read from resolved.json rather
    than the analysis artifact, since tail-v2.json records only the derived
    shelf coefficients and predicted decay, not the ratio/corner inputs
    that produced them. Shared by the requested-parameters table below and
    the canonical ratio table's "requested ratio" column."""
    try:
        loop = analyze_tail.locate_feedback_loop(resolved)
    except (KeyError, ValueError):
        return None
    return loop.get("damping")


def _requested_damping_html(damping):
    if damping is None:
        return "<p>Damping: disabled.</p>"
    return f"""<table>
<tr><th>highRatio</th><th>highHz</th><th>lowRatio</th><th>lowHz</th></tr>
<tr><td>{damping['highRatio']:.3f}</td><td>{damping['highHz']:.0f} Hz</td>
<td>{damping['lowRatio']:.3f}</td><td>{damping['lowHz']:.0f} Hz</td></tr>
</table>"""


def _rt60_word(value):
    return f"{value:.3f} s" if value is not None else "n/a"


def _predicted_word(predicted, gain_mode):
    if gain_mode == "uniform":
        low, high = predicted["rangeRt60Sec"]
        return f"{low:.3f}-{high:.3f} s (range)"
    return f"{predicted['targetRt60Sec']:.3f} s (target)"


def _requested_ratio_word(label, damping):
    """The requested ratio behind one canonical band, for the "requested
    vs. measured" comparison the report needs alongside each band's
    measuredRatio (tail-v2.json's canonicalRatios carries no requested-ratio
    field of its own -- only the shelf's own highRatio/lowRatio do). The
    Reference band has no ratio parameter at all: ratios are defined
    relative to it, so its requested ratio is 1.0 by definition."""
    if damping is None:
        return "n/a"
    if label == "reference":
        return "1.000"
    return f'{damping["lowRatio" if label == "low" else "highRatio"]:.3f}'


def _canonical_ratios_html(decay, damping):
    rows = []
    for label in ("low", "reference", "high"):
        band_ratio = decay["canonicalRatios"][label]
        if band_ratio is None:
            continue
        rows.append(
            "<tr><td>{}</td><td>{:.0f} Hz</td><td>{}</td><td>{}</td><td>{}</td></tr>".format(
                label.capitalize(),
                band_ratio["centerHz"],
                _requested_ratio_word(label, damping),
                _rt60_word(band_ratio["measuredRt60Sec"]),
                f'{band_ratio["measuredRatio"]:.3f}'
                if band_ratio["measuredRatio"] is not None
                else "n/a",
            )
        )
    return f"""<table>
<tr><th>Band</th><th>Center</th><th>Requested ratio</th><th>Measured RT60</th>
<th>Measured ratio</th></tr>
{"".join(rows)}
</table>"""


def _octave_band_table_html(decay, gain_mode):
    rows = "".join(
        "<tr><td>{:.0f} Hz</td><td>{}</td><td>{}</td><td>{}</td></tr>".format(
            band["centerHz"],
            _rt60_word(band["t30"]["rt60Sec"] if band["t30"] else None),
            _predicted_word(band["predictedRt60Sec"], gain_mode),
            yes_no(band["withinPredictedTolerance"]),
        )
        for band in decay["bands"]
    )
    return f"""<details><summary>Full octave-band decay curve</summary>
<table>
<tr><th>Band</th><th>Measured T30</th><th>Predicted</th><th>Within +/-10%</th></tr>
{rows}
</table></details>"""


def _decay_section_html(analysis, damping):
    decay = analysis["decay"]
    error_word = (
        f'{decay["relativeError"] * 100:.2f}%'
        if decay["relativeError"] is not None
        else "n/a"
    )
    significant = decay["significantDeviation"]
    significant_word = (
        '<span class="flag-significant">significant</span>'
        if significant
        else "no"
    )
    complete = analysis["completeResponse"]
    contraction = analysis["eventualContraction"]
    return f"""<table>
<tr><th>Requested RT60 (1 kHz)</th><th>Measured RT60</th><th>Relative error</th>
<th>Within +/-5%</th><th>Deviation &gt;10%</th></tr>
<tr><td>{decay['requestedRt60Sec']:.3f} s</td>
<td>{_rt60_word(decay['measuredRt60Sec'])}</td>
<td>{error_word}</td>
<td>{yes_no(decay["withinAccuracyInvariant"])}</td>
<td>{significant_word}</td></tr>
</table>
{_canonical_ratios_html(decay, damping)}
{_octave_band_table_html(decay, analysis["gainMode"])}
<table>
<tr><th>Frame count check</th><th>Eventual contraction</th></tr>
<tr><td>{html.escape(complete["frameCountCheck"])}</td>
<td>{yes_no(contraction["negativeTrend"])}</td></tr>
</table>"""


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

    analysis, resolved, benchmark = _point_evidence(point_dir)
    damping = _requested_damping(resolved) if resolved is not None else None
    damping_html = (
        _requested_damping_html(damping)
        if resolved is not None
        else "<p>Resolved Configuration unavailable.</p>"
    )
    decay_html = (
        _decay_section_html(analysis, damping)
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
{damping_html}
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


_REPORT_STYLE = REPORT_STYLE + (
    ".flag-significant { color: #b06a00; font-weight: bold; }\n"
)


def generate_report(sample_root, sample_name, points, summary):
    """Writes one self-contained listening-report.html presenting every
    point's renders, requested and measured Damping ratios, the octave-band
    decay curve, complete-response and eventual-contraction status, and
    benchmark cost -- no external resource requests, no JavaScript, and no
    dependency on anything but the evidence this sweep already published to
    sample_root. Unconditional and read-only against already-published
    evidence, so rerunning a fully resumed sweep regenerates the report
    without re-rendering anything."""
    sections = "\n".join(
        _point_section_html(name, directory, point_dir, outcome)
        for name, directory, point_dir, outcome in points
    )
    document = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Damping sweep: {html.escape(sample_name)}</title>
<style>{_REPORT_STYLE}</style>
</head>
<body>
<h1>Damping sweep: {html.escape(sample_name)}</h1>
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
            "Sweep one curated listening sample across the versioned "
            "Damping axis catalog."
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
        print(f"damping sweep failed: {error}", file=sys.stderr)
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
        print(f"damping sweep failed: {error}", file=sys.stderr)
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
        print("Damping sweep had failures:", file=sys.stderr)
        for outcome in failures:
            print(f"  {outcome.name}: {outcome.detail}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
