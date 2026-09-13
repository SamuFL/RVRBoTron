#!/usr/bin/env python3

import json
import shutil
import struct
import subprocess
import sys
import wave
from pathlib import Path


def write_pcm16_mono_wav(path, sample_rate, samples):
    with wave.open(str(path), "wb") as writer:
        writer.setnchannels(1)
        writer.setsampwidth(2)
        writer.setframerate(sample_rate)
        writer.writeframes(
            b"".join(
                struct.pack("<h", int(max(-1.0, min(1.0, sample)) * 32767))
                for sample in samples
            )
        )


def run_renderer(renderer, fixture, request, output):
    return subprocess.run(
        [
            str(renderer),
            "render",
            "--input",
            str(fixture),
            "--config",
            str(request),
            "--capture-stages",
            "all",
            "--output",
            str(output),
        ],
        check=False,
        capture_output=True,
        text=True,
    )


def run_analyzer(analyzer, render_result):
    return subprocess.run(
        [sys.executable, str(analyzer), str(render_result)],
        check=False,
        capture_output=True,
        text=True,
    )


def main():
    analyzer = Path(sys.argv[1])
    renderer = Path(sys.argv[2])
    fixture = Path(sys.argv[3])
    workspace = Path(sys.argv[4])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    request_document = {
        "formatVersion": 2,
        "seed": 42,
        "composition": {
            "stages": [
                {
                    "type": "split",
                    "channels": 4,
                    "strategy": "duplicate",
                    "normalisation": "energy",
                },
                {
                    "type": "diffuser",
                    "steps": 2,
                    "totalMs": 4,
                    "distribution": "even",
                    "step": {
                        "delayStrategy": "segmented-random",
                        "mix": "hadamard",
                        "shuffle": True,
                        "polarity": "seeded-random",
                    },
                },
                {
                    "type": "downmix",
                    "strategy": "select",
                    "leftChannel": 0,
                    "rightChannel": 1,
                    "normalisation": "energy",
                },
            ],
            "early": {
                "taps": [
                    {"stepIndex": 1},
                    {"stepIndex": 0},
                ],
            },
        },
    }
    request = workspace / "request.json"
    request.write_text(json.dumps(request_document))

    render_result = workspace / "render-result"
    rendered = run_renderer(renderer, fixture, request, render_result)
    if rendered.returncode != 0:
        raise AssertionError(rendered.stderr)

    analyzed = run_analyzer(analyzer, render_result)
    if analyzed.returncode != 0:
        raise AssertionError(analyzed.stderr)

    resolved = json.loads((render_result / "resolved.json").read_text())
    resolved_taps = resolved["composition"]["early"]["taps"]

    artifact = render_result / "analysis" / "early-support-v1.json"
    analysis = json.loads(artifact.read_text())
    if analysis["analyzer"] != "early-support" or analysis["analyzerVersion"] != 1:
        raise AssertionError(f"unexpected analyzer identity: {analysis}")
    if len(analysis["taps"]) != len(resolved_taps):
        raise AssertionError(f"analyzer did not report every tap: {analysis}")

    for analyzed_tap, resolved_tap in zip(analysis["taps"], resolved_taps):
        if analyzed_tap["stepIndex"] != resolved_tap["stepIndex"]:
            raise AssertionError(
                f"analyzer taps are not in canonical (Resolved) order: "
                f"{analysis}"
            )
        if (
            analyzed_tap["nominalSupportMinSamples"]
            != resolved_tap["nominalSupportMinSamples"]
            or analyzed_tap["nominalSupportMaxSamples"]
            != resolved_tap["nominalSupportMaxSamples"]
            or analyzed_tap["conservativeSupportMinSamples"]
            != resolved_tap["conservativeSupportMinSamples"]
            or analyzed_tap["conservativeSupportMaxSamples"]
            != resolved_tap["conservativeSupportMaxSamples"]
        ):
            raise AssertionError(
                f"analyzer did not report the resolved structural support "
                f"bounds: {analysis}"
            )
        first = analyzed_tap["measuredFirstNonZeroSample"]
        last = analyzed_tap["measuredLastNonZeroSample"]
        if first is None or last is None:
            raise AssertionError(f"tap measured no activity at all: {analysis}")
        if first > last:
            raise AssertionError(f"measured first sample after last: {analysis}")
        if not analyzed_tap["withinConservativeSupport"]:
            raise AssertionError(
                f"analyzer did not report measured support as within "
                f"conservative support: {analysis}"
            )
        if (
            first < resolved_tap["conservativeSupportMinSamples"]
            or last > resolved_tap["conservativeSupportMaxSamples"]
        ):
            raise AssertionError(
                f"measured support fell outside resolved conservative "
                f"support: {analysis}"
            )

    # Republishing over the same Render Result is idempotent, matching
    # every other analyzer's own immutable-artifact contract.
    republished = run_analyzer(analyzer, render_result)
    if republished.returncode != 0:
        raise AssertionError(republished.stderr)
    if json.loads(artifact.read_text()) != analysis:
        raise AssertionError("republishing changed the analysis artifact")

    # A Render Result with no Early Reflections branch has nothing to
    # analyze.
    no_early_document = json.loads(json.dumps(request_document))
    del no_early_document["composition"]["early"]
    no_early_request = workspace / "no-early-request.json"
    no_early_request.write_text(json.dumps(no_early_document))
    no_early_result = workspace / "no-early-render-result"
    no_early_rendered = run_renderer(
        renderer, fixture, no_early_request, no_early_result
    )
    if no_early_rendered.returncode != 0:
        raise AssertionError(no_early_rendered.stderr)
    no_early_analyzed = run_analyzer(analyzer, no_early_result)
    if no_early_analyzed.returncode == 0:
        raise AssertionError(
            "analyzer accepted a Render Result with no Early Reflections "
            "branch"
        )

    # Resolved Tap support bounds are offsets from a frame-0 impulse
    # entering the Diffuser; the analyzer must reject a source whose own
    # non-zero content is not exactly one frame-0 sample rather than
    # silently comparing a shifted/widened measured window against those
    # bounds (PR review on #112).
    shifted_impulse_fixture = workspace / "shifted-impulse.wav"
    write_pcm16_mono_wav(shifted_impulse_fixture, 48000, [0.0, 1.0, 0.0])
    shifted_result = workspace / "shifted-impulse-render-result"
    shifted_rendered = run_renderer(
        renderer, shifted_impulse_fixture, request, shifted_result
    )
    if shifted_rendered.returncode != 0:
        raise AssertionError(shifted_rendered.stderr)
    shifted_analyzed = run_analyzer(analyzer, shifted_result)
    if shifted_analyzed.returncode == 0:
        raise AssertionError(
            "analyzer accepted a source whose non-zero content was not a "
            "single frame-0 impulse"
        )


if __name__ == "__main__":
    main()
