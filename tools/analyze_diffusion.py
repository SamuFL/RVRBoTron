#!/usr/bin/env python3

import argparse
import json
import math
import os
import sys
import tempfile
from pathlib import Path

import numpy as np

import analyze_render


ANALYZER_NAME = "diffusion"
ANALYZER_VERSION = 1
ARTIFACT_NAME = f"{ANALYZER_NAME}-v{ANALYZER_VERSION}.json"

# Alignment score and Distinct-arrival density both classify a sample as
# "active" against one shared floor: -120 dB relative to the peak absolute
# sample of the capture being measured. See docs/design/reverb/README.md's
# Alignment score vocabulary entry and stage 02's Echo arithmetic section.
ACTIVITY_FLOOR_DB = -120.0

# 10 ms Distinct-arrival density bins (stage 03's "10 ms density curve").
DENSITY_BIN_MS = 10.0

# 1/12-octave Coloration curve range (stage 02/README's Coloration evidence).
TWELFTH_OCTAVE_START_HZ = 20.0

_TINY_POWER = np.finfo(np.float64).tiny


def verified_capture(render_result, metadata, capture, expected_frames):
    relative = Path(capture["path"])
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError("Stage capture path must remain inside Render Result")
    path = render_result / relative
    if analyze_render.file_sha256(path) != capture["sha256"]:
        raise ValueError(f"Stage capture SHA-256 mismatch: {relative}")
    wav = analyze_render.inspect_wav(path)
    expected = (
        capture["channels"],
        capture["sampleRate"],
        capture["frames"],
    )
    actual = (wav["channels"], wav["sampleRate"], wav["frameCount"])
    if actual != expected:
        raise ValueError(f"Stage capture manifest does not match {relative}")
    if (
        wav["sampleRate"] != metadata["sampleRate"]
        or wav["frameCount"] != expected_frames
        or capture["frames"] != expected_frames
    ):
        raise ValueError(
            f"Stage capture is not a complete finite response: {relative}"
        )
    return wav


def measured_energy(wav):
    metrics, non_finite = analyze_render.sample_metrics(
        analyze_render.decoded_samples(wav)
    )
    if non_finite:
        raise ValueError("Stage capture contains non-finite samples")
    return {
        "channelCount": wav["channels"],
        "sumOfSquares": metrics["sumOfSquares"],
    }


def orthogonality_evidence(step):
    matrix = step["matrix"]
    dimension = len(matrix)
    if dimension == 0 or any(len(row) != dimension for row in matrix):
        raise ValueError("resolved Diffusion Step matrix is not square")
    errors = []
    for row_a in range(dimension):
        for row_b in range(dimension):
            dot = math.fsum(
                matrix[row_a][column] * matrix[row_b][column]
                for column in range(dimension)
            )
            expected = 1.0 if row_a == row_b else 0.0
            errors.append(abs(dot - expected))
    maximum = max(errors)
    rms = math.sqrt(math.fsum(error * error for error in errors) / len(errors))
    return {
        "stepIndex": step["index"],
        "dimension": dimension,
        "maximumAbsoluteError": maximum,
        "rmsError": rms,
        "orthogonal": maximum <= 1e-12,
    }


def numpy_frames(wav):
    """Read a canonical IEEE float WAV directly into a (frames, channels)
    float64 array. Every Stage capture and output.wav this analyzer reads is
    always IEEE float32 or float64 -- Milestone 1's PCM formats are only
    ever used for source fixtures, never renderer output."""
    dtype = np.dtype("<f4") if wav["sampleBits"] == 32 else np.dtype("<f8")
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


def correlation_evidence(frames):
    """Signed normalized zero-lag dot product between every Channel pair:
    1.0 identical, -1.0 polarity-inverted, 0.0 linearly independent."""
    channels = frames.shape[1]
    norms = np.sqrt(np.sum(frames * frames, axis=0))
    zero_energy = norms <= 0.0
    safe_norms = np.where(zero_energy, 1.0, norms)
    gram = frames.T @ frames
    matrix = gram / safe_norms[:, None] / safe_norms[None, :]
    # A zero-energy Channel has no defined direction to correlate against;
    # report 0.0 (linearly independent) rather than a divide-by-zero NaN.
    matrix[zero_energy, :] = 0.0
    matrix[:, zero_energy] = 0.0
    np.fill_diagonal(matrix, np.where(zero_energy, 0.0, 1.0))
    off_diagonal = matrix[~np.eye(channels, dtype=bool)]
    return {
        "matrix": matrix.tolist(),
        "meanAbsoluteOffDiagonal": (
            float(np.mean(np.abs(off_diagonal))) if off_diagonal.size else 0.0
        ),
        "maxAbsoluteOffDiagonal": (
            float(np.max(np.abs(off_diagonal))) if off_diagonal.size else 0.0
        ),
    }


def activity_floor(frames):
    peak = float(np.max(np.abs(frames))) if frames.size else 0.0
    return peak, peak * (10.0 ** (ACTIVITY_FLOOR_DB / 20.0))


def alignment_evidence(active):
    """Pairwise Jaccard overlap of active-frame sets between every Channel
    pair, independent of amplitude sign."""
    channels = active.shape[1]
    pairwise = []
    scores = []
    for channel_a in range(channels):
        for channel_b in range(channel_a + 1, channels):
            union = int(
                np.count_nonzero(active[:, channel_a] | active[:, channel_b])
            )
            intersection = int(
                np.count_nonzero(active[:, channel_a] & active[:, channel_b])
            )
            # Two Channels with no activity at all trivially agree.
            jaccard = 1.0 if union == 0 else intersection / union
            pairwise.append(
                {"channelA": channel_a, "channelB": channel_b, "jaccard": jaccard}
            )
            scores.append(jaccard)
    return {
        "pairwise": pairwise,
        "minimum": min(scores) if scores else 1.0,
        "mean": (sum(scores) / len(scores)) if scores else 1.0,
    }


def density_evidence(active, sample_rate, echo_paths):
    """Theoretical Echo paths beside measured Distinct-arrival evidence: a
    per-Channel active-sample count and a 10 ms binned density curve of
    frames where any Channel is active."""
    frame_count, channels = active.shape
    any_active = np.any(active, axis=1)
    bin_frames = max(1, round(DENSITY_BIN_MS / 1000.0 * sample_rate))
    bin_count = (frame_count + bin_frames - 1) // bin_frames if frame_count else 0
    bins = [
        int(np.count_nonzero(any_active[start : start + bin_frames]))
        for start in range(0, bin_count * bin_frames, bin_frames)
    ]
    return {
        "echoPaths": echo_paths,
        "distinctArrivalCounts": [
            int(count) for count in np.count_nonzero(active, axis=0)
        ],
        "totalDistinctArrivals": int(np.count_nonzero(any_active)),
        "binMs": DENSITY_BIN_MS,
        "bins": bins,
    }


def _power_spectrum(frames):
    """Combined power spectrum (sum of |FFT|^2 over every Channel) of the
    complete drained response, unwindowed and zero-padded to the next
    power of two."""
    length = frames.shape[0]
    fft_length = 1
    while fft_length < length:
        fft_length *= 2
    fft_length = max(fft_length, 1)
    spectrum = np.fft.rfft(frames, n=fft_length, axis=0)
    power = np.sum(np.abs(spectrum) ** 2, axis=1)
    return power, fft_length


def _twelfth_octave_curve(power, frequencies, nyquist):
    curve = []
    band = 0
    while True:
        center = TWELFTH_OCTAVE_START_HZ * (2.0 ** (band / 12.0))
        if center > nyquist:
            break
        low = TWELFTH_OCTAVE_START_HZ * (2.0 ** ((band - 0.5) / 12.0))
        high = TWELFTH_OCTAVE_START_HZ * (2.0 ** ((band + 0.5) / 12.0))
        mask = (frequencies >= low) & (frequencies < high)
        if np.any(mask):
            band_power = float(np.sum(power[mask]))
            curve.append(
                {
                    "centerHz": center,
                    "energyDb": 10.0 * math.log10(max(band_power, _TINY_POWER)),
                }
            )
        band += 1
    return curve


def coloration_evidence(frames, sample_rate):
    power, fft_length = _power_spectrum(frames)
    safe_power = np.maximum(power, _TINY_POWER)
    db = 10.0 * np.log10(safe_power)
    deviation = db - np.mean(db)
    # safe_power is clamped at _TINY_POWER, so both means are always > 0.
    geometric_mean = float(np.exp(np.mean(np.log(safe_power))))
    arithmetic_mean = float(np.mean(safe_power))
    frequencies = np.fft.rfftfreq(fft_length, d=1.0 / sample_rate)
    nyquist = sample_rate / 2.0
    return {
        "fftLength": fft_length,
        "peakToPeakDb": float(np.max(db) - np.min(db)),
        "rmsDb": float(np.sqrt(np.mean(deviation**2))),
        "spectralFlatness": geometric_mean / arithmetic_mean,
        "twelfthOctaveCurve": _twelfth_octave_curve(power, frequencies, nyquist),
    }


def publish_artifact(render_result, contents):
    analysis_directory = render_result / "analysis"
    analysis_directory.mkdir(exist_ok=True)
    artifact = analysis_directory / ARTIFACT_NAME
    if artifact.exists():
        if artifact.read_bytes() == contents:
            return artifact
        raise ValueError(
            "analysis artifact already exists with different content"
        )

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


def analyze(render_result, source_path):
    metadata = json.loads((render_result / "render.json").read_text())
    resolved = json.loads((render_result / "resolved.json").read_text())
    source_hash = analyze_render.file_sha256(source_path)
    if source_hash != metadata["inputSha256"]:
        raise ValueError("source SHA-256 does not match render metadata")
    source = analyze_render.inspect_wav(source_path)
    if (
        source["sampleRate"] != metadata["sampleRate"]
        or source["frameCount"] != metadata["inputFrames"]
    ):
        raise ValueError("source audio facts do not match render metadata")
    output = analyze_render.inspect_wav(render_result / "output.wav")
    if (
        output["sampleRate"] != metadata["sampleRate"]
        or output["channels"] != metadata["channels"]
        or output["frameCount"] != metadata["frames"]
    ):
        raise ValueError("render metadata does not match output.wav")

    stages = resolved["composition"]["stages"]
    if [stage["type"] for stage in stages] != [
        "split",
        "diffuser",
        "downmix",
    ]:
        raise ValueError("diffusion analysis requires a finite Diffuser")
    split = stages[0]
    diffuser = stages[1]
    if source["channels"] != split["inputChannels"]:
        raise ValueError("source Channel count does not match resolved Split")
    total_samples = diffuser["totalSamples"]
    if not isinstance(total_samples, int) or total_samples < 0:
        raise ValueError("resolved Diffuser totalSamples is invalid")
    expected_frames = metadata["inputFrames"] + total_samples
    if metadata["frames"] != expected_frames:
        raise ValueError(
            "render metadata does not contain the complete finite response: "
            f"expected {expected_frames} frames"
        )
    if output["frameCount"] != expected_frames:
        raise ValueError(
            "output.wav does not contain the complete finite response: "
            f"expected {expected_frames} frames"
        )
    captures = metadata.get("stageCaptures")
    if metadata.get("stageCaptureProfile") != "all-v1" or not captures:
        raise ValueError("diffusion analysis requires --capture-stages all")

    split_capture = None
    diffusion_captures = {}
    for capture in captures:
        wav = verified_capture(
            render_result, metadata, capture, expected_frames
        )
        if capture["boundary"] == "split":
            if split_capture is not None:
                raise ValueError("Stage capture manifest has duplicate Split")
            split_capture = wav
        elif capture["boundary"] == "diffusion-step":
            if capture["index"] in diffusion_captures:
                raise ValueError(
                    "Stage capture manifest has duplicate Diffusion Step "
                    f"{capture['index']}"
                )
            diffusion_captures[capture["index"]] = wav
    if split_capture is None:
        raise ValueError("Stage capture manifest is missing Split")

    split_energy = measured_energy(split_capture)
    split_sum_of_squares = split_energy["sumOfSquares"]
    split_frames = numpy_frames(split_capture)
    correlation = [
        {"boundary": "split", **correlation_evidence(split_frames)}
    ]
    step_energy = []
    orthogonality = []
    last_step_index = max(step["index"] for step in diffuser["steps"])
    last_step_frames = None
    for step in diffuser["steps"]:
        index = step["index"]
        if index not in diffusion_captures:
            raise ValueError(
                f"Stage capture manifest is missing Diffusion Step {index}"
            )
        capture_wav = diffusion_captures[index]
        evidence = measured_energy(capture_wav)
        step_sum_of_squares = evidence["sumOfSquares"]
        evidence.update(
            {
                "index": index,
                "reference": "split",
                "ratio": (
                    step_sum_of_squares / split_sum_of_squares
                    if split_sum_of_squares
                    else 1.0
                ),
                "relativeError": (
                    abs(step_sum_of_squares - split_sum_of_squares)
                    / split_sum_of_squares
                    if split_sum_of_squares
                    else abs(step_sum_of_squares - split_sum_of_squares)
                ),
            }
        )
        step_energy.append(evidence)
        orthogonality.append(orthogonality_evidence(step))
        step_frames = numpy_frames(capture_wav)
        correlation.append(
            {
                "boundary": "diffusion-step",
                "index": index,
                **correlation_evidence(step_frames),
            }
        )
        if index == last_step_index:
            last_step_frames = step_frames

    capture_frames = {capture["frames"] for capture in captures}
    if len(capture_frames) != 1:
        raise ValueError("Stage captures do not share one complete timeline")

    # Alignment, Distinct-arrival density, and combined-Channel Coloration
    # are properties of the complete Diffuser output (the diffuser's own
    # stage document: "Diffuser output: aligned"), so they are measured on
    # the final cumulative Diffusion Step capture rather than every step.
    channels = split["channels"]
    echo_paths = channels ** len(diffuser["steps"])
    peak, floor = activity_floor(last_step_frames)
    active = np.abs(last_step_frames) > floor
    sample_rate = metadata["sampleRate"]
    # stepIndex/activityFloorDb identify which capture and which floor the
    # Alignment score and density evidence below share.
    capture_evidence = {
        "stepIndex": last_step_index,
        "activityFloorDb": ACTIVITY_FLOOR_DB,
    }
    alignment = {
        **capture_evidence,
        "peakAbsoluteSample": peak,
        "activityFloor": floor,
        **alignment_evidence(active),
    }
    density = {
        **capture_evidence,
        **density_evidence(active, sample_rate, echo_paths),
    }
    output_frames = numpy_frames(output)
    coloration = {
        "combined": {
            "stepIndex": last_step_index,
            **coloration_evidence(last_step_frames, sample_rate),
        },
        "stereo": coloration_evidence(output_frames, sample_rate),
    }

    analysis = {
        "formatVersion": 1,
        "analyzer": ANALYZER_NAME,
        "analyzerVersion": ANALYZER_VERSION,
        "source": {
            "filename": metadata["inputFilename"],
            "sha256": source_hash,
            "verified": True,
        },
        "completeResponse": {
            "inputFrames": metadata["inputFrames"],
            "resolvedDiffuserTotalSamples": total_samples,
            "expectedFrames": expected_frames,
            "outputFrames": metadata["frames"],
            "tailFrames": metadata["frames"] - metadata["inputFrames"],
            "stageCaptureFrames": capture_frames.pop(),
        },
        "energy": {
            "split": split_energy,
            "diffusionSteps": step_energy,
        },
        "orthogonality": orthogonality,
        "correlation": correlation,
        "alignment": alignment,
        "density": density,
        "coloration": coloration,
    }
    return analysis


def compare_render_results(render_result, other_render_result):
    """Optional pairwise comparison: two Render Results of the same
    Resolved Configuration and source, rendered at different block sizes,
    must decode to exactly equal output.wav and Stage captures. Does not
    publish an artifact -- this is a diagnostic, not versioned evidence."""
    metadata = json.loads((render_result / "render.json").read_text())
    other_metadata = json.loads((other_render_result / "render.json").read_text())
    resolved = (render_result / "resolved.json").read_bytes()
    other_resolved = (other_render_result / "resolved.json").read_bytes()
    if resolved != other_resolved:
        raise ValueError(
            "Render Results do not share the same Resolved Configuration"
        )
    if (
        metadata["inputSha256"] != other_metadata["inputSha256"]
        or metadata["sampleRate"] != other_metadata["sampleRate"]
        or metadata["samplePrecision"] != other_metadata["samplePrecision"]
    ):
        raise ValueError("Render Results do not share matching provenance")
    if metadata["blockSize"] == other_metadata["blockSize"]:
        raise ValueError(
            "pairwise comparison requires Render Results rendered at "
            "different block sizes"
        )

    captures = {
        capture["path"]: capture
        for capture in metadata.get("stageCaptures") or []
    }
    other_captures = {
        capture["path"]: capture
        for capture in other_metadata.get("stageCaptures") or []
    }
    if set(captures) != set(other_captures):
        raise ValueError("Render Results do not share the same Stage captures")

    comparisons = []
    for relative in ["output.wav"] + sorted(captures):
        wav = analyze_render.inspect_wav(render_result / relative)
        other_wav = analyze_render.inspect_wav(other_render_result / relative)
        comparisons.append(
            {
                "path": relative,
                **analyze_render.identity_analysis(wav, other_wav, "float64"),
            }
        )

    return {
        "equal": all(comparison["equal"] for comparison in comparisons),
        "blockSizes": [metadata["blockSize"], other_metadata["blockSize"]],
        "comparisons": comparisons,
    }


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Analyze finite diffusion evidence in a Render Result."
    )
    parser.add_argument("render_result", type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument(
        "--compare",
        type=Path,
        metavar="OTHER_RENDER_RESULT",
        help=(
            "compare this Render Result's output.wav and Stage captures "
            "against another Render Result of the same Resolved "
            "Configuration for exact decoded equality, instead of "
            "publishing analysis/diffusion-v1.json"
        ),
    )
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    try:
        if arguments.compare is not None:
            comparison = compare_render_results(
                arguments.render_result, arguments.compare
            )
            print(
                json.dumps(comparison, allow_nan=False, indent=2, sort_keys=True)
            )
            return 0 if comparison["equal"] else 1
        if arguments.source is None:
            raise ValueError("--source is required unless --compare is given")
        analysis = analyze(arguments.render_result, arguments.source)
        contents = (
            json.dumps(analysis, allow_nan=False, indent=2, sort_keys=True)
            + "\n"
        ).encode()
        artifact = publish_artifact(arguments.render_result, contents)
        print(
            f'{analysis["completeResponse"]["outputFrames"]} frames, '
            f'{len(analysis["energy"]["diffusionSteps"])} Diffusion Step, '
            f'{analysis["orthogonality"][0]["maximumAbsoluteError"]:.3g} '
            "maximum orthogonality error, "
            f'{analysis["density"]["echoPaths"]} Echo paths, '
            f'{analysis["density"]["totalDistinctArrivals"]} Distinct '
            "arrivals"
        )
        print(artifact)
        return 0
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"diffusion analysis failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
