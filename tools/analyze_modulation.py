#!/usr/bin/env python3

"""Modulation analysis version 1: movement evidence for a Render Result,
published beside the unmodified tail-v1/tail-v2 artifacts (see issue #94).

Measurement happens at the Downmix output only -- there is no Feedback Loop
or Diffusion Step Stage capture boundary, per ADR-0005 (docs/adr/
0005-measure-movement-at-the-output.md). Reuses tail-v1/tail-v2's public
provenance checks, octave-band decay evidence and Damping-aware RT60
prediction (analyze_tail.py, analyze_tail_v2.py) and analyze_diffusion.py's
Channel correlation evidence unchanged; bounded-energy evidence is computed
here rather than imported, so this analyzer's central claim -- the tail
stayed bounded under movement -- stands alone (see the acceptance criteria
in issue #94).

Everything here is reported, never enforced: a deviation past its threshold
is flagged as significant, but no measurement rejects a render (ADR-0004,
docs/adr/0004-validate-structure-not-acoustics.md).
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
import analyze_tail_v2

ANALYZER_NAME = "modulation"
ANALYZER_VERSION = 1
ARTIFACT_NAME = f"{ANALYZER_NAME}-v{ANALYZER_VERSION}.json"

# Coarse windows spanning the *late* half of the post-input-EOF tail, the
# same shape as analyze_tail_v2.eventual_contraction_evidence -- duplicated
# rather than imported, per this analyzer's own acceptance criterion that
# bounded-energy evidence must be computed within this analyzer so it
# stands alone.
BOUNDED_ENERGY_SEGMENTS = 16
BOUNDED_ENERGY_LATE_FRACTION = 0.5

# A per-octave decay-tilt slope past this many ratio units (measured T30
# divided by the loop's requested rt60Sec, the same denominator Damping's
# own highRatio/lowRatio use) is flagged as significant: half of tail-v2's
# own SIGNIFICANT_DEVIATION_THRESHOLD (0.10), since a full octave already
# spans several measured bands and a modest per-octave tilt compounds
# across the handful of octaves a typical render covers.
DECAY_TILT_SIGNIFICANT_RATIO_PER_OCTAVE = 0.05

# A configured rate whose instantaneous-frequency magnitude exceeds this
# multiple of the broadband mean is flagged as coherent pitch movement that
# survived into the summed output.
COHERENT_PITCH_MOVEMENT_SIGNIFICANT_RELATIVE_MAGNITUDE = 3.0

# The two Downmix outputs are flagged as significantly correlated once their
# off-diagonal correlation exceeds this magnitude -- close enough to +/-1
# that per-Channel decorrelation (structural, from distinct positional
# seeds; see ADR-0005) is not surviving into the summed pair.
OUTPUT_CORRELATION_SIGNIFICANT_MAX_ABSOLUTE_OFF_DIAGONAL = 0.9

_KNOWN_MODULATION_SHAPES = {"smoothed-random", "sine", "triangle"}
_KNOWN_MODULATION_INTERPOLATIONS = {"lagrange3", "linear", "allpass"}

_TINY_MAGNITUDE = np.finfo(np.float64).tiny


def is_finite_number(value):
    """Duplicated from analyze_tail_v2.py's own private helper, for the
    same reason that module gives for duplicating analyze_tail.py's private
    FFT helpers: a leading underscore marks it internal to that module."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    try:
        return math.isfinite(value)
    except OverflowError:
        return False


def validate_modulation_shape(modulation, channels, owner_label):
    """Defensive shape check of one resolved Modulation object, mirroring
    analyze_tail_v2.validate_damping_shape's stance: this analyzer trusts
    but verifies resolved.json rather than assuming a well-formed producer,
    so a hand-edited or corrupted file fails with a descriptive reason."""
    if modulation is None:
        return
    if not isinstance(modulation, dict):
        raise ValueError(
            "resolved Modulation evidence is inconsistent: "
            f"{owner_label} modulation must be an object"
        )
    depth_ms = modulation.get("depthMs")
    if not is_finite_number(depth_ms) or depth_ms < 0.0:
        raise ValueError(
            "resolved Modulation evidence is inconsistent: "
            f"{owner_label} depthMs is not a finite non-negative number"
        )
    rate_hz = modulation.get("rateHz")
    if not is_finite_number(rate_hz) or rate_hz < 0.0:
        raise ValueError(
            "resolved Modulation evidence is inconsistent: "
            f"{owner_label} rateHz is not a finite non-negative number"
        )
    if modulation.get("shape") not in _KNOWN_MODULATION_SHAPES:
        raise ValueError(
            "resolved Modulation evidence is inconsistent: "
            f"{owner_label} shape is unrecognized"
        )
    if modulation.get("interpolation") not in _KNOWN_MODULATION_INTERPOLATIONS:
        raise ValueError(
            "resolved Modulation evidence is inconsistent: "
            f"{owner_label} interpolation is unrecognized"
        )
    modulated = modulation.get("channelModulated")
    if not isinstance(modulated, list) or len(modulated) != channels:
        raise ValueError(
            "resolved Modulation evidence is inconsistent: "
            f"{owner_label} channelModulated does not have one flag per "
            "Channel"
        )


def locate_stages(resolved):
    """Reuses analyze_tail.locate_feedback_loop's own composition-shape
    validation (the same two Feedback-Loop-bearing shapes tail-v2
    requires: RT60 deviation and Decay tilt both need the loop's own
    resolved gains/delays/shelf response to predict against) rather than
    duplicating that check here; only the error text is this analyzer's
    own, matching every sibling analyzer's own composition-requirement
    message (analyze_diffusion's "diffusion analysis requires a finite
    Diffuser", analyze_tail's "tail analysis requires a Feedback Loop")."""
    try:
        loop = analyze_tail.locate_feedback_loop(resolved)
    except ValueError:
        raise ValueError("modulation analysis requires a Feedback Loop") from None
    stages = resolved["composition"]["stages"]
    split = stages[0]
    diffuser = next(
        (stage for stage in stages if stage["type"] == "diffuser"), None
    )
    return split, diffuser, loop


def modulation_summary(owner, modulation):
    return {
        "owner": owner,
        "depthMs": modulation["depthMs"],
        "rateHz": modulation["rateHz"],
        "shape": modulation["shape"],
        "channelFraction": modulation["channelFraction"],
        "interpolation": modulation["interpolation"],
    }


def collect_active_modulations(split, diffuser, loop):
    """Validates every resolved Modulation object present (Feedback Loop
    and/or each Diffusion Step) and returns their activeModulations
    summary list. Each entry's own "owner"/"rateHz" fields already carry
    what coherent-pitch-movement evidence needs, so callers derive that
    lookup directly from this one list rather than this function
    returning a second, parallel one."""
    active_modulations = []

    loop_modulation = loop.get("modulation")
    validate_modulation_shape(loop_modulation, len(loop["gains"]), "Feedback Loop")
    if loop_modulation is not None:
        active_modulations.append(modulation_summary("feedback-loop", loop_modulation))

    if diffuser is not None:
        for step in diffuser.get("steps") or []:
            owner = f'diffusion-step[{step["index"]}]'
            step_modulation = step.get("modulation")
            validate_modulation_shape(step_modulation, split["channels"], owner)
            if step_modulation is not None:
                active_modulations.append(modulation_summary(owner, step_modulation))

    return active_modulations


def bounded_energy_evidence(frames, sample_rate, tail_start_frame):
    """A negative late-tail log-energy trend, tolerant of local modal
    beating -- the same construction as analyze_tail_v2.
    eventual_contraction_evidence, duplicated here (not imported) so this
    analyzer's bounded-energy claim stands alone (issue #94's own
    acceptance criteria)."""
    tail = frames[tail_start_frame:]
    late_start = int(tail.shape[0] * BOUNDED_ENERGY_LATE_FRACTION)
    late = tail[late_start:]
    segment_frames = late.shape[0] // BOUNDED_ENERGY_SEGMENTS
    if segment_frames < 1:
        return {
            "segmentCount": 0,
            "lateFraction": BOUNDED_ENERGY_LATE_FRACTION,
            "segmentEnergiesDb": [],
            "slopeDbPerSegment": None,
            "negativeTrend": None,
        }
    usable = segment_frames * BOUNDED_ENERGY_SEGMENTS
    energy = np.sum(late[:usable] * late[:usable], axis=1)
    segments = energy.reshape(BOUNDED_ENERGY_SEGMENTS, segment_frames).sum(axis=1)
    total = float(np.sum(segments))
    if not (total > 0.0):
        return {
            "segmentCount": BOUNDED_ENERGY_SEGMENTS,
            "lateFraction": BOUNDED_ENERGY_LATE_FRACTION,
            "segmentEnergiesDb": [],
            "slopeDbPerSegment": None,
            "negativeTrend": None,
        }
    segments_db = 10.0 * np.log10(np.maximum(segments, _TINY_MAGNITUDE))
    indices = np.arange(BOUNDED_ENERGY_SEGMENTS)
    slope, _intercept = np.polyfit(indices, segments_db, 1)
    return {
        "segmentCount": BOUNDED_ENERGY_SEGMENTS,
        "lateFraction": BOUNDED_ENERGY_LATE_FRACTION,
        "segmentEnergiesDb": [float(value) for value in segments_db],
        "slopeDbPerSegment": float(slope),
        "negativeTrend": bool(slope < 0.0),
    }


def decay_tilt_evidence(bands, requested_rt60_sec):
    """Slope of measured per-octave-band T30 against log-frequency,
    expressed in ratio units per octave -- each band's measured T30 divided
    by the loop's own requested rt60Sec, the same denominator Damping's
    resolved highRatio/lowRatio use (src/config/DampingResolution.cpp), so
    "this interpolation method behaved like a highRatio of 0.85" is a
    statement this number can defend (docs/design/reverb/stages/
    06-modulation.md's Measurement section)."""
    points = [
        (math.log2(band["centerHz"]), band["t30"]["rt60Sec"] / requested_rt60_sec)
        for band in bands
        if band["t30"] is not None
    ]
    if len(points) < 2:
        return {
            "bandCount": len(points),
            "slopeRatioPerOctave": None,
            "interceptRatio": None,
            "significant": None,
        }
    log_frequencies = np.array([point[0] for point in points])
    ratios = np.array([point[1] for point in points])
    slope, intercept = np.polyfit(log_frequencies, ratios, 1)
    return {
        "bandCount": len(points),
        "slopeRatioPerOctave": float(slope),
        "interceptRatio": float(intercept),
        "significant": abs(float(slope)) > DECAY_TILT_SIGNIFICANT_RATIO_PER_OCTAVE,
    }


def _analytic_signal(samples):
    """The analytic signal of a real 1-D array via an FFT-domain Hilbert
    transform: zero the negative-frequency bins, double the positive ones,
    keep DC and (for even length) Nyquist unscaled, inverse-FFT. Pure numpy
    -- tools/requirements.txt carries no scipy, so scipy.signal.hilbert is
    unavailable."""
    length = samples.shape[0]
    spectrum = np.fft.fft(samples)
    multiplier = np.zeros(length)
    if length % 2 == 0:
        multiplier[0] = 1.0
        multiplier[length // 2] = 1.0
        multiplier[1 : length // 2] = 2.0
    else:
        multiplier[0] = 1.0
        multiplier[1 : (length + 1) // 2] = 2.0
    return np.fft.ifft(spectrum * multiplier)


def coherent_pitch_movement_evidence(summed, sample_rate, active_rates):
    """Instantaneous frequency of the summed Downmix output (its analytic
    signal's unwrapped phase derivative), then the magnitude of that
    instantaneous-frequency signal's own spectrum at each configured
    Modulation rate, relative to the broadband mean magnitude of that
    spectrum -- did movement survive into the output as coherent pitch
    wobble at its own rate (docs/design/reverb/stages/06-modulation.md's
    Measurement section), measured only on the Downmix output per
    ADR-0005."""
    length = summed.shape[0]
    if length < 4:
        return {
            "fftLength": 0,
            "broadbandMeanMagnitude": 0.0,
            "rates": [
                {
                    "owner": owner,
                    "rateHz": rate_hz,
                    "binHz": None,
                    "magnitude": None,
                    "relativeMagnitude": None,
                    "significant": None,
                }
                for owner, rate_hz in active_rates
            ],
        }
    window = np.hanning(length)
    analytic = _analytic_signal(summed * window)
    phase = np.unwrap(np.angle(analytic))
    instantaneous_frequency = np.diff(phase) / (2.0 * np.pi) * sample_rate

    derivative_length = instantaneous_frequency.shape[0]
    fft_length = 1
    while fft_length < derivative_length:
        fft_length *= 2
    spectrum = np.fft.rfft(
        instantaneous_frequency * np.hanning(derivative_length), n=fft_length
    )
    magnitude = np.abs(spectrum)
    frequencies = np.fft.rfftfreq(fft_length, d=1.0 / sample_rate)
    # DC (and the near-DC bins the analytic-signal phase unwrap's own drift
    # occupies) is excluded from the broadband mean: it is not evidence of
    # coherent movement at a configured rate.
    broadband_mean_magnitude = (
        float(np.mean(magnitude[1:])) if magnitude.size > 1 else 0.0
    )

    rates = []
    for owner, rate_hz in active_rates:
        bin_index = int(np.argmin(np.abs(frequencies - rate_hz)))
        rate_magnitude = float(magnitude[bin_index])
        relative_magnitude = (
            rate_magnitude / broadband_mean_magnitude
            if broadband_mean_magnitude > 0.0
            else None
        )
        rates.append(
            {
                "owner": owner,
                "rateHz": rate_hz,
                "binHz": float(frequencies[bin_index]),
                "magnitude": rate_magnitude,
                "relativeMagnitude": relative_magnitude,
                "significant": (
                    relative_magnitude is not None
                    and relative_magnitude
                    > COHERENT_PITCH_MOVEMENT_SIGNIFICANT_RELATIVE_MAGNITUDE
                ),
            }
        )
    return {
        "fftLength": fft_length,
        "broadbandMeanMagnitude": broadband_mean_magnitude,
        "rates": rates,
    }


def output_correlation_evidence(frames):
    """Signed normalized zero-lag correlation between the two Downmix
    output signals (analyze_diffusion.correlation_evidence, reused
    unchanged): are the two outputs moving together, the complement to
    per-Channel decorrelation, which is asserted structurally rather than
    measured (ADR-0005)."""
    correlation = analyze_diffusion.correlation_evidence(frames)
    return {
        **correlation,
        "significant": (
            correlation["maxAbsoluteOffDiagonal"]
            > OUTPUT_CORRELATION_SIGNIFICANT_MAX_ABSOLUTE_OFF_DIAGONAL
        ),
    }


def publish_artifact(render_result, contents):
    """Append-only, idempotent publication: the same shape as tail-v1/
    tail-v2's own publish_artifact, duplicated rather than shared, per
    those modules' own established precedent."""
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

    split, diffuser, loop = locate_stages(resolved)
    channels = len(loop["gains"])
    damping = loop.get("damping")
    analyze_tail_v2.validate_damping_shape(damping, channels)
    active_modulations = collect_active_modulations(split, diffuser, loop)
    active_rates = [
        (entry["owner"], entry["rateHz"]) for entry in active_modulations
    ]

    if metadata["channels"] != 2:
        raise ValueError("modulation analysis requires canonical stereo output")
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

    expected_frames = metadata["inputFrames"] + metadata["tailBudgetFrames"]
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

    decay = analyze_tail_v2.decay_evidence(frames, sample_rate, loop, damping)
    decay_tilt = decay_tilt_evidence(decay["bands"], loop["rt60Sec"])
    bounded_energy = bounded_energy_evidence(
        frames, sample_rate, metadata["inputFrames"]
    )
    summed = frames[:, 0] + frames[:, 1]
    coherent_pitch_movement = coherent_pitch_movement_evidence(
        summed, sample_rate, active_rates
    )
    output_correlation = output_correlation_evidence(frames)

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
        "modulationPresent": bool(active_modulations),
        "activeModulations": active_modulations,
        "boundedEnergy": bounded_energy,
        "decay": decay,
        "decayTilt": decay_tilt,
        "coherentPitchMovement": coherent_pitch_movement,
        "outputCorrelation": output_correlation,
    }


def parse_arguments():
    parser = argparse.ArgumentParser(
        description=(
            "Analyze reproducible movement evidence in a Render Result "
            "(modulation analysis version 1)."
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
        decay_tilt = analysis["decayTilt"]
        tilt_word = (
            f'{decay_tilt["slopeRatioPerOctave"]:.4f}/octave'
            if decay_tilt["slopeRatioPerOctave"] is not None
            else "n/a"
        )
        print(
            f'{analysis["completeResponse"]["outputFrames"]} frames, '
            f'Modulation present={analysis["modulationPresent"]} '
            f'({len(analysis["activeModulations"])} active), '
            f"decay tilt {tilt_word}, "
            "bounded-energy negative trend "
            f'{analysis["boundedEnergy"]["negativeTrend"]}, '
            "Output correlation max off-diagonal "
            f'{analysis["outputCorrelation"]["maxAbsoluteOffDiagonal"]:.4f}, '
            f'{analysis["nonFiniteSampleCount"]} non-finite samples'
        )
        print(artifact)
        return 0
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"modulation analysis failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
