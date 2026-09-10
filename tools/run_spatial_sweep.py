#!/usr/bin/env python3

"""Runs the versioned spatial-output axis catalog (see spatial_sweep_v1.json):
sweeps one curated listening sample across the Early/Main Downmix research
axes -- tap index, Early level, Early-envelope slope, aligned select versus
sum-all, Main strategy, selected Channel pair, Early width, Main width, and
N -- one axis at a time from a single Reference configuration, never
combinatorially.

Every sweep point renders both the selected listening sample and the
deterministic impulse under an identical Resolved Configuration (the
sample render's resolved.json, reused via `--resolved` rather than
re-resolving from the impulse), matching run_tail_sweep.py's own reasoning:
Downmix evidence (Alignment score, branch energy ratio, spectral deviation)
needs Stage captures, and capturing programme material would work but the
deterministic impulse keeps every point's evidence directly comparable to
every other. analyze_downmix.py then runs against the impulse render, with
`--capture-stages all`.

Two properties this sweep proves that no earlier catalog needed to (issue
#116's own "Catalog validation proves each point changes only its named
axis and remains deterministic across repeated execution"):

- **Isolation.** Each axis declares a `scope` (a JSON-Pointer prefix) in
  the catalog; every point's materialized Requested Configuration is
  diffed against the Reference, and every changed field must fall under
  that axis's own scope. A catalog-authoring mistake that changes more
  than its one named axis fails loudly here rather than silently
  confounding the sweep.
- **Determinism.** Each point's impulse render is repeated into a scratch
  directory and byte-compared (resolved.json, output.wav, every Stage
  capture) against the original; a mismatch fails the point.

The Reference's Main wet path reads the Feedback Loop (unaligned), so the
Coherent Downmix ablation (issue #114) is demonstrated on Early Reflections
instead, which is always aligned by construction: the `coherent-downmix`
axis's `sum-all` point is compared against the Reference's own `select`
Early Downmix (the matched control) using each point's own published
analyze_downmix.py evidence, producing a non-fatal warning with measured
peak-factor and spectral-deviation deltas -- never rejecting either render
(issue #116's own AC: "Matched Coherent Downmix points warn and compare
measured deltas without rejecting either render").

Benchmarking is deliberately out of scope here (unlike run_diffusion_catalog.py
and run_tail_sweep.py): issue #116's own acceptance criteria say nothing
about processing cost, only spatial measurements and listening-report
evidence.

Materialisation, command execution, render invocation, per-step
resumability, one-axis-at-a-time catalog loading, and Git-LFS-aware sample
matching are catalog-agnostic and live in experiment_runner (shared with
run_tail_sweep.py/run_damping_sweep.py, the other axis-catalog sweeps);
this module supplies only what is specific to the spatial sweep: the
downmix analyzer, isolation/determinism validation, the Coherent Downmix
comparator, and the listening-report layout downmix-v1 evidence produces.
"""

import argparse
import html
import json
import shutil
import subprocess
import sys
from pathlib import Path

from experiment_runner import (
    CatalogError,
    CaseOutcome,
    load_catalog,
    materialize,
    matching_impulse,
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

# The Coherent Downmix ablation axis this catalog is expected to declare
# (see spatial_sweep_v1.json's own "coherent-downmix" axis) -- hardcoded
# here rather than discovered generically, matching build_benchmark_summary's
# own hardcoded "reference" name elsewhere: this script is specific to this
# one catalog's own schema, not a generic comparator.
COHERENT_DOWNMIX_CONTROL_POINT = "reference"
COHERENT_DOWNMIX_ABLATION_POINT = "coherent-downmix/sum-all"


def _json_pointer_escape(token):
    return token.replace("~", "~0").replace("/", "~1")


def changed_pointer_paths(before, after, prefix=""):
    """Yields the JSON-Pointer path of every point where `before` and
    `after` differ -- a field added, removed, or holding a different
    scalar/array -- recursing through nested objects and matching-length
    arrays. An array whose length changed is reported once, at its own
    path, rather than per-element (this catalog's own array-valued
    overrides, like tap-index's whole-array replace, only need to prove
    *that* they stayed within scope, not diff their own elements)."""
    if isinstance(before, dict) and isinstance(after, dict):
        for key in sorted(set(before) | set(after)):
            child = f"{prefix}/{_json_pointer_escape(key)}"
            if key not in before or key not in after:
                yield child
            else:
                yield from changed_pointer_paths(before[key], after[key], child)
    elif isinstance(before, list) and isinstance(after, list):
        if len(before) != len(after):
            yield prefix
            return
        for index, (b, a) in enumerate(zip(before, after)):
            yield from changed_pointer_paths(b, a, f"{prefix}/{index}")
    else:
        if before != after:
            yield prefix


def validate_point_isolation(reference, materialized, scope, point_name):
    """Raises CatalogError unless every field the point's overrides
    actually changed, relative to the Reference, falls under the axis's
    own declared `scope` (a JSON-Pointer prefix) -- issue #116's own
    "each point changes only its named axis", mechanically proven rather
    than merely intended."""
    changed = list(changed_pointer_paths(reference, materialized, ""))
    if not changed:
        raise CatalogError(
            f"{point_name}: its overrides produced no change from the Reference"
        )
    outside_scope = [
        path for path in changed if path != scope and not path.startswith(scope + "/")
    ]
    if outside_scope:
        raise CatalogError(
            f"{point_name}: changed field(s) outside its declared scope "
            f"{scope!r}: {outside_scope}"
        )


def _verify_determinism(renderer, impulse, resolved_path, impulse_dir, repeat_dir, block_size):
    try:
        rendered = render_from_resolved(
            renderer,
            impulse,
            resolved_path,
            repeat_dir,
            block_size=block_size,
            capture_stages=True,
        )
        if rendered.returncode != 0:
            return rendered
        mismatches = []
        for path in sorted(impulse_dir.rglob("*")):
            relative = path.relative_to(impulse_dir)
            if path.is_dir() or relative.parts[0] == "analysis":
                continue
            repeat_path = repeat_dir / relative
            if not repeat_path.exists() or repeat_path.read_bytes() != path.read_bytes():
                mismatches.append(str(relative))
    finally:
        shutil.rmtree(repeat_dir, ignore_errors=True)
    if mismatches:
        return subprocess.CompletedProcess(
            args=[],
            returncode=1,
            stdout="",
            stderr=f"repeated render differed in: {mismatches}",
        )
    return subprocess.CompletedProcess(args=[], returncode=0, stdout="", stderr="")


def run_sweep_point(
    name,
    point_dir,
    overrides,
    reference,
    scope,
    sample,
    impulse,
    renderer,
    analyzer,
    block_size,
):
    """Renders, downmix-analyzes, and determinism-verifies one sweep point.
    Each of the four steps below is independently resumable, matching
    run_tail_sweep.py's own established per-point shape: a point that
    failed partway through finishes on the next run instead of failing the
    same way forever. Isolation is checked first and unconditionally (pure
    JSON comparison, no rendering, so cheap enough to redo every run);
    `scope` is None only for the Reference point itself, which carries no
    overrides to validate."""
    materialized = materialize(reference, overrides)
    if scope is not None:
        validate_point_isolation(reference, materialized, scope, name)

    sample_dir = point_dir / "sample"
    impulse_dir = point_dir / "impulse"
    resolved_path = sample_dir / "resolved.json"
    determinism_path = point_dir / "determinism.json"

    def render_sample_step():
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

    def determinism_step():
        result = _verify_determinism(
            renderer,
            impulse,
            resolved_path,
            impulse_dir,
            point_dir / "impulse-repeat",
            block_size,
        )
        if result.returncode == 0:
            determinism_path.write_text(
                json.dumps({"formatVersion": 1, "deterministic": True}, indent=2)
                + "\n"
            )
        return result

    steps = [
        (sample_dir / "render.json", "render sample", render_sample_step),
        (
            impulse_dir / "render.json",
            "render impulse",
            lambda: render_from_resolved(
                renderer,
                impulse,
                resolved_path,
                impulse_dir,
                block_size=block_size,
                capture_stages=True,
            ),
        ),
        (
            impulse_dir / "analysis" / "downmix-v1.json",
            "analyze",
            lambda: run_command(
                sys.executable, analyzer, impulse_dir, "--source", impulse
            ),
        ),
        (determinism_path, "verify determinism", determinism_step),
    ]

    did_new_work, failure = run_resumable_steps(steps)
    if failure is not None:
        return CaseOutcome(name, "failed", failure)
    status = "completed" if did_new_work else "resumed"
    return CaseOutcome(name, status)


def compare_coherent_downmix(reference_analysis, ablation_analysis):
    """Issue #116's AC: "Matched Coherent Downmix points warn and compare
    measured deltas without rejecting either render." Compares the
    Reference's own select-strategy Early Downmix (the matched control)
    against the coherent-downmix/sum-all point's aligned sum-all ablation
    -- Requested Configurations otherwise identical, differing only in
    Early's own Downmix strategy (that axis's own declared scope) -- using
    each point's already-published analyze_downmix.py evidence. Always
    returns a report; never raises, and never affects either render's own
    exit code."""
    control_early = reference_analysis["early"]
    ablation_early = ablation_analysis["early"]
    control_spectral = control_early["spectralDeviation"]
    ablation_spectral = ablation_early["spectralDeviation"]
    spectral_available = (
        control_spectral["available"] and ablation_spectral["available"]
    )
    return {
        "formatVersion": 1,
        "controlPoint": COHERENT_DOWNMIX_CONTROL_POINT,
        "controlStrategy": control_early["strategy"],
        "ablationPoint": COHERENT_DOWNMIX_ABLATION_POINT,
        "ablationStrategy": ablation_early["strategy"],
        "ablationTagged": ablation_early["coherentDownmixAblation"],
        "peakFactorDelta": (
            ablation_analysis["peakFactor"]["peakFactor"]
            - reference_analysis["peakFactor"]["peakFactor"]
        ),
        "spectralDeviation": (
            {
                "available": True,
                "maxDeviationDelta": (
                    ablation_spectral["maxDeviation"]
                    - control_spectral["maxDeviation"]
                ),
                "rmsDeviationDelta": (
                    ablation_spectral["rmsDeviation"]
                    - control_spectral["rmsDeviation"]
                ),
            }
            if spectral_available
            else {"available": False}
        ),
        "warning": (
            "aligned sum-all coherently reinforces Early's Channels that "
            "the matched select control keeps separate; compare peak "
            "factor and spectral deviation before treating this as a "
            "defect (docs/design/reverb/stages/08-downmix.md's Coherent "
            "Downmix ablation)"
        ),
    }


def _point_evidence(point_dir):
    """Best-effort read of a point's already-published downmix analysis
    for the listening report -- None for whatever a failed point never
    produced, rather than raising."""
    analysis_path = point_dir / "impulse" / "analysis" / "downmix-v1.json"
    if not analysis_path.exists():
        return None
    try:
        return json.loads(analysis_path.read_text())
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return None


def _branch_section_html(title, branch):
    if branch is None:
        return f"<p>{html.escape(title)}: not configured.</p>"
    score = branch["alignmentScore"]
    score_word = f'{score["mean"]:.3f}' if score is not None else "n/a"
    ratio = branch["branchEnergyRatio"]
    ratio_word = (
        f'{ratio["ratio"]:.3f}' if ratio["available"] and ratio["ratio"] is not None
        else "n/a"
    )
    return f"""<table>
<tr><th>{html.escape(title)}</th><th>Strategy</th><th>Alignment</th>
<th>Coherent ablation</th><th>Alignment score</th><th>Branch/source ratio</th></tr>
<tr><td></td><td>{html.escape(branch["strategy"])}</td>
<td>{html.escape(branch["alignmentExpectation"])}</td>
<td>{yes_no(branch["coherentDownmixAblation"])}</td>
<td>{score_word}</td><td>{ratio_word}</td></tr>
</table>"""


def _downmix_section_html(analysis):
    return f"""{_branch_section_html("Main", analysis["main"])}
{_branch_section_html("Early", analysis.get("early"))}
<table>
<tr><th>Peak factor</th><th>Output correlation</th><th>Mono fold-down energy loss</th></tr>
<tr><td>{analysis["peakFactor"]["peakFactor"]:.3f}</td>
<td>{analysis["outputCorrelation"]["maxAbsoluteOffDiagonal"]:.3f}</td>
<td>{analysis["monoFoldDown"]["energyLossRatio"]:.3f}</td></tr>
</table>"""


def _comparison_section_html(comparison):
    if comparison is None:
        return ""
    spectral = comparison["spectralDeviation"]
    spectral_word = (
        f'max {spectral["maxDeviationDelta"]:.4g}, rms {spectral["rmsDeviationDelta"]:.4g}'
        if spectral["available"]
        else "n/a"
    )
    return f"""<h2>Coherent Downmix comparison</h2>
<p class="meta">{html.escape(comparison["controlPoint"])} ({html.escape(comparison["controlStrategy"])})
vs {html.escape(comparison["ablationPoint"])} ({html.escape(comparison["ablationStrategy"])})</p>
<table>
<tr><th>Peak factor delta</th><th>Spectral deviation delta</th></tr>
<tr><td>{comparison["peakFactorDelta"]:+.3f}</td><td>{spectral_word}</td></tr>
</table>
<p class="warning">{html.escape(comparison["warning"])}</p>"""


def _point_section_html(name, directory, point_dir, outcome):
    heading = html.escape(name)
    if outcome.status == "failed":
        return (
            f"<h2>{heading}</h2>\n"
            f'<p class="status-failed">FAILED: {html.escape(outcome.detail or "")}</p>'
        )
    analysis = _point_evidence(point_dir)
    downmix_html = (
        _downmix_section_html(analysis)
        if analysis is not None
        else "<p>Downmix analysis unavailable.</p>"
    )
    sample_audio = html.escape(f"{directory}/sample/output.wav")
    impulse_audio = html.escape(f"{directory}/impulse/output.wav")
    return f"""<h2>{heading} <span class="status-{outcome.status}">[{outcome.status}]</span></h2>
<p class="meta">Sample render</p>
<audio controls src="{sample_audio}"></audio>
<p class="meta">Impulse render (used for Downmix analysis)</p>
<audio controls src="{impulse_audio}"></audio>
{downmix_html}"""


_REPORT_STYLE = REPORT_STYLE + (
    ".warning { color: #8a6100; background: #fff8e6; padding: .5rem .75rem; "
    "border: 1px solid #f0d78c; }\n"
)


def generate_report(sample_root, sample_name, points, comparison):
    """Writes one self-contained listening-report.html presenting every
    point's renders and measured Downmix evidence, plus the Coherent
    Downmix comparison -- no external resource requests, no JavaScript, no
    dependency on anything but evidence this sweep already published.
    Unconditional and read-only, so rerunning a fully resumed sweep
    refreshes the report without re-rendering anything."""
    sections = "\n".join(
        _point_section_html(name, directory, point_dir, outcome)
        for name, directory, point_dir, outcome in points
    )
    document = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Spatial sweep: {html.escape(sample_name)}</title>
<style>{_REPORT_STYLE}</style>
</head>
<body>
<h1>Spatial sweep: {html.escape(sample_name)}</h1>
{_comparison_section_html(comparison)}
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
            "spatial-output axis catalog."
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
        "--block-size",
        type=int,
        default=None,
        help=(
            "override render block size for every point (sample, impulse, "
            "and its determinism repeat); the Reference's smallest "
            "resolved delay comfortably exceeds the renderer's own "
            "default, so this is normally only needed for a "
            "millisecond-scale tracer catalog"
        ),
    )
    return parser.parse_args()


def _axis_scope(point_name, scope_by_axis_name):
    """The declared scope for the axis that owns `point_name`, or None for
    the Reference point (no `/`, no axis) or a legacy axis with no `scope`
    declared at all. Relies on sweep_points' own documented point-name
    format (`f"{axis['name']}/{value['label']}"`) to recover which axis a
    point belongs to -- centralized here as the one place that format is
    load-bearing, rather than re-parsed at each call site."""
    if "/" not in point_name:
        return None
    axis_name = point_name.split("/", 1)[0]
    return scope_by_axis_name.get(axis_name)


def main():
    arguments = parse_arguments()
    try:
        catalog = load_catalog(arguments.catalog)
    except (CatalogError, OSError, json.JSONDecodeError, KeyError) as error:
        print(f"spatial sweep failed: {error}", file=sys.stderr)
        return 1
    missing_scope = [
        axis["name"] for axis in catalog["axes"] if "scope" not in axis
    ]
    if missing_scope:
        print(
            "spatial sweep failed: every axis must declare its own "
            f"'scope' for isolation validation, missing from: "
            f"{missing_scope}",
            file=sys.stderr,
        )
        return 1
    scope_by_axis_name = {axis["name"]: axis["scope"] for axis in catalog["axes"]}

    reason = sample_unavailable_reason(arguments.sample)
    if reason is not None:
        print(f"[skipped] {reason}")
        return 0

    try:
        impulse = matching_impulse(
            arguments.sample, arguments.mono_impulse, arguments.stereo_impulse
        )
    except CatalogError as error:
        print(f"spatial sweep failed: {error}", file=sys.stderr)
        return 1

    sample_root = arguments.output / arguments.sample.stem
    sample_root.mkdir(parents=True, exist_ok=True)

    points = []
    for name, directory, overrides in sweep_points(catalog):
        point_dir = sample_root / directory
        try:
            outcome = run_sweep_point(
                name,
                point_dir,
                overrides,
                catalog["reference"],
                _axis_scope(name, scope_by_axis_name),
                arguments.sample,
                impulse,
                arguments.renderer,
                arguments.analyzer,
                arguments.block_size,
            )
        except CatalogError as error:
            outcome = CaseOutcome(name, "failed", str(error))
        points.append((name, directory, point_dir, outcome))
        print(f'[{outcome.status}] {outcome.name}' + (
            f': {outcome.detail}' if outcome.detail else ''
        ))

    point_by_name = {name: (point_dir, outcome) for name, _, point_dir, outcome in points}

    comparison = None
    control = point_by_name.get(COHERENT_DOWNMIX_CONTROL_POINT)
    ablation = point_by_name.get(COHERENT_DOWNMIX_ABLATION_POINT)
    if control is not None and ablation is not None:
        control_dir, control_outcome = control
        ablation_dir, ablation_outcome = ablation
        if control_outcome.status != "failed" and ablation_outcome.status != "failed":
            reference_analysis = _point_evidence(control_dir)
            ablation_analysis = _point_evidence(ablation_dir)
            if reference_analysis is not None and ablation_analysis is not None:
                comparison = compare_coherent_downmix(
                    reference_analysis, ablation_analysis
                )
                comparison_path = sample_root / "coherent-downmix-comparison.json"
                comparison_path.write_text(
                    json.dumps(comparison, allow_nan=False, indent=2, sort_keys=True)
                    + "\n"
                )
                print()
                print(f"Coherent Downmix: {comparison['warning']}")
                print(comparison_path)

    status_report_path = sample_root / "sweep-report.json"
    status_report_path.write_text(
        json.dumps(
            {
                "formatVersion": 1,
                "sample": arguments.sample.name,
                "points": [outcome.to_json() for _, _, _, outcome in points],
                "coherentDownmixComparison": comparison,
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
        sample_root, arguments.sample.name, points, comparison
    )
    print()
    print(listening_report_path)

    failures = [outcome for _, _, _, outcome in points if outcome.status == "failed"]
    if failures:
        print(file=sys.stderr)
        print("Spatial sweep had failures:", file=sys.stderr)
        for outcome in failures:
            print(f"  {outcome.name}: {outcome.detail}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
