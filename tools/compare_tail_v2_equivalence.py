#!/usr/bin/env python3

"""Compare compact tail-v2-equivalence summaries (see
extract_tail_v2_equivalence.py) captured on different platforms, grouped by
sample precision, across every platform pair.

Mirrors compare_tail_equivalence.py. Discrete facts about the committed CI
tracer -- the octave-band count, whether each band's T20/T30 fit exists,
whether each band's measured decay lands within its predicted tolerance,
the Reference-band accuracy/significant-deviation flags, the
eventual-contraction trend sign, and the echoed dampingEnabled/gainMode
config -- are compared for exact equality unconditionally, on the same
footing as tail-v1's own decayEnvelopeMonotonic (see docs/adr/
0001-cross-platform-reproducibility.md). Every measured metric, including
each band's own T20/T30 fit and predicted target/range, is compared against
the same-architecture or cross-architecture absolute tolerance committed in
a tolerances file (tools/tail_tolerances_v2.json by default).
"""

import argparse
import itertools
import json
import sys
from pathlib import Path


def platform_label(entry):
    return f'{entry["platform"]}-{entry["architecture"]}'


def numeric_metrics(entry):
    """Yield (name, value) for every tolerance-checked scalar metric."""
    yield "nonFiniteSampleCount", entry["nonFiniteSampleCount"]
    for band in entry["decay"]["bands"]:
        prefix = f'[band={band["centerHz"]:.0f}]'
        if band["t20Rt60Sec"] is not None:
            yield f"decayRt60Sec{prefix}[fit=t20]", band["t20Rt60Sec"]
        if band["t30Rt60Sec"] is not None:
            yield f"decayRt60Sec{prefix}[fit=t30]", band["t30Rt60Sec"]
        yield f"predictedTargetRt60Sec{prefix}", band["predictedTargetRt60Sec"]
        low, high = band["predictedRangeRt60Sec"]
        yield f"predictedRangeLowRt60Sec{prefix}", low
        yield f"predictedRangeHighRt60Sec{prefix}", high
    if entry["decay"]["relativeError"] is not None:
        yield "decayRelativeError", entry["decay"]["relativeError"]
    if entry["eventualContraction"]["slopeDbPerSegment"] is not None:
        yield (
            "eventualContractionSlopeDbPerSegment",
            entry["eventualContraction"]["slopeDbPerSegment"],
        )
    yield "alignmentScore", entry["alignmentScore"]
    coloration = entry["coloration"]
    yield "colorationPeakToPeakDb", coloration["peakToPeakDb"]
    yield "colorationRmsDb", coloration["rmsDb"]
    yield "colorationSpectralFlatness", coloration["spectralFlatness"]


def tolerance_key(metric_name):
    """Strip the trailing "[...]" disambiguator to find the tolerance
    category a metric name belongs to."""
    bracket = metric_name.find("[")
    return metric_name if bracket < 0 else metric_name[:bracket]


def structural_metrics(entry):
    """Facts compared for exact equality unconditionally, never looked up
    in a tolerances document: the octave-band count, whether each band's
    T20/T30 fit exists and lands within its predicted tolerance, the
    Reference-band accuracy/significant-deviation flags, the
    eventual-contraction trend sign, and the echoed Damping config."""
    bands = entry["decay"]["bands"]
    yield "dampingEnabled", entry["dampingEnabled"]
    yield "gainMode", entry["gainMode"]
    yield "decay.bandCount", len(bands)
    for band in bands:
        prefix = f'[band={band["centerHz"]:.0f}]'
        yield f"decay.t20Present{prefix}", band["t20Rt60Sec"] is not None
        yield f"decay.t30Present{prefix}", band["t30Rt60Sec"] is not None
        yield (
            f"decay.withinPredictedTolerance{prefix}",
            band["withinPredictedTolerance"],
        )
    yield "decay.withinAccuracyInvariant", entry["decay"]["withinAccuracyInvariant"]
    yield "decay.significantDeviation", entry["decay"]["significantDeviation"]
    yield (
        "eventualContraction.negativeTrend",
        entry["eventualContraction"]["negativeTrend"],
    )


_MISSING = object()


def _compare_metric(
    precision,
    first_platform,
    second_platform,
    comparison_class,
    name,
    expected,
    actual,
    tolerance,
):
    if actual is _MISSING:
        return {
            "precision": precision,
            "firstPlatform": first_platform,
            "secondPlatform": second_platform,
            "comparisonClass": comparison_class,
            "metric": name,
            "baseline": expected,
            "candidate": None,
            "delta": None,
            "tolerance": tolerance,
            "pass": False,
        }
    # A discrete label (gainMode) has no meaningful numeric distance --
    # compared for exact equality, like every other structural metric, but
    # without attempting a subtraction strings do not support.
    if isinstance(expected, str) or isinstance(actual, str):
        delta = None
        passed = actual == expected
    else:
        delta = abs(actual - expected)
        passed = delta <= tolerance
    return {
        "precision": precision,
        "firstPlatform": first_platform,
        "secondPlatform": second_platform,
        "comparisonClass": comparison_class,
        "metric": name,
        "baseline": expected,
        "candidate": actual,
        "delta": delta,
        "tolerance": tolerance,
        "pass": passed,
    }


def compare_pair(precision, first, second, tolerances):
    rows = []
    first_label = platform_label(first)
    second_label = platform_label(second)
    comparison_class = (
        "sameArchitecture"
        if first["architecture"] == second["architecture"]
        else "crossArchitecture"
    )
    first_structural = dict(structural_metrics(first))
    first_numeric = dict(numeric_metrics(first))
    second_structural = dict(structural_metrics(second))
    second_numeric = dict(numeric_metrics(second))
    for name, expected in first_structural.items():
        rows.append(
            _compare_metric(
                precision,
                first_label,
                second_label,
                comparison_class,
                name,
                expected,
                second_structural.get(name, _MISSING),
                0,
            )
        )
    for name, expected in first_numeric.items():
        tolerance = tolerances[tolerance_key(name)][
            f"{comparison_class}AbsoluteTolerance"
        ]
        rows.append(
            _compare_metric(
                precision,
                first_label,
                second_label,
                comparison_class,
                name,
                expected,
                second_numeric.get(name, _MISSING),
                tolerance,
            )
        )
    return all(row["pass"] for row in rows), rows


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
        tolerances = tolerances_document["byPrecision"][precision]
        rows = []
        group_ok = True
        ordered = sorted(group, key=lambda entry: platform_label(entry).lower())
        for first, second in itertools.combinations(ordered, 2):
            pair_ok, pair_rows = compare_pair(
                precision, first, second, tolerances
            )
            group_ok = group_ok and pair_ok
            rows.extend(pair_rows)
        ok = ok and group_ok
        groups.append(
            {
                "precision": precision,
                "skipped": False,
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
        print(f'{group["precision"]}:')
        for row in group["rows"]:
            status = "ok" if row["pass"] else "FAIL"
            delta = "n/a" if row["delta"] is None else f'{row["delta"]:.3g}'
            print(
                f'  [{status}] {row["firstPlatform"]} -> '
                f'{row["secondPlatform"]} {row["metric"]}: '
                f'baseline={row["baseline"]!r} candidate={row["candidate"]!r} '
                f'delta={delta} tolerance={row["tolerance"]!r} '
                f'comparison={row["comparisonClass"]}'
            )


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Compare compact tail-v2-equivalence summaries across "
            "platforms, grouped by sample precision, against committed "
            "tolerances."
        )
    )
    parser.add_argument("equivalence_files", nargs="+", type=Path)
    parser.add_argument(
        "--tolerances",
        type=Path,
        default=Path(__file__).parent / "tail_tolerances_v2.json",
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
            arguments.json.parent.mkdir(parents=True, exist_ok=True)
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
        print(f"tail-v2-equivalence comparison failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
