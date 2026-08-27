#!/usr/bin/env python3

"""Extract a compact, cross-platform-comparable summary from a Render
Result already analyzed with analysis/diffusion-v1.json.

The extracted artifact is deliberately small and WAV-free: it carries only
the scalar/vector metrics that Cross-platform equivalence is judged
against (see docs/adr/0001-cross-platform-reproducibility.md), dropping
full correlation matrices, per-pair Alignment listings, and per-band
Coloration curves that compare_diffusion_equivalence.py does not need.
"""

import argparse
import json
import sys
from pathlib import Path


def extract(render_result):
    metadata = json.loads((render_result / "render.json").read_text())
    analysis = json.loads(
        (render_result / "analysis" / "diffusion-v1.json").read_text()
    )

    energy = [
        {
            "index": step["index"],
            "ratio": step["ratio"],
            "relativeError": step["relativeError"],
        }
        for step in analysis["energy"]["diffusionSteps"]
    ]
    orthogonality = [
        {
            "stepIndex": step["stepIndex"],
            "maximumAbsoluteError": step["maximumAbsoluteError"],
            "rmsError": step["rmsError"],
        }
        for step in analysis["orthogonality"]
    ]
    correlation = [
        {
            "boundary": entry["boundary"],
            "index": entry.get("index"),
            "meanAbsoluteOffDiagonal": entry["meanAbsoluteOffDiagonal"],
            "maxAbsoluteOffDiagonal": entry["maxAbsoluteOffDiagonal"],
        }
        for entry in analysis["correlation"]
    ]
    alignment = {
        "minimum": analysis["alignment"]["minimum"],
        "mean": analysis["alignment"]["mean"],
    }
    density = {
        "echoPaths": analysis["density"]["echoPaths"],
        "totalDistinctArrivals": analysis["density"]["totalDistinctArrivals"],
        "bins": analysis["density"]["bins"],
    }

    def coloration_summary(evidence):
        return {
            "peakToPeakDb": evidence["peakToPeakDb"],
            "rmsDb": evidence["rmsDb"],
            "spectralFlatness": evidence["spectralFlatness"],
        }

    coloration = {
        "combined": coloration_summary(analysis["coloration"]["combined"]),
        "stereo": coloration_summary(analysis["coloration"]["stereo"]),
    }

    return {
        "formatVersion": 1,
        "platform": metadata["platform"],
        "architecture": metadata["architecture"],
        "samplePrecision": metadata["samplePrecision"],
        "rendererVersion": metadata["rendererVersion"],
        "energy": energy,
        "orthogonality": orthogonality,
        "correlation": correlation,
        "alignment": alignment,
        "density": density,
        "coloration": coloration,
    }


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Extract a compact, WAV-free diffusion-equivalence summary "
            "from an already-analyzed Render Result."
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
        print(f"diffusion-equivalence extraction failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
