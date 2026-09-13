#!/usr/bin/env python3

import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

import numpy as np

import analyze_render

ANALYZER_NAME = "early-support"
ANALYZER_VERSION = 1
ARTIFACT_NAME = f"{ANALYZER_NAME}-v{ANALYZER_VERSION}.json"

# The same shared floor Diffusion analysis uses to classify a sample as
# "active" (see tools/analyze_diffusion.py's own ACTIVITY_FLOOR_DB and
# docs/design/reverb/README.md's Alignment score vocabulary entry): -120 dB
# relative to the peak absolute sample of the capture being measured.
ACTIVITY_FLOOR_DB = -120.0


def numpy_frames(wav):
    """Read a canonical IEEE float WAV directly into a (frames, channels)
    float64 array -- every Diffusion Step Stage capture this analyzer
    reads is always IEEE float32 or float64 (see tools/analyze_diffusion.py's
    own numpy_frames, reimplemented here rather than imported so this
    analyzer stays a self-contained script, matching that module's own
    convention)."""
    if wav["formatTag"] != 3 or wav["sampleBits"] not in (32, 64):
        raise ValueError(f'{wav["path"]} is not a canonical IEEE float WAV')
    dtype = np.dtype("<f4") if wav["sampleBits"] == 32 else np.dtype("<f8")
    with wav["path"].open("rb") as source:
        source.seek(wav["dataOffset"])
        raw = source.read(wav["dataSize"])
    flat = np.frombuffer(raw, dtype=dtype)
    expected = wav["frameCount"] * wav["channels"]
    if flat.size != expected:
        raise ValueError(f'{wav["path"]} does not contain {expected} samples')
    frames = flat.reshape(wav["frameCount"], wav["channels"]).astype(np.float64)
    if not np.all(np.isfinite(frames)):
        raise ValueError(f'{wav["path"]} contains non-finite samples')
    return frames


def verified_capture(render_result, capture, metadata):
    relative = Path(capture["path"])
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError("Stage capture path must remain inside Render Result")
    path = render_result / relative
    if analyze_render.file_sha256(path) != capture["sha256"]:
        raise ValueError(f"Stage capture SHA-256 mismatch: {relative}")
    wav = analyze_render.inspect_wav(path)
    expected = (capture["channels"], capture["sampleRate"], capture["frames"])
    actual = (wav["channels"], wav["sampleRate"], wav["frameCount"])
    if actual != expected:
        raise ValueError(f"Stage capture manifest does not match {relative}")
    if wav["sampleRate"] != metadata["sampleRate"]:
        raise ValueError(
            f"Stage capture sample rate does not match render metadata: "
            f"{relative}"
        )
    return wav


def verify_frame_zero_impulse(render_result, metadata):
    """Resolved Tap support bounds are offsets from the instant the
    source signal enters the Diffuser -- currently frame 0 of Split's own
    capture, since no pre-delay exists ahead of Split yet. That equates a
    tap's own measured non-zero window with its Diffusion Step's own
    impulse response only when the source itself is a single-sample,
    frame-0 impulse; for ordinary (or differently-timed) source audio the
    measured window is shifted and widened by the source's own extent,
    and naively comparing it against the resolved bounds would report a
    correct render as falling outside its own conservative support (PR
    review on #112). Mirrors tools/analyze_tail.py's own requirement that
    Schroeder backward integration be run against a deterministic impulse
    render rather than a musical sample."""
    split_captures = [
        capture
        for capture in metadata.get("stageCaptures") or []
        if capture["boundary"] == "split"
    ]
    if len(split_captures) != 1:
        raise ValueError(
            "render with --capture-stages all to verify a frame-0 impulse "
            "source"
        )
    frames = numpy_frames(verified_capture(render_result, split_captures[0], metadata))
    nonzero_indices = np.flatnonzero(np.any(frames != 0.0, axis=1))
    if list(nonzero_indices) != [0]:
        raise ValueError(
            "this analyzer requires a source with exactly one non-zero "
            "frame, at frame 0 (a deterministic single-sample impulse "
            "render) -- resolved Tap support bounds are offsets from that "
            "instant"
        )


def measured_support(frames):
    """First/last frame index where any Channel exceeds the shared
    activity floor, plus that peak's own frame index and an energy-
    weighted time centroid (docs/design/reverb/stages/
    07-early-reflections.md's "Tap support": "Analysis records measured
    first and last non-zero samples, plus peak and centroid when useful.
    Cancellation may make measured support narrower than its structural
    bound.").

    measuredFirstNonZeroSample/measuredLastNonZeroSample use exact
    non-zero detection (`> 0.0`), not the -120 dB activity floor
    tools/analyze_diffusion.py's own Alignment evidence uses for a
    different purpose (classifying "active" content against measurement
    noise across many Channels): the spec's own word is "non-zero", and a
    floor would silently narrow that claim and could hide real energy
    that falls outside the resolved conservative bound (PR review on
    #112). activityFloor/peakAbsoluteSample are still reported alongside,
    as separate floor-based evidence for anyone who wants it.

    All fields are None when every sample is exactly zero."""
    absolute = np.abs(frames)
    peak = float(np.max(absolute)) if absolute.size else 0.0
    floor = peak * (10.0 ** (ACTIVITY_FLOOR_DB / 20.0))
    nonzero_indices = np.flatnonzero(np.any(absolute > 0.0, axis=1))
    if nonzero_indices.size == 0:
        return {
            "peakAbsoluteSample": peak,
            "activityFloor": floor,
            "measuredFirstNonZeroSample": None,
            "measuredLastNonZeroSample": None,
            "measuredPeakSample": None,
            "measuredCentroidSample": None,
        }
    energy_per_frame = np.sum(frames * frames, axis=1)
    total_energy = float(np.sum(energy_per_frame))
    centroid = (
        float(
            np.sum(np.arange(frames.shape[0]) * energy_per_frame) / total_energy
        )
        if total_energy > 0.0
        else None
    )
    return {
        "peakAbsoluteSample": peak,
        "activityFloor": floor,
        "measuredFirstNonZeroSample": int(nonzero_indices[0]),
        "measuredLastNonZeroSample": int(nonzero_indices[-1]),
        "measuredPeakSample": int(np.argmax(np.max(absolute, axis=1))),
        "measuredCentroidSample": centroid,
    }


def publish_artifact(render_result, contents):
    analysis_directory = render_result / "analysis"
    analysis_directory.mkdir(exist_ok=True)
    artifact = analysis_directory / ARTIFACT_NAME
    if artifact.exists():
        if artifact.read_bytes() == contents:
            return artifact
        raise ValueError("analysis artifact already exists with different content")

    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{ARTIFACT_NAME}.tmp-",
        dir=analysis_directory,
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(contents)
            output.flush()
            os.fsync(output.fileno())
        try:
            if os.name == "nt":
                os.rename(temporary, artifact)
            else:
                os.link(temporary, artifact)
        except FileExistsError:
            if artifact.read_bytes() != contents:
                raise ValueError(
                    "analysis artifact already exists with different content"
                )
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
    return artifact


def analyze(render_result):
    metadata = json.loads((render_result / "render.json").read_text())
    resolved = json.loads((render_result / "resolved.json").read_text())
    early = resolved["composition"].get("early")
    if not early:
        raise ValueError("Resolved Configuration has no Early Reflections branch")
    verify_frame_zero_impulse(render_result, metadata)

    captures_by_step = {
        capture["index"]: capture
        for capture in metadata.get("stageCaptures") or []
        if capture["boundary"] == "diffusion-step"
    }

    taps = []
    for tap in early["taps"]:
        step_index = tap["stepIndex"]
        capture = captures_by_step.get(step_index)
        if capture is None:
            raise ValueError(
                f"no captured Diffusion Step for tapped step {step_index} -- "
                f"render with --capture-stages all"
            )
        wav = verified_capture(render_result, capture, metadata)
        support = measured_support(numpy_frames(wav))
        within_conservative = (
            support["measuredFirstNonZeroSample"] is not None
            and support["measuredFirstNonZeroSample"]
            >= tap["conservativeSupportMinSamples"]
            and support["measuredLastNonZeroSample"]
            <= tap["conservativeSupportMaxSamples"]
        )
        taps.append(
            {
                "stepIndex": step_index,
                "capturePath": capture["path"],
                "nominalSupportMinSamples": tap["nominalSupportMinSamples"],
                "nominalSupportMaxSamples": tap["nominalSupportMaxSamples"],
                "conservativeSupportMinSamples": tap[
                    "conservativeSupportMinSamples"
                ],
                "conservativeSupportMaxSamples": tap[
                    "conservativeSupportMaxSamples"
                ],
                "withinConservativeSupport": within_conservative,
                **support,
            }
        )

    return {
        "formatVersion": 1,
        "analyzer": ANALYZER_NAME,
        "analyzerVersion": ANALYZER_VERSION,
        "activityFloorDb": ACTIVITY_FLOOR_DB,
        "taps": taps,
    }


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Report each Early Reflections tap's measured first/last "
            "non-zero sample, peak, and centroid against its resolved "
            "nominal/conservative Tap support (issue #112)."
        )
    )
    parser.add_argument("render_result", type=Path)
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    try:
        analysis = analyze(arguments.render_result)
        contents = (
            json.dumps(analysis, allow_nan=False, indent=2, sort_keys=True) + "\n"
        ).encode()
        artifact = publish_artifact(arguments.render_result, contents)
        for tap in analysis["taps"]:
            print(
                f'stepIndex {tap["stepIndex"]}: measured '
                f'[{tap["measuredFirstNonZeroSample"]}, '
                f'{tap["measuredLastNonZeroSample"]}], conservative '
                f'[{tap["conservativeSupportMinSamples"]}, '
                f'{tap["conservativeSupportMaxSamples"]}], within: '
                f'{tap["withinConservativeSupport"]}'
            )
        print(artifact)
        return 0
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"early support analysis failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
