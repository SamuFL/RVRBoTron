#!/usr/bin/env python3

"""Extract a compact, cross-platform-comparable summary from a Render
Result already analyzed with analysis/tail-v1.json.

The extracted artifact is deliberately small and WAV-free, mirroring
extract_diffusion_equivalence.py: it drops the full decay-envelope
segment-energy series and Coloration's per-band twelfth-octave curve that
compare_tail_equivalence.py does not need. Most fields below are also the
metrics compare_tail_equivalence.py judges cross-platform equivalence
against (see docs/adr/0001-cross-platform-reproducibility.md);
decay.requestedRt60Sec/measuredRt60Sec/relativeError are the exception, kept only
as human-readable summary context -- measuredRt60Sec is, by construction,
the same value as the Reference band's own t30Rt60Sec entry in
decay.bands, so comparing it separately would just re-check that one
number under a second name. extract_diffusion_equivalence.py has the same
kind of inert context field (energy.ratio, alongside the relativeError
that is actually compared).
"""

import argparse
import json
import sys
from pathlib import Path


def extract(render_result):
    metadata = json.loads((render_result / "render.json").read_text())
    analysis = json.loads(
        (render_result / "analysis" / "tail-v1.json").read_text()
    )

    bands = [
        {
            "centerHz": band["centerHz"],
            "t20Rt60Sec": band["t20"]["rt60Sec"] if band["t20"] else None,
            "t30Rt60Sec": band["t30"]["rt60Sec"] if band["t30"] else None,
        }
        for band in analysis["decay"]["bands"]
    ]
    coloration = analysis["coloration"]

    return {
        "formatVersion": 1,
        "platform": metadata["platform"],
        "architecture": metadata["architecture"],
        "samplePrecision": metadata["samplePrecision"],
        "rendererVersion": metadata["rendererVersion"],
        "nonFiniteSampleCount": analysis["nonFiniteSampleCount"],
        "decay": {
            "requestedRt60Sec": analysis["decay"]["requestedRt60Sec"],
            "measuredRt60Sec": analysis["decay"]["measuredRt60Sec"],
            "relativeError": analysis["decay"]["relativeError"],
            "bands": bands,
        },
        "decayEnvelopeMonotonic": analysis["decayEnvelope"]["monotonic"],
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
            "Extract a compact, WAV-free tail-equivalence summary from an "
            "already-analyzed Render Result."
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
        print(f"tail-equivalence extraction failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
