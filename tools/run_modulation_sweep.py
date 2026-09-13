#!/usr/bin/env python3

"""Runs the versioned Modulation axis catalog (see modulation_sweep_v1.json):
sweeps one curated listening sample across the Stage 6 research axes --
depth, rate, Detune product, interpolation, target (Feedback Loop vs.
Diffusion Step), shape, channel fraction, and a Damping-interaction point --
one axis at a time from a single Reference configuration, never
combinatorially (issue #95). The Reference deliberately carries Damping at
unity, the only configuration in which measured Decay tilt is attributable
to interpolation and movement alone rather than to deliberate Damping.

Every sweep point renders both the selected listening sample and the
matching deterministic impulse under an identical Resolved Configuration
(the sample render's resolved.json, reused via `--resolved` rather than
re-resolving from the impulse), because modulation-v1 measures the Downmix
output directly and programme material would contaminate that measurement
with its own content. modulation-v1 (analyze_modulation.py) and
benchmarking then both run against that paired impulse/Resolved
Configuration.

The depth axis carries both an "omitted" point (no Modulation object at
all) and a "zero" point (depthMs: 0): both resolve to the same DSP identity
by construction (docs/design/reverb/stages/06-modulation.md's "Identity is
guaranteed by construction, not by arithmetic"), so once both points
complete this module compares their rendered output byte-for-byte and
publishes the result as its own artifact -- canonical evidence, not only a
test assertion.

Output is laid out per sample, per axis, and per axis value, with numeric
prefixes so a folder plays back in sweep order: <output>/<sample>/
00-reference/, 01-depth/01-omitted/, 01-depth/02-zero/, and so on.

Every run also (re)generates <output>/<sample>/listening-report.html: one
self-contained page presenting every point's renders (playable in place, via
relative paths -- no external resource requests, no JavaScript), the
requested Modulation (and Damping) parameters, and all five modulation-v1
measurements -- bounded energy, Decay tilt, coherent pitch movement, Output
correlation, and RT60 deviation against the nominal per-Channel prediction
-- alongside benchmark cost. Regeneration is unconditional and reads only
already-published evidence, so rerunning a fully resumed sweep refreshes the
report without re-rendering anything.

Materialisation, command execution, render/benchmark invocation,
per-step resumability, one-axis-at-a-time catalog loading, and Git-LFS-aware
sample matching are catalog-agnostic and live in experiment_runner (shared
with run_tail_sweep.py and run_damping_sweep.py, the other axis-catalog
sweeps); this module supplies only what is specific to the Modulation
sweep: the modulation-v1 analyzer, the depth axis's identity-invariant
check, and the listening-report layout its evidence produces.
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

ANALYSIS_ARTIFACT_NAME = "modulation-v1.json"

# The depth axis's two identity-invariant points (see module docstring):
# names as sweep_points() derives them, "<axis name>/<value label>".
DEPTH_AXIS_NAME = "depth"
OMITTED_POINT_NAME = f"{DEPTH_AXIS_NAME}/omitted"
ZERO_POINT_NAME = f"{DEPTH_AXIS_NAME}/zero"


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
    """Renders, modulation-v1-analyzes, and benchmarks one sweep point.
    Each of the four steps below is independently resumable, matching
    run_damping_sweep.py's established per-case shape: a case that failed
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
            "analyze modulation",
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
    """Best-effort read of a point's already-published modulation-v1
    analysis, Resolved Configuration, and benchmark evidence for the
    listening report -- None for whatever a failed point never produced,
    rather than raising."""
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


def _requested_loop(resolved):
    """The resolved Feedback Loop stage for this point, or None when the
    Resolved Configuration doesn't parse as one -- read via
    analyze_tail.locate_feedback_loop rather than reimplementing "find the
    feedback-loop stage" inline (mirrors run_damping_sweep.py's own
    _requested_damping), so a Resolved Configuration missing a Feedback
    Loop degrades to "unavailable" in the report instead of an uncaught
    StopIteration crashing report generation."""
    try:
        return analyze_tail.locate_feedback_loop(resolved)
    except (KeyError, ValueError):
        return None


def _requested_damping_html(damping):
    if damping is None:
        return "<p>Damping: disabled.</p>"
    return f"""<table>
<tr><th>highRatio</th><th>highHz</th><th>lowRatio</th><th>lowHz</th></tr>
<tr><td>{damping['highRatio']:.3f}</td><td>{damping['highHz']:.0f} Hz</td>
<td>{damping['lowRatio']:.3f}</td><td>{damping['lowHz']:.0f} Hz</td></tr>
</table>"""


def _requested_modulation_html(active_modulations):
    if not active_modulations:
        return "<p>Modulation: absent.</p>"
    rows = "".join(
        "<tr><td>{}</td><td>{:.2f} ms</td><td>{:.2f} Hz</td><td>{}</td>"
        "<td>{:.2f}</td><td>{}</td></tr>".format(
            html.escape(entry["owner"]),
            entry["depthMs"],
            entry["rateHz"],
            html.escape(entry["shape"]),
            entry["channelFraction"],
            html.escape(entry["interpolation"]),
        )
        for entry in active_modulations
    )
    return f"""<table>
<tr><th>Owner</th><th>depthMs</th><th>rateHz</th><th>shape</th>
<th>channelFraction</th><th>interpolation</th></tr>
{rows}
</table>"""


def _rt60_word(value):
    return f"{value:.3f} s" if value is not None else "n/a"


def _predicted_word(predicted, gain_mode):
    if gain_mode == "uniform":
        low, high = predicted["rangeRt60Sec"]
        return f"{low:.3f}-{high:.3f} s (range)"
    return f"{predicted['targetRt60Sec']:.3f} s (target)"


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
    return f"""<details><summary>Full octave-band decay curve (RT60 deviation from nominal)</summary>
<table>
<tr><th>Band</th><th>Measured T30</th><th>Predicted (nominal)</th><th>Within +/-10%</th></tr>
{rows}
</table></details>"""


def _rt60_deviation_html(analysis, gain_mode):
    # gainMode is a Feedback Loop-level field, not carried at modulation-v1's
    # own top level (unlike tail-v2's own analysis dict) -- callers read it
    # from the Resolved Configuration instead (see _point_section_html).
    decay = analysis["decay"]
    error_word = (
        f'{decay["relativeError"] * 100:.2f}%'
        if decay["relativeError"] is not None
        else "n/a"
    )
    significant_word = (
        '<span class="flag-significant">significant</span>'
        if decay["significantDeviation"]
        else "no"
    )
    return f"""<h3>RT60 deviation from nominal</h3>
<table>
<tr><th>Requested RT60 (1 kHz)</th><th>Measured RT60</th><th>Relative error</th>
<th>Within +/-5%</th><th>Deviation &gt;10%</th></tr>
<tr><td>{decay['requestedRt60Sec']:.3f} s</td>
<td>{_rt60_word(decay['measuredRt60Sec'])}</td>
<td>{error_word}</td>
<td>{yes_no(decay["withinAccuracyInvariant"])}</td>
<td>{significant_word}</td></tr>
</table>
{_octave_band_table_html(decay, gain_mode)}"""


def _decay_tilt_html(analysis):
    tilt = analysis["decayTilt"]
    slope_word = (
        f'{tilt["slopeRatioPerOctave"]:.4f} ratio/octave'
        if tilt["slopeRatioPerOctave"] is not None
        else "n/a"
    )
    significant_word = (
        '<span class="flag-significant">significant</span>'
        if tilt["significant"]
        else "no"
    )
    return f"""<h3>Decay tilt</h3>
<table>
<tr><th>Bands fit</th><th>Slope</th><th>Intercept</th><th>Significant</th></tr>
<tr><td>{tilt['bandCount']}</td><td>{slope_word}</td>
<td>{tilt['interceptRatio']:.4f}</td><td>{significant_word}</td></tr>
</table>"""


def _bounded_energy_html(analysis):
    bounded = analysis["boundedEnergy"]
    slope_word = (
        f'{bounded["slopeDbPerSegment"]:.3f} dB/segment'
        if bounded["slopeDbPerSegment"] is not None
        else "n/a"
    )
    return f"""<h3>Bounded energy</h3>
<table>
<tr><th>Late-tail segments</th><th>Slope</th><th>Negative trend</th></tr>
<tr><td>{bounded['segmentCount']}</td><td>{slope_word}</td>
<td>{yes_no(bounded['negativeTrend'])}</td></tr>
</table>"""


def _coherent_pitch_movement_html(analysis):
    coherent = analysis["coherentPitchMovement"]
    if not coherent["rates"]:
        return "<h3>Coherent pitch movement</h3><p>No configured rate to measure.</p>"
    rows = "".join(
        "<tr><td>{}</td><td>{:.3f} Hz</td><td>{}</td><td>{}</td></tr>".format(
            html.escape(rate["owner"]),
            rate["rateHz"],
            f'{rate["relativeMagnitude"]:.2f}x mean'
            if rate["relativeMagnitude"] is not None
            else "n/a",
            '<span class="flag-significant">significant</span>'
            if rate["significant"]
            else "no",
        )
        for rate in coherent["rates"]
    )
    return f"""<h3>Coherent pitch movement</h3>
<table>
<tr><th>Owner</th><th>Rate</th><th>Magnitude vs. broadband mean</th><th>Significant</th></tr>
{rows}
</table>"""


def _output_correlation_html(analysis):
    correlation = analysis["outputCorrelation"]
    significant_word = (
        '<span class="flag-significant">significant</span>'
        if correlation["significant"]
        else "no"
    )
    return f"""<h3>Output correlation</h3>
<table>
<tr><th>Mean |off-diagonal|</th><th>Max |off-diagonal|</th><th>Significant</th></tr>
<tr><td>{correlation['meanAbsoluteOffDiagonal']:.4f}</td>
<td>{correlation['maxAbsoluteOffDiagonal']:.4f}</td><td>{significant_word}</td></tr>
</table>"""


def _benchmark_section_html(benchmark):
    return f"""<h3>Benchmark</h3>
<table>
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
    loop = _requested_loop(resolved) if resolved is not None else None
    damping_html = (
        _requested_damping_html(loop.get("damping"))
        if loop is not None
        else "<p>Resolved Configuration unavailable.</p>"
    )
    modulation_html = (
        _requested_modulation_html(analysis["activeModulations"])
        if analysis is not None
        else "<p>Modulation analysis unavailable.</p>"
    )
    measurements_html = (
        f"""{_bounded_energy_html(analysis)}
{_decay_tilt_html(analysis)}
{_coherent_pitch_movement_html(analysis)}
{_output_correlation_html(analysis)}
{_rt60_deviation_html(analysis, loop["gainMode"])}"""
        if analysis is not None and loop is not None
        else "<p>Modulation analysis unavailable.</p>"
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
<p class="meta">Impulse render (used for modulation-v1 analysis)</p>
<audio controls src="{impulse_audio}"></audio>
<h3>Requested Modulation</h3>
{modulation_html}
<h3>Requested Damping</h3>
{damping_html}
{measurements_html}
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


def _identity_invariant_html(identity_invariant):
    if identity_invariant is None:
        return ""
    if identity_invariant.get("skipped"):
        return f"""<h2>Identity invariant (depth: omitted vs. zero)</h2>
<p>{html.escape(identity_invariant["skipped"])}</p>"""
    sample_word = yes_no(identity_invariant["sampleOutputIdentical"])
    impulse_word = yes_no(identity_invariant["impulseOutputIdentical"])
    both = (
        identity_invariant["sampleOutputIdentical"]
        and identity_invariant["impulseOutputIdentical"]
    )
    status_class = "status-completed" if both else "status-failed"
    return f"""<h2>Identity invariant (depth: omitted vs. zero)</h2>
<p class="{status_class}">Modulation omitted entirely and depthMs: 0 must
produce byte-identical rendered output, by construction rather than by
arithmetic (docs/design/reverb/stages/06-modulation.md).</p>
<table>
<tr><th>Sample output identical</th><th>Impulse output identical</th></tr>
<tr><td>{sample_word}</td><td>{impulse_word}</td></tr>
</table>"""


_REPORT_STYLE = """
body { font-family: system-ui, sans-serif; margin: 2rem; max-width: 960px; }
h1 { font-size: 1.4rem; }
h2 { font-size: 1.1rem; margin-top: 2.5rem; border-bottom: 1px solid #ccc; padding-bottom: .25rem; }
h3 { font-size: .95rem; margin-top: 1.25rem; color: #333; }
table { border-collapse: collapse; margin: .5rem 0 1rem; }
th, td { border: 1px solid #ccc; padding: .25rem .5rem; text-align: right; font-variant-numeric: tabular-nums; }
th { text-align: center; background: #f2f2f2; }
td:first-child, th:first-child { text-align: left; }
audio { width: 100%; margin: .25rem 0 .75rem; }
.status-failed { color: #b00020; font-weight: bold; }
.status-completed, .status-resumed { color: #1a7a1a; }
.flag-significant { color: #b06a00; font-weight: bold; }
.meta { color: #555; font-size: .9rem; margin-bottom: 0; }
"""


def generate_report(sample_root, sample_name, points, summary, identity_invariant):
    """Writes one self-contained listening-report.html presenting every
    point's renders, requested Modulation/Damping parameters, all five
    modulation-v1 measurements, the depth axis's identity invariant, and
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
<title>Modulation sweep: {html.escape(sample_name)}</title>
<style>{_REPORT_STYLE}</style>
</head>
<body>
<h1>Modulation sweep: {html.escape(sample_name)}</h1>
<h2>Benchmark comparison</h2>
{_benchmark_summary_html(summary)}
{_identity_invariant_html(identity_invariant)}
{sections}
</body>
</html>
"""
    report_path = sample_root / "listening-report.html"
    report_path.write_text(document)
    return report_path


def depth_identity_invariant(sample_root, points):
    """Once both the depth axis's "omitted" and "zero" points have
    completed, compares their rendered sample and impulse output byte-for-
    byte and publishes the result as its own artifact -- canonical evidence
    that the resolved bypass (docs/design/reverb/stages/06-modulation.md's
    "Identity is guaranteed by construction, not by arithmetic") holds for
    this sweep's own Reference, not only as a test assertion elsewhere in
    this codebase. Returns None (nothing published) when either point is
    missing from this catalog or did not complete; a completed comparison
    is written regardless of whether the two points actually matched, since
    a mismatch is itself real evidence of a regression worth publishing."""
    by_name = {name: (point_dir, outcome) for name, _, point_dir, outcome in points}
    if OMITTED_POINT_NAME not in by_name or ZERO_POINT_NAME not in by_name:
        return None
    omitted_dir, omitted_outcome = by_name[OMITTED_POINT_NAME]
    zero_dir, zero_outcome = by_name[ZERO_POINT_NAME]
    if omitted_outcome.status == "failed" or zero_outcome.status == "failed":
        result = {
            "formatVersion": 1,
            "omittedPoint": OMITTED_POINT_NAME,
            "zeroPoint": ZERO_POINT_NAME,
            "skipped": "one or both points did not complete",
        }
    else:
        sample_identical = (omitted_dir / "sample" / "output.wav").read_bytes() == (
            zero_dir / "sample" / "output.wav"
        ).read_bytes()
        impulse_identical = (
            omitted_dir / "impulse" / "output.wav"
        ).read_bytes() == (zero_dir / "impulse" / "output.wav").read_bytes()
        result = {
            "formatVersion": 1,
            "omittedPoint": OMITTED_POINT_NAME,
            "zeroPoint": ZERO_POINT_NAME,
            "sampleOutputIdentical": sample_identical,
            "impulseOutputIdentical": impulse_identical,
        }
    axis_directory = omitted_dir.parent
    path = axis_directory / "identity-invariant.json"
    path.write_text(json.dumps(result, allow_nan=False, indent=2) + "\n")
    return result


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Sweep one curated listening sample across the versioned "
            "Modulation axis catalog."
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
        print(f"modulation sweep failed: {error}", file=sys.stderr)
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
        print(f"modulation sweep failed: {error}", file=sys.stderr)
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

    identity_invariant = depth_identity_invariant(sample_root, points)
    if identity_invariant is not None:
        print()
        if identity_invariant.get("skipped"):
            print(f"identity invariant: {identity_invariant['skipped']}")
        else:
            print(
                "identity invariant (depth: omitted vs. zero): "
                f'sample={identity_invariant["sampleOutputIdentical"]} '
                f'impulse={identity_invariant["impulseOutputIdentical"]}'
            )

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
        sample_root, arguments.sample.name, points, summary, identity_invariant
    )
    print()
    print(listening_report_path)

    failures = [outcome for outcome in outcomes if outcome.status == "failed"]
    if failures:
        print(file=sys.stderr)
        print("Modulation sweep had failures:", file=sys.stderr)
        for outcome in failures:
            print(f"  {outcome.name}: {outcome.detail}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
