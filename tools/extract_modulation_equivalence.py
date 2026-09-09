#!/usr/bin/env python3

"""Extract a compact, cross-platform-comparable summary from a Render
Result already analyzed with analysis/modulation-v1.json.

Mirrors extract_tail_v2_equivalence.py's own shape and reasoning (a small,
WAV-free artifact dropping bulky per-sample evidence -- here, each band's
per-Channel predicted RT60 list, the bounded-energy per-segment dB series,
and the Decay evidence's twelfth-octave-scale bulky fields tail-v2's own
extraction already drops).
"""

import argparse
import json
import sys
from pathlib import Path


def extract(render_result):
    metadata = json.loads((render_result / "render.json").read_text())
    analysis = json.loads(
        (render_result / "analysis" / "modulation-v1.json").read_text()
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
    bounded_energy = analysis["boundedEnergy"]
    decay_tilt = analysis["decayTilt"]
    coherent_pitch_movement = analysis["coherentPitchMovement"]
    output_correlation = analysis["outputCorrelation"]

    return {
        "formatVersion": 1,
        "platform": metadata["platform"],
        "architecture": metadata["architecture"],
        "samplePrecision": metadata["samplePrecision"],
        "rendererVersion": metadata["rendererVersion"],
        "nonFiniteSampleCount": analysis["nonFiniteSampleCount"],
        "modulationPresent": analysis["modulationPresent"],
        "activeModulationCount": len(analysis["activeModulations"]),
        "decay": {
            "measuredRt60Sec": decay["measuredRt60Sec"],
            "relativeError": decay["relativeError"],
            "withinAccuracyInvariant": decay["withinAccuracyInvariant"],
            "significantDeviation": decay["significantDeviation"],
            "bands": bands,
        },
        "decayTilt": {
            "bandCount": decay_tilt["bandCount"],
            "slopeRatioPerOctave": decay_tilt["slopeRatioPerOctave"],
            "interceptRatio": decay_tilt["interceptRatio"],
            "significant": decay_tilt["significant"],
        },
        "boundedEnergy": {
            "slopeDbPerSegment": bounded_energy["slopeDbPerSegment"],
            "negativeTrend": bounded_energy["negativeTrend"],
        },
        "coherentPitchMovement": {
            "broadbandMeanMagnitude": coherent_pitch_movement[
                "broadbandMeanMagnitude"
            ],
            "rates": [
                {
                    "owner": rate["owner"],
                    "rateHz": rate["rateHz"],
                    "magnitude": rate["magnitude"],
                    "relativeMagnitude": rate["relativeMagnitude"],
                    "significant": rate["significant"],
                }
                for rate in coherent_pitch_movement["rates"]
            ],
        },
        "outputCorrelation": {
            "meanAbsoluteOffDiagonal": output_correlation["meanAbsoluteOffDiagonal"],
            "maxAbsoluteOffDiagonal": output_correlation["maxAbsoluteOffDiagonal"],
            "significant": output_correlation["significant"],
        },
    }


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Extract a compact modulation-v1-equivalence summary from an "
            "already-analyzed Render Result."
        )
    )
    parser.add_argument("render_result", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    try:
        summary = extract(arguments.render_result)
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(
            json.dumps(summary, allow_nan=False, indent=2, sort_keys=True) + "\n"
        )
        print(arguments.output)
        return 0
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"modulation-equivalence extraction failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
