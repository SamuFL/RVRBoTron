#!/usr/bin/env python3

"""Compare compact diffusion-equivalence summaries (see
extract_diffusion_equivalence.py) captured on different platforms, grouped
by sample precision, against one baseline per group.

The structural Echo-path count and Distinct-arrival density bin count
(density.echoPaths, density.binCount) are deterministic functions of the
Resolved Configuration, not measurements, so they are compared for exact
equality unconditionally rather than against a committed tolerance, per
docs/adr/0001-cross-platform-reproducibility.md. Every other metric --
including the measured Distinct-arrival counts themselves
(densityCount[metric=totalDistinctArrivals], densityCount[bin=N]) -- is
compared against the absolute tolerance committed in a tolerances file
(tools/diffusion_tolerances_v1.json by default).
"""

import argparse
import json
import sys
from pathlib import Path


def platform_label(entry):
    return f'{entry["platform"]}-{entry["architecture"]}'


def choose_baseline(entries):
    for entry in entries:
        if entry["platform"] == "macOS" and entry["architecture"] == "arm64":
            return entry
    return sorted(entries, key=platform_label)[0]


def numeric_metrics(entry):
    """Yield (name, value) for every tolerance-checked scalar metric."""
    for step in entry["energy"]:
        yield f'energyRelativeError[step={step["index"]}]', step["relativeError"]
    for step in entry["orthogonality"]:
        prefix = f'[step={step["stepIndex"]}]'
        yield f"orthogonalityMaximumAbsoluteError{prefix}", step["maximumAbsoluteError"]
        yield f"orthogonalityRmsError{prefix}", step["rmsError"]
    for correlation in entry["correlation"]:
        prefix = f'[boundary={correlation["boundary"]},index={correlation["index"]}]'
        yield (
            f"correlationMeanAbsoluteOffDiagonal{prefix}",
            correlation["meanAbsoluteOffDiagonal"],
        )
        yield (
            f"correlationMaxAbsoluteOffDiagonal{prefix}",
            correlation["maxAbsoluteOffDiagonal"],
        )
    yield "alignmentMinimum", entry["alignment"]["minimum"]
    yield "alignmentMean", entry["alignment"]["mean"]
    for scope, coloration in entry["coloration"].items():
        prefix = f"[scope={scope}]"
        yield f"colorationPeakToPeakDb{prefix}", coloration["peakToPeakDb"]
        yield f"colorationRmsDb{prefix}", coloration["rmsDb"]
        yield f"colorationSpectralFlatness{prefix}", coloration["spectralFlatness"]
    density = entry["density"]
    yield "densityCount[metric=totalDistinctArrivals]", density["totalDistinctArrivals"]
    for index, count in enumerate(density["bins"]):
        yield f"densityCount[bin={index}]", count


def tolerance_key(metric_name):
    """Strip the trailing "[...]" disambiguator to find the tolerance
    category a metric name belongs to."""
    bracket = metric_name.find("[")
    return metric_name if bracket < 0 else metric_name[:bracket]


def structural_metrics(entry):
    """Facts that are a deterministic function of the Resolved
    Configuration rather than a floating-point measurement -- compared for
    exact equality unconditionally, never looked up in a tolerances
    document. echoPaths is the structural N^k Echo-path count; binCount is
    the Distinct-arrival density curve's bin count, itself derived from
    integer sample-rate/bin-width arithmetic."""
    density = entry["density"]
    yield "density.echoPaths", density["echoPaths"]
    yield "density.binCount", len(density["bins"])


def _compare_metric(precision, platform, name, expected, actual, tolerance):
    delta = abs(actual - expected)
    passed = delta <= tolerance
    return {
        "precision": precision,
        "platform": platform,
        "metric": name,
        "baseline": expected,
        "candidate": actual,
        "delta": delta,
        "tolerance": tolerance,
        "pass": passed,
    }


def compare_group(precision, baseline, candidates, tolerances):
    rows = []
    ok = True
    baseline_structural = dict(structural_metrics(baseline))
    baseline_numeric = dict(numeric_metrics(baseline))
    for candidate in candidates:
        label = platform_label(candidate)
        candidate_structural = dict(structural_metrics(candidate))
        candidate_numeric = dict(numeric_metrics(candidate))
        for name, expected in baseline_structural.items():
            rows.append(
                _compare_metric(
                    precision, label, name, expected, candidate_structural[name], 0
                )
            )
        for name, expected in baseline_numeric.items():
            tolerance = tolerances[tolerance_key(name)]["absoluteTolerance"]
            rows.append(
                _compare_metric(
                    precision, label, name, expected, candidate_numeric[name], tolerance
                )
            )
        ok = ok and all(row["pass"] for row in rows if row["platform"] == label)
    return ok, rows


def compare(entries, tolerances_document):
    by_precision = {}
    for entry in entries:
        by_precision.setdefault(entry["samplePrecision"], []).append(entry)

    ok = True
    groups = []
    for precision, group in sorted(by_precision.items()):
        if len(group) < 2:
            groups.append(
                {
                    "precision": precision,
                    "skipped": True,
                    "reason": (
                        f"only {len(group)} platform(s) reported {precision!r}; "
                        "nothing to compare"
                    ),
                }
            )
            continue
        baseline = choose_baseline(group)
        candidates = [entry for entry in group if entry is not baseline]
        tolerances = tolerances_document["byPrecision"][precision]
        group_ok, rows = compare_group(precision, baseline, candidates, tolerances)
        ok = ok and group_ok
        groups.append(
            {
                "precision": precision,
                "skipped": False,
                "baseline": platform_label(baseline),
                "rows": rows,
                "pass": group_ok,
            }
        )
    return ok, groups


def print_report(groups):
    for group in groups:
        if group["skipped"]:
            print(f'{group["precision"]}: {group["reason"]}')
            continue
        print(f'{group["precision"]} (baseline {group["baseline"]}):')
        for row in group["rows"]:
            status = "ok" if row["pass"] else "FAIL"
            delta = "n/a" if row["delta"] is None else f'{row["delta"]:.3g}'
            print(
                f'  [{status}] {row["platform"]} {row["metric"]}: '
                f'baseline={row["baseline"]!r} candidate={row["candidate"]!r} '
                f'delta={delta} tolerance={row["tolerance"]!r}'
            )


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Compare compact diffusion-equivalence summaries across "
            "platforms, grouped by sample precision, against committed "
            "tolerances."
        )
    )
    parser.add_argument("equivalence_files", nargs="+", type=Path)
    parser.add_argument(
        "--tolerances",
        type=Path,
        default=Path(__file__).parent / "diffusion_tolerances_v1.json",
    )
    parser.add_argument("--json", type=Path, help="also write the full report here")
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    try:
        entries = [
            json.loads(path.read_text()) for path in arguments.equivalence_files
        ]
        tolerances_document = json.loads(arguments.tolerances.read_text())
        ok, groups = compare(entries, tolerances_document)
        print_report(groups)
        if arguments.json is not None:
            arguments.json.write_text(
                json.dumps(
                    {"pass": ok, "groups": groups},
                    allow_nan=False,
                    indent=2,
                    sort_keys=True,
                )
                + "\n"
            )
        return 0 if ok else 1
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"diffusion-equivalence comparison failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
