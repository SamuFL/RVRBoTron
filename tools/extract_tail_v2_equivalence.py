#!/usr/bin/env python3

"""Extract a compact, cross-platform-comparable summary from a Render
Result already analyzed with analysis/tail-v2.json.

Mirrors extract_tail_equivalence.py's own shape and reasoning (a small,
WAV-free artifact dropping bulky per-sample evidence -- here, each band's
per-Channel predicted RT60 list and eventualContraction's per-segment
energy series, alongside Coloration's twelfth-octave curve tail-v1 already
drops). decay.requestedRt60Sec/measuredRt60Sec are likewise kept only as
human-readable context; comparison uses the per-band figures instead.
"""

import argparse
import json
import sys
from pathlib import Path


def extract(render_result):
    metadata = json.loads((render_result / "render.json").read_text())
    analysis = json.loads(
        (render_result / "analysis" / "tail-v2.json").read_text()
    )
    decay = analysis["decay"]

    bands = [
        {
            "centerHz": band["centerHz"],
            "t20Rt60Sec": band["t20"]["rt60Sec"] if band["t20"] else None,
            "t30Rt60Sec": band["t30"]["rt60Sec"] if band["t30"] else None,
            "predictedTargetRt60Sec": band["predictedRt60Sec"]["targetRt60Sec"],
            "predictedRangeRt60Sec": band["predictedRt60Sec"]["rangeRt60Sec"],
            "withinPredictedTolerance": band["withinPredictedTolerance"],
        }
        for band in decay["bands"]
    ]
    coloration = analysis["coloration"]
    eventual_contraction = analysis["eventualContraction"]

    return {
        "formatVersion": 1,
        "platform": metadata["platform"],
        "architecture": metadata["architecture"],
        "samplePrecision": metadata["samplePrecision"],
        "rendererVersion": metadata["rendererVersion"],
        "nonFiniteSampleCount": analysis["nonFiniteSampleCount"],
        "dampingEnabled": analysis["dampingEnabled"],
        "gainMode": analysis["gainMode"],
        "decay": {
            "requestedRt60Sec": decay["requestedRt60Sec"],
            "measuredRt60Sec": decay["measuredRt60Sec"],
            "relativeError": decay["relativeError"],
            "withinAccuracyInvariant": decay["withinAccuracyInvariant"],
            "significantDeviation": decay["significantDeviation"],
            "bands": bands,
        },
        "eventualContraction": {
            "segmentCount": eventual_contraction["segmentCount"],
            "slopeDbPerSegment": eventual_contraction["slopeDbPerSegment"],
            "negativeTrend": eventual_contraction["negativeTrend"],
        },
        "alignmentScore": analysis["alignment"]["score"],
        "coloration": {
            "peakToPeakDb": coloration["peakToPeakDb"],
            "rmsDb": coloration["rmsDb"],
            "spectralFlatness": coloration["spectralFlatness"],
        },
    }


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Extract a compact, WAV-free tail-v2-equivalence summary from "
            "an already-analyzed Render Result."
        )
    )
    parser.add_argument("render_result", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    try:
        equivalence = extract(arguments.render_result)
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(
            json.dumps(equivalence, allow_nan=False, indent=2, sort_keys=True)
            + "\n"
        )
        print(arguments.output)
        return 0
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"tail-v2-equivalence extraction failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
