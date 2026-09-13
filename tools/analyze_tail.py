#!/usr/bin/env python3

import argparse
import json
import math
import os
import sys
import tempfile
from pathlib import Path

import numpy as np

import analyze_diffusion
import analyze_render


ANALYZER_NAME = "tail"
ANALYZER_VERSION = 1
ARTIFACT_NAME = f"{ANALYZER_NAME}-v{ANALYZER_VERSION}.json"

# Standard octave-band centers, 63 Hz to 16 kHz (stage 04/05's per-band RT60
# requirement). Edges sit at center / sqrt(2) .. center * sqrt(2).
OCTAVE_BAND_CENTERS_HZ = [63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0]

# CONTEXT.md's Reference band: the 1 kHz octave, where RT60 is defined and
# against which the requested rt60Sec is compared.
REFERENCE_BAND_HZ = 1000.0

# Stage 04's Decay accuracy invariant: measured RT60 within +/-5% of
# rt60Sec, damping disabled (no damping stage exists in this milestone, so
# every render measured here is implicitly undamped).
RT60_ACCURACY_INVARIANT = 0.05

# T20 is fit over the -5 to -25 dB range of the Schroeder decay curve; T30
# over -5 to -35 dB. Both extrapolate their fitted slope out to -60 dB.
T20_RANGE_DB = (-5.0, -25.0)
T30_RANGE_DB = (-5.0, -35.0)
EXTRAPOLATE_TO_DB = -60.0

# A regression needs enough points to mean anything rather than fitting
# noise between two adjacent samples.
MINIMUM_FIT_SAMPLES = 8

# Coarse segments spanning the post-input-EOF tail, for the decay-envelope
# monotonicity check: wide enough that natural beating between the Feedback
# Loop's circulating Channels averages out within one segment, instead of
# masking a genuine amplitude buildup (see the open design question in
# issue #63 on delayMinMs/Diffuser-totalMs overlap) the way the
# Schroeder-integrated curve above would -- that curve is non-increasing by
# construction regardless of what the raw output does, so it cannot serve
# as this check.
DECAY_ENVELOPE_SEGMENTS = 16
# Tolerance against floating-point and filter-quantization jitter between
# segments, not against a genuine trend reversal.
DECAY_ENVELOPE_GROWTH_TOLERANCE = 0.02

_TINY_POWER = np.finfo(np.float64).tiny


def octave_band_edges(center_hz):
    return center_hz / math.sqrt(2.0), center_hz * math.sqrt(2.0)


def octave_band_gain(frequencies, low_hz, high_hz):
    """A single raised-cosine hump spanning the octave in log-frequency: 0 at
    both edges, 1 at the center. A brick-wall bin mask corresponds to a sinc
    kernel in time (1/n sidelobe decay) that leaks across an entire render;
    this shape's much faster sidelobe rolloff keeps the filter localized.
    Public (unlike this module's other band-filtering internals) because
    analyze_downmix.py's own octave-band deviation evidence reuses this
    exact gain shape rather than a second, drifting re-derivation."""
    gain = np.zeros_like(frequencies)
    within = (frequencies > low_hz) & (frequencies < high_hz)
    log_edges = math.log2(low_hz), math.log2(high_hz)
    log_frequency = np.log2(np.clip(frequencies[within], low_hz, high_hz))
    normalized = (log_frequency - log_edges[0]) / (log_edges[1] - log_edges[0])
    gain[within] = 0.5 * (1.0 - np.cos(2.0 * np.pi * normalized))
    return gain


def _band_energy(spectrum, frequencies, fft_length, length, low_hz, high_hz):
    """Apply one octave band's raised-cosine gain to an already-computed
    rFFT spectrum (the same approach analyze_diffusion.py's twelfth-octave
    curve uses for a power sum, applied here as a time-domain filter) and
    inverse-transform, returning the per-sample energy summed across
    Channels. The forward FFT is computed once per render by the caller and
    reused across every band -- it dominates the cost of this filter, and
    every band only differs in which bins its gain shape keeps."""
    gain = octave_band_gain(frequencies, low_hz, high_hz)
    filtered = np.fft.irfft(spectrum * gain[:, None], n=fft_length, axis=0)[
        :length
    ]
    return np.sum(filtered * filtered, axis=1)


def schroeder_decay_curve(energy):
    """Schroeder backward integration of a per-sample energy sequence:
    E(n) = sum_{k=n}^{N-1} energy[k], expressed in dB relative to E(0) (the
    total energy). Non-increasing in n by construction -- each step removes
    one non-negative term -- regardless of what the underlying energy
    sequence does, which is why decay_envelope_evidence below checks the raw
    envelope separately rather than reusing this curve's own monotonicity."""
    total = float(np.sum(energy))
    if not (total > 0.0):
        return None
    cumulative = np.cumsum(energy[::-1])[::-1]
    return 10.0 * np.log10(np.maximum(cumulative, _TINY_POWER) / total)


def fit_decay_time(decay_db, sample_rate, start_db, end_db):
    """Linear regression of the Schroeder decay curve between the first
    sample at or below start_db and the first at or below end_db,
    extrapolated to EXTRAPOLATE_TO_DB. Returns None when the curve never
    reaches end_db (too short a capture or too slow a decay) or the fitted
    segment does not actually decay."""
    at_start = np.flatnonzero(decay_db <= start_db)
    if at_start.size == 0:
        return None
    start_index = int(at_start[0])
    at_end = np.flatnonzero(decay_db[start_index:] <= end_db)
    if at_end.size == 0:
        return None
    end_index = start_index + int(at_end[0])
    if end_index - start_index + 1 < MINIMUM_FIT_SAMPLES:
        return None

    times = np.arange(start_index, end_index + 1) / sample_rate
    segment = decay_db[start_index : end_index + 1]
    slope, intercept = np.polyfit(times, segment, 1)
    if not (slope < 0.0):
        return None
    return {
        "startDb": start_db,
        "endDb": end_db,
        "slopeDbPerSec": float(slope),
        "interceptDb": float(intercept),
        "sampleCount": int(end_index - start_index + 1),
        "rt60Sec": float(EXTRAPOLATE_TO_DB / slope),
    }


def band_evidence(center_hz, low_hz, high_hz, energy, sample_rate):
    decay_db = schroeder_decay_curve(energy)
    if decay_db is None:
        return {
            "centerHz": center_hz,
            "lowHz": low_hz,
            "highHz": high_hz,
            "t20": None,
            "t30": None,
        }
    return {
        "centerHz": center_hz,
        "lowHz": low_hz,
        "highHz": high_hz,
        "t20": fit_decay_time(decay_db, sample_rate, *T20_RANGE_DB),
        "t30": fit_decay_time(decay_db, sample_rate, *T30_RANGE_DB),
    }


def decay_envelope_evidence(frames, sample_rate, tail_start_frame):
    """Coarse-windowed raw (non-Schroeder-integrated) broadband energy from
    input EOF onward, verified non-increasing beyond DECAY_ENVELOPE_
    GROWTH_TOLERANCE: real evidence about the render's own output, unlike a
    Schroeder curve's structural monotonicity."""
    tail = frames[tail_start_frame:]
    segment_frames = tail.shape[0] // DECAY_ENVELOPE_SEGMENTS
    if segment_frames < 1:
        return {"segmentCount": 0, "segmentEnergies": [], "monotonic": None}
    usable = segment_frames * DECAY_ENVELOPE_SEGMENTS
    energy = np.sum(tail[:usable] * tail[:usable], axis=1)
    segments = energy.reshape(DECAY_ENVELOPE_SEGMENTS, segment_frames).sum(axis=1)
    # Below this floor the tail has already decayed past any audible or
    # measurable relevance, so residual jitter there does not count against
    # monotonicity.
    floor = float(np.max(segments)) * 1e-9
    monotonic = True
    for previous, current in zip(segments[:-1], segments[1:]):
        if previous < floor:
            continue
        if current > previous * (1.0 + DECAY_ENVELOPE_GROWTH_TOLERANCE):
            monotonic = False
            break
    return {
        "segmentCount": DECAY_ENVELOPE_SEGMENTS,
        "segmentEnergies": [float(value) for value in segments],
        "monotonic": monotonic,
    }


def decay_evidence(frames, sample_rate, requested_rt60_sec):
    nyquist = sample_rate / 2.0
    length = frames.shape[0]
    # Zero-padded to the next power of two beyond double the frame count so
    # every band's filter is a linear rather than a circular convolution --
    # otherwise the filter's own smearing wraps around a finite-length
    # render and inflates the measured decay. Computed once and reused
    # across every band below, rather than once per band.
    fft_length = 1
    while fft_length < 2 * length:
        fft_length *= 2
    spectrum = np.fft.rfft(frames, n=fft_length, axis=0)
    frequencies = np.fft.rfftfreq(fft_length, d=1.0 / sample_rate)

    bands = []
    for center in OCTAVE_BAND_CENTERS_HZ:
        low, high = octave_band_edges(center)
        if low >= nyquist:
            continue
        high = min(high, nyquist)
        energy = _band_energy(spectrum, frequencies, fft_length, length, low, high)
        bands.append(band_evidence(center, low, high, energy, sample_rate))

    reference = next(
        (band for band in bands if band["centerHz"] == REFERENCE_BAND_HZ), None
    )
    measured_rt60_sec = (
        reference["t30"]["rt60Sec"] if reference and reference["t30"] else None
    )
    relative_error = (
        abs(measured_rt60_sec - requested_rt60_sec) / requested_rt60_sec
        if measured_rt60_sec is not None
        else None
    )
    return {
        "referenceBandHz": REFERENCE_BAND_HZ,
        "requestedRt60Sec": requested_rt60_sec,
        "measuredRt60Sec": measured_rt60_sec,
        "relativeError": relative_error,
        "withinAccuracyInvariant": (
            relative_error is not None and relative_error <= RT60_ACCURACY_INVARIANT
        ),
        "bands": bands,
    }


def stereo_frames_reporting_non_finite(wav):
    """Decode a canonical IEEE float stereo WAV to a (frames, 2) float64
    array, reporting non-finite samples rather than rejecting them (they are
    evidence to publish, per this analyzer's own acceptance criteria) --
    unlike analyze_diffusion.numpy_frames, which hard-fails because the
    all-pass Diffuser tolerates none."""
    if wav["formatTag"] != 3 or wav["sampleBits"] not in (32, 64):
        raise ValueError(f'{wav["path"]} is not a canonical IEEE float WAV')
    dtype = np.dtype("<f4") if wav["sampleBits"] == 32 else np.dtype("<f8")
    with wav["path"].open("rb") as source:
        source.seek(wav["dataOffset"])
        raw = source.read(wav["dataSize"])
    flat = np.frombuffer(raw, dtype=dtype).astype(np.float64)
    expected = wav["frameCount"] * wav["channels"]
    if flat.size != expected:
        raise ValueError(f'{wav["path"]} does not contain {expected} samples')
    finite = np.isfinite(flat)
    non_finite_count = int(flat.size - np.count_nonzero(finite))
    if non_finite_count:
        flat = np.where(finite, flat, 0.0)
    frames = flat.reshape(wav["frameCount"], wav["channels"])
    return frames, non_finite_count


def locate_feedback_loop(resolved):
    stages = resolved["composition"]["stages"]
    types = [stage["type"] for stage in stages]
    if types not in (
        ["split", "feedback-loop", "downmix"],
        ["split", "diffuser", "feedback-loop", "downmix"],
    ):
        raise ValueError("tail analysis requires a Feedback Loop")
    return next(stage for stage in stages if stage["type"] == "feedback-loop")


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

    loop = locate_feedback_loop(resolved)

    if metadata["channels"] != 2:
        raise ValueError("tail analysis requires canonical stereo output")
    output = analyze_render.inspect_wav(render_result / "output.wav")
    if (
        output["sampleRate"] != metadata["sampleRate"]
        or output["channels"] != metadata["channels"]
    ):
        raise ValueError("render metadata does not match output.wav")

    # output.wav must match what render.json itself claims regardless of
    # silenceFloorDb -- a distinct failure from whether that claimed frame
    # count actually respects the Tail budget, checked separately below so
    # each failure names its own accurate cause.
    if output["frameCount"] != metadata["frames"]:
        raise ValueError(
            "render metadata does not match output.wav: render.json claims "
            f'{metadata["frames"]} frames, output.wav has '
            f'{output["frameCount"]}'
        )

    # Total drain is inputFrames + preDelayFrames + tailBudgetFrames
    # (stage 09's automatic drain, which already sums a Diffuser's finite
    # response with the Feedback Loop's Tail budget when both are
    # present, plus Pre-delay's own additional drain, issue #133), so
    # this analyzer never needs to inspect a Diffuser stage itself.
    # preDelayFrames defaults to 0 for a render.json predating Pre-delay.
    expected_frames = (
        metadata["inputFrames"]
        + metadata.get("preDelayFrames", 0)
        + metadata["tailBudgetFrames"]
    )
    silence_floor_enabled = loop.get("silenceFloorDb") is not None
    if silence_floor_enabled:
        # Dormant in this milestone (docs/design/reverb/stages/
        # 04-feedback-loop.md): nothing yet drains early, but the check is
        # keyed off the Resolved Configuration so it needs no change once a
        # later milestone enables early termination.
        if metadata["frames"] > expected_frames:
            raise ValueError(
                "render metadata exceeds the Tail budget upper bound: "
                f'{metadata["frames"]} frames > {expected_frames}'
            )
        frame_count_check = "upper-bound"
    else:
        if metadata["frames"] != expected_frames:
            raise ValueError(
                "render metadata does not contain the complete Tail budget "
                f"response: expected exactly {expected_frames} frames, got "
                f'{metadata["frames"]}'
            )
        frame_count_check = "exact"

    frames, non_finite_count = stereo_frames_reporting_non_finite(output)
    sample_rate = metadata["sampleRate"]

    decay = decay_evidence(frames, sample_rate, loop["rt60Sec"])
    # The decay envelope's own "post-input" window must start once real
    # source material has finished arriving at the wet path, not at
    # inputFrames itself: Pre-delay (issue #133) keeps delayed source
    # samples entering Split until inputFrames + preDelayFrames, and a
    # window starting earlier would span still-arriving signal and
    # already-decaying tail together, which is not the render's own
    # decay evidence this check exists to verify.
    decay_envelope = decay_envelope_evidence(
        frames,
        sample_rate,
        metadata["inputFrames"] + metadata.get("preDelayFrames", 0),
    )

    peak, floor = analyze_diffusion.activity_floor(frames)
    active = np.abs(frames) > floor
    # analyze_diffusion.alignment_evidence's "channelA"/"channelB" fields
    # assume internal-Channel semantics; here the two positions are the
    # post-Downmix stereo output, which CONTEXT.md's Channel entry
    # explicitly distinguishes ("Avoid: Speaker, output channel"), so only
    # the numeric score is kept, not that pairwise labeling.
    alignment = {
        "activityFloorDb": analyze_diffusion.ACTIVITY_FLOOR_DB,
        "peakAbsoluteSample": peak,
        "activityFloor": floor,
        "score": analyze_diffusion.alignment_evidence(active)["mean"],
    }
    coloration = analyze_diffusion.coloration_evidence(frames, sample_rate)

    return {
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
            "tailBudgetFrames": metadata["tailBudgetFrames"],
            "expectedFrames": expected_frames,
            "outputFrames": output["frameCount"],
            "frameCountCheck": frame_count_check,
            "silenceFloorEnabled": silence_floor_enabled,
        },
        "nonFiniteSampleCount": non_finite_count,
        "decay": decay,
        "decayEnvelope": decay_envelope,
        "alignment": alignment,
        "coloration": coloration,
    }


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Analyze sustained-tail evidence in a Render Result."
    )
    parser.add_argument("render_result", type=Path)
    parser.add_argument("--source", type=Path, required=True)
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    try:
        analysis = analyze(arguments.render_result, arguments.source)
        contents = (
            json.dumps(analysis, allow_nan=False, indent=2, sort_keys=True)
            + "\n"
        ).encode()
        artifact = publish_artifact(arguments.render_result, contents)
        decay = analysis["decay"]
        measured = decay["measuredRt60Sec"]
        measured_word = f"{measured:.3f}s" if measured is not None else "n/a"
        error_word = (
            f"{decay['relativeError'] * 100:.1f}%"
            if decay["relativeError"] is not None
            else "n/a"
        )
        print(
            f'{analysis["completeResponse"]["outputFrames"]} frames, '
            f"RT60 {measured_word} at {decay['referenceBandHz']:.0f} Hz "
            f"(requested {decay['requestedRt60Sec']:.3f}s, {error_word} "
            "error), "
            f'Alignment score {analysis["alignment"]["score"]:.3f}, '
            f'decay envelope monotonic {analysis["decayEnvelope"]["monotonic"]}, '
            f'{analysis["nonFiniteSampleCount"]} non-finite samples'
        )
        print(artifact)
        return 0
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"tail analysis failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
