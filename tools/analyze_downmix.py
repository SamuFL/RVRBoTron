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
import analyze_tail


ANALYZER_NAME = "downmix"
ANALYZER_VERSION = 1
ARTIFACT_NAME = f"{ANALYZER_NAME}-v{ANALYZER_VERSION}.json"

_TINY_POWER = np.finfo(np.float64).tiny


def sample_precision_dtype(precision):
    """The numpy dtype the renderer's own `Sample` type actually was for
    this render (`render.json`'s `samplePrecision`). analyze_diffusion.
    numpy_frames always promotes a decoded capture to float64 regardless
    of the WAV's own encoding, which is exact (a lossless upcast) for
    reading a single capture in isolation, but combining multiple
    captures with plain float64 arithmetic does *not* reproduce a float32
    render's own single-precision rounding at each step -- reconstructing
    a signal the renderer computed (rather than merely reading one it
    already wrote) must first replay that arithmetic at this dtype."""
    if precision == "float64":
        return np.float64
    if precision == "float32":
        return np.float32
    raise ValueError(f"unknown render sample precision: {precision}")


def _band_energy(spectrum, frequencies, fft_length, length, low_hz, high_hz):
    """Total energy (summed across time and Channels) that one octave
    band's raised-cosine gain (analyze_tail.octave_band_gain, public and
    reused directly rather than re-derived) keeps of an already-computed
    rFFT spectrum, via inverse transform -- the same filtering approach
    analyze_tail.py's own private _band_energy uses for a per-sample decay
    curve, duplicated here (not imported: that leading underscore marks it
    internal to that module, the same cross-module privacy convention
    analyze_modulation.py's own comment documents) and adapted to a single
    scalar since this analyzer compares total band energy between two
    signals rather than fitting a decay time."""
    gain = analyze_tail.octave_band_gain(frequencies, low_hz, high_hz)
    filtered = np.fft.irfft(spectrum * gain[:, None], n=fft_length, axis=0)[
        :length
    ]
    return float(np.sum(filtered * filtered))


def octave_band_deviation_evidence(measured_frames, reference_frames, sample_rate):
    """Per-octave-band energy deviation between two same-length,
    same-sample-rate multi-Channel signals (docs/design/reverb/stages/
    08-downmix.md's "octave-band spectral deviation"), reusing
    analyze_tail.py's own established octave-band raised-cosine filter
    (OCTAVE_BAND_CENTERS_HZ/octave_band_edges) rather than a fresh binning
    scheme -- a raw per-FFT-bin comparison would depend on FFT length
    rather than being a stable, comparable quantity (issue #114's PR
    review). Channel count may differ between the two signals (e.g. a
    stereo Downmix output against its N-Channel source): each is reduced
    to one energy scalar per band before comparing."""
    length = measured_frames.shape[0]
    nyquist = sample_rate / 2.0
    fft_length = 1
    while fft_length < 2 * length:
        fft_length *= 2
    measured_spectrum = np.fft.rfft(measured_frames, n=fft_length, axis=0)
    reference_spectrum = np.fft.rfft(reference_frames, n=fft_length, axis=0)
    frequencies = np.fft.rfftfreq(fft_length, d=1.0 / sample_rate)

    bands = []
    for center in analyze_tail.OCTAVE_BAND_CENTERS_HZ:
        low, high = analyze_tail.octave_band_edges(center)
        if low >= nyquist:
            continue
        high = min(high, nyquist)
        measured_energy = _band_energy(
            measured_spectrum, frequencies, fft_length, length, low, high
        )
        reference_energy = _band_energy(
            reference_spectrum, frequencies, fft_length, length, low, high
        )
        bands.append(
            {
                "centerHz": center,
                "measuredEnergy": measured_energy,
                "referenceEnergy": reference_energy,
                "deviation": abs(measured_energy - reference_energy),
            }
        )
    deviations = [band["deviation"] for band in bands]
    return {
        "bands": bands,
        "maxDeviation": max(deviations) if deviations else 0.0,
        "rmsDeviation": (
            math.sqrt(math.fsum(d * d for d in deviations) / len(deviations))
            if deviations
            else 0.0
        ),
    }


def inter_channel_level_difference_db(frames):
    """Signed dB energy difference between Channel 0 (left) and Channel 1
    (right) of a stereo (frames, 2) array: positive means left carries
    more energy. Reported rather than gated against an acoustic threshold
    (docs/design/reverb/stages/08-downmix.md's Decorrelation evidence).
    Each energy is floored at _TINY_POWER before taking its log (the same
    clamp analyze_diffusion.coloration_evidence uses), so one silent
    Channel produces a large finite value -- never +/-inf, which
    json.dumps(allow_nan=False) below would reject."""
    left_energy = float(np.sum(frames[:, 0] ** 2))
    right_energy = float(np.sum(frames[:, 1] ** 2))
    return 10.0 * math.log10(max(left_energy, _TINY_POWER)) - 10.0 * math.log10(
        max(right_energy, _TINY_POWER)
    )


def peak_factor_evidence(frames):
    """A crest-factor-style "peak factor": peak absolute sample over RMS
    amplitude of a multi-Channel array, comparable across Downmix
    strategies -- coherent reinforcement (issue #114's `sum-all`) raises
    peaks relative to RMS."""
    peak = float(np.max(np.abs(frames))) if frames.size else 0.0
    rms = float(np.sqrt(np.mean(frames * frames))) if frames.size else 0.0
    return {
        "peakAbsoluteSample": peak,
        "rmsAmplitude": rms,
        "peakFactor": peak / rms if rms > 0.0 else 0.0,
    }


def mono_fold_down_evidence(stereo_frames, sample_rate):
    """Equal-power mono fold-down (docs/design/reverb/stages/08-downmix.md:
    "mono = (L + R) / sqrt(2)"): folded energy relative to stereo energy,
    and octave-band spectral deviation between the folded mono signal and
    the same stereo signal's own combined per-band energy, exposing
    cancellation (especially above 90 degrees Width) without imposing an
    acoustic rejection threshold."""
    half = 1.0 / math.sqrt(2.0)
    mono = (stereo_frames[:, 0] + stereo_frames[:, 1]) * half
    mono_energy = float(np.sum(mono * mono))
    stereo_energy = float(np.sum(stereo_frames * stereo_frames))
    energy_loss_ratio = (
        1.0 - mono_energy / stereo_energy if stereo_energy > 0.0 else 0.0
    )
    spectral = octave_band_deviation_evidence(
        mono.reshape(-1, 1), stereo_frames, sample_rate
    )
    return {
        "monoEnergy": mono_energy,
        "stereoEnergy": stereo_energy,
        "energyLossRatio": energy_loss_ratio,
        "spectralMaxDeviation": spectral["maxDeviation"],
        "spectralRmsDeviation": spectral["rmsDeviation"],
    }


def alignment_score_evidence(source_frames, source_descriptor):
    """Measured Alignment score of a Downmix's own immediate N-Channel
    source -- a fact about the *source* (echo arrival times a level or two
    upstream of any per-branch processing), so unlike branch_energy_ratio_
    evidence below, this remains meaningful even when the branch consuming
    that source was itself disabled and never processed it.
    `source_descriptor` identifies which Diffusion Step(s) `source_frames`
    came from -- a single stepIndex for Main, one or more for Early, whose
    taps may draw from several Diffusion Steps at once."""
    peak, floor = analyze_diffusion.activity_floor(source_frames)
    active = np.abs(source_frames) > floor
    return {
        **source_descriptor,
        "activityFloorDb": analyze_diffusion.ACTIVITY_FLOOR_DB,
        "peakAbsoluteSample": peak,
        "activityFloor": floor,
        **analyze_diffusion.alignment_evidence(active),
    }


def branch_energy_ratio_evidence(source_frames, branch_frames, branch_energy, sample_rate):
    """The branch's own captured energy relative to its immediate
    N-Channel source's energy, and spectral deviation between them --
    reported as a *combined* ratio across the Downmix's row/compensation
    projection, Width, and branch level together, deliberately not named
    or claimed as an isolated Width effect: `branch_frames` is captured
    after all three (issue #113's mainStereo/earlyStereo boundary placement
    "after each branch's own shaping, Downmix, Width, and level"), and no
    capture exists between Downmix and Width to separate them (adding one
    would need its own versioned capture boundary per ADR-0005). Only
    meaningful when `source_frames` genuinely is that Downmix's own
    immediate input and the branch was actually enabled -- see analyze()'s
    own gating for both conditions."""
    source_energy = float(np.sum(source_frames * source_frames))
    branch_energy_ratio = {
        "available": True,
        "sourceEnergy": source_energy,
        "branchEnergy": branch_energy,
        "ratio": branch_energy / source_energy if source_energy > 0.0 else None,
    }
    spectral = octave_band_deviation_evidence(
        branch_frames, source_frames, sample_rate
    )
    spectral_deviation = {"available": True, **spectral}
    return branch_energy_ratio, spectral_deviation


def unavailable_branch_energy_ratio_evidence(reason):
    return (
        {"available": False, "reason": reason},
        {"available": False, "reason": reason},
    )


def branch_facts(downmix):
    """Structural facts read directly from one resolved Downmix -- never
    recomputed from strategy/alignment inside this analyzer, so a change
    to resolveCoherentDownmixAblation's own production derivation
    (ResolveConfig.cpp) is exactly what this reports, not an independent
    guess at it."""
    return {
        "strategy": downmix["strategy"],
        "alignmentExpectation": downmix["alignment"],
        "coherentDownmixAblation": downmix["coherentDownmixAblation"],
    }


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
    output_wav = analyze_render.inspect_wav(render_result / "output.wav")
    # The renderer always drains its complete resolved Tail budget (see
    # RenderMetadata.h), so inputFrames + preDelayFrames + tailBudgetFrames
    # equals the complete-response frame count regardless of whether the
    # Composition contains a Diffuser, a Feedback Loop, or both, and
    # regardless of Pre-delay (issue #133) -- unlike analyze_diffusion.py,
    # which is scoped to the Diffuser-only shape and so derives it from
    # diffuser.totalSamples instead.
    expected_frames = (
        metadata["inputFrames"]
        + metadata.get("preDelayFrames", 0)
        + metadata["tailBudgetFrames"]
    )
    if (
        metadata["frames"] != expected_frames
        or output_wav["sampleRate"] != metadata["sampleRate"]
        or output_wav["channels"] != metadata["channels"]
        or output_wav["frameCount"] != expected_frames
    ):
        raise ValueError("render metadata does not match output.wav")

    stages = resolved["composition"]["stages"]
    if not stages:
        raise ValueError("downmix analysis requires a non-empty Composition")
    if stages[-1]["type"] != "downmix":
        raise ValueError(
            "downmix analysis requires a Composition ending in a Downmix "
            "stage"
        )

    captures = metadata.get("stageCaptures")
    if metadata.get("stageCaptureProfile") != "all-v2" or not captures:
        raise ValueError("downmix analysis requires --capture-stages all")

    main_capture = None
    early_capture = None
    diffusion_captures = {}
    for capture in captures:
        wav = analyze_diffusion.verified_capture(
            render_result, metadata, capture, expected_frames
        )
        if capture["boundary"] == "main-stereo":
            main_capture = (capture, wav)
        elif capture["boundary"] == "early-stereo":
            early_capture = (capture, wav)
        elif capture["boundary"] == "diffusion-step":
            diffusion_captures[capture["index"]] = wav
    if main_capture is None:
        raise ValueError("Stage capture manifest is missing Main-stereo")

    main_frames = analyze_diffusion.numpy_frames(main_capture[1])
    early_frames = (
        analyze_diffusion.numpy_frames(early_capture[1])
        if early_capture is not None
        else np.zeros_like(main_frames)
    )
    output_frames = analyze_diffusion.numpy_frames(output_wav)
    sample_dtype = sample_precision_dtype(metadata["samplePrecision"])
    # Reverb.cpp computes the Wet sum, the Composition's own dry/wet
    # envelope (issue #131), and the final summation entirely in `Sample`
    # precision -- a sequence of correctly-rounded operations, not the
    # exact float64 arithmetic plain numpy on numpy_frames' own
    # already-promoted arrays would give. Replaying that same sequence at
    # `sample_dtype` before promoting back to float64 reproduces the
    # renderer's own rounding (float32's round-to-nearest at each step,
    # then an exact upcast) rather than a second, different rounding of
    # the exact result.
    #
    # A resolved.json predating the envelope (issue #131) carries none of
    # these fields; ADR-0007's legacy neutral reading -- wet-only, unity
    # wet gain -- reproduces this check's own pre-envelope behavior
    # exactly, so an old Render Result remains analyzable unchanged.
    composition = resolved["composition"]
    wet_only = composition.get("wetOnly", True)
    wet_gain = sample_dtype(composition.get("wetGain", 1.0))
    # The Wet sum itself (issue #131's canonical term): Main plus Early,
    # before the global wet gain -- kept separate from `wet_sum` below
    # because the branch-energy reconciliation a few lines down checks
    # the superposition identity `|a+b|^2 = |a|^2 + |b|^2 + 2*a.b` on the
    # two *branches*, an invariant of how they combine that the envelope
    # (applied afterward, uniformly, to the whole sum) cannot affect --
    # scaling it by wetGain here would make every one of those four
    # energies wrong by the same factor for no reason connected to what
    # this reconciliation actually checks.
    raw_wet_sum = (
        main_frames.astype(sample_dtype) + early_frames.astype(sample_dtype)
    )
    wet_sum = raw_wet_sum * wet_gain
    if wet_only:
        reconstructed_output = wet_sum.astype(np.float64)
    else:
        # Dry contribution (issue #131): the exact source samples the
        # renderer itself decoded, channel-for-channel in stereo or
        # duplicated without energy compensation in mono, zero past
        # source EOF through the Tail-budget drain -- mirroring
        # Reverb.cpp's own dry mapping exactly rather than reusing
        # `source`'s facts-only inspection for anything but format
        # validation above.
        dry_gain = sample_dtype(composition.get("dryGain", 1.0))
        dry_source = np.fromiter(
            analyze_render.decoded_samples(source),
            dtype=np.float64,
            count=source["frameCount"] * source["channels"],
        ).reshape(source["frameCount"], source["channels"])
        dry_stereo = (
            dry_source[:, :2]
            if source["channels"] >= 2
            else np.repeat(dry_source[:, :1], 2, axis=1)
        )
        if dry_stereo.shape[0] < expected_frames:
            dry_stereo = np.pad(
                dry_stereo,
                ((0, expected_frames - dry_stereo.shape[0]), (0, 0)),
            )
        dry_contribution = dry_stereo.astype(sample_dtype) * dry_gain
        reconstructed_output = (dry_contribution + wet_sum).astype(np.float64)
    if not np.array_equal(output_frames, reconstructed_output):
        raise ValueError(
            "output.wav did not equal the dry contribution plus the "
            "wet-gain-scaled sample-wise sum of the captured Main-stereo "
            "and Early-stereo branches"
        )

    main_energy = float(np.sum(main_frames * main_frames))
    early_energy = float(np.sum(early_frames * early_frames))
    cross_term = float(np.sum(main_frames * early_frames))
    # The Wet sum's own energy (issue #131): output.wav's energy once a
    # non-neutral envelope is configured, not before, so
    # branchEnergyReconciliation stays about Main/Early superposition
    # specifically. See raw_wet_sum's own comment above.
    combined_energy = float(
        np.sum(raw_wet_sum.astype(np.float64) * raw_wet_sum.astype(np.float64))
    )
    expected_combined_energy = main_energy + early_energy + 2.0 * cross_term

    sample_rate = metadata["sampleRate"]
    main_downmix = stages[-1]
    main = branch_facts(main_downmix)
    main["branchEnergy"] = main_energy

    # A Diffusion Step capture is a Downmix's own immediate N-Channel
    # input only when a Diffuser is what actually feeds it -- unaligned
    # (Feedback Loop) Composition shapes have no equivalent capture
    # (ADR-0005 deliberately omits a Feedback Loop boundary), so measured
    # spectral evidence against source and the branch energy ratio are
    # both structurally unavailable there rather than computed against
    # the wrong signal. This also means a damped tail (Feedback Loop plus
    # Damping) never gets an "absolute flatness" spectral claim from this
    # analyzer -- only an aligned source ever reaches the available
    # branch below, and damped Feedback Loop renders are excluded by
    # construction. A disabled branch (its own capture manifested
    # `disabled: true`, exact zero throughout, issue #113) skips its own
    # Downmix/Width/level processing entirely (Reverb.cpp), so its own
    # branch energy ratio and spectral deviation are unavailable too --
    # comparing a zero capture against a nonzero source would otherwise
    # report measurements for processing that never ran. Alignment score
    # alone stays available regardless of the branch's own enablement: it
    # characterizes the source feeding the Downmix, not that Downmix's
    # own processing. Main's own immediate input is a single Diffusion
    # Step (whichever stage directly precedes its Downmix); Early's is
    # the gain-weighted sum of every one of its own taps' Diffusion Step
    # captures -- exactly reconstructing Diffuser.cpp's own per-tap
    # accumulation (`accumulator[channel] += outputs[channel] *
    # tap.gain`) from captures alone, replayed at the render's own Sample
    # precision (see sample_precision_dtype), since Early's accumulator
    # itself has no capture boundary of its own.
    diffuser_immediately_precedes_main = (
        len(stages) >= 2 and stages[-2]["type"] == "diffuser"
    )
    if diffuser_immediately_precedes_main and diffusion_captures:
        source_index = max(diffusion_captures)
        source_frames = analyze_diffusion.numpy_frames(
            diffusion_captures[source_index]
        )
        main["alignmentScore"] = alignment_score_evidence(
            source_frames, {"stepIndex": source_index}
        )
        if main_capture[0].get("disabled", False):
            (
                main["branchEnergyRatio"],
                main["spectralDeviation"],
            ) = unavailable_branch_energy_ratio_evidence(
                "Main is disabled: its own Downmix/Width/level processing "
                "never ran"
            )
        else:
            (
                main["branchEnergyRatio"],
                main["spectralDeviation"],
            ) = branch_energy_ratio_evidence(
                source_frames, main_frames, main_energy, sample_rate
            )
    else:
        main["alignmentScore"] = None
        (
            main["branchEnergyRatio"],
            main["spectralDeviation"],
        ) = unavailable_branch_energy_ratio_evidence(
            "no Diffusion Step capture is the Downmix's own immediate "
            "input (the source is unaligned, or no Diffuser precedes it)"
        )

    early = None
    early_resolved = resolved["composition"].get("early")
    if early_resolved is not None:
        early = branch_facts(early_resolved["downmix"])
        early["branchEnergy"] = early_energy
        early_taps = early_resolved["taps"]
        if early_taps and all(
            tap["stepIndex"] in diffusion_captures for tap in early_taps
        ):
            early_source_frames = None
            for tap in early_taps:
                step_frames = analyze_diffusion.numpy_frames(
                    diffusion_captures[tap["stepIndex"]]
                ).astype(sample_dtype)
                weighted = step_frames * sample_dtype(tap["gain"])
                early_source_frames = (
                    weighted
                    if early_source_frames is None
                    else early_source_frames + weighted
                )
            early_source_frames = early_source_frames.astype(np.float64)
            early["alignmentScore"] = alignment_score_evidence(
                early_source_frames,
                {"stepIndices": [tap["stepIndex"] for tap in early_taps]},
            )
            if early_capture is not None and early_capture[0].get(
                "disabled", False
            ):
                (
                    early["branchEnergyRatio"],
                    early["spectralDeviation"],
                ) = unavailable_branch_energy_ratio_evidence(
                    "Early is disabled: its own Downmix/Width/level "
                    "processing never ran"
                )
            else:
                (
                    early["branchEnergyRatio"],
                    early["spectralDeviation"],
                ) = branch_energy_ratio_evidence(
                    early_source_frames, early_frames, early_energy, sample_rate
                )
        else:
            early["alignmentScore"] = None
            (
                early["branchEnergyRatio"],
                early["spectralDeviation"],
            ) = unavailable_branch_energy_ratio_evidence(
                "one or more of Early's own taps has no captured "
                "Diffusion Step to reconstruct its own weighted "
                "N-Channel input from"
            )

    analysis = {
        "formatVersion": 1,
        "analyzer": ANALYZER_NAME,
        "analyzerVersion": ANALYZER_VERSION,
        "source": {
            "filename": metadata["inputFilename"],
            "sha256": source_hash,
            "verified": True,
        },
        "main": main,
        "early": early,
        "branchEnergyReconciliation": {
            "mainEnergy": main_energy,
            "earlyEnergy": early_energy,
            "crossTerm": cross_term,
            "combinedEnergy": combined_energy,
            "expectedCombinedEnergy": expected_combined_energy,
        },
        "outputCorrelation": analyze_diffusion.correlation_evidence(output_frames),
        "interChannelLevelDifferenceDb": inter_channel_level_difference_db(
            output_frames
        ),
        "peakFactor": peak_factor_evidence(output_frames),
        "monoFoldDown": mono_fold_down_evidence(output_frames, sample_rate),
    }
    return analysis


def publish_artifact(render_result, contents):
    """Append-only, idempotent publication: the same shape as every other
    analyzer's own publish_artifact, duplicated rather than shared, per
    those modules' own established precedent."""
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


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Analyze spatial-output Downmix evidence in a Render Result."
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
        print(
            f'{analysis["main"]["strategy"]} Main Downmix, '
            f'{analysis["main"]["alignmentExpectation"]}, '
            f'coherentDownmixAblation='
            f'{analysis["main"]["coherentDownmixAblation"]}, '
            f'peak factor {analysis["peakFactor"]["peakFactor"]:.3g}, '
            f'mono fold-down energy loss '
            f'{analysis["monoFoldDown"]["energyLossRatio"]:.3g}'
        )
        print(artifact)
        return 0
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"downmix analysis failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
