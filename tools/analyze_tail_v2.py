#!/usr/bin/env python3

"""Tail analysis version 2: Damping-aware octave-band decay evidence for a
Render Result, alongside the unmodified tail-v1 artifact (see issue #78).

Reuses tail-v1's provenance checks, octave-band FFT extraction, Schroeder
integration, and T20/T30 linear-regression fit (analyze_tail.py) and its
Alignment/Coloration evidence (analyze_diffusion.py) unchanged -- this
analyzer adds a per-Channel, per-band *predicted* RT60 derived from each
Channel's resolved delay, gain, and Two-shelf response (reusing the
already-resolved shelf coefficients from resolved.json, not re-deriving
them from ratios/corners), and replaces tail-v1's literal envelope
monotonicity with eventual-contraction evidence: a negative late-tail
log-energy trend that tolerates local modal beating.

Publishes analysis/tail-v2.json beside, not instead of, analysis/tail-v1.json
(both analyzers can run against the same Render Result; neither one's
artifact depends on the other having run). See ADR-0004
(docs/adr/0004-validate-structure-not-acoustics.md) for why a Reference-band
deviation is reported here, not rejected: a gentle one-pole shelf's wide
transition band means even the documented research-baseline Damping default
lands a few percent outside the plain +/-5% comparison against rt60Sec, a
verified property of the filter rather than a defect. A deviation past
SIGNIFICANT_DEVIATION_THRESHOLD is flagged as significant; nothing here
rejects a render for missing that target.
"""

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
import analyze_tail

ANALYZER_NAME = "tail"
ANALYZER_VERSION = 2
ARTIFACT_NAME = f"{ANALYZER_NAME}-v{ANALYZER_VERSION}.json"

# Beyond tail-v1's own +/-5% RT60_ACCURACY_INVARIANT, a Damping-enabled
# render's Reference-band response can legitimately land further from
# rt60Sec (ADR-0004); a deviation past this threshold is flagged as
# significant in the artifact but never rejects the analysis.
SIGNIFICANT_DEVIATION_THRESHOLD = 0.10

# Stage 05's Ratio/Reference-band accuracy invariants: predicted-vs-measured
# T30 agreement, and the tolerance added on top of a uniform-gain-mode
# predicted range's own endpoints.
PREDICTED_TOLERANCE = 0.10

# Coarse windows spanning the *late* half of the post-input-EOF tail (see
# eventual_contraction_evidence): wide enough that modal beating between
# Channels averages out within one window, short enough that a genuine
# non-decaying or growing tail still shows as a positive regression slope.
EVENTUAL_CONTRACTION_SEGMENTS = 16
EVENTUAL_CONTRACTION_LATE_FRACTION = 0.5

_TINY_POWER = np.finfo(np.float64).tiny


def is_finite_number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    try:
        return math.isfinite(value)
    except OverflowError:
        return False


def _octave_band_gain(frequencies, low_hz, high_hz):
    """Duplicated from analyze_tail.py's own private helper rather than
    imported: that leading underscore marks it internal to that module, so
    reaching into it from here would be relying on another module's
    implementation detail rather than its public surface (which this file
    otherwise imports freely -- octave_band_edges, band_evidence,
    locate_feedback_loop, stereo_frames_reporting_non_finite, and the
    OCTAVE_BAND_CENTERS_HZ/REFERENCE_BAND_HZ/RT60_ACCURACY_INVARIANT
    constants are all public and reused directly below). Same shape: a
    single raised-cosine hump spanning the octave in log-frequency, 0 at
    both edges and 1 at the center, chosen over a brick-wall bin mask for
    the same reason (see analyze_tail.py's own comment: a brick wall's sinc
    kernel leaks across the entire render in the time domain)."""
    gain = np.zeros_like(frequencies)
    within = (frequencies > low_hz) & (frequencies < high_hz)
    log_edges = math.log2(low_hz), math.log2(high_hz)
    log_frequency = np.log2(np.clip(frequencies[within], low_hz, high_hz))
    normalized = (log_frequency - log_edges[0]) / (log_edges[1] - log_edges[0])
    gain[within] = 0.5 * (1.0 - np.cos(2.0 * np.pi * normalized))
    return gain


def _band_energy(spectrum, frequencies, fft_length, length, low_hz, high_hz):
    """Duplicated alongside _octave_band_gain above, for the same reason."""
    gain = _octave_band_gain(frequencies, low_hz, high_hz)
    filtered = np.fft.irfft(spectrum * gain[:, None], n=fft_length, axis=0)[:length]
    return np.sum(filtered * filtered, axis=1)


def shelf_magnitude_at_frequency(b0, b1, a1, frequency_hz, sample_rate):
    """|H(e^(j*2*pi*frequencyHz/sampleRateHz))| for one resolved one-pole
    shelf section -- the Python twin of
    rvrbotron::config::shelfMagnitudeAtFrequency (src/config/
    DampingResolution.cpp), evaluated here against the already-resolved
    coefficients from resolved.json rather than re-derived from a ratio and
    corner."""
    omega = 2.0 * math.pi * frequency_hz / sample_rate
    cos_omega, sin_omega = math.cos(omega), math.sin(omega)
    numerator_real = b0 + b1 * cos_omega
    numerator_imag = -b1 * sin_omega
    denominator_real = 1.0 + a1 * cos_omega
    denominator_imag = -a1 * sin_omega
    return math.sqrt(
        (numerator_real * numerator_real + numerator_imag * numerator_imag)
        / (denominator_real * denominator_real + denominator_imag * denominator_imag)
    )


def validate_damping_shape(damping, channels):
    """Defensive shape check of resolved Damping evidence, mirroring the
    DSP's own "validate resolved shape and finite invariants defensively"
    stance (docs/design/reverb/stages/05-damping.md): this analyzer trusts
    but verifies resolved.json rather than assuming a well-formed producer,
    since a hand-edited or corrupted file must fail with a descriptive
    reason, not a raw KeyError deep inside the prediction math."""
    if damping is None:
        return
    if not isinstance(damping, dict):
        raise ValueError(
            "resolved Damping evidence is inconsistent: "
            "damping must be an object"
        )
    for field in (
        "highShelfB0",
        "highShelfB1",
        "highShelfA1",
        "lowShelfB0",
        "lowShelfB1",
        "lowShelfA1",
    ):
        values = damping.get(field)
        if not isinstance(values, list) or len(values) != channels:
            raise ValueError(
                "resolved Damping evidence is inconsistent: "
                f"{field!r} does not have one value per Channel"
            )
        if not all(is_finite_number(value) for value in values):
            raise ValueError(
                "resolved Damping evidence is inconsistent: "
                f"{field!r} contains a non-finite or non-numeric value"
            )


def predicted_channel_rt60_sec(loop, damping, channel, frequency_hz, sample_rate):
    """One Channel's predicted RT60 at frequency_hz: that Channel's own
    resolved decay gain, combined with both shelves' actual response at
    frequency_hz when Damping is enabled (magnitude 1.0, i.e. no shelf,
    when it is not) -- the same per-loop-dB-loss model
    rvrbotron::config::resolveFeedbackLoop uses for expectedReferenceRt60Sec,
    generalized from 1 kHz to an arbitrary band center."""
    gain = loop["gains"][channel]
    loss_db = 20.0 * math.log10(gain)
    magnitude = 1.0
    if damping is not None:
        magnitude = shelf_magnitude_at_frequency(
            damping["lowShelfB0"][channel],
            damping["lowShelfB1"][channel],
            damping["lowShelfA1"][channel],
            frequency_hz,
            sample_rate,
        ) * shelf_magnitude_at_frequency(
            damping["highShelfB0"][channel],
            damping["highShelfB1"][channel],
            damping["highShelfA1"][channel],
            frequency_hz,
            sample_rate,
        )
    total_loss_db = loss_db + 20.0 * math.log10(magnitude)
    loop_time_sec = loop["delaysSamples"][channel] / sample_rate
    return -60.0 * loop_time_sec / total_loss_db


def predicted_band_evidence(
    loop, damping, frequency_hz, sample_rate, gain_mode
):
    channels = len(loop["gains"])
    per_channel = [
        predicted_channel_rt60_sec(loop, damping, channel, frequency_hz, sample_rate)
        for channel in range(channels)
    ]
    return {
        "perChannelRt60Sec": per_channel,
        "targetRt60Sec": (
            float(np.mean(per_channel)) if gain_mode == "per-channel" else None
        ),
        "rangeRt60Sec": [min(per_channel), max(per_channel)],
    }


def within_predicted_tolerance(measured_rt60_sec, predicted, gain_mode):
    if measured_rt60_sec is None:
        return None
    if gain_mode == "uniform":
        low, high = predicted["rangeRt60Sec"]
        return (
            measured_rt60_sec >= low * (1.0 - PREDICTED_TOLERANCE)
            and measured_rt60_sec <= high * (1.0 + PREDICTED_TOLERANCE)
        )
    target = predicted["targetRt60Sec"]
    return abs(measured_rt60_sec - target) <= PREDICTED_TOLERANCE * target


def band_ratio_evidence(band, requested_rt60_sec):
    measured = band["t30"]["rt60Sec"] if band["t30"] else None
    return {
        "centerHz": band["centerHz"],
        "measuredRt60Sec": measured,
        "measuredRatio": (
            measured / requested_rt60_sec if measured is not None else None
        ),
    }


def decay_evidence(frames, sample_rate, loop, damping):
    """Reuses tail-v1's own octave-band extraction (FFT, Schroeder curve,
    T20/T30 fit -- analyze_tail.py's public octave_band_edges/band_evidence,
    plus this module's own duplicated _band_energy), then layers
    Damping-aware prediction and the canonical low/Reference/high summary
    on top -- tail-v1's own artifact/behavior is untouched, this module
    only imports from it."""
    nyquist = sample_rate / 2.0
    length = frames.shape[0]
    fft_length = 1
    while fft_length < 2 * length:
        fft_length *= 2
    spectrum = np.fft.rfft(frames, n=fft_length, axis=0)
    frequencies = np.fft.rfftfreq(fft_length, d=1.0 / sample_rate)

    gain_mode = loop["gainMode"]
    requested_rt60_sec = loop["rt60Sec"]

    bands = []
    for center in analyze_tail.OCTAVE_BAND_CENTERS_HZ:
        low, high = analyze_tail.octave_band_edges(center)
        if center >= nyquist:
            continue
        high = min(high, nyquist)
        energy = _band_energy(spectrum, frequencies, fft_length, length, low, high)
        band = analyze_tail.band_evidence(center, low, high, energy, sample_rate)
        predicted = predicted_band_evidence(
            loop, damping, center, sample_rate, gain_mode
        )
        measured_rt60_sec = band["t30"]["rt60Sec"] if band["t30"] else None
        band["predictedRt60Sec"] = predicted
        band["withinPredictedTolerance"] = within_predicted_tolerance(
            measured_rt60_sec, predicted, gain_mode
        )
        bands.append(band)

    reference = next(
        (band for band in bands if band["centerHz"] == analyze_tail.REFERENCE_BAND_HZ),
        None,
    )
    measured_rt60_sec = (
        reference["t30"]["rt60Sec"] if reference and reference["t30"] else None
    )
    relative_error = (
        abs(measured_rt60_sec - requested_rt60_sec) / requested_rt60_sec
        if measured_rt60_sec is not None
        else None
    )

    canonical_ratios = {
        "low": band_ratio_evidence(bands[0], requested_rt60_sec)
        if bands
        else None,
        "reference": band_ratio_evidence(reference, requested_rt60_sec)
        if reference is not None
        else None,
        "high": band_ratio_evidence(bands[-1], requested_rt60_sec)
        if bands
        else None,
    }

    return {
        "referenceBandHz": analyze_tail.REFERENCE_BAND_HZ,
        "requestedRt60Sec": requested_rt60_sec,
        "measuredRt60Sec": measured_rt60_sec,
        "relativeError": relative_error,
        "withinAccuracyInvariant": (
            relative_error is not None
            and relative_error <= analyze_tail.RT60_ACCURACY_INVARIANT
        ),
        "significantDeviation": (
            relative_error is not None
            and relative_error > SIGNIFICANT_DEVIATION_THRESHOLD
        ),
        "canonicalRatios": canonical_ratios,
        "bands": bands,
    }


def eventual_contraction_evidence(frames, sample_rate, tail_start_frame):
    """A negative late-tail log-energy trend, tolerant of local modal
    beating: unlike tail-v1's decay_envelope_evidence (a strict pairwise
    non-increase check this analyzer deliberately replaces, per issue #78),
    a linear-regression slope over the *late* half of the tail (skipping
    the early build-up/onset region entirely) is negative exactly when the
    overall trend decays, regardless of local ups and downs in between."""
    tail = frames[tail_start_frame:]
    late_start = int(tail.shape[0] * EVENTUAL_CONTRACTION_LATE_FRACTION)
    late = tail[late_start:]
    segment_frames = late.shape[0] // EVENTUAL_CONTRACTION_SEGMENTS
    if segment_frames < 1:
        return {
            "segmentCount": 0,
            "lateFraction": EVENTUAL_CONTRACTION_LATE_FRACTION,
            "segmentEnergiesDb": [],
            "slopeDbPerSegment": None,
            "negativeTrend": None,
        }
    usable = segment_frames * EVENTUAL_CONTRACTION_SEGMENTS
    energy = np.sum(late[:usable] * late[:usable], axis=1)
    segments = energy.reshape(EVENTUAL_CONTRACTION_SEGMENTS, segment_frames).sum(
        axis=1
    )
    total = float(np.sum(segments))
    if not (total > 0.0):
        return {
            "segmentCount": EVENTUAL_CONTRACTION_SEGMENTS,
            "lateFraction": EVENTUAL_CONTRACTION_LATE_FRACTION,
            "segmentEnergiesDb": [],
            "slopeDbPerSegment": None,
            "negativeTrend": None,
        }
    segments_db = 10.0 * np.log10(np.maximum(segments, _TINY_POWER))
    indices = np.arange(EVENTUAL_CONTRACTION_SEGMENTS)
    slope, _intercept = np.polyfit(indices, segments_db, 1)
    return {
        "segmentCount": EVENTUAL_CONTRACTION_SEGMENTS,
        "lateFraction": EVENTUAL_CONTRACTION_LATE_FRACTION,
        "segmentEnergiesDb": [float(value) for value in segments_db],
        "slopeDbPerSegment": float(slope),
        "negativeTrend": bool(slope < 0.0),
    }


def publish_artifact(render_result, contents):
    """Append-only, idempotent publication: identical to tail-v1's own
    publish_artifact (analyze_tail.py), duplicated rather than shared, per
    that module's own established precedent (it does not share this
    function with analyze_render.py either)."""
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

    loop = analyze_tail.locate_feedback_loop(resolved)
    channels = len(loop["gains"])
    damping = loop.get("damping")
    validate_damping_shape(damping, channels)

    if metadata["channels"] != 2:
        raise ValueError("tail analysis requires canonical stereo output")
    output = analyze_render.inspect_wav(render_result / "output.wav")
    if (
        output["sampleRate"] != metadata["sampleRate"]
        or output["channels"] != metadata["channels"]
    ):
        raise ValueError("render metadata does not match output.wav")

    if output["frameCount"] != metadata["frames"]:
        raise ValueError(
            "render metadata does not match output.wav: render.json claims "
            f'{metadata["frames"]} frames, output.wav has '
            f'{output["frameCount"]}'
        )

    # Total drain is inputFrames + preDelayFrames + tailBudgetFrames
    # (Pre-delay's own additional drain, issue #133); preDelayFrames
    # defaults to 0 for a render.json predating Pre-delay.
    expected_frames = (
        metadata["inputFrames"]
        + metadata.get("preDelayFrames", 0)
        + metadata["tailBudgetFrames"]
    )
    silence_floor_enabled = loop.get("silenceFloorDb") is not None
    if silence_floor_enabled:
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

    frames, non_finite_count = analyze_tail.stereo_frames_reporting_non_finite(output)
    sample_rate = metadata["sampleRate"]

    decay = decay_evidence(frames, sample_rate, loop, damping)
    # The tail window must start once real source material has finished
    # arriving at the wet path, not at inputFrames itself: Pre-delay
    # (issue #133) keeps delayed source samples entering Split until
    # inputFrames + preDelayFrames.
    eventual_contraction = eventual_contraction_evidence(
        frames,
        sample_rate,
        metadata["inputFrames"] + metadata.get("preDelayFrames", 0),
    )

    peak, floor = analyze_diffusion.activity_floor(frames)
    active = np.abs(frames) > floor
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
        "dampingEnabled": damping is not None,
        "gainMode": loop["gainMode"],
        "decay": decay,
        "eventualContraction": eventual_contraction,
        "alignment": alignment,
        "coloration": coloration,
    }


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Analyze Damping-aware octave-band decay evidence in a Render "
            "Result (tail analysis version 2)."
        )
    )
    parser.add_argument("render_result", type=Path)
    parser.add_argument("--source", type=Path, required=True)
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    try:
        analysis = analyze(arguments.render_result, arguments.source)
        contents = (
            json.dumps(analysis, allow_nan=False, indent=2, sort_keys=True) + "\n"
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
            f'error, significant={decay["significantDeviation"]}), '
            f'Alignment score {analysis["alignment"]["score"]:.3f}, '
            "eventual contraction negative trend "
            f'{analysis["eventualContraction"]["negativeTrend"]}, '
            f'{analysis["nonFiniteSampleCount"]} non-finite samples'
        )
        print(artifact)
        return 0
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"tail analysis (v2) failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
