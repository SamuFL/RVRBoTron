#!/usr/bin/env python3

import json
import math
import shutil
import struct
import subprocess
import sys
from pathlib import Path


def read_float_wav(path: Path):
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise AssertionError(f"{path} is not a RIFF/WAVE file")

    offset = 12
    channels = bits_per_sample = None
    data_chunk = None
    while offset + 8 <= len(data):
        chunk_id = data[offset : offset + 4]
        chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
        chunk = data[offset + 8 : offset + 8 + chunk_size]
        if chunk_id == b"fmt ":
            _, channels, _ = struct.unpack_from("<HHI", chunk)
            bits_per_sample = struct.unpack_from("<H", chunk, 14)[0]
        elif chunk_id == b"data":
            data_chunk = chunk
        offset += 8 + chunk_size + (chunk_size % 2)

    if data_chunk is None:
        raise AssertionError(f"{path} has no audio data")
    if bits_per_sample == 32:
        samples = struct.unpack("<" + "f" * (len(data_chunk) // 4), data_chunk)
    elif bits_per_sample == 64:
        samples = struct.unpack("<" + "d" * (len(data_chunk) // 8), data_chunk)
    else:
        raise AssertionError(f"unexpected bits per sample: {bits_per_sample}")
    return channels, samples


def write_float32_wav(path: Path, sample_rate: int, channels: int, samples):
    # A minimal canonical IEEE float32 RIFF/WAVE file -- the renderer's
    # own accepted input format, mirroring tests/fixtures/
    # generate_wav_matrix.py's make_wav rather than introducing a second
    # WAV-writing convention.
    audio = struct.pack("<" + "f" * len(samples), *samples)
    bytes_per_sample = 4
    block_align = channels * bytes_per_sample
    fmt = struct.pack(
        "<HHIIHH",
        3,
        channels,
        sample_rate,
        sample_rate * block_align,
        block_align,
        bytes_per_sample * 8,
    )
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(audio)) + audio
    if len(audio) % 2:
        chunks += b"\x00"
    path.write_bytes(b"RIFF" + struct.pack("<I", len(chunks) + 4) + b"WAVE" + chunks)


def deinterleave(channels: int, samples):
    return [samples[channel::channels] for channel in range(channels)]


def zero_lag_correlation(a, b):
    # The repository's canonical Output correlation: an uncentered
    # normalized zero-lag dot product (tools/analyze_diffusion.py's
    # correlation_evidence, lines 123-131), not Pearson correlation --
    # impulse responses generally have nonzero mean, so centering first
    # would report a different quantity.
    numerator = math.fsum(x * y for x, y in zip(a, b))
    denominator = math.sqrt(
        math.fsum(x * x for x in a) * math.fsum(y * y for y in b)
    )
    if denominator == 0.0:
        return 0.0
    return numerator / denominator


def halves_row(channels, left_group):
    # `halves` (#110): the first ceil(N/2) Channels map left, the
    # remainder maps right, each group using equal 1/sqrt(groupSize)
    # coefficients -- an independent Python re-derivation of
    # src/config/ResolveConfig.cpp's halvesRow, for comparing against the
    # rendered resolved.json rather than reusing that C++ code.
    row = [0.0] * channels
    left_count = (channels + 1) // 2
    begin, end = (0, left_count) if left_group else (left_count, channels)
    coefficient = 1.0 / math.sqrt(end - begin)
    for index in range(begin, end):
        row[index] = coefficient
    return row


def alternating_row(channels, left_group):
    # `alternating` (#110): even Channel indices map left, odd indices
    # map right, each group using equal 1/sqrt(groupSize) coefficients --
    # an independent Python re-derivation of alternatingRow.
    row = [0.0] * channels
    first = 0 if left_group else 1
    indices = list(range(first, channels, 2))
    coefficient = 1.0 / math.sqrt(len(indices))
    for index in indices:
        row[index] = coefficient
    return row


def inter_channel_level_difference_db(left, right):
    left_energy = math.fsum(v * v for v in left)
    right_energy = math.fsum(v * v for v in right)
    if left_energy <= 0.0 or right_energy <= 0.0:
        return 0.0 if left_energy == right_energy else float("inf")
    return 10.0 * math.log10(left_energy / right_energy)


def width_matrix(width_deg):
    # Width's resolved 2x2 mid/side matrix (#109, docs/design/reverb/
    # stages/08-downmix.md's "Width as a constant-power mid/side law") --
    # an independent Python re-derivation of resolveWidthMatrix, for
    # comparing against the rendered resolved.json rather than reusing
    # that C++ code. Exact endpoints at 0/90/180 degrees, matching the
    # production resolver's own bypass of the general trig formula there.
    half = 1.0 / math.sqrt(2.0)
    if width_deg == 0.0:
        return [half, half, half, half]
    if width_deg == 90.0:
        return [1.0, 0.0, 0.0, 1.0]
    if width_deg == 180.0:
        return [half, -half, -half, half]
    half_angle_rad = math.radians(width_deg) / 2.0
    cos_half = math.cos(half_angle_rad)
    sin_half = math.sin(half_angle_rad)
    a = (cos_half + sin_half) * half
    b = (cos_half - sin_half) * half
    return [a, b, b, a]


def dft_power_spectrum(samples):
    # A direct O(n^2) DFT is fine here: fixtures below are a few dozen
    # samples (a millisecond-scale Diffuser response), not a signal this
    # test suite needs FFT-scale performance for.
    n = len(samples)
    power = []
    for k in range(n // 2 + 1):
        real = math.fsum(
            value * math.cos(-2.0 * math.pi * k * t / n)
            for t, value in enumerate(samples)
        )
        imag = math.fsum(
            value * math.sin(-2.0 * math.pi * k * t / n)
            for t, value in enumerate(samples)
        )
        power.append(real * real + imag * imag)
    return power


def sum_all_row(channels):
    # `sum-all` (#114): equal `1/sqrt(channels)` coefficients over every
    # Channel, the same row duplicated to both L and R -- an independent
    # Python re-derivation of src/config/ResolveConfig.cpp's sumAllRow,
    # for comparing against the rendered resolved.json rather than
    # reusing that C++ code.
    coefficient = 1.0 / math.sqrt(channels)
    return [coefficient] * channels


def alignment_score(channel_frames, activity_floor_db=-120.0):
    # Alignment score (#114, CONTEXT.md's "pairwise overlap of active
    # arrival times between Channels, measured independently of amplitude
    # sign") -- an independent Python re-derivation of
    # tools/analyze_diffusion.py's alignment_evidence/activity_floor,
    # given per-Channel sample lists (e.g. a deinterleaved Diffusion Step
    # capture) rather than a NumPy array.
    peak = max(
        (abs(value) for samples in channel_frames for value in samples),
        default=0.0,
    )
    floor = peak * (10.0 ** (activity_floor_db / 20.0))
    active = [
        [abs(value) > floor for value in samples] for samples in channel_frames
    ]
    channels = len(active)
    scores = []
    for channel_a in range(channels):
        for channel_b in range(channel_a + 1, channels):
            union = intersection = 0
            for a, b in zip(active[channel_a], active[channel_b]):
                if a or b:
                    union += 1
                    if a and b:
                        intersection += 1
            # Two Channels with no activity at all trivially agree.
            scores.append(1.0 if union == 0 else intersection / union)
    return {
        "mean": (sum(scores) / len(scores)) if scores else 1.0,
        "minimum": min(scores) if scores else 1.0,
    }


def mono_fold_down_evidence(left, right):
    # Equal-power mono fold-down (docs/design/reverb/stages/08-downmix.md:
    # "mono = (L + R) / sqrt(2)"), reporting folded energy relative to
    # stereo energy so cancellation is visible without an acoustic
    # rejection threshold (#114).
    half = 1.0 / math.sqrt(2.0)
    mono = [(l + r) * half for l, r in zip(left, right)]
    mono_energy = math.fsum(v * v for v in mono)
    stereo_energy = math.fsum(v * v for v in left) + math.fsum(
        v * v for v in right
    )
    ratio = (
        0.0
        if stereo_energy <= 0.0
        else mono_energy / stereo_energy
    )
    return {
        "monoEnergy": mono_energy,
        "stereoEnergy": stereo_energy,
        "foldedEnergyRatio": ratio,
    }


def peak_factor(samples):
    # A crest-factor-style "peak factor" (#114): peak absolute sample over
    # RMS amplitude, comparable between a coherent `sum-all` render and its
    # matched `select` control -- coherent reinforcement raises peaks
    # relative to RMS.
    if not samples:
        return 0.0
    peak = max(abs(value) for value in samples)
    rms = math.sqrt(math.fsum(value * value for value in samples) / len(samples))
    return peak / rms if rms > 0.0 else 0.0


OCTAVE_BAND_START_HZ = 20.0


def octave_band_powers(power, sample_rate, fft_length):
    # Groups a raw per-bin power spectrum (dft_power_spectrum's own
    # output: bin k at k * sample_rate / fft_length Hz) into full-octave
    # bands centered at 20 Hz * 2**band, so a comparison between two
    # spectra is a stable, perceptually-grouped quantity rather than one
    # that depends on the raw per-bin FFT resolution (issue #114's PR
    # review: "the resulting max/RMS values therefore depend on FFT
    # length"). One octave wide rather than tools/analyze_diffusion.py's
    # twelfth-octave Coloration curve -- an independent Python
    # re-derivation of that same idea, for this stdlib-only test file,
    # at the coarser band width docs/design/reverb/stages/08-downmix.md
    # itself names ("octave-band spectral deviation").
    nyquist = sample_rate / 2.0
    bands = []
    band = 0
    while True:
        center = OCTAVE_BAND_START_HZ * (2.0**band)
        if center > nyquist:
            break
        low = OCTAVE_BAND_START_HZ * (2.0 ** (band - 0.5))
        high = OCTAVE_BAND_START_HZ * (2.0 ** (band + 0.5))
        bands.append(
            math.fsum(
                value
                for index, value in enumerate(power)
                if low <= (index * sample_rate / fft_length) < high
            )
        )
        band += 1
    return bands


def spectral_deviation_evidence(left, right, aggregate_source_power, sample_rate):
    # Octave-band spectral deviation of the downmixed L/R power spectra
    # against the same N-Channel source's aggregate power spectrum
    # (docs/design/reverb/stages/08-downmix.md's "Mono compatibility"
    # section and issue #114's own evidence requirement) -- the same
    # comparison the orthogonal-rows fixture above makes, factored out
    # for reuse by the sum-all/select matched comparison below.
    fft_length = len(left)
    left_power = dft_power_spectrum(left)
    right_power = dft_power_spectrum(right)
    combined_bands = octave_band_powers(
        [l + r for l, r in zip(left_power, right_power)], sample_rate, fft_length
    )
    aggregate_bands = octave_band_powers(
        aggregate_source_power, sample_rate, fft_length
    )
    deviations = [
        abs(combined - aggregate)
        for combined, aggregate in zip(combined_bands, aggregate_bands)
    ]
    max_deviation = max(deviations)
    rms_deviation = math.sqrt(
        math.fsum(value * value for value in deviations) / len(deviations)
    )
    return max_deviation, rms_deviation


def run_renderer(renderer: Path, *arguments: str):
    return subprocess.run(
        [str(renderer), "render", *map(str, arguments)],
        check=False,
        capture_output=True,
        text=True,
    )


def require_success(completed):
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)


def require_failure(completed, expected_error: str, output: Path):
    if completed.returncode == 0:
        raise AssertionError("renderer unexpectedly succeeded")
    if expected_error not in completed.stderr:
        raise AssertionError(
            f"expected error {expected_error!r}, got {completed.stderr!r}"
        )
    if output.exists():
        raise AssertionError("configuration failure created a Render Result")


def main():
    renderer = Path(sys.argv[1])
    fixture = Path(sys.argv[2])
    stereo_fixture = (
        fixture.parent / "matrix" / "identity-stereo-48000-float32.wav"
    )
    workspace = Path(sys.argv[3])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    request = workspace / "request.json"
    requested_result = workspace / "requested-result"
    resolved_result = workspace / "resolved-result"
    request_bytes = (
        b"{\r\n"
        b'  "formatVersion": 2,\n'
        b'  "seed": 18446744073709551615,\r\n'
        b'  "composition": {"stages": []}\n'
        b"}\r\n"
    )
    request.write_bytes(request_bytes)

    requested = run_renderer(
        renderer,
        "--input",
        fixture,
        "--config",
        request,
        "--output",
        requested_result,
    )
    require_success(requested)

    if (requested_result / "request.json").read_bytes() != request_bytes:
        raise AssertionError("Render Result did not preserve the raw request")
    if (
        json.loads((requested_result / "render.json").read_text())[
            "configurationInput"
        ]
        != "requested"
    ):
        raise AssertionError("Render Result did not record requested input mode")

    resolved = json.loads((requested_result / "resolved.json").read_text())
    expected = {
        "formatVersion": 2,
        "seed": 18446744073709551615,
        "sampleRate": 48000,
        "composition": {"stages": []},
    }
    if resolved != expected:
        raise AssertionError(f"unexpected resolved request: {resolved}")

    rerendered = run_renderer(
        renderer,
        "--input",
        fixture,
        "--resolved",
        requested_result / "resolved.json",
        "--output",
        resolved_result,
    )
    require_success(rerendered)

    if (resolved_result / "request.json").exists():
        raise AssertionError("resolved render synthesized request.json")
    if (
        json.loads((resolved_result / "render.json").read_text())[
            "configurationInput"
        ]
        != "resolved"
    ):
        raise AssertionError("Render Result did not record resolved input mode")
    if (resolved_result / "resolved.json").read_bytes() != (
        requested_result / "resolved.json"
    ).read_bytes():
        raise AssertionError("resolved render changed the Resolved Configuration")
    if (resolved_result / "output.wav").read_bytes() != (
        requested_result / "output.wav"
    ).read_bytes():
        raise AssertionError("resolved rerender changed identity output")

    # A totally empty request is missing the now-mandatory formatVersion
    # and must fail at /formatVersion rather than silently defaulting.
    empty_request = workspace / "empty-request.json"
    empty_result = workspace / "empty-result"
    empty_request.write_text("{}\n")
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            empty_request,
            "--output",
            empty_result,
        ),
        "invalid_configuration at /formatVersion: required field is missing",
        empty_result,
    )

    minimal_request = workspace / "minimal-request.json"
    minimal_result = workspace / "minimal-result"
    minimal_request.write_text('{"formatVersion": 2}\n')
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            minimal_request,
            "--output",
            minimal_result,
        )
    )
    if json.loads((minimal_result / "resolved.json").read_text()) != {
        "formatVersion": 2,
        "seed": 0,
        "sampleRate": 48000,
        "composition": {"stages": []},
    }:
        raise AssertionError("minimal request did not use current defaults")
    if (minimal_result / "request.json").read_text() != '{"formatVersion": 2}\n':
        raise AssertionError("minimal raw request was not preserved")

    omitted_stages_request = workspace / "omitted-stages-request.json"
    omitted_stages_result = workspace / "omitted-stages-result"
    omitted_stages_request.write_text(
        '{"formatVersion": 2, "composition": {}}\n'
    )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            omitted_stages_request,
            "--output",
            omitted_stages_result,
        )
    )
    if (omitted_stages_result / "output.wav").read_bytes() != (
        minimal_result / "output.wav"
    ).read_bytes():
        raise AssertionError("omitted stages were not exact identity")
    if json.loads((omitted_stages_result / "resolved.json").read_text()) != {
        "formatVersion": 2,
        "seed": 0,
        "sampleRate": 48000,
        "composition": {"stages": []},
    }:
        raise AssertionError("omitted stages did not resolve to empty identity")

    reference_request = workspace / "reference-request.json"
    reference_result = workspace / "reference-result"
    reference_rerender = workspace / "reference-rerender"
    reference_request.write_text(
        json.dumps(
            {
                "formatVersion": 2,
                "seed": 42,
                "composition": {
                    "stages": [
                        {
                            "type": "split",
                            "channels": 8,
                            "strategy": "duplicate",
                            "normalisation": "energy",
                        },
                        {
                            "type": "diffuser",
                            "steps": 1,
                            "totalMs": 1,
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
                        },
                    ]
                },
            },
            indent=2,
        )
        + "\n"
    )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            reference_request,
            "--output",
            reference_result,
        )
    )
    reference_resolved = json.loads(
        (reference_result / "resolved.json").read_text()
    )
    stages = reference_resolved["composition"]["stages"]
    if stages[0] != {
        "type": "split",
        "inputChannels": 1,
        "channels": 8,
        "strategy": "duplicate",
        "normalisation": "energy",
        "sourceGain": 1.0,
        "channelGain": 0.35355339059327373,
    }:
        raise AssertionError(f"unexpected resolved Split: {stages[0]}")
    if stages[1]["type"] != "diffuser" or stages[1]["totalSamples"] != 48:
        raise AssertionError(f"unexpected resolved Diffuser: {stages[1]}")
    if len(stages[1]["steps"]) != 1:
        raise AssertionError("reference request did not resolve one Diffusion Step")
    step = stages[1]["steps"][0]
    if step["delaysSamples"] != [5, 8, 16, 22, 27, 32, 41, 44]:
        raise AssertionError(f"unexpected segmented delays: {step}")
    if len(set(step["delaysSamples"])) != 8:
        raise AssertionError("segmented-random delays are not distinct")
    if step["permutation"] != [2, 4, 0, 3, 7, 6, 1, 5]:
        raise AssertionError(f"unexpected deterministic shuffle: {step}")
    if step["polaritySigns"] != [1, 1, -1, 1, -1, -1, 1, 1]:
        raise AssertionError(f"unexpected deterministic polarity: {step}")
    hadamard_scale = 0.35355339059327373
    expected_hadamard = [
        [hadamard_scale, hadamard_scale, hadamard_scale, hadamard_scale,
         hadamard_scale, hadamard_scale, hadamard_scale, hadamard_scale],
        [hadamard_scale, -hadamard_scale, hadamard_scale, -hadamard_scale,
         hadamard_scale, -hadamard_scale, hadamard_scale, -hadamard_scale],
        [hadamard_scale, hadamard_scale, -hadamard_scale, -hadamard_scale,
         hadamard_scale, hadamard_scale, -hadamard_scale, -hadamard_scale],
        [hadamard_scale, -hadamard_scale, -hadamard_scale, hadamard_scale,
         hadamard_scale, -hadamard_scale, -hadamard_scale, hadamard_scale],
        [hadamard_scale, hadamard_scale, hadamard_scale, hadamard_scale,
         -hadamard_scale, -hadamard_scale, -hadamard_scale, -hadamard_scale],
        [hadamard_scale, -hadamard_scale, hadamard_scale, -hadamard_scale,
         -hadamard_scale, hadamard_scale, -hadamard_scale, hadamard_scale],
        [hadamard_scale, hadamard_scale, -hadamard_scale, -hadamard_scale,
         -hadamard_scale, -hadamard_scale, hadamard_scale, hadamard_scale],
        [hadamard_scale, -hadamard_scale, -hadamard_scale, hadamard_scale,
         -hadamard_scale, hadamard_scale, hadamard_scale, -hadamard_scale],
    ]
    if step["matrix"] != expected_hadamard:
        raise AssertionError(f"unexpected normalized Hadamard: {step['matrix']}")
    if stages[2] != {
        "type": "downmix",
        "inputChannels": 8,
        "outputChannels": 2,
        "strategy": "select",
        "leftChannel": 0,
        "rightChannel": 1,
        "normalisation": "energy",
        "compensation": 2.0,
        "leftRow": [1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
        "rightRow": [0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
        "effectiveLeftRow": [2.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
        "effectiveRightRow": [0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
        "alignment": "aligned",
        "widthDeg": 90.0,
        "widthMatrix": [1.0, 0.0, 0.0, 1.0],
        "coherentDownmixAblation": False,
    }:
        raise AssertionError(f"unexpected resolved Downmix: {stages[2]}")

    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            reference_result / "resolved.json",
            "--output",
            reference_rerender,
        )
    )
    if (reference_rerender / "resolved.json").read_bytes() != (
        reference_result / "resolved.json"
    ).read_bytes():
        raise AssertionError("resolved diffusion rerender changed configuration")
    if (reference_rerender / "output.wav").read_bytes() != (
        reference_result / "output.wav"
    ).read_bytes():
        raise AssertionError("resolved diffusion rerender changed output")

    multi_step_request = json.loads(reference_request.read_text())
    multi_step_request["composition"]["stages"][1]["steps"] = 2
    multi_step_result = workspace / "multi-step-result"
    multi_step_rerender = workspace / "multi-step-rerender"
    multi_step_config = workspace / "multi-step-request.json"
    multi_step_config.write_text(json.dumps(multi_step_request))
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            multi_step_config,
            "--output",
            multi_step_result,
        )
    )
    multi_step_resolved = json.loads(
        (multi_step_result / "resolved.json").read_text()
    )
    multi_step_stages = multi_step_resolved["composition"]["stages"]
    multi_step_steps = multi_step_stages[1]["steps"]
    if len(multi_step_steps) != 2:
        raise AssertionError(
            f"expected two resolved Diffusion Steps, got {multi_step_steps}"
        )
    if [entry["index"] for entry in multi_step_steps] != [0, 1]:
        raise AssertionError(
            f"unexpected resolved step ordering: {multi_step_steps}"
        )
    if sum(entry["lengthSamples"] for entry in multi_step_steps) != (
        multi_step_stages[1]["totalSamples"]
    ):
        raise AssertionError(
            "resolved step sample budgets did not sum to the resolved total"
        )
    # Positional seed stability: step index 0's permutation and polarity are
    # pure functions of (seed, step index, Channel) and must be unaffected
    # by how many Diffusion Steps the Diffuser now has.
    if multi_step_steps[0]["permutation"] != step["permutation"]:
        raise AssertionError(
            "step 0 permutation changed when the step count changed"
        )
    if multi_step_steps[0]["polaritySigns"] != step["polaritySigns"]:
        raise AssertionError(
            "step 0 polarity changed when the step count changed"
        )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            multi_step_result / "resolved.json",
            "--output",
            multi_step_rerender,
        )
    )
    if (multi_step_rerender / "resolved.json").read_bytes() != (
        multi_step_result / "resolved.json"
    ).read_bytes():
        raise AssertionError("multi-step rerender changed configuration")

    lengths_ms_request = json.loads(reference_request.read_text())
    del lengths_ms_request["composition"]["stages"][1]["steps"]
    del lengths_ms_request["composition"]["stages"][1]["totalMs"]
    del lengths_ms_request["composition"]["stages"][1]["distribution"]
    lengths_ms_request["composition"]["stages"][1]["lengthsMs"] = [0.5, 1.0, 1.5]
    lengths_ms_request["composition"]["stages"][1]["stepOverrides"] = [
        {"index": 1, "polarity": "none"}
    ]
    lengths_ms_config = workspace / "lengths-ms-request.json"
    lengths_ms_config.write_text(json.dumps(lengths_ms_request))
    lengths_ms_result = workspace / "lengths-ms-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            lengths_ms_config,
            "--output",
            lengths_ms_result,
        )
    )
    lengths_ms_resolved = json.loads(
        (lengths_ms_result / "resolved.json").read_text()
    )
    lengths_ms_stages = lengths_ms_resolved["composition"]["stages"]
    lengths_ms_steps = lengths_ms_stages[1]["steps"]
    if len(lengths_ms_steps) != 3:
        raise AssertionError(
            f"expected three resolved Diffusion Steps, got {lengths_ms_steps}"
        )
    resolved_lengths = [entry["lengthSamples"] for entry in lengths_ms_steps]
    # 0.5ms : 1.0ms : 1.5ms at 48kHz apportions to 24 : 48 : 72 samples exactly.
    if resolved_lengths != [24, 48, 72]:
        raise AssertionError(
            f"unexpected lengthsMs apportionment: {resolved_lengths}"
        )
    if sum(resolved_lengths) != lengths_ms_stages[1]["totalSamples"]:
        raise AssertionError(
            "lengthsMs step sample budgets did not sum to the resolved total"
        )
    # stepOverrides only touches index 1's polarity: steps 0 and 2 keep the
    # shared seeded-random polarity, step 1 must resolve to all-positive.
    if lengths_ms_steps[1]["polaritySigns"] != [1] * 8:
        raise AssertionError(
            f"stepOverrides polarity override did not apply: {lengths_ms_steps[1]}"
        )
    if lengths_ms_steps[0]["polaritySigns"] == lengths_ms_steps[1]["polaritySigns"]:
        raise AssertionError(
            "expected step 0 and overridden step 1 polarity to differ"
        )
    if lengths_ms_steps[2]["polaritySigns"] == lengths_ms_steps[1]["polaritySigns"]:
        raise AssertionError(
            "expected step 2 and overridden step 1 polarity to differ"
        )

    memory_budget_request = json.loads(reference_request.read_text())
    memory_budget_request["composition"]["stages"][0]["channels"] = 64
    memory_budget_request["composition"]["stages"][1]["totalMs"] = 500000
    memory_budget_config = workspace / "memory-budget-request.json"
    memory_budget_config.write_text(json.dumps(memory_budget_request))
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            memory_budget_config,
            "--memory-budget-mib",
            "1",
            "--output",
            workspace / "memory-budget-result",
        ),
        "resolved Diffuser DSP memory footprint exceeds the configured memory budget",
        workspace / "memory-budget-result",
    )

    # Omitting steps/totalMs/distribution entirely must resolve to the
    # Reference four-step doubling chain (4 steps, 300ms total).
    default_diffuser_request = json.loads(reference_request.read_text())
    del default_diffuser_request["composition"]["stages"][1]["steps"]
    del default_diffuser_request["composition"]["stages"][1]["totalMs"]
    del default_diffuser_request["composition"]["stages"][1]["distribution"]
    default_diffuser_config = workspace / "default-diffuser-request.json"
    default_diffuser_config.write_text(json.dumps(default_diffuser_request))
    default_diffuser_result = workspace / "default-diffuser-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            default_diffuser_config,
            "--output",
            default_diffuser_result,
        )
    )
    default_diffuser_resolved = json.loads(
        (default_diffuser_result / "resolved.json").read_text()
    )
    default_diffuser_stages = default_diffuser_resolved["composition"]["stages"]
    default_diffuser_steps = default_diffuser_stages[1]["steps"]
    if len(default_diffuser_steps) != 4:
        raise AssertionError(
            f"expected the Reference four-step chain by default, got "
            f"{default_diffuser_steps}"
        )
    # totalMs 300 at 48kHz is 14400 samples; doubling weights step i by 2**i.
    default_lengths = [entry["lengthSamples"] for entry in default_diffuser_steps]
    if default_lengths != [960, 1920, 3840, 7680]:
        raise AssertionError(
            f"unexpected default Reference doubling apportionment: "
            f"{default_lengths}"
        )
    if sum(default_lengths) != default_diffuser_stages[1]["totalSamples"]:
        raise AssertionError(
            "Reference default step sample budgets did not sum to the "
            "resolved total"
        )

    stereo_result = workspace / "stereo-reference-result"
    stereo_rerender = workspace / "stereo-reference-rerender"
    require_success(
        run_renderer(
            renderer,
            "--input",
            stereo_fixture,
            "--config",
            reference_request,
            "--output",
            stereo_result,
        )
    )
    stereo_resolved = json.loads(
        (stereo_result / "resolved.json").read_text()
    )
    stereo_split = stereo_resolved["composition"]["stages"][0]
    if stereo_split["sourceGain"] != 0.7071067811865475:
        raise AssertionError(
            f"stereo source selection gain was not resolved: {stereo_split}"
        )
    if stereo_split["channelGain"] != 0.35355339059327373:
        raise AssertionError(
            f"stereo Channel gain was not resolved: {stereo_split}"
        )
    require_success(
        run_renderer(
            renderer,
            "--input",
            stereo_fixture,
            "--resolved",
            stereo_result / "resolved.json",
            "--output",
            stereo_rerender,
        )
    )
    if (stereo_rerender / "resolved.json").read_bytes() != (
        stereo_result / "resolved.json"
    ).read_bytes():
        raise AssertionError("resolved stereo Split changed configuration")
    if (stereo_rerender / "output.wav").read_bytes() != (
        stereo_result / "output.wav"
    ).read_bytes():
        raise AssertionError("resolved stereo Split changed output")

    invalid_stereo_source = json.loads(json.dumps(stereo_resolved))
    invalid_stereo_source["composition"]["stages"][0]["sourceGain"] = 1.0
    invalid_stereo_source_path = workspace / "invalid-stereo-source.json"
    invalid_stereo_source_result = workspace / "invalid-stereo-source-result"
    invalid_stereo_source_path.write_text(json.dumps(invalid_stereo_source))
    require_failure(
        run_renderer(
            renderer,
            "--input",
            stereo_fixture,
            "--resolved",
            invalid_stereo_source_path,
            "--output",
            invalid_stereo_source_result,
        ),
        "/composition/stages/0/sourceGain: "
        "expected gain derived from Split input mapping",
        invalid_stereo_source_result,
    )

    ablation_request = workspace / "ablation-request.json"
    ablation_result = workspace / "ablation-result"
    ablation_rerender = workspace / "ablation-rerender"
    ablation_request.write_text(
        json.dumps(
            {
                "formatVersion": 2,
                "composition": {
                    "stages": [
                        {
                            "type": "split",
                            "channels": 4,
                            "strategy": "duplicate",
                            "normalisation": "none",
                        },
                        {
                            "type": "diffuser",
                            "steps": 1,
                            "totalMs": 0.125,
                            "distribution": "even",
                            "step": {
                                "delayStrategy": "even",
                                "mix": "hadamard",
                                "shuffle": False,
                                "polarity": "none",
                            },
                        },
                        {
                            "type": "downmix",
                            "strategy": "select",
                            "leftChannel": 0,
                            "normalisation": "none",
                        },
                    ]
                },
            },
            indent=2,
        )
        + "\n"
    )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            ablation_request,
            "--output",
            ablation_result,
        )
    )
    ablation_resolved = json.loads(
        (ablation_result / "resolved.json").read_text()
    )
    ablation_stages = ablation_resolved["composition"]["stages"]
    if ablation_stages[0]["normalisation"] != "none":
        raise AssertionError("Split ablation did not round-trip")
    if ablation_stages[0]["sourceGain"] != 1.0:
        raise AssertionError("mono source selection gain changed level")
    if ablation_stages[0]["channelGain"] != 1.0:
        raise AssertionError("Split none normalisation changed level")
    ablation_step = ablation_stages[1]["steps"][0]
    if ablation_step["delayStrategy"] != "even":
        raise AssertionError("even delay strategy did not round-trip")
    if ablation_step["delaysSamples"] != [0, 2, 4, 6]:
        raise AssertionError(f"unexpected even delays: {ablation_step}")
    if ablation_step["shuffle"] is not False:
        raise AssertionError("shuffle false did not round-trip")
    if ablation_step["permutation"] != [0, 1, 2, 3]:
        raise AssertionError("shuffle false did not resolve identity")
    if ablation_step["polarity"] != "none":
        raise AssertionError("none polarity did not round-trip")
    if ablation_step["polaritySigns"] != [1, 1, 1, 1]:
        raise AssertionError("none polarity did not resolve all positive")
    if ablation_stages[2]["normalisation"] != "none":
        raise AssertionError("Downmix ablation did not round-trip")
    if ablation_stages[2]["compensation"] != 1.0:
        raise AssertionError("Downmix none normalisation compensated select")
    # An omitted rightChannel duplicates leftChannel to mono (issue #107),
    # covering the "one selected Channel" acceptance scenario at N=4.
    if "rightChannel" in ablation_stages[2]:
        raise AssertionError(
            f"omitted rightChannel did not stay omitted: {ablation_stages[2]}"
        )
    if (
        ablation_stages[2]["leftRow"] != [1.0, 0.0, 0.0, 0.0]
        or ablation_stages[2]["rightRow"] != [1.0, 0.0, 0.0, 0.0]
    ):
        raise AssertionError(
            f"omitted rightChannel did not duplicate leftChannel's row: "
            f"{ablation_stages[2]}"
        )
    if ablation_stages[2]["alignment"] != "aligned":
        raise AssertionError(
            f"Diffuser-only Downmix did not resolve aligned: {ablation_stages[2]}"
        )

    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            ablation_result / "resolved.json",
            "--output",
            ablation_rerender,
        )
    )
    if (ablation_rerender / "resolved.json").read_bytes() != (
        ablation_result / "resolved.json"
    ).read_bytes():
        raise AssertionError("resolved ablations changed configuration")
    if (ablation_rerender / "output.wav").read_bytes() != (
        ablation_result / "output.wav"
    ).read_bytes():
        raise AssertionError("resolved ablations changed output")

    # Uniform-random deliberately samples with replacement, so it tolerates a
    # step budget (1 sample, 2 positions) far shorter than 4 Channels would
    # ever need for segmented-random or even -- and this seed/budget/Channel
    # combination is independently known (see docs/adr/0002) to draw the
    # same position twice.
    uniform_random_request = workspace / "uniform-random-request.json"
    uniform_random_result = workspace / "uniform-random-result"
    uniform_random_rerender = workspace / "uniform-random-rerender"
    uniform_random_request.write_text(
        json.dumps(
            {
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
                            "steps": 1,
                            "totalMs": 0.02,
                            "distribution": "even",
                            "step": {
                                "delayStrategy": "uniform-random",
                                "mix": "hadamard",
                                "shuffle": True,
                                "polarity": "seeded-random",
                            },
                        },
                        {"type": "downmix", "strategy": "select", "leftChannel": 0, "rightChannel": 1},
                    ]
                },
            },
            indent=2,
        )
        + "\n"
    )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            uniform_random_request,
            "--output",
            uniform_random_result,
        )
    )
    uniform_random_resolved = json.loads(
        (uniform_random_result / "resolved.json").read_text()
    )
    uniform_random_step = uniform_random_resolved["composition"]["stages"][1][
        "steps"
    ][0]
    if uniform_random_step["lengthSamples"] != 1:
        raise AssertionError(
            f"unexpected uniform-random step budget: {uniform_random_step}"
        )
    if uniform_random_step["delaysSamples"] != [1, 0, 0, 0]:
        raise AssertionError(f"unexpected uniform-random delays: {uniform_random_step}")
    if len(set(uniform_random_step["delaysSamples"])) == len(
        uniform_random_step["delaysSamples"]
    ):
        raise AssertionError("expected uniform-random to permit a delay collision")
    if uniform_random_step["permutation"] != [2, 1, 0, 3]:
        raise AssertionError(f"unexpected deterministic shuffle: {uniform_random_step}")
    if uniform_random_step["polaritySigns"] != [1, 1, -1, 1]:
        raise AssertionError(
            f"unexpected deterministic polarity: {uniform_random_step}"
        )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            uniform_random_result / "resolved.json",
            "--output",
            uniform_random_rerender,
        )
    )
    if (uniform_random_rerender / "resolved.json").read_bytes() != (
        uniform_random_result / "resolved.json"
    ).read_bytes():
        raise AssertionError("resolved uniform-random rerender changed configuration")
    if (uniform_random_rerender / "output.wav").read_bytes() != (
        uniform_random_result / "output.wav"
    ).read_bytes():
        raise AssertionError("resolved uniform-random rerender changed output")

    # The same duplicate-delay evidence that uniform-random resolved above
    # must be accepted directly as a hand-authored Resolved Configuration.
    uniform_ablation_document = json.loads(json.dumps(ablation_resolved))
    uniform_duplicate_step = uniform_ablation_document["composition"]["stages"][1][
        "steps"
    ][0]
    uniform_duplicate_step["delayStrategy"] = "uniform-random"
    uniform_duplicate_step["delaysSamples"][1] = uniform_duplicate_step[
        "delaysSamples"
    ][0]
    uniform_duplicate_step["delaysMs"][1] = uniform_duplicate_step["delaysMs"][0]
    uniform_duplicate_step["bufferSizes"][1] = uniform_duplicate_step["bufferSizes"][
        0
    ]
    uniform_ablation_path = workspace / "uniform-ablation-resolved.json"
    uniform_ablation_result = workspace / "uniform-ablation-result"
    uniform_ablation_path.write_text(json.dumps(uniform_ablation_document))
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            uniform_ablation_path,
            "--output",
            uniform_ablation_result,
        )
    )
    resolved_uniform_ablation = json.loads(
        (uniform_ablation_result / "resolved.json").read_text()
    )
    resolved_uniform_step = resolved_uniform_ablation["composition"]["stages"][1][
        "steps"
    ][0]
    if resolved_uniform_step["delayStrategy"] != "uniform-random":
        raise AssertionError("uniform-random delayStrategy did not round-trip")
    if (
        resolved_uniform_step["delaysSamples"][0]
        != resolved_uniform_step["delaysSamples"][1]
    ):
        raise AssertionError(
            "uniform-random ablation should permit a resolved delay collision"
        )

    # Householder is valid for any N, including non-powers-of-two: N=3.
    householder_request = workspace / "householder-request.json"
    householder_result = workspace / "householder-result"
    householder_rerender = workspace / "householder-rerender"
    householder_request.write_text(
        json.dumps(
            {
                "formatVersion": 2,
                "seed": 42,
                "composition": {
                    "stages": [
                        {
                            "type": "split",
                            "channels": 3,
                            "strategy": "duplicate",
                            "normalisation": "energy",
                        },
                        {
                            "type": "diffuser",
                            "steps": 1,
                            "totalMs": 1,
                            "distribution": "even",
                            "step": {
                                "delayStrategy": "segmented-random",
                                "mix": "householder",
                                "shuffle": True,
                                "polarity": "seeded-random",
                            },
                        },
                        {"type": "downmix", "strategy": "select", "leftChannel": 0, "rightChannel": 1},
                    ]
                },
            },
            indent=2,
        )
        + "\n"
    )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            householder_request,
            "--output",
            householder_result,
        )
    )
    householder_resolved = json.loads(
        (householder_result / "resolved.json").read_text()
    )
    householder_step = householder_resolved["composition"]["stages"][1]["steps"][0]
    if householder_step["mix"] != "householder":
        raise AssertionError("householder mix did not round-trip")
    # Mean of Channels, subtracted twice: diagonal 1 - 2/N, off-diagonal
    # -2/N. N=3 is not a power of two, unlike every other mix fixture above.
    householder_off_diagonal = -2.0 / 3.0
    householder_diagonal = 1.0 + householder_off_diagonal
    expected_householder_3 = [
        [householder_diagonal, householder_off_diagonal,
         householder_off_diagonal],
        [householder_off_diagonal, householder_diagonal,
         householder_off_diagonal],
        [householder_off_diagonal, householder_off_diagonal,
         householder_diagonal],
    ]
    if householder_step["matrix"] != expected_householder_3:
        raise AssertionError(
            f"unexpected normalized Householder matrix: "
            f"{householder_step['matrix']}"
        )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            householder_result / "resolved.json",
            "--output",
            householder_rerender,
        )
    )
    if (householder_rerender / "resolved.json").read_bytes() != (
        householder_result / "resolved.json"
    ).read_bytes():
        raise AssertionError("resolved Householder rerender changed configuration")
    if (householder_rerender / "output.wav").read_bytes() != (
        householder_result / "output.wav"
    ).read_bytes():
        raise AssertionError("resolved Householder rerender changed output")

    # RandomOrthogonal is seeded and dense, with no Haar-uniformity claim;
    # this expected matrix is independently derived from the documented
    # positional derivation (ADR 0002): fill [-1,1] with usage "MIXORTHO"
    # (itemIndex = row, valueIndex = column), then Householder QR with the
    # sign convention documented alongside `householderQrOrthogonalize`.
    random_orthogonal_request = workspace / "random-orthogonal-request.json"
    random_orthogonal_result = workspace / "random-orthogonal-result"
    random_orthogonal_rerender = workspace / "random-orthogonal-rerender"
    random_orthogonal_request.write_text(
        json.dumps(
            {
                "formatVersion": 2,
                "seed": 42,
                "composition": {
                    "stages": [
                        {
                            "type": "split",
                            "channels": 2,
                            "strategy": "duplicate",
                            "normalisation": "energy",
                        },
                        {
                            "type": "diffuser",
                            "steps": 1,
                            "totalMs": 1,
                            "distribution": "even",
                            "step": {
                                "delayStrategy": "segmented-random",
                                "mix": "random-orthogonal",
                                "shuffle": True,
                                "polarity": "seeded-random",
                            },
                        },
                        {"type": "downmix", "strategy": "select", "leftChannel": 0, "rightChannel": 1},
                    ]
                },
            },
            indent=2,
        )
        + "\n"
    )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            random_orthogonal_request,
            "--output",
            random_orthogonal_result,
        )
    )
    random_orthogonal_resolved = json.loads(
        (random_orthogonal_result / "resolved.json").read_text()
    )
    random_orthogonal_step = random_orthogonal_resolved["composition"]["stages"][1][
        "steps"
    ][0]
    if random_orthogonal_step["mix"] != "random-orthogonal":
        raise AssertionError("random-orthogonal mix did not round-trip")
    # Tolerance-based: Householder QR's sqrt makes the exact double result
    # path-dependent on operation order, so an independently worked port of
    # the documented algorithm can differ from the production result by a
    # handful of ULPs while both remain correct.
    expected_random_orthogonal_2 = [
        [-0.042407822634498826, -0.9991003836348983],
        [0.9991003836348983, -0.042407822634498715],
    ]
    random_orthogonal_flat = [
        value
        for row in random_orthogonal_step["matrix"]
        for value in row
    ]
    expected_random_orthogonal_flat = [
        value for row in expected_random_orthogonal_2 for value in row
    ]
    if len(random_orthogonal_flat) != len(expected_random_orthogonal_flat) or any(
        abs(actual - expected) > 1e-9
        for actual, expected in zip(
            random_orthogonal_flat, expected_random_orthogonal_flat
        )
    ):
        raise AssertionError(
            f"unexpected RandomOrthogonal matrix: "
            f"{random_orthogonal_step['matrix']}"
        )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            random_orthogonal_result / "resolved.json",
            "--output",
            random_orthogonal_rerender,
        )
    )
    if (random_orthogonal_rerender / "resolved.json").read_bytes() != (
        random_orthogonal_result / "resolved.json"
    ).read_bytes():
        raise AssertionError(
            "resolved RandomOrthogonal rerender changed configuration"
        )
    if (random_orthogonal_rerender / "output.wav").read_bytes() != (
        random_orthogonal_result / "output.wav"
    ).read_bytes():
        raise AssertionError("resolved RandomOrthogonal rerender changed output")

    # A matrix of a given type is shared across steps unless an indexed
    # override selects another type: step 0 keeps the shared Hadamard
    # default, steps 1 and 2 both override to Householder and must resolve
    # to the identical shared Householder(8) matrix.
    matrix_override_request = json.loads(reference_request.read_text())
    matrix_override_request["composition"]["stages"][1]["steps"] = 3
    matrix_override_request["composition"]["stages"][1]["totalMs"] = 3
    matrix_override_request["composition"]["stages"][1]["stepOverrides"] = [
        {"index": 1, "mix": "householder"},
        {"index": 2, "mix": "householder"},
    ]
    matrix_override_config = workspace / "matrix-override-request.json"
    matrix_override_config.write_text(json.dumps(matrix_override_request))
    matrix_override_result = workspace / "matrix-override-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            matrix_override_config,
            "--output",
            matrix_override_result,
        )
    )
    matrix_override_steps = json.loads(
        (matrix_override_result / "resolved.json").read_text()
    )["composition"]["stages"][1]["steps"]
    if matrix_override_steps[0]["matrix"] != expected_hadamard:
        raise AssertionError(
            "step 0 did not keep the shared default Hadamard(8) matrix"
        )
    householder_off_diagonal_8 = -2.0 / 8.0
    householder_diagonal_8 = 1.0 - 0.25
    if (
        matrix_override_steps[1]["matrix"][0][0] != householder_diagonal_8
        or matrix_override_steps[1]["matrix"][0][1] != householder_off_diagonal_8
    ):
        raise AssertionError(
            f"step 1 override did not resolve Householder(8): "
            f"{matrix_override_steps[1]['matrix']}"
        )
    if matrix_override_steps[1]["matrix"] != matrix_override_steps[2]["matrix"]:
        raise AssertionError(
            "overridden steps 1 and 2 did not share the same Householder "
            "matrix"
        )
    if matrix_override_steps[1]["matrix"] == matrix_override_steps[0]["matrix"]:
        raise AssertionError(
            "expected the Householder override to differ from the shared "
            "Hadamard default"
        )

    # RandomOrthogonal validation trusts the resolved coefficients' M M^T=I
    # property, not the seeded construction that produced them: a
    # hand-authored ablation may substitute any valid orthogonal matrix.
    substituted_random_orthogonal = json.loads(
        json.dumps(random_orthogonal_resolved)
    )
    substituted_random_orthogonal["composition"]["stages"][1]["steps"][0][
        "matrix"
    ] = [[0.0, 1.0], [-1.0, 0.0]]
    substituted_random_orthogonal_path = (
        workspace / "substituted-random-orthogonal.json"
    )
    substituted_random_orthogonal_result = (
        workspace / "substituted-random-orthogonal-result"
    )
    substituted_random_orthogonal_path.write_text(
        json.dumps(substituted_random_orthogonal)
    )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            substituted_random_orthogonal_path,
            "--output",
            substituted_random_orthogonal_result,
        )
    )

    invalid_householder = json.loads(json.dumps(householder_resolved))
    invalid_householder["composition"]["stages"][1]["steps"][0]["matrix"][0][
        1
    ] += 0.1
    invalid_householder_path = workspace / "invalid-householder.json"
    invalid_householder_result = workspace / "invalid-householder-result"
    invalid_householder_path.write_text(json.dumps(invalid_householder))
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            invalid_householder_path,
            "--output",
            invalid_householder_result,
        ),
        "/composition/stages/1/steps/0/matrix: "
        "expected the normalized canonical Householder matrix",
        invalid_householder_result,
    )

    invalid_random_orthogonal = json.loads(json.dumps(random_orthogonal_resolved))
    invalid_random_orthogonal["composition"]["stages"][1]["steps"][0]["matrix"] = [
        [1.0, 1.0],
        [0.0, 1.0],
    ]
    invalid_random_orthogonal_path = workspace / "invalid-random-orthogonal.json"
    invalid_random_orthogonal_result = workspace / "invalid-random-orthogonal-result"
    invalid_random_orthogonal_path.write_text(json.dumps(invalid_random_orthogonal))
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            invalid_random_orthogonal_path,
            "--output",
            invalid_random_orthogonal_result,
        ),
        "/composition/stages/1/steps/0/matrix: "
        "expected an orthogonal resolved matrix (M M^T = I)",
        invalid_random_orthogonal_result,
    )

    single_channel_request = workspace / "single-channel-ablation-request.json"
    single_channel_result = workspace / "single-channel-ablation-result"
    single_channel_document = json.loads(ablation_request.read_text())
    single_channel_document["composition"]["stages"][0]["channels"] = 1
    single_channel_document["composition"]["stages"][1]["totalMs"] = 1
    single_channel_request.write_text(
        json.dumps(single_channel_document, indent=2) + "\n"
    )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            single_channel_request,
            "--output",
            single_channel_result,
        )
    )
    single_channel_stages = json.loads(
        (single_channel_result / "resolved.json").read_text()
    )["composition"]["stages"]
    if single_channel_stages[1]["steps"][0]["delaysSamples"] != [0]:
        raise AssertionError("single-Channel even delay is not deterministic")
    if single_channel_stages[2]["compensation"] != 1.0:
        raise AssertionError("single-Channel Downmix none changed select level")
    # N=1: leftChannel 0 is the only valid index, and rightChannel stays
    # omitted (mono duplication), covering the N=1 acceptance scenario.
    if (
        single_channel_stages[2]["leftChannel"] != 0
        or "rightChannel" in single_channel_stages[2]
        or single_channel_stages[2]["leftRow"] != [1.0]
        or single_channel_stages[2]["rightRow"] != [1.0]
    ):
        raise AssertionError(
            f"unexpected single-Channel resolved Downmix: "
            f"{single_channel_stages[2]}"
        )

    single_channel_rerender = workspace / "single-channel-ablation-rerender"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            single_channel_result / "resolved.json",
            "--output",
            single_channel_rerender,
        )
    )
    if (single_channel_rerender / "resolved.json").read_bytes() != (
        single_channel_result / "resolved.json"
    ).read_bytes():
        raise AssertionError("single-Channel resolved rerender changed configuration")
    if (single_channel_rerender / "output.wav").read_bytes() != (
        single_channel_result / "output.wav"
    ).read_bytes():
        raise AssertionError("single-Channel resolved rerender changed output")

    # N=1 under the default `energy` normalisation (the ablation fixture
    # above uses `none`) must resolve the documented 1/sqrt(2) mono
    # duplication compensation, not the N>1 sqrt(N/2) formula.
    single_channel_energy_request = workspace / "single-channel-energy-request.json"
    single_channel_energy_result = workspace / "single-channel-energy-result"
    single_channel_energy_rerender = workspace / "single-channel-energy-rerender"
    single_channel_energy_document = json.loads(reference_request.read_text())
    single_channel_energy_document["composition"]["stages"][0]["channels"] = 1
    del single_channel_energy_document["composition"]["stages"][2]["rightChannel"]
    single_channel_energy_request.write_text(
        json.dumps(single_channel_energy_document, indent=2) + "\n"
    )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            single_channel_energy_request,
            "--output",
            single_channel_energy_result,
        )
    )
    single_channel_energy_downmix = json.loads(
        (single_channel_energy_result / "resolved.json").read_text()
    )["composition"]["stages"][2]
    expected_mono_energy_compensation = 0.7071067811865475
    if (
        single_channel_energy_downmix["normalisation"] != "energy"
        or single_channel_energy_downmix["compensation"]
        != expected_mono_energy_compensation
        or single_channel_energy_downmix["leftRow"] != [1.0]
        or single_channel_energy_downmix["rightRow"] != [1.0]
        or single_channel_energy_downmix["effectiveLeftRow"]
        != [expected_mono_energy_compensation]
        or single_channel_energy_downmix["effectiveRightRow"]
        != [expected_mono_energy_compensation]
        or "rightChannel" in single_channel_energy_downmix
    ):
        raise AssertionError(
            f"unexpected N=1 energy-normalized resolved Downmix: "
            f"{single_channel_energy_downmix}"
        )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            single_channel_energy_result / "resolved.json",
            "--output",
            single_channel_energy_rerender,
        )
    )
    if (single_channel_energy_rerender / "resolved.json").read_bytes() != (
        single_channel_energy_result / "resolved.json"
    ).read_bytes():
        raise AssertionError(
            "N=1 energy-normalized resolved rerender changed configuration"
        )
    if (single_channel_energy_rerender / "output.wav").read_bytes() != (
        single_channel_energy_result / "output.wav"
    ).read_bytes():
        raise AssertionError("N=1 energy-normalized resolved rerender changed output")

    # A Feedback Loop source resolves the Main Downmix's Alignment
    # expectation as "unaligned" (Composition wiring, not Requested
    # configuration, decides this -- issue #107), covering the "unaligned
    # Feedback Loop input" acceptance scenario with two selected Channels.
    feedback_loop_downmix_request = workspace / "feedback-loop-downmix-request.json"
    feedback_loop_downmix_result = workspace / "feedback-loop-downmix-result"
    feedback_loop_downmix_request.write_text(
        json.dumps(
            {
                "formatVersion": 2,
                "composition": {
                    "stages": [
                        {
                            "type": "split",
                            "channels": 2,
                            "strategy": "duplicate",
                            "normalisation": "energy",
                        },
                        {
                            "type": "feedback-loop",
                            "delayMinMs": 1.0,
                            "delayMaxMs": 2.0,
                            "delayStrategy": "even",
                            "rt60Sec": 1.0,
                            "mix": "householder",
                        },
                        {
                            "type": "downmix",
                            "strategy": "select",
                            "leftChannel": 0,
                            "rightChannel": 1,
                        },
                    ]
                },
            }
        )
    )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            feedback_loop_downmix_request,
            "--output",
            feedback_loop_downmix_result,
            "--block-size",
            "32",
        )
    )
    feedback_loop_downmix = json.loads(
        (feedback_loop_downmix_result / "resolved.json").read_text()
    )["composition"]["stages"][2]
    if (
        feedback_loop_downmix["alignment"] != "unaligned"
        or feedback_loop_downmix["leftChannel"] != 0
        or feedback_loop_downmix["rightChannel"] != 1
        or feedback_loop_downmix["leftRow"] != [1.0, 0.0]
        or feedback_loop_downmix["rightRow"] != [0.0, 1.0]
    ):
        raise AssertionError(
            f"unexpected Feedback Loop resolved Downmix: {feedback_loop_downmix}"
        )

    feedback_loop_downmix_rerender = workspace / "feedback-loop-downmix-rerender"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            feedback_loop_downmix_result / "resolved.json",
            "--output",
            feedback_loop_downmix_rerender,
            "--block-size",
            "32",
        )
    )
    if (feedback_loop_downmix_rerender / "resolved.json").read_bytes() != (
        feedback_loop_downmix_result / "resolved.json"
    ).read_bytes():
        raise AssertionError(
            "unaligned Feedback Loop resolved rerender changed configuration"
        )
    if (feedback_loop_downmix_rerender / "output.wav").read_bytes() != (
        feedback_loop_downmix_result / "output.wav"
    ).read_bytes():
        raise AssertionError("unaligned Feedback Loop resolved rerender changed output")

    # A non-default Channel pair (2/3 of N=4, rather than the legacy 0/1)
    # must actually drive the DSP: capture the N-Channel signal entering
    # the Downmix and verify output.wav reads exactly Channels 2 and 3 of
    # it, not the archived implicit pair -- a regression that silently
    # kept reading Channels 0/1 would pass every other Downmix scenario
    # above, since they all happen to select 0 (and 1).
    non_default_channel_request = workspace / "non-default-channel-request.json"
    non_default_channel_result = workspace / "non-default-channel-result"
    non_default_channel_request.write_text(
        json.dumps(
            {
                "formatVersion": 2,
                "seed": 7,
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
                            "steps": 1,
                            "totalMs": 1,
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
                            "leftChannel": 2,
                            "rightChannel": 3,
                            "normalisation": "none",
                        },
                    ]
                },
            }
        )
    )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            non_default_channel_request,
            "--capture-stages",
            "all",
            "--output",
            non_default_channel_result,
        )
    )
    output_channels, output_samples = read_float_wav(
        non_default_channel_result / "output.wav"
    )
    diffusion_channels, diffusion_samples = read_float_wav(
        non_default_channel_result / "captures" / "01-diffusion-step-0.wav"
    )
    if output_channels != 2 or diffusion_channels != 4:
        raise AssertionError(
            f"unexpected Channel counts: output={output_channels}, "
            f"captured diffusion-step={diffusion_channels}"
        )
    frame_count = len(output_samples) // 2
    if len(diffusion_samples) != frame_count * 4:
        raise AssertionError(
            "captured Diffusion Step did not share output.wav's timeline"
        )
    for frame in range(frame_count):
        expected_left = diffusion_samples[frame * 4 + 2]
        expected_right = diffusion_samples[frame * 4 + 3]
        actual_left = output_samples[frame * 2]
        actual_right = output_samples[frame * 2 + 1]
        if actual_left != expected_left or actual_right != expected_right:
            raise AssertionError(
                f"select Downmix did not read the requested non-default "
                f"Channels 2/3 at frame {frame}: "
                f"({actual_left}, {actual_right}) != "
                f"({expected_left}, {expected_right})"
            )

    # orthogonal-rows (#108): the dense branch-specific RandomOrthogonal
    # Main Downmix. Two N values, each a Diffuser-only fixed-total-power
    # fixture (Split's own energy normalisation keeps per-Channel power at
    # 1/N of the fixed impulse energy, matching the design's expected-power
    # fixture), captured with --capture-stages all so the N-Channel signal
    # entering the Downmix is available for spectral evidence. Expected
    # level and Output correlation are measured on a separate same-N
    # Feedback-Loop (unaligned) render instead -- see the comment further
    # below, at the point that second render is built.
    orthogonal_rows_energy_by_channels = {}
    for orthogonal_rows_channels in (4, 8):
        orthogonal_request_path = workspace / (
            f"orthogonal-rows-{orthogonal_rows_channels}-request.json"
        )
        orthogonal_result = workspace / (
            f"orthogonal-rows-{orthogonal_rows_channels}-result"
        )
        orthogonal_rerender = workspace / (
            f"orthogonal-rows-{orthogonal_rows_channels}-rerender"
        )
        orthogonal_request_path.write_text(
            json.dumps(
                {
                    "formatVersion": 2,
                    "seed": 42,
                    "composition": {
                        "stages": [
                            {
                                "type": "split",
                                "channels": orthogonal_rows_channels,
                                "strategy": "duplicate",
                                "normalisation": "energy",
                            },
                            {
                                "type": "diffuser",
                                "steps": 1,
                                "totalMs": 1,
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
                                "strategy": "orthogonal-rows",
                                "normalisation": "energy",
                            },
                        ]
                    },
                }
            )
        )
        require_success(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--config",
                orthogonal_request_path,
                "--capture-stages",
                "all",
                "--output",
                orthogonal_result,
            )
        )
        orthogonal_downmix = json.loads(
            (orthogonal_result / "resolved.json").read_text()
        )["composition"]["stages"][2]
        if (
            "leftChannel" in orthogonal_downmix
            or "rightChannel" in orthogonal_downmix
        ):
            raise AssertionError(
                f"orthogonal-rows resolved a Channel selection it has no "
                f"use for: {orthogonal_downmix}"
            )
        expected_compensation = math.sqrt(orthogonal_rows_channels / 2.0)
        if (
            orthogonal_downmix["strategy"] != "orthogonal-rows"
            or abs(
                orthogonal_downmix["compensation"] - expected_compensation
            )
            > 1e-9
            or len(orthogonal_downmix["leftRow"]) != orthogonal_rows_channels
            or len(orthogonal_downmix["rightRow"]) != orthogonal_rows_channels
            or orthogonal_downmix["alignment"] != "aligned"
        ):
            raise AssertionError(
                f"unexpected orthogonal-rows resolved Downmix: "
                f"{orthogonal_downmix}"
            )
        left_row = orthogonal_downmix["leftRow"]
        right_row = orthogonal_downmix["rightRow"]

        def row_dot(a, b):
            return math.fsum(x * y for x, y in zip(a, b))

        if (
            abs(row_dot(left_row, left_row) - 1.0) > 1e-9
            or abs(row_dot(right_row, right_row) - 1.0) > 1e-9
            or abs(row_dot(left_row, right_row)) > 1e-9
        ):
            raise AssertionError(
                f"orthogonal-rows leftRow/rightRow are not orthonormal: "
                f"{orthogonal_downmix}"
            )
        for row_name, row, effective_row_name in (
            ("leftRow", left_row, "effectiveLeftRow"),
            ("rightRow", right_row, "effectiveRightRow"),
        ):
            expected_effective_row = [
                value * expected_compensation for value in row
            ]
            actual_effective_row = orthogonal_downmix[effective_row_name]
            if any(
                abs(actual - expected) > 1e-9
                for actual, expected in zip(
                    actual_effective_row, expected_effective_row
                )
            ):
                raise AssertionError(
                    f"{effective_row_name} did not match {row_name} scaled "
                    f"by compensation: {orthogonal_downmix}"
                )

        require_success(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--resolved",
                orthogonal_result / "resolved.json",
                "--output",
                orthogonal_rerender,
            )
        )
        if (orthogonal_rerender / "resolved.json").read_bytes() != (
            orthogonal_result / "resolved.json"
        ).read_bytes():
            raise AssertionError(
                "orthogonal-rows resolved rerender changed configuration"
            )
        if (orthogonal_rerender / "output.wav").read_bytes() != (
            orthogonal_result / "output.wav"
        ).read_bytes():
            raise AssertionError("orthogonal-rows resolved rerender changed output")

        output_channels, output_samples = read_float_wav(
            orthogonal_result / "output.wav"
        )
        diffusion_channels, diffusion_samples = read_float_wav(
            orthogonal_result / "captures" / "01-diffusion-step-0.wav"
        )
        if output_channels != 2 or diffusion_channels != orthogonal_rows_channels:
            raise AssertionError(
                f"unexpected Channel counts at N={orthogonal_rows_channels}: "
                f"output={output_channels}, "
                f"captured diffusion-step={diffusion_channels}"
            )
        left, right = deinterleave(2, output_samples)
        source_channels = deinterleave(
            orthogonal_rows_channels, diffusion_samples
        )

        # Expected level independence and Output correlation are defined
        # over seeded *unaligned* fixtures (docs/design/reverb/stages/
        # 08-downmix.md: "Tests use seeded unaligned fixtures" and the
        # Invariants section's "Expected level independence"), so those
        # two are measured on a same-N, same-strategy Feedback-Loop
        # (unaligned) render rather than the Diffuser-only (aligned)
        # render above -- an aligned source's shared onset would let
        # correlated interference confound both measurements. Spectral
        # evidence stays on the aligned render above: the diffusion-step
        # capture it depends on has no Feedback-Loop equivalent (see
        # dsp::StageCaptureBoundary, which only captures split/
        # diffusion-step boundaries), and the "Spectral evidence"
        # invariant does not itself specify unaligned input.
        orthogonal_unaligned_request_path = workspace / (
            f"orthogonal-rows-{orthogonal_rows_channels}-unaligned-"
            f"request.json"
        )
        orthogonal_unaligned_document = json.loads(
            orthogonal_request_path.read_text()
        )
        orthogonal_unaligned_document["composition"]["stages"][1] = {
            "type": "feedback-loop",
        }
        orthogonal_unaligned_request_path.write_text(
            json.dumps(orthogonal_unaligned_document)
        )
        orthogonal_unaligned_result = workspace / (
            f"orthogonal-rows-{orthogonal_rows_channels}-unaligned-result"
        )
        require_success(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--config",
                orthogonal_unaligned_request_path,
                "--output",
                orthogonal_unaligned_result,
            )
        )
        orthogonal_unaligned_downmix = json.loads(
            (orthogonal_unaligned_result / "resolved.json").read_text()
        )["composition"]["stages"][2]
        if orthogonal_unaligned_downmix["alignment"] != "unaligned":
            raise AssertionError(
                f"orthogonal-rows Feedback Loop Downmix did not resolve "
                f"unaligned: {orthogonal_unaligned_downmix}"
            )
        _, unaligned_output_samples = read_float_wav(
            orthogonal_unaligned_result / "output.wav"
        )
        unaligned_left, unaligned_right = deinterleave(
            2, unaligned_output_samples
        )

        # Expected level: total downmix output energy, compared across N
        # below (compensation is designed to keep it roughly independent of
        # N under this fixed-total-power fixture -- an expected-power
        # contract, not exact per-instance equality; see issue #108 and
        # docs/design/reverb/stages/08-downmix.md).
        output_energy = math.fsum(
            v * v for v in unaligned_left
        ) + math.fsum(v * v for v in unaligned_right)
        orthogonal_rows_energy_by_channels[orthogonal_rows_channels] = (
            output_energy
        )

        # Output correlation: finite and within the mathematically valid
        # range, reported rather than gated against an acoustic threshold
        # (no universal pass/fail on decorrelation -- see the parent spec's
        # Out of Scope).
        correlation = zero_lag_correlation(unaligned_left, unaligned_right)
        if not math.isfinite(correlation) or abs(correlation) > 1.0 + 1e-9:
            raise AssertionError(
                f"orthogonal-rows Output correlation is not a valid "
                f"correlation coefficient at N={orthogonal_rows_channels}: "
                f"{correlation}"
            )

        # Spectral evidence: the downmixed L/R power spectra against the
        # same N-Channel source's aggregate power spectrum, reported as
        # max/RMS band deviation -- again exposed as evidence, not an
        # acoustic pass/fail (docs/design/reverb/stages/08-downmix.md's own
        # "Worth sweeping early" and Invariants sections). Measured on the
        # aligned render (see the capture-boundary note above).
        aggregate_source_power = [0.0] * (len(left) // 2 + 1)
        for channel_samples in source_channels:
            channel_power = dft_power_spectrum(channel_samples)
            aggregate_source_power = [
                total + value
                for total, value in zip(aggregate_source_power, channel_power)
            ]
        max_deviation, rms_deviation = spectral_deviation_evidence(
            left, right, aggregate_source_power, 48000
        )
        if not (math.isfinite(max_deviation) and math.isfinite(rms_deviation)):
            raise AssertionError(
                f"orthogonal-rows spectral evidence did not resolve to "
                f"finite max/RMS band deviation at N="
                f"{orthogonal_rows_channels}"
            )

        # Persist the evidence this fixture exists to gather (issue #108,
        # docs/design/reverb/stages/08-downmix.md's "Decorrelation
        # evidence" and "Spectral evidence") -- otherwise it is computed
        # and immediately discarded, leaving nothing for a human to read
        # even though every threshold above is deliberately non-gating.
        orthogonal_evidence_path = workspace / (
            f"orthogonal-rows-{orthogonal_rows_channels}-evidence.json"
        )
        orthogonal_evidence_path.write_text(
            json.dumps(
                {
                    "channels": orthogonal_rows_channels,
                    "outputEnergy": output_energy,
                    "outputCorrelation": correlation,
                    "spectralMaxDeviation": max_deviation,
                    "spectralRmsDeviation": rms_deviation,
                },
                indent=2,
            )
        )

    # Expected-level independence across N (within a generous tolerance --
    # an expected-power contract over a single seeded realization, not
    # exact equality; see docs/design/reverb/stages/08-downmix.md).
    energies = list(orthogonal_rows_energy_by_channels.values())
    # A silent Downmix (energies all 0.0) would otherwise pass the ratio
    # check below vacuously -- 0.0 > 3.0 * 0.0 is false -- so require each
    # seeded fixture to have actually produced a nonzero expected level
    # before comparing their ratio across N.
    if any(energy <= 0.0 for energy in energies):
        raise AssertionError(
            f"orthogonal-rows output energy was not positive: "
            f"{orthogonal_rows_energy_by_channels}"
        )
    if max(energies) > 3.0 * min(energies):
        raise AssertionError(
            f"orthogonal-rows output energy was not roughly level-"
            f"independent of N: {orthogonal_rows_energy_by_channels}"
        )

    # normalisation: "none" omits only the common compensation scalar --
    # rows stay the same unit-norm intrinsic rows as under "energy", but
    # compensation and the effective rows collapse to 1.0/row itself.
    orthogonal_none_document = json.loads(orthogonal_request_path.read_text())
    orthogonal_none_document["composition"]["stages"][2][
        "normalisation"
    ] = "none"
    orthogonal_none_request = workspace / "orthogonal-rows-none-request.json"
    orthogonal_none_request.write_text(json.dumps(orthogonal_none_document))
    orthogonal_none_result = workspace / "orthogonal-rows-none-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            orthogonal_none_request,
            "--output",
            orthogonal_none_result,
        )
    )
    orthogonal_none_downmix = json.loads(
        (orthogonal_none_result / "resolved.json").read_text()
    )["composition"]["stages"][2]
    if (
        orthogonal_none_downmix["normalisation"] != "none"
        or orthogonal_none_downmix["compensation"] != 1.0
        or orthogonal_none_downmix["leftRow"] != orthogonal_none_downmix["effectiveLeftRow"]
        or orthogonal_none_downmix["rightRow"] != orthogonal_none_downmix["effectiveRightRow"]
    ):
        raise AssertionError(
            f"unexpected orthogonal-rows none-normalisation resolved "
            f"Downmix: {orthogonal_none_downmix}"
        )

    # orthogonal-rows requires N >= 2: no row 1 exists at N=1.
    orthogonal_single_channel_document = json.loads(
        orthogonal_request_path.read_text()
    )
    orthogonal_single_channel_document["composition"]["stages"][0][
        "channels"
    ] = 1
    orthogonal_single_channel_request = (
        workspace / "orthogonal-rows-single-channel-request.json"
    )
    orthogonal_single_channel_request.write_text(
        json.dumps(orthogonal_single_channel_document)
    )
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            orthogonal_single_channel_request,
            "--output",
            workspace / "orthogonal-rows-single-channel-result",
        ),
        "/composition/stages/2/strategy: "
        "orthogonal-rows requires at least two Channels",
        workspace / "orthogonal-rows-single-channel-result",
    )

    # leftChannel/rightChannel are `select`-specific: providing either
    # alongside orthogonal-rows is rejected, not silently ignored.
    orthogonal_with_left_channel_document = json.loads(
        orthogonal_request_path.read_text()
    )
    orthogonal_with_left_channel_document["composition"]["stages"][2][
        "leftChannel"
    ] = 0
    orthogonal_with_left_channel_request = (
        workspace / "orthogonal-rows-with-left-channel-request.json"
    )
    orthogonal_with_left_channel_request.write_text(
        json.dumps(orthogonal_with_left_channel_document)
    )
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            orthogonal_with_left_channel_request,
            "--output",
            workspace / "orthogonal-rows-with-left-channel-result",
        ),
        "/composition/stages/2/leftChannel: "
        "not applicable to strategy orthogonal-rows",
        workspace / "orthogonal-rows-with-left-channel-result",
    )

    # halves/alternating (#110): equal-coefficient disjoint Channel-group
    # Main Downmixes. Diffuser-only (aligned) fixed-total-power fixtures at
    # even and odd N verify row/compensation/replay and the "aligned"
    # Alignment expectation; a same-N Feedback-Loop (unaligned) fixture
    # supplies the expected-level/correlation/level-difference evidence
    # (see the comment further below, at the point that render is built,
    # for why) -- see docs/design/reverb/stages/08-downmix.md.
    group_strategies = {
        "halves": halves_row,
        "alternating": alternating_row,
    }
    for strategy_name, expected_row_fn in group_strategies.items():
        group_energy_by_channels = {}
        for group_channels in (4, 5, 8):
            group_request_path = workspace / (
                f"{strategy_name}-{group_channels}-request.json"
            )
            group_result = (
                workspace / f"{strategy_name}-{group_channels}-result"
            )
            group_rerender = (
                workspace / f"{strategy_name}-{group_channels}-rerender"
            )
            group_request_path.write_text(
                json.dumps(
                    {
                        "formatVersion": 2,
                        "seed": 42,
                        "composition": {
                            "stages": [
                                {
                                    "type": "split",
                                    "channels": group_channels,
                                    "strategy": "duplicate",
                                    "normalisation": "energy",
                                },
                                {
                                    "type": "diffuser",
                                    "steps": 1,
                                    "totalMs": 1,
                                    "distribution": "even",
                                    "step": {
                                        "delayStrategy": "segmented-random",
                                        "mix": "householder",
                                        "shuffle": True,
                                        "polarity": "seeded-random",
                                    },
                                },
                                {
                                    "type": "downmix",
                                    "strategy": strategy_name,
                                    "normalisation": "energy",
                                },
                            ]
                        },
                    }
                )
            )
            require_success(
                run_renderer(
                    renderer,
                    "--input",
                    fixture,
                    "--config",
                    group_request_path,
                    "--output",
                    group_result,
                )
            )
            group_downmix = json.loads(
                (group_result / "resolved.json").read_text()
            )["composition"]["stages"][2]
            if (
                "leftChannel" in group_downmix
                or "rightChannel" in group_downmix
            ):
                raise AssertionError(
                    f"{strategy_name} resolved a Channel selection it has "
                    f"no use for: {group_downmix}"
                )
            expected_compensation = math.sqrt(group_channels / 2.0)
            if (
                group_downmix["strategy"] != strategy_name
                or abs(group_downmix["compensation"] - expected_compensation)
                > 1e-9
                or group_downmix["alignment"] != "aligned"
            ):
                raise AssertionError(
                    f"unexpected {strategy_name} resolved Downmix: "
                    f"{group_downmix}"
                )
            expected_left_row = expected_row_fn(group_channels, True)
            expected_right_row = expected_row_fn(group_channels, False)
            for row_name, actual_row, expected_row in (
                ("leftRow", group_downmix["leftRow"], expected_left_row),
                ("rightRow", group_downmix["rightRow"], expected_right_row),
            ):
                if any(
                    abs(actual - expected) > 1e-9
                    for actual, expected in zip(actual_row, expected_row)
                ):
                    raise AssertionError(
                        f"{strategy_name} {row_name} did not match the "
                        f"expected equal-coefficient group at N="
                        f"{group_channels}: {group_downmix}"
                    )
            for row_name, expected_row, effective_row_name in (
                ("leftRow", expected_left_row, "effectiveLeftRow"),
                ("rightRow", expected_right_row, "effectiveRightRow"),
            ):
                expected_effective_row = [
                    value * expected_compensation for value in expected_row
                ]
                actual_effective_row = group_downmix[effective_row_name]
                if any(
                    abs(actual - expected) > 1e-9
                    for actual, expected in zip(
                        actual_effective_row, expected_effective_row
                    )
                ):
                    raise AssertionError(
                        f"{strategy_name} {effective_row_name} did not "
                        f"match {row_name} scaled by compensation: "
                        f"{group_downmix}"
                    )

            require_success(
                run_renderer(
                    renderer,
                    "--input",
                    fixture,
                    "--resolved",
                    group_result / "resolved.json",
                    "--output",
                    group_rerender,
                )
            )
            if (group_rerender / "resolved.json").read_bytes() != (
                group_result / "resolved.json"
            ).read_bytes():
                raise AssertionError(
                    f"{strategy_name} resolved rerender changed "
                    f"configuration at N={group_channels}"
                )
            if (group_rerender / "output.wav").read_bytes() != (
                group_result / "output.wav"
            ).read_bytes():
                raise AssertionError(
                    f"{strategy_name} resolved rerender changed output at "
                    f"N={group_channels}"
                )

            output_channels, output_samples = read_float_wav(
                group_result / "output.wav"
            )
            if output_channels != 2:
                raise AssertionError(
                    f"unexpected output Channel count for {strategy_name} "
                    f"at N={group_channels}: {output_channels}"
                )

            # Expected level independence and Output correlation are
            # defined over seeded *unaligned* fixtures (docs/design/
            # reverb/stages/08-downmix.md: "Tests use seeded unaligned
            # fixtures" and the Invariants section's "Expected level
            # independence"), so both are measured on a same-N,
            # same-strategy Feedback-Loop (unaligned) render rather than
            # the Diffuser-only (aligned) render above -- an aligned
            # source's shared onset would let correlated interference
            # confound both measurements.
            unaligned_document = json.loads(group_request_path.read_text())
            unaligned_document["composition"]["stages"][1] = {
                "type": "feedback-loop",
            }
            unaligned_request = workspace / (
                f"{strategy_name}-{group_channels}-unaligned-request.json"
            )
            unaligned_request.write_text(json.dumps(unaligned_document))
            unaligned_result = workspace / (
                f"{strategy_name}-{group_channels}-unaligned-result"
            )
            require_success(
                run_renderer(
                    renderer,
                    "--input",
                    fixture,
                    "--config",
                    unaligned_request,
                    "--output",
                    unaligned_result,
                )
            )
            unaligned_downmix = json.loads(
                (unaligned_result / "resolved.json").read_text()
            )["composition"]["stages"][2]
            if unaligned_downmix["alignment"] != "unaligned":
                raise AssertionError(
                    f"{strategy_name} Feedback Loop Downmix did not "
                    f"resolve unaligned at N={group_channels}: "
                    f"{unaligned_downmix}"
                )
            _, unaligned_output_samples = read_float_wav(
                unaligned_result / "output.wav"
            )
            left, right = deinterleave(2, unaligned_output_samples)

            # Expected level: total downmix output energy, compared across
            # N below (an expected-power contract, not exact per-instance
            # equality; see issue #110 and
            # docs/design/reverb/stages/08-downmix.md).
            output_energy = math.fsum(v * v for v in left) + math.fsum(
                v * v for v in right
            )
            group_energy_by_channels[group_channels] = output_energy

            # Output correlation and inter-channel level difference:
            # finite and reported, not gated against an acoustic threshold
            # (issue #110's "no universal pass/fail on decorrelation").
            correlation = zero_lag_correlation(left, right)
            level_difference_db = inter_channel_level_difference_db(
                left, right
            )
            if (
                not math.isfinite(correlation)
                or abs(correlation) > 1.0 + 1e-9
            ):
                raise AssertionError(
                    f"{strategy_name} Output correlation is not a valid "
                    f"correlation coefficient at N={group_channels}: "
                    f"{correlation}"
                )
            if not math.isfinite(level_difference_db):
                raise AssertionError(
                    f"{strategy_name} inter-channel level difference is "
                    f"not finite at N={group_channels}: "
                    f"{level_difference_db}"
                )
            evidence_path = workspace / (
                f"{strategy_name}-{group_channels}-evidence.json"
            )
            evidence_path.write_text(
                json.dumps(
                    {
                        "channels": group_channels,
                        "outputEnergy": output_energy,
                        "outputCorrelation": correlation,
                        "interChannelLevelDifferenceDb": level_difference_db,
                    },
                    indent=2,
                )
            )

        group_energies = list(group_energy_by_channels.values())
        # A silent Downmix would otherwise pass the ratio check below
        # vacuously (see the analogous orthogonal-rows fix, issue #119).
        if any(energy <= 0.0 for energy in group_energies):
            raise AssertionError(
                f"{strategy_name} output energy was not positive: "
                f"{group_energy_by_channels}"
            )
        if max(group_energies) > 3.0 * min(group_energies):
            raise AssertionError(
                f"{strategy_name} output energy was not roughly "
                f"level-independent of N: {group_energy_by_channels}"
            )

        # normalisation: "none" omits only the common compensation scalar
        # -- rows stay the same unit-norm intrinsic rows as under
        # "energy", but compensation and the effective rows collapse to
        # 1.0/row itself.
        none_document = json.loads(group_request_path.read_text())
        none_document["composition"]["stages"][2]["normalisation"] = "none"
        none_request = workspace / f"{strategy_name}-none-request.json"
        none_request.write_text(json.dumps(none_document))
        none_result = workspace / f"{strategy_name}-none-result"
        require_success(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--config",
                none_request,
                "--output",
                none_result,
            )
        )
        none_downmix = json.loads(
            (none_result / "resolved.json").read_text()
        )["composition"]["stages"][2]
        if (
            none_downmix["normalisation"] != "none"
            or none_downmix["compensation"] != 1.0
            or none_downmix["leftRow"] != none_downmix["effectiveLeftRow"]
            or none_downmix["rightRow"] != none_downmix["effectiveRightRow"]
        ):
            raise AssertionError(
                f"unexpected {strategy_name} none-normalisation resolved "
                f"Downmix: {none_downmix}"
            )

        # Both strategies require N >= 2 (#110).
        single_channel_document = json.loads(group_request_path.read_text())
        single_channel_document["composition"]["stages"][0][
            "channels"
        ] = 1
        single_channel_request = (
            workspace / f"{strategy_name}-single-channel-request.json"
        )
        single_channel_request.write_text(
            json.dumps(single_channel_document)
        )
        require_failure(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--config",
                single_channel_request,
                "--output",
                workspace / f"{strategy_name}-single-channel-result",
            ),
            f"/composition/stages/2/strategy: "
            f"{strategy_name} requires at least two Channels",
            workspace / f"{strategy_name}-single-channel-result",
        )

        # leftChannel/rightChannel are `select`-specific: providing either
        # alongside halves/alternating is rejected, not silently ignored.
        with_left_channel_document = json.loads(
            group_request_path.read_text()
        )
        with_left_channel_document["composition"]["stages"][2][
            "leftChannel"
        ] = 0
        with_left_channel_request = (
            workspace / f"{strategy_name}-with-left-channel-request.json"
        )
        with_left_channel_request.write_text(
            json.dumps(with_left_channel_document)
        )
        require_failure(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--config",
                with_left_channel_request,
                "--output",
                workspace / f"{strategy_name}-with-left-channel-result",
            ),
            f"/composition/stages/2/leftChannel: "
            f"not applicable to strategy {strategy_name}",
            workspace / f"{strategy_name}-with-left-channel-result",
        )

    # `sum-all` (#114): the diagnostic Coherent Downmix ablation -- the
    # same `1/sqrt(N)` row duplicated to both L and R. Unlike halves/
    # alternating/orthogonal-rows, it supports N>=1 like `select`
    # (docs/design/reverb/stages/08-downmix.md), so N=1 is exercised
    # directly here rather than rejected. Each Diffuser-only (aligned) N
    # fixture verifies rows/compensation/alignment/replay, an Alignment
    # score measured on its own captured Diffusion Step, a Coherent
    # Downmix ablation tag derived from strategy *and* resolved alignment
    # together (never the strategy name alone), and mono fold-down/peak
    # factor/spectral deviation evidence. N=4 also renders a matched
    # `select` control to compare peak factor and spectral deviation as a
    # non-fatal warning; the other N values instead demonstrate that the
    # comparison is reported unavailable absent a control.
    for sum_all_channels in (1, 4, 8):
        sum_all_request_path = workspace / (
            f"sum-all-{sum_all_channels}-request.json"
        )
        sum_all_result = workspace / f"sum-all-{sum_all_channels}-result"
        sum_all_rerender = workspace / f"sum-all-{sum_all_channels}-rerender"
        sum_all_request_path.write_text(
            json.dumps(
                {
                    "formatVersion": 2,
                    "seed": 42,
                    "composition": {
                        "stages": [
                            {
                                "type": "split",
                                "channels": sum_all_channels,
                                "strategy": "duplicate",
                                "normalisation": "energy",
                            },
                            {
                                "type": "diffuser",
                                "steps": 1,
                                "totalMs": 1,
                                "distribution": "even",
                                "step": {
                                    "delayStrategy": "segmented-random",
                                    # Householder (unlike the default
                                    # Hadamard) is valid at N=1.
                                    "mix": "householder",
                                    "shuffle": True,
                                    "polarity": "seeded-random",
                                },
                            },
                            {
                                "type": "downmix",
                                "strategy": "sum-all",
                                "normalisation": "energy",
                            },
                        ]
                    },
                }
            )
        )
        require_success(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--config",
                sum_all_request_path,
                "--capture-stages",
                "all",
                "--output",
                sum_all_result,
            )
        )
        sum_all_downmix = json.loads(
            (sum_all_result / "resolved.json").read_text()
        )["composition"]["stages"][2]
        if "leftChannel" in sum_all_downmix or "rightChannel" in sum_all_downmix:
            raise AssertionError(
                f"sum-all resolved a Channel selection it has no use for: "
                f"{sum_all_downmix}"
            )
        expected_compensation = (
            1.0 / math.sqrt(2.0)
            if sum_all_channels == 1
            else math.sqrt(sum_all_channels / 2.0)
        )
        if (
            sum_all_downmix["strategy"] != "sum-all"
            or abs(sum_all_downmix["compensation"] - expected_compensation)
            > 1e-9
            or sum_all_downmix["alignment"] != "aligned"
        ):
            raise AssertionError(
                f"unexpected sum-all resolved Downmix at N="
                f"{sum_all_channels}: {sum_all_downmix}"
            )
        expected_row = sum_all_row(sum_all_channels)
        for row_name in ("leftRow", "rightRow"):
            actual_row = sum_all_downmix[row_name]
            if any(
                abs(actual - expected) > 1e-9
                for actual, expected in zip(actual_row, expected_row)
            ):
                raise AssertionError(
                    f"sum-all {row_name} did not match the expected "
                    f"duplicated 1/sqrt(N) row at N={sum_all_channels}: "
                    f"{sum_all_downmix}"
                )
        if sum_all_downmix["leftRow"] != sum_all_downmix["rightRow"]:
            raise AssertionError(
                f"sum-all leftRow/rightRow were not the same duplicated "
                f"row at N={sum_all_channels}: {sum_all_downmix}"
            )
        for row_name, effective_row_name in (
            ("leftRow", "effectiveLeftRow"),
            ("rightRow", "effectiveRightRow"),
        ):
            expected_effective_row = [
                value * expected_compensation
                for value in sum_all_downmix[row_name]
            ]
            actual_effective_row = sum_all_downmix[effective_row_name]
            if any(
                abs(actual - expected) > 1e-9
                for actual, expected in zip(
                    actual_effective_row, expected_effective_row
                )
            ):
                raise AssertionError(
                    f"{effective_row_name} did not match {row_name} scaled "
                    f"by compensation at N={sum_all_channels}: "
                    f"{sum_all_downmix}"
                )

        # A Coherent Downmix ablation tag (issue #114): resolved.json's
        # own `coherentDownmixAblation` field (production code --
        # ResolveConfig.cpp's resolveCoherentDownmixAblation, derived from
        # strategy *and* resolved Alignment together, never the strategy
        # name alone) is asserted directly here, not re-derived from
        # strategy/alignment inside the test, so this genuinely exercises
        # the emitted artifact rather than a tautology. Given the
        # strategy/alignment checks above already confirmed "sum-all"/
        # "aligned", the field must be True; the matching Feedback-Loop
        # fixture below confirms the *same* emitted field is False once
        # the source is unaligned.
        if sum_all_downmix["coherentDownmixAblation"] is not True:
            raise AssertionError(
                f"aligned sum-all's own coherentDownmixAblation field was "
                f"not True at N={sum_all_channels}: {sum_all_downmix}"
            )

        require_success(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--resolved",
                sum_all_result / "resolved.json",
                "--output",
                sum_all_rerender,
            )
        )
        if (sum_all_rerender / "resolved.json").read_bytes() != (
            sum_all_result / "resolved.json"
        ).read_bytes():
            raise AssertionError(
                f"sum-all resolved rerender changed configuration at N="
                f"{sum_all_channels}"
            )
        if (sum_all_rerender / "output.wav").read_bytes() != (
            sum_all_result / "output.wav"
        ).read_bytes():
            raise AssertionError(
                f"sum-all resolved rerender changed output at N="
                f"{sum_all_channels}"
            )

        output_channels, output_samples = read_float_wav(
            sum_all_result / "output.wav"
        )
        diffusion_channels, diffusion_samples = read_float_wav(
            sum_all_result / "captures" / "01-diffusion-step-0.wav"
        )
        if output_channels != 2 or diffusion_channels != sum_all_channels:
            raise AssertionError(
                f"unexpected Channel counts at N={sum_all_channels}: "
                f"output={output_channels}, "
                f"captured diffusion-step={diffusion_channels}"
            )
        left, right = deinterleave(2, output_samples)
        source_channels = deinterleave(sum_all_channels, diffusion_samples)

        # Alignment score (#114): measured on the captured Diffusion Step
        # feeding this Downmix, independent of Downmix strategy itself --
        # reported alongside the Alignment expectation above rather than
        # conflated with it (CONTEXT.md's "Alignment score" entry).
        score = alignment_score(source_channels)
        if not (
            0.0 <= score["mean"] <= 1.0 and 0.0 <= score["minimum"] <= 1.0
        ):
            raise AssertionError(
                f"sum-all Alignment score is not a valid Jaccard overlap "
                f"at N={sum_all_channels}: {score}"
            )

        fold_down = mono_fold_down_evidence(left, right)
        if not math.isfinite(fold_down["foldedEnergyRatio"]):
            raise AssertionError(
                f"sum-all mono fold-down ratio is not finite at N="
                f"{sum_all_channels}: {fold_down}"
            )
        sum_all_peak_factor = peak_factor(list(left) + list(right))
        if not math.isfinite(sum_all_peak_factor):
            raise AssertionError(
                f"sum-all peak factor is not finite at N={sum_all_channels}: "
                f"{sum_all_peak_factor}"
            )
        aggregate_source_power = [0.0] * (len(left) // 2 + 1)
        for channel_samples in source_channels:
            channel_power = dft_power_spectrum(channel_samples)
            aggregate_source_power = [
                total + value
                for total, value in zip(aggregate_source_power, channel_power)
            ]
        sum_all_max_deviation, sum_all_rms_deviation = (
            spectral_deviation_evidence(
                left, right, aggregate_source_power, 48000
            )
        )
        if not (
            math.isfinite(sum_all_max_deviation)
            and math.isfinite(sum_all_rms_deviation)
        ):
            raise AssertionError(
                f"sum-all spectral deviation is not finite at N="
                f"{sum_all_channels}: {sum_all_max_deviation}, "
                f"{sum_all_rms_deviation}"
            )

        evidence = {
            "channels": sum_all_channels,
            "alignmentExpectation": sum_all_downmix["alignment"],
            "alignmentScore": score,
            "coherentDownmixAblation": sum_all_downmix["coherentDownmixAblation"],
            "monoFoldDown": fold_down,
            "peakFactor": sum_all_peak_factor,
            "spectralMaxDeviation": sum_all_max_deviation,
            "spectralRmsDeviation": sum_all_rms_deviation,
        }

        # A matched `select` control (issue #114): rendered from the same
        # Diffuser-only Composition and seed, differing only in Downmix
        # strategy, so peak factor and spectral deviation can be compared
        # directly. Exercised once (N=4) to keep the render count
        # bounded; the other N values instead demonstrate the AC's own
        # "absent a control, the report states that comparison is
        # unavailable".
        if sum_all_channels == 4:
            control_document = json.loads(sum_all_request_path.read_text())
            control_document["composition"]["stages"][2] = {
                "type": "downmix",
                "strategy": "select",
                "leftChannel": 0,
                "rightChannel": 1,
                "normalisation": "energy",
            }
            control_request = workspace / "sum-all-4-control-request.json"
            control_request.write_text(json.dumps(control_document))
            control_result = workspace / "sum-all-4-control-result"
            require_success(
                run_renderer(
                    renderer,
                    "--input",
                    fixture,
                    "--config",
                    control_request,
                    "--capture-stages",
                    "all",
                    "--output",
                    control_result,
                )
            )
            control_downmix = json.loads(
                (control_result / "resolved.json").read_text()
            )["composition"]["stages"][2]
            if control_downmix["strategy"] != "select":
                raise AssertionError(
                    f"matched control did not resolve select: "
                    f"{control_downmix}"
                )
            _, control_output_samples = read_float_wav(
                control_result / "output.wav"
            )
            control_left, control_right = deinterleave(
                2, control_output_samples
            )
            control_peak_factor = peak_factor(
                list(control_left) + list(control_right)
            )
            control_max_deviation, control_rms_deviation = (
                spectral_deviation_evidence(
                    control_left, control_right, aggregate_source_power, 48000
                )
            )
            if not (
                math.isfinite(control_peak_factor)
                and math.isfinite(control_max_deviation)
                and math.isfinite(control_rms_deviation)
            ):
                raise AssertionError(
                    f"matched select control evidence is not finite: "
                    f"{control_peak_factor}, {control_max_deviation}, "
                    f"{control_rms_deviation}"
                )
            evidence["comparativeControl"] = {
                "available": True,
                "controlStrategy": "select",
                "peakFactorDelta": sum_all_peak_factor - control_peak_factor,
                "spectralMaxDeviationDelta": (
                    sum_all_max_deviation - control_max_deviation
                ),
                "spectralRmsDeviationDelta": (
                    sum_all_rms_deviation - control_rms_deviation
                ),
                # Non-fatal: a comparative warning, never a Render Result
                # rejection (issue #114's "No acoustic measurement or
                # warning rejects the Render Result").
                "warning": (
                    "aligned sum-all coherently reinforces Channels that a "
                    "matched select control keeps separate; compare peak "
                    "factor and spectral deviation before treating this "
                    "as a defect"
                ),
            }
        else:
            evidence["comparativeControl"] = {
                "available": False,
                "reason": "no matched select control provided",
            }

        evidence_path = workspace / f"sum-all-{sum_all_channels}-evidence.json"
        evidence_path.write_text(json.dumps(evidence, indent=2))

    # normalisation: "none" omits only the common compensation scalar --
    # rows stay the same unit-norm intrinsic rows as under "energy", but
    # compensation and the effective rows collapse to 1.0/row itself
    # (#114, matching orthogonal-rows/halves/alternating's own contract
    # above). Uses the last loop iteration's N=8 fixture.
    sum_all_none_document = json.loads(sum_all_request_path.read_text())
    sum_all_none_document["composition"]["stages"][2]["normalisation"] = "none"
    sum_all_none_request = workspace / "sum-all-none-request.json"
    sum_all_none_request.write_text(json.dumps(sum_all_none_document))
    sum_all_none_result = workspace / "sum-all-none-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            sum_all_none_request,
            "--output",
            sum_all_none_result,
        )
    )
    sum_all_none_downmix = json.loads(
        (sum_all_none_result / "resolved.json").read_text()
    )["composition"]["stages"][2]
    if (
        sum_all_none_downmix["normalisation"] != "none"
        or sum_all_none_downmix["compensation"] != 1.0
        or sum_all_none_downmix["leftRow"] != sum_all_none_downmix["effectiveLeftRow"]
        or sum_all_none_downmix["rightRow"] != sum_all_none_downmix["effectiveRightRow"]
    ):
        raise AssertionError(
            f"unexpected sum-all none-normalisation resolved Downmix: "
            f"{sum_all_none_downmix}"
        )

    # leftChannel/rightChannel are select-specific: providing either
    # alongside sum-all is rejected, not silently ignored (#114, matching
    # halves/alternating's own contract above).
    sum_all_with_left_channel_document = json.loads(
        (workspace / "sum-all-4-request.json").read_text()
    )
    sum_all_with_left_channel_document["composition"]["stages"][2][
        "leftChannel"
    ] = 0
    sum_all_with_left_channel_request = (
        workspace / "sum-all-with-left-channel-request.json"
    )
    sum_all_with_left_channel_request.write_text(
        json.dumps(sum_all_with_left_channel_document)
    )
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            sum_all_with_left_channel_request,
            "--output",
            workspace / "sum-all-with-left-channel-result",
        ),
        "/composition/stages/2/leftChannel: not applicable to strategy sum-all",
        workspace / "sum-all-with-left-channel-result",
    )

    # sum-all through a Feedback Loop (#114): Alignment expectation is
    # derived from Composition wiring, so a source that includes a
    # Feedback Loop must resolve unaligned, and the same strategy name
    # must then *not* be tagged as a Coherent Downmix ablation -- proving
    # the tag is never derived from the strategy name alone. This
    # unaligned fixture also supplies Output correlation and
    # inter-channel level difference evidence (docs/design/reverb/
    # stages/08-downmix.md: "Tests use seeded unaligned fixtures").
    sum_all_unaligned_request = workspace / "sum-all-unaligned-request.json"
    sum_all_unaligned_request.write_text(
        json.dumps(
            {
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
                        {"type": "feedback-loop"},
                        {
                            "type": "downmix",
                            "strategy": "sum-all",
                            "normalisation": "energy",
                        },
                    ]
                },
            }
        )
    )
    sum_all_unaligned_result = workspace / "sum-all-unaligned-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            sum_all_unaligned_request,
            "--output",
            sum_all_unaligned_result,
        )
    )
    sum_all_unaligned_downmix = json.loads(
        (sum_all_unaligned_result / "resolved.json").read_text()
    )["composition"]["stages"][2]
    if sum_all_unaligned_downmix["alignment"] != "unaligned":
        raise AssertionError(
            f"sum-all Feedback Loop Downmix did not resolve unaligned: "
            f"{sum_all_unaligned_downmix}"
        )
    # The same emitted `coherentDownmixAblation` field asserted directly
    # above (not re-derived here either) must be False for this fixture:
    # the strategy name is still "sum-all", but Alignment now resolves
    # "unaligned", proving production code never tags solely from the
    # strategy name.
    unaligned_tag = sum_all_unaligned_downmix["coherentDownmixAblation"]
    if unaligned_tag is not False:
        raise AssertionError(
            f"unaligned sum-all's own coherentDownmixAblation field was "
            f"not False: {sum_all_unaligned_downmix}"
        )
    _, sum_all_unaligned_samples = read_float_wav(
        sum_all_unaligned_result / "output.wav"
    )
    unaligned_left, unaligned_right = deinterleave(
        2, sum_all_unaligned_samples
    )
    # sum-all's leftRow/rightRow are the same duplicated row by
    # construction (asserted above), so L and R are always bit-identical
    # regardless of Alignment: correlation is trivially 1.0 and level
    # difference trivially 0 dB. Still reported, as the AC requires, but
    # expected to be a structural constant rather than a varying
    # measurement -- unlike peak factor and spectral deviation above,
    # which the matched-control comparison actually distinguishes.
    unaligned_correlation = zero_lag_correlation(unaligned_left, unaligned_right)
    unaligned_level_difference_db = inter_channel_level_difference_db(
        unaligned_left, unaligned_right
    )
    if (
        not math.isfinite(unaligned_correlation)
        or abs(unaligned_correlation) > 1.0 + 1e-9
    ):
        raise AssertionError(
            f"sum-all Output correlation is not a valid correlation "
            f"coefficient: {unaligned_correlation}"
        )
    if not math.isfinite(unaligned_level_difference_db):
        raise AssertionError(
            f"sum-all inter-channel level difference is not finite: "
            f"{unaligned_level_difference_db}"
        )
    unaligned_evidence_path = workspace / "sum-all-unaligned-evidence.json"
    unaligned_evidence_path.write_text(
        json.dumps(
            {
                "channels": 4,
                "alignmentExpectation": sum_all_unaligned_downmix["alignment"],
                "coherentDownmixAblation": unaligned_tag,
                "outputCorrelation": unaligned_correlation,
                "interChannelLevelDifferenceDb": unaligned_level_difference_db,
            },
            indent=2,
        )
    )

    # Main wet path enablement, level, and Width (#109): docs/design/
    # reverb/stages/09-composition.md's mainEnabled/mainLevelDb and
    # docs/design/reverb/stages/08-downmix.md's Width.
    main_base_document = {
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
                    "steps": 1,
                    "totalMs": 1,
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
            ]
        },
    }

    # Branch controls are rejected on the empty identity Composition.
    for main_field, main_field_value in (
        ("mainEnabled", True),
        ("mainLevelDb", -6.0),
        ("dryDb", -6.0),
        ("wetDb", -6.0),
        ("wetOnly", False),
        ("preDelayMs", 20.0),
    ):
        empty_with_field_document = {
            "formatVersion": 2,
            "composition": {main_field: main_field_value},
        }
        empty_with_field_request = workspace / (
            f"main-empty-with-{main_field}-request.json"
        )
        empty_with_field_request.write_text(
            json.dumps(empty_with_field_document)
        )
        require_failure(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--config",
                empty_with_field_request,
                "--output",
                workspace / f"main-empty-with-{main_field}-result",
            ),
            f"/composition/{main_field}: not applicable to the empty "
            f"identity Composition",
            workspace / f"main-empty-with-{main_field}-result",
        )

    # A non-empty Composition exposes documented mainEnabled/mainLevelDb
    # defaults: enabled, 0 dB, and the linear gain that implies.
    main_default_request = workspace / "main-default-request.json"
    main_default_request.write_text(json.dumps(main_base_document))
    main_default_result = workspace / "main-default-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            main_default_request,
            "--output",
            main_default_result,
        )
    )
    main_default_composition = json.loads(
        (main_default_result / "resolved.json").read_text()
    )["composition"]
    if (
        main_default_composition["mainEnabled"] is not True
        or main_default_composition["mainLevelDb"] != 0.0
        or main_default_composition["mainGain"] != 1.0
    ):
        raise AssertionError(
            f"unexpected default Main wet path controls: "
            f"{main_default_composition}"
        )
    # The Composition's own dry/wet envelope (#114) exposes documented
    # defaults on the same non-empty Composition: 0 dB dry/wet, 1.0
    # dry/wet gain, and wet-only -- reproducing every existing non-empty
    # format-v2 request's wet-only rendering exactly.
    if (
        main_default_composition["dryDb"] != 0.0
        or main_default_composition["dryGain"] != 1.0
        or main_default_composition["wetDb"] != 0.0
        or main_default_composition["wetGain"] != 1.0
        or main_default_composition["wetOnly"] is not True
    ):
        raise AssertionError(
            f"unexpected default dry/wet envelope: {main_default_composition}"
        )
    # Pre-delay (#133) completes the envelope: 0 ms / 0 sample default.
    if (
        main_default_composition["preDelayMs"] != 0.0
        or main_default_composition["preDelaySamples"] != 0
    ):
        raise AssertionError(
            f"unexpected default Pre-delay: {main_default_composition}"
        )
    _, main_default_output_samples = read_float_wav(
        main_default_result / "output.wav"
    )

    # A disabled Main wet path contributes exact stereo zero.
    main_disabled_document = json.loads(json.dumps(main_base_document))
    main_disabled_document["composition"]["mainEnabled"] = False
    main_disabled_request = workspace / "main-disabled-request.json"
    main_disabled_request.write_text(json.dumps(main_disabled_document))
    main_disabled_result = workspace / "main-disabled-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            main_disabled_request,
            "--output",
            main_disabled_result,
        )
    )
    main_disabled_composition = json.loads(
        (main_disabled_result / "resolved.json").read_text()
    )["composition"]
    if main_disabled_composition["mainEnabled"] is not False:
        raise AssertionError("mainEnabled: false did not resolve disabled")
    _, main_disabled_output_samples = read_float_wav(
        main_disabled_result / "output.wav"
    )
    if any(value != 0.0 for value in main_disabled_output_samples):
        raise AssertionError(
            "a disabled Main wet path did not contribute exact stereo "
            "zero"
        )

    # mainLevelDb scales the post-Width stereo output by its resolved
    # linear gain, applied once after Downmix.
    main_leveled_document = json.loads(json.dumps(main_base_document))
    main_leveled_document["composition"]["mainLevelDb"] = -6.0
    main_leveled_request = workspace / "main-leveled-request.json"
    main_leveled_request.write_text(json.dumps(main_leveled_document))
    main_leveled_result = workspace / "main-leveled-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            main_leveled_request,
            "--output",
            main_leveled_result,
        )
    )
    main_leveled_composition = json.loads(
        (main_leveled_result / "resolved.json").read_text()
    )["composition"]
    expected_main_gain = 10.0 ** (-6.0 / 20.0)
    if abs(main_leveled_composition["mainGain"] - expected_main_gain) > 1e-9:
        raise AssertionError(
            f"mainLevelDb: -6 did not resolve the expected linear gain: "
            f"{main_leveled_composition}"
        )
    _, main_leveled_output_samples = read_float_wav(
        main_leveled_result / "output.wav"
    )
    if len(main_leveled_output_samples) != len(main_default_output_samples):
        raise AssertionError("mainLevelDb: -6 changed the rendered frame count")
    for leveled_value, unleveled_value in zip(
        main_leveled_output_samples, main_default_output_samples
    ):
        if abs(leveled_value - unleveled_value * expected_main_gain) > 1e-6:
            raise AssertionError(
                "mainLevelDb: -6 did not scale the Main wet path output "
                "by its resolved gain"
            )

    # The Composition's own dry/wet envelope (#114, ADR-0007): command-
    # level cases for wetOnly send-style gating, insert-style dry+wet,
    # stereo channel-for-channel mapping, mono duplication, legacy
    # resolved.json omission, and partial-envelope rejection.
    envelope_tolerance = 2e-6

    def render_envelope_document(document, input_fixture, name):
        request_path = workspace / f"envelope-{name}-request.json"
        request_path.write_text(json.dumps(document))
        result_path = workspace / f"envelope-{name}-result"
        require_success(
            run_renderer(
                renderer,
                "--input",
                input_fixture,
                "--config",
                request_path,
                "--output",
                result_path,
            )
        )
        return result_path

    def envelope_document(overrides):
        document = json.loads(json.dumps(main_base_document))
        document["composition"].update(overrides)
        return document

    # Insert-style dry+wet: final output is the fixed-order sum of the
    # dry contribution and the scaled Wet sum. Proven by rendering dry
    # and wet in isolation (Main disabled for the dry-only reference, so
    # its Wet sum is exact zero) and comparing their sample-wise sum
    # against the combined render, on both mono and stereo dry input.
    for input_fixture, input_name in (
        (fixture, "mono"),
        (stereo_fixture, "stereo"),
    ):
        combined_result = render_envelope_document(
            envelope_document(
                {"wetOnly": False, "dryDb": -6.0, "wetDb": -3.0}
            ),
            input_fixture,
            f"combined-{input_name}",
        )
        dry_only_result = render_envelope_document(
            envelope_document(
                {
                    "mainEnabled": False,
                    "wetOnly": False,
                    "dryDb": -6.0,
                }
            ),
            input_fixture,
            f"dry-only-{input_name}",
        )
        wet_only_result = render_envelope_document(
            envelope_document({"wetOnly": True, "wetDb": -3.0}),
            input_fixture,
            f"wet-only-{input_name}",
        )
        _, combined_samples = read_float_wav(combined_result / "output.wav")
        _, dry_only_samples = read_float_wav(dry_only_result / "output.wav")
        _, wet_only_samples = read_float_wav(wet_only_result / "output.wav")
        if (
            len(combined_samples) != len(dry_only_samples)
            or len(combined_samples) != len(wet_only_samples)
        ):
            raise AssertionError(
                f"dry/wet envelope renders ({input_name}) did not share "
                f"the same rendered frame count"
            )
        for combined_value, dry_value, wet_value in zip(
            combined_samples, dry_only_samples, wet_only_samples
        ):
            if abs(combined_value - (dry_value + wet_value)) > envelope_tolerance:
                raise AssertionError(
                    f"combined dry+wet output ({input_name}) was not the "
                    f"fixed-order sum of its dry-only and wet-only "
                    f"references"
                )

    # wetOnly (the default) gates dry to exact zero while preserving
    # dryDb: a non-default dryDb with wetOnly omitted (still true)
    # renders byte-identical to the plain wet-only default above.
    wet_only_preserved_dry_result = render_envelope_document(
        envelope_document({"dryDb": -6.0}), fixture, "wet-only-preserved-dry"
    )
    wet_only_preserved_dry_composition = json.loads(
        (wet_only_preserved_dry_result / "resolved.json").read_text()
    )["composition"]
    if wet_only_preserved_dry_composition["dryDb"] != -6.0:
        raise AssertionError("wetOnly: true (default) did not preserve dryDb")
    if (
        wet_only_preserved_dry_result / "output.wav"
    ).read_bytes() != (main_default_result / "output.wav").read_bytes():
        raise AssertionError(
            "a preserved but gated dryDb changed wet-only output"
        )

    # Newly emitted Resolved configurations record the complete envelope
    # set, and rerendering from them is bit-identical.
    envelope_requested_result = render_envelope_document(
        envelope_document(
            {"wetOnly": False, "dryDb": -6.0, "wetDb": -3.0}
        ),
        stereo_fixture,
        "round-trip-requested",
    )
    envelope_resolved_composition = json.loads(
        (envelope_requested_result / "resolved.json").read_text()
    )["composition"]
    for envelope_field in (
        "dryDb",
        "wetDb",
        "wetOnly",
        "dryGain",
        "wetGain",
        "preDelayMs",
        "preDelaySamples",
    ):
        if envelope_field not in envelope_resolved_composition:
            raise AssertionError(
                f"newly emitted resolved.json omitted {envelope_field}"
            )
    envelope_resolved_result = workspace / "envelope-round-trip-resolved-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            stereo_fixture,
            "--resolved",
            envelope_requested_result / "resolved.json",
            "--output",
            envelope_resolved_result,
        )
    )
    if (envelope_resolved_result / "output.wav").read_bytes() != (
        envelope_requested_result / "output.wav"
    ).read_bytes():
        raise AssertionError(
            "rerendering the complete dry/wet envelope from resolved.json "
            "was not bit-identical"
        )

    # A non-empty format-v2 Resolved Composition carrying no envelope
    # fields at all loads as the ADR-0007 legacy neutral reading (wet-
    # only, 0 dB dry/wet, 1.0 dry/wet gain) -- byte-identical to an
    # explicit neutral Resolved Composition.
    legacy_resolved_document = json.loads(
        (main_default_result / "resolved.json").read_text()
    )
    for envelope_field in (
        "dryDb",
        "wetDb",
        "wetOnly",
        "dryGain",
        "wetGain",
        "preDelayMs",
        "preDelaySamples",
    ):
        del legacy_resolved_document["composition"][envelope_field]
    legacy_resolved_path = workspace / "legacy-resolved.json"
    legacy_resolved_path.write_text(json.dumps(legacy_resolved_document))
    legacy_resolved_result = workspace / "legacy-resolved-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            legacy_resolved_path,
            "--output",
            legacy_resolved_result,
        )
    )
    if (legacy_resolved_result / "output.wav").read_bytes() != (
        main_default_result / "output.wav"
    ).read_bytes():
        raise AssertionError(
            "a legacy resolved.json without envelope fields did not "
            "render the neutral (wet-only) reading"
        )

    # A partial envelope set -- hand-corrupted or truncated evidence, not
    # a legitimate complete artifact -- is rejected at the Composition
    # path rather than guessed at.
    partial_resolved_document = json.loads(json.dumps(legacy_resolved_document))
    partial_resolved_document["composition"]["dryDb"] = -6.0
    partial_resolved_path = workspace / "partial-envelope-resolved.json"
    partial_resolved_path.write_text(json.dumps(partial_resolved_document))
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            partial_resolved_path,
            "--output",
            workspace / "partial-envelope-result",
        ),
        "expected the complete envelope set",
        workspace / "partial-envelope-result",
    )

    # An envelope field on an empty-stage Resolved Composition is
    # rejected there too, mirroring the Requested-side empty-Composition
    # rejection above -- a resolved.json is a direct, non-JSON-adjacent
    # entry point in its own right.
    for empty_resolved_field, empty_resolved_value in (
        ("dryDb", -6.0),
        ("wetDb", -6.0),
        ("wetOnly", False),
        ("dryGain", 0.5),
        ("wetGain", 0.5),
    ):
        empty_resolved_document = {
            "formatVersion": 2,
            "seed": 0,
            "sampleRate": 48000,
            "composition": {
                "stages": [],
                empty_resolved_field: empty_resolved_value,
            },
        }
        empty_resolved_path = workspace / (
            f"empty-resolved-with-{empty_resolved_field}.json"
        )
        empty_resolved_path.write_text(json.dumps(empty_resolved_document))
        require_failure(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--resolved",
                empty_resolved_path,
                "--output",
                workspace / f"empty-resolved-with-{empty_resolved_field}-result",
            ),
            f"/composition/{empty_resolved_field}: not applicable to the "
            f"empty identity Composition",
            workspace / f"empty-resolved-with-{empty_resolved_field}-result",
        )

    # An extreme dryDb/wetDb resolves a gain that is a valid finite
    # positive double but not representable at float precision, mirroring
    # mainLevelDb's own extreme-value check.
    for extreme_field in ("dryDb", "wetDb"):
        for extreme_level in (1000.0, -1000.0):
            extreme_document = envelope_document({extreme_field: extreme_level})
            extreme_request = workspace / (
                f"envelope-extreme-{extreme_field}-{extreme_level}-request.json"
            )
            extreme_request.write_text(json.dumps(extreme_document))
            require_failure(
                run_renderer(
                    renderer,
                    "--input",
                    fixture,
                    "--config",
                    extreme_request,
                    "--output",
                    workspace
                    / f"envelope-extreme-{extreme_field}-{extreme_level}-result",
                ),
                f"/composition/{'dryGain' if extreme_field == 'dryDb' else 'wetGain'}: "
                f"expected finite positive gain representable at float "
                f"precision",
                workspace
                / f"envelope-extreme-{extreme_field}-{extreme_level}-result",
            )

    # Pre-delay (#133, docs/design/reverb/stages/09-composition.md's
    # "Pre-delay and dry/wet"): completes the Composition envelope.
    pre_delay_ms = 0.5
    pre_delay_samples = 24  # 0.5ms @ 48kHz is exact.

    pre_delay_document = envelope_document(
        {"wetOnly": False, "preDelayMs": pre_delay_ms}
    )
    pre_delay_result = render_envelope_document(
        pre_delay_document, fixture, "pre-delay"
    )
    pre_delay_composition = json.loads(
        (pre_delay_result / "resolved.json").read_text()
    )["composition"]
    if (
        pre_delay_composition["preDelayMs"] != pre_delay_ms
        or pre_delay_composition["preDelaySamples"] != pre_delay_samples
    ):
        raise AssertionError(
            f"preDelayMs: {pre_delay_ms} did not resolve the expected "
            f"nearest-frame sample count: {pre_delay_composition}"
        )
    pre_delay_metadata = json.loads(
        (pre_delay_result / "render.json").read_text()
    )
    if pre_delay_metadata["preDelayFrames"] != pre_delay_samples:
        raise AssertionError(
            f"render.json did not carry the resolved preDelayFrames: "
            f"{pre_delay_metadata}"
        )
    if pre_delay_metadata["frames"] != (
        pre_delay_metadata["inputFrames"]
        + pre_delay_metadata["preDelayFrames"]
        + pre_delay_metadata["tailBudgetFrames"]
    ):
        raise AssertionError(
            f"total output length was not inputFrames + preDelayFrames + "
            f"tailBudgetFrames: {pre_delay_metadata}"
        )

    # The same total-length formula holds even when both wet branches
    # are disabled: preDelayFrames and tailBudgetFrames are resolved
    # from the Composition's own structure, not from whether Main/Early
    # happen to be silenced. wetOnly stays at its default (true), so
    # output.wav is pure silence throughout despite reserving the full
    # timeline.
    pre_delay_both_disabled_document = envelope_document(
        {"mainEnabled": False, "preDelayMs": pre_delay_ms}
    )
    pre_delay_both_disabled_result = render_envelope_document(
        pre_delay_both_disabled_document, fixture, "pre-delay-both-disabled"
    )
    pre_delay_both_disabled_metadata = json.loads(
        (pre_delay_both_disabled_result / "render.json").read_text()
    )
    if pre_delay_both_disabled_metadata["preDelayFrames"] != pre_delay_samples:
        raise AssertionError(
            f"a disabled Main wet path (and no Early Reflections) changed "
            f"the resolved preDelayFrames: {pre_delay_both_disabled_metadata}"
        )
    if pre_delay_both_disabled_metadata["frames"] != (
        pre_delay_both_disabled_metadata["inputFrames"]
        + pre_delay_both_disabled_metadata["preDelayFrames"]
        + pre_delay_both_disabled_metadata["tailBudgetFrames"]
    ):
        raise AssertionError(
            f"total output length was not inputFrames + preDelayFrames + "
            f"tailBudgetFrames with both wet branches disabled: "
            f"{pre_delay_both_disabled_metadata}"
        )
    _, pre_delay_both_disabled_samples = read_float_wav(
        pre_delay_both_disabled_result / "output.wav"
    )
    if any(value != 0.0 for value in pre_delay_both_disabled_samples):
        raise AssertionError(
            "both wet branches disabled (with wetOnly still muting dry) "
            "did not render exact stereo silence"
        )

    # The wet path receives silence for exactly the resolved Pre-delay
    # interval before the (delayed) source reaches Split; the dry signal
    # stays sample-aligned from frame zero. Verified by exact equivalence
    # against a reference fed a manually zero-padded copy of the same
    # source at zero Pre-delay, using the "split" Stage capture -- and,
    # separately, frame zero of output.wav, where wet is still exact
    # silence, isolating dry directly.
    #
    # Every local name below is prefixed pre_delay_ (not the shorter
    # names this pattern would otherwise suggest, e.g. reference_result):
    # this function is one long flat scope, and a handful of short,
    # generic names -- reference_request/reference_result chief among
    # them -- are already established and reused hundreds of lines
    # further down, so reusing them here would silently overwrite state
    # later tests still depend on.
    #
    # read_float_wav decodes canonical IEEE float WAVs only (matching
    # analyze_diffusion.numpy_frames' own contract); stereo_fixture is
    # float32, unlike the mono PCM16 fixture used for the JSON-only
    # checks above.
    pre_delay_source_channels, pre_delay_source_samples = read_float_wav(
        stereo_fixture
    )
    pre_delay_padded_source = (
        0.0,
    ) * (pre_delay_samples * pre_delay_source_channels) + tuple(
        pre_delay_source_samples
    )
    pre_delay_padded_source_path = workspace / "pre-delay-padded-source.wav"
    write_float32_wav(
        pre_delay_padded_source_path,
        48000,
        pre_delay_source_channels,
        pre_delay_padded_source,
    )

    pre_delay_capture_document = json.loads(json.dumps(pre_delay_document))
    pre_delay_capture_request = workspace / "pre-delay-capture-request.json"
    pre_delay_capture_request.write_text(json.dumps(pre_delay_capture_document))
    pre_delay_capture_result = workspace / "pre-delay-capture-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            stereo_fixture,
            "--config",
            pre_delay_capture_request,
            "--capture-stages",
            "all",
            "--output",
            pre_delay_capture_result,
        )
    )

    pre_delay_zero_reference_document = json.loads(json.dumps(pre_delay_document))
    del pre_delay_zero_reference_document["composition"]["preDelayMs"]
    pre_delay_zero_reference_request = workspace / (
        "pre-delay-zero-reference-request.json"
    )
    pre_delay_zero_reference_request.write_text(
        json.dumps(pre_delay_zero_reference_document)
    )
    pre_delay_zero_reference_result = workspace / "pre-delay-zero-reference-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            pre_delay_padded_source_path,
            "--config",
            pre_delay_zero_reference_request,
            "--capture-stages",
            "all",
            "--output",
            pre_delay_zero_reference_result,
        )
    )

    def pre_delay_split_capture_samples(result_dir):
        metadata = json.loads((result_dir / "render.json").read_text())
        split_capture = next(
            capture
            for capture in metadata["stageCaptures"]
            if capture["boundary"] == "split"
        )
        return read_float_wav(result_dir / split_capture["path"])

    (
        pre_delay_split_channels,
        pre_delay_split_samples,
    ) = pre_delay_split_capture_samples(pre_delay_capture_result)
    (
        pre_delay_zero_reference_split_channels,
        pre_delay_zero_reference_split_samples,
    ) = pre_delay_split_capture_samples(pre_delay_zero_reference_result)
    if (
        pre_delay_split_channels != pre_delay_zero_reference_split_channels
        or pre_delay_split_samples != pre_delay_zero_reference_split_samples
    ):
        raise AssertionError(
            "Pre-delay did not reproduce a manually zero-padded, "
            "zero-Pre-delay reference at the split Stage capture"
        )
    pre_delay_silent_prefix_samples = pre_delay_split_channels * pre_delay_samples
    if any(
        value != 0.0
        for value in pre_delay_split_samples[:pre_delay_silent_prefix_samples]
    ):
        raise AssertionError(
            "the wet path was not exact silence during the resolved "
            "Pre-delay interval"
        )

    _, pre_delay_output_samples = read_float_wav(
        pre_delay_capture_result / "output.wav"
    )
    pre_delay_expected_dry_frame_zero = tuple(
        pre_delay_source_samples[channel]
        for channel in range(pre_delay_source_channels)
    )
    if (
        pre_delay_output_samples[:pre_delay_source_channels]
        != pre_delay_expected_dry_frame_zero
    ):
        raise AssertionError(
            "dry was not sample-aligned from frame zero under a nonzero "
            "Pre-delay"
        )

    # Rerendering from resolved.json is bit-identical for a nonzero
    # Pre-delay too, and the complete envelope set is recorded.
    for envelope_field in (
        "dryDb",
        "wetDb",
        "wetOnly",
        "dryGain",
        "wetGain",
        "preDelayMs",
        "preDelaySamples",
    ):
        if envelope_field not in pre_delay_composition:
            raise AssertionError(
                f"newly emitted resolved.json omitted {envelope_field}"
            )
    pre_delay_rerendered_result = workspace / "pre-delay-rerendered-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            pre_delay_result / "resolved.json",
            "--output",
            pre_delay_rerendered_result,
        )
    )
    if (pre_delay_rerendered_result / "output.wav").read_bytes() != (
        pre_delay_result / "output.wav"
    ).read_bytes():
        raise AssertionError(
            "rerendering a nonzero Pre-delay from resolved.json was not "
            "bit-identical"
        )

    # Modulation advances while the initial silence traverses the wet
    # path: a modulated Pre-delay render is deterministic and repeatable
    # (Repeat determinism, CONTEXT.md) -- proven directly here, without
    # asserting any byte-shift equivalence to a zero-Pre-delay
    # configuration, which #133 explicitly does not require.
    modulated_pre_delay_document = envelope_document(
        {"wetOnly": False, "preDelayMs": pre_delay_ms}
    )
    modulated_pre_delay_document["composition"]["stages"][1]["stepOverrides"] = [
        {"index": 0, "modulation": {"depthMs": 0.01, "rateHz": 5.0}}
    ]
    modulated_pre_delay_result_a = render_envelope_document(
        modulated_pre_delay_document, stereo_fixture, "modulated-pre-delay-a"
    )
    modulated_pre_delay_result_b = render_envelope_document(
        modulated_pre_delay_document, stereo_fixture, "modulated-pre-delay-b"
    )
    if (modulated_pre_delay_result_a / "output.wav").read_bytes() != (
        modulated_pre_delay_result_b / "output.wav"
    ).read_bytes():
        raise AssertionError(
            "a modulated Pre-delay render was not deterministic and "
            "repeatable"
        )

    # Out-of-range preDelayMs is rejected.
    for invalid_pre_delay_ms in (-1.0, 201.0):
        invalid_document = envelope_document({"preDelayMs": invalid_pre_delay_ms})
        invalid_request = workspace / (
            f"pre-delay-invalid-{invalid_pre_delay_ms}-request.json"
        )
        invalid_request.write_text(json.dumps(invalid_document))
        require_failure(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--config",
                invalid_request,
                "--output",
                workspace / f"pre-delay-invalid-{invalid_pre_delay_ms}-result",
            ),
            "/composition/preDelayMs: expected a finite value within "
            "0-200 ms",
            workspace / f"pre-delay-invalid-{invalid_pre_delay_ms}-result",
        )

    # A preDelaySamples inconsistent with its preDelayMs is rejected at
    # the Composition path, naming the derivation it violates.
    inconsistent_resolved_document = json.loads(
        (pre_delay_result / "resolved.json").read_text()
    )
    inconsistent_resolved_document["composition"]["preDelaySamples"] += 1
    inconsistent_resolved_path = workspace / "pre-delay-inconsistent-resolved.json"
    inconsistent_resolved_path.write_text(
        json.dumps(inconsistent_resolved_document)
    )
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            inconsistent_resolved_path,
            "--output",
            workspace / "pre-delay-inconsistent-result",
        ),
        "/composition/preDelaySamples: expected samples derived from "
        "preDelayMs by the nearest-frame rule",
        workspace / "pre-delay-inconsistent-result",
    )

    # Width (#109): endpoint identity (0/90/180) and an intermediate
    # angle, each checked against the resolved matrix and replayed from
    # resolved.json.
    for width_deg in (0.0, 45.0, 90.0, 135.0, 180.0):
        width_document = json.loads(json.dumps(main_base_document))
        width_document["composition"]["stages"][2]["widthDeg"] = width_deg
        width_request = workspace / f"main-width-{width_deg}-request.json"
        width_request.write_text(json.dumps(width_document))
        width_result = workspace / f"main-width-{width_deg}-result"
        require_success(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--config",
                width_request,
                "--output",
                width_result,
            )
        )
        width_downmix = json.loads(
            (width_result / "resolved.json").read_text()
        )["composition"]["stages"][2]
        if width_downmix["widthDeg"] != width_deg:
            raise AssertionError(
                f"Width did not resolve the requested widthDeg: "
                f"{width_downmix}"
            )
        expected_width_matrix = width_matrix(width_deg)
        if any(
            abs(actual - expected) > 1e-9
            for actual, expected in zip(
                width_downmix["widthMatrix"], expected_width_matrix
            )
        ):
            raise AssertionError(
                f"Width matrix at {width_deg} degrees did not match the "
                f"documented formula: {width_downmix}"
            )

        width_rerender = workspace / f"main-width-{width_deg}-rerender"
        require_success(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--resolved",
                width_result / "resolved.json",
                "--output",
                width_rerender,
            )
        )
        if (width_rerender / "resolved.json").read_bytes() != (
            width_result / "resolved.json"
        ).read_bytes():
            raise AssertionError(
                f"Width resolved rerender changed configuration at "
                f"{width_deg} degrees"
            )
        if (width_rerender / "output.wav").read_bytes() != (
            width_result / "output.wav"
        ).read_bytes():
            raise AssertionError(
                f"Width resolved rerender changed output at {width_deg} "
                f"degrees"
            )

    # 90 degrees defaults identically to an omitted widthDeg: a
    # bit-identical bypass against the default render above.
    width_90_result = workspace / "main-width-90.0-result"
    if (width_90_result / "output.wav").read_bytes() != (
        main_default_result / "output.wav"
    ).read_bytes():
        raise AssertionError(
            "Width at 90 degrees was not a bit-identical bypass"
        )

    # Actual energy change across Width is measured and reported, not
    # gated, on a seeded unaligned (Feedback-Loop) fixed-total-power
    # fixture (docs/design/reverb/stages/08-downmix.md's "Width evidence"
    # and "actual energy change is always reported").
    width_energy_by_degrees = {}
    for width_deg in (0.0, 90.0, 180.0):
        width_energy_document = {
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
                    {"type": "feedback-loop"},
                    {
                        "type": "downmix",
                        "strategy": "select",
                        "leftChannel": 0,
                        "rightChannel": 1,
                        "normalisation": "energy",
                        "widthDeg": width_deg,
                    },
                ]
            },
        }
        width_energy_request = workspace / (
            f"main-width-energy-{width_deg}-request.json"
        )
        width_energy_request.write_text(json.dumps(width_energy_document))
        width_energy_result = workspace / (
            f"main-width-energy-{width_deg}-result"
        )
        require_success(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--config",
                width_energy_request,
                "--output",
                width_energy_result,
            )
        )
        _, width_energy_output_samples = read_float_wav(
            width_energy_result / "output.wav"
        )
        width_energy_left, width_energy_right = deinterleave(
            2, width_energy_output_samples
        )
        width_energy = math.fsum(
            v * v for v in width_energy_left
        ) + math.fsum(v * v for v in width_energy_right)
        if not math.isfinite(width_energy):
            raise AssertionError(
                f"Width output energy is not finite at {width_deg} degrees"
            )
        width_energy_by_degrees[width_deg] = width_energy

    # Width actually changes energy on this fixture -- not a universal
    # acoustic threshold (docs/design/reverb/stages/08-downmix.md's
    # "Width evidence" explicitly reports rather than gates this), but a
    # genuinely measured change rather than the same value reported three
    # times over.
    distinct_width_energies = {
        round(value, 9) for value in width_energy_by_degrees.values()
    }
    if len(distinct_width_energies) < 2:
        raise AssertionError(
            f"Width did not measurably change output energy across "
            f"0/90/180 degrees: {width_energy_by_degrees}"
        )

    width_evidence_path = workspace / "main-width-energy-evidence.json"
    width_evidence_path.write_text(
        json.dumps(width_energy_by_degrees, indent=2)
    )

    # widthDeg must stay within its declared structural domain (#109,
    # docs/design/reverb/stages/09-composition.md's Rules table: "width
    # must be finite and in their declared structural domains") -- both
    # above 180 and below 0.
    for out_of_range_width_deg in (200.0, -10.0):
        width_out_of_range_document = json.loads(
            json.dumps(main_base_document)
        )
        width_out_of_range_document["composition"]["stages"][2][
            "widthDeg"
        ] = out_of_range_width_deg
        width_out_of_range_request = workspace / (
            f"main-width-out-of-range-{out_of_range_width_deg}-"
            f"request.json"
        )
        width_out_of_range_request.write_text(
            json.dumps(width_out_of_range_document)
        )
        require_failure(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--config",
                width_out_of_range_request,
                "--output",
                workspace
                / f"main-width-out-of-range-{out_of_range_width_deg}-result",
            ),
            "/composition/stages/2/widthDeg: "
            "expected a finite value within [0, 180]",
            workspace
            / f"main-width-out-of-range-{out_of_range_width_deg}-result",
        )

    # An extreme mainLevelDb resolves a mainGain that is a valid finite
    # positive double but is not representable at float precision --
    # +1000 dB overflows float to infinity, -1000 dB underflows it to
    # exact zero -- and both are rejected rather than silently applying a
    # gain DSP construction cannot actually reproduce (see Downmix's own
    # double-resolved/Sample-applied split for why float precision is
    # the binding constraint regardless of this build's own Sample type).
    for extreme_main_level_db in (1000.0, -1000.0):
        main_extreme_level_document = json.loads(
            json.dumps(main_base_document)
        )
        main_extreme_level_document["composition"][
            "mainLevelDb"
        ] = extreme_main_level_db
        main_extreme_level_request = workspace / (
            f"main-extreme-level-{extreme_main_level_db}-request.json"
        )
        main_extreme_level_request.write_text(
            json.dumps(main_extreme_level_document)
        )
        require_failure(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--config",
                main_extreme_level_request,
                "--output",
                workspace / f"main-extreme-level-{extreme_main_level_db}-result",
            ),
            "/composition/mainGain: expected finite positive gain "
            "representable at float precision",
            workspace / f"main-extreme-level-{extreme_main_level_db}-result",
        )

    # The parallel Early Reflections branch (issue #111, docs/design/
    # reverb/stages/07-early-reflections.md and docs/design/reverb/
    # stages/09-composition.md): Requested configuration, Resolved
    # evidence, stereo WAV output, and replay covered end-to-end for one
    # tap.
    early_base_document = json.loads(json.dumps(main_base_document))
    early_base_document["composition"]["early"] = {
        "enabled": True,
        "levelDb": -6.0,
        "taps": [{"stepIndex": 0}],
        "downmix": {
            "strategy": "select",
            "leftChannel": 0,
            "rightChannel": 1,
            "normalisation": "energy",
        },
    }
    early_request = workspace / "early-request.json"
    early_request_bytes = json.dumps(early_base_document).encode()
    early_request.write_bytes(early_request_bytes)
    early_result = workspace / "early-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            early_request,
            "--output",
            early_result,
        )
    )
    if (early_result / "request.json").read_bytes() != early_request_bytes:
        raise AssertionError(
            "Early Reflections Render Result did not preserve the raw "
            "request"
        )

    early_resolved_composition = json.loads(
        (early_result / "resolved.json").read_text()
    )["composition"]
    early_composition = early_resolved_composition["early"]
    expected_early_gain = 10.0 ** (-6.0 / 20.0)
    early_tap = early_composition["taps"][0] if early_composition["taps"] else None
    if (
        early_composition["enabled"] is not True
        or early_composition["levelDb"] != -6.0
        or abs(early_composition["gain"] - expected_early_gain) > 1e-9
        or early_composition["decayDbPerSec"] != 0.0
        or len(early_composition["taps"]) != 1
        or early_tap["stepIndex"] != 0
        or early_tap["gainDb"] != 0.0
        or early_tap["shapingGainDb"] != 0.0
        or early_tap["gain"] != 1.0
        or early_composition["downmix"]["strategy"] != "select"
        or early_composition["downmix"]["leftChannel"] != 0
        or early_composition["downmix"]["rightChannel"] != 1
        or early_composition["downmix"]["alignment"] != "aligned"
    ):
        raise AssertionError(
            f"Resolved Configuration did not record the requested Early "
            f"Reflections branch: {early_composition}"
        )

    early_output_channels, _ = read_float_wav(early_result / "output.wav")
    if early_output_channels != 2:
        raise AssertionError(
            f"Early Reflections render did not produce stereo output: "
            f"{early_output_channels} Channels"
        )

    # Replay from Resolved Configuration reproduces the exact same
    # Requested-derived render.
    early_rerender = workspace / "early-rerender"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            early_result / "resolved.json",
            "--output",
            early_rerender,
        )
    )
    if (early_rerender / "resolved.json").read_bytes() != (
        early_result / "resolved.json"
    ).read_bytes():
        raise AssertionError(
            "Early Reflections resolved rerender changed configuration"
        )
    if (early_rerender / "output.wav").read_bytes() != (
        early_result / "output.wav"
    ).read_bytes():
        raise AssertionError("Early Reflections resolved rerender changed output")

    # A present Early branch defaults to enabled, 0 dB, and a `select`
    # Downmix of Channels 0/1 -- unlike the Main Downmix's own `select`,
    # which has no implicit Channel choice (issue #107).
    early_default_document = json.loads(json.dumps(main_base_document))
    early_default_document["composition"]["early"] = {
        "taps": [{"stepIndex": 0}]
    }
    early_default_request = workspace / "early-default-request.json"
    early_default_request.write_text(json.dumps(early_default_document))
    early_default_result = workspace / "early-default-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            early_default_request,
            "--output",
            early_default_result,
        )
    )
    early_default_composition = json.loads(
        (early_default_result / "resolved.json").read_text()
    )["composition"]["early"]
    if (
        early_default_composition["enabled"] is not True
        or early_default_composition["levelDb"] != 0.0
        or early_default_composition["gain"] != 1.0
        or early_default_composition["downmix"]["strategy"] != "select"
        or early_default_composition["downmix"]["leftChannel"] != 0
        or early_default_composition["downmix"]["rightChannel"] != 1
    ):
        raise AssertionError(
            f"a present Early branch did not default to enabled/0 dB/"
            f"select Channels 0-1: {early_default_composition}"
        )

    # Multiple distinct taps, each with a per-tap gainDb offset and a
    # shared decayDbPerSec envelope slope, are covered end-to-end too
    # (issue #112): canonical (ascending) order in Resolved evidence
    # regardless of Requested order, each tap's own shaping gain, stereo
    # WAV output, and replay.
    early_multi_tap_document = {
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
                    "steps": 4,
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
                "decayDbPerSec": 12.0,
                "taps": [
                    {"stepIndex": 3, "gainDb": 2.0},
                    {"stepIndex": 0, "gainDb": -1.0},
                ],
            },
        },
    }
    early_multi_tap_request = workspace / "early-multi-tap-request.json"
    early_multi_tap_request.write_text(json.dumps(early_multi_tap_document))
    early_multi_tap_result = workspace / "early-multi-tap-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            early_multi_tap_request,
            "--output",
            early_multi_tap_result,
        )
    )
    early_multi_tap_composition = json.loads(
        (early_multi_tap_result / "resolved.json").read_text()
    )["composition"]["early"]
    multi_taps = early_multi_tap_composition["taps"]
    if (
        len(multi_taps) != 2
        or multi_taps[0]["stepIndex"] != 0
        or multi_taps[0]["gainDb"] != -1.0
        or multi_taps[1]["stepIndex"] != 3
        or multi_taps[1]["gainDb"] != 2.0
    ):
        raise AssertionError(
            f"multiple Early taps did not resolve in canonical ascending "
            f"order with their own requested gainDb: {multi_taps}"
        )
    for tap in multi_taps:
        expected_shaping_gain_db = tap["gainDb"] - 12.0 * (
            tap["nominalSupportMaxMs"] / 1000.0
        )
        if abs(tap["shapingGainDb"] - expected_shaping_gain_db) > 1e-9:
            raise AssertionError(
                f"a tap's shapingGainDb did not match gainDb minus "
                f"decayDbPerSec times its own nominal support end: {tap}"
            )
        expected_gain = 10.0 ** (expected_shaping_gain_db / 20.0)
        if abs(tap["gain"] - expected_gain) > 1e-9:
            raise AssertionError(
                f"a tap's resolved linear gain did not match its own "
                f"shapingGainDb: {tap}"
            )
    multi_tap_output_channels, _ = read_float_wav(
        early_multi_tap_result / "output.wav"
    )
    if multi_tap_output_channels != 2:
        raise AssertionError(
            "multi-tap Early Reflections render did not produce stereo "
            "output"
        )

    early_multi_tap_rerender = workspace / "early-multi-tap-rerender"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            early_multi_tap_result / "resolved.json",
            "--output",
            early_multi_tap_rerender,
        )
    )
    if (early_multi_tap_rerender / "resolved.json").read_bytes() != (
        early_multi_tap_result / "resolved.json"
    ).read_bytes():
        raise AssertionError(
            "multi-tap Early Reflections resolved rerender changed "
            "configuration"
        )
    if (early_multi_tap_rerender / "output.wav").read_bytes() != (
        early_multi_tap_result / "output.wav"
    ).read_bytes():
        raise AssertionError(
            "multi-tap Early Reflections resolved rerender changed output"
        )

    # Configuring the tap leaves the Diffuser's own Main output
    # bit-identical: the captured Diffusion Step WAV is unaffected by
    # whether an Early Reflections branch taps it.
    no_early_capture_result = workspace / "early-no-capture-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            main_default_request,
            "--capture-stages",
            "all",
            "--output",
            no_early_capture_result,
        )
    )
    early_capture_result = workspace / "early-capture-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            early_default_request,
            "--capture-stages",
            "all",
            "--output",
            early_capture_result,
        )
    )
    if (
        no_early_capture_result / "captures" / "01-diffusion-step-0.wav"
    ).read_bytes() != (
        early_capture_result / "captures" / "01-diffusion-step-0.wav"
    ).read_bytes():
        raise AssertionError(
            "configuring an Early tap changed the Diffuser's own captured "
            "Main output"
        )

    # Early Reflections without a Diffuser are rejected.
    early_without_diffuser_document = {
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
                {"type": "feedback-loop"},
                {
                    "type": "downmix",
                    "strategy": "select",
                    "leftChannel": 0,
                    "rightChannel": 1,
                    "normalisation": "energy",
                },
            ],
            "early": {"taps": [{"stepIndex": 0}]},
        },
    }
    early_without_diffuser_request = workspace / (
        "early-without-diffuser-request.json"
    )
    early_without_diffuser_request.write_text(
        json.dumps(early_without_diffuser_document)
    )
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            early_without_diffuser_request,
            "--output",
            workspace / "early-without-diffuser-result",
        ),
        "/composition/early: requires the Main wet path to contain a "
        "Diffuser",
        workspace / "early-without-diffuser-result",
    )

    # Diffuser-then-Feedback-Loop routes the tap in parallel too, and
    # Early's own Downmix resolves an aligned Alignment expectation
    # independently of the Main Downmix, which is unaligned when its
    # source includes a Feedback Loop (issue #107).
    early_loop_document = {
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
                    "steps": 1,
                    "totalMs": 1,
                    "distribution": "even",
                    "step": {
                        "delayStrategy": "segmented-random",
                        "mix": "hadamard",
                        "shuffle": True,
                        "polarity": "seeded-random",
                    },
                },
                {"type": "feedback-loop"},
                {
                    "type": "downmix",
                    "strategy": "select",
                    "leftChannel": 0,
                    "rightChannel": 1,
                    "normalisation": "energy",
                },
            ],
            "early": {"taps": [{"stepIndex": 0}]},
        },
    }
    early_loop_request = workspace / "early-loop-request.json"
    early_loop_request.write_text(json.dumps(early_loop_document))
    early_loop_result = workspace / "early-loop-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            early_loop_request,
            "--output",
            early_loop_result,
        )
    )
    early_loop_composition = json.loads(
        (early_loop_result / "resolved.json").read_text()
    )["composition"]
    if (
        early_loop_composition["stages"][3]["alignment"] != "unaligned"
        or early_loop_composition["early"]["downmix"]["alignment"]
        != "aligned"
    ):
        raise AssertionError(
            "Early's Downmix did not resolve an aligned Alignment "
            "expectation independently of an unaligned Main Downmix"
        )
    early_loop_output_channels, _ = read_float_wav(
        early_loop_result / "output.wav"
    )
    if early_loop_output_channels != 2:
        raise AssertionError(
            "Diffuser-then-Feedback-Loop with Early Reflections did not "
            "render stereo output"
        )

    # Duplicate tap indices are rejected (issue #112).
    early_two_taps_document = json.loads(json.dumps(main_base_document))
    early_two_taps_document["composition"]["early"] = {
        "taps": [{"stepIndex": 0}, {"stepIndex": 0}]
    }
    early_two_taps_request = workspace / "early-two-taps-request.json"
    early_two_taps_request.write_text(json.dumps(early_two_taps_document))
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            early_two_taps_request,
            "--output",
            workspace / "early-two-taps-result",
        ),
        "/composition/early/taps: expected unique Diffusion Step indices",
        workspace / "early-two-taps-result",
    )

    # Branch controls without any tap are rejected: they could not
    # affect sound.
    early_no_taps_document = json.loads(json.dumps(main_base_document))
    early_no_taps_document["composition"]["early"] = {"enabled": False}
    early_no_taps_request = workspace / "early-no-taps-request.json"
    early_no_taps_request.write_text(json.dumps(early_no_taps_document))
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            early_no_taps_request,
            "--output",
            workspace / "early-no-taps-result",
        ),
        "/composition/early: enabled/levelDb/decayDbPerSec/downmix are "
        "not applicable without at least one tap",
        workspace / "early-no-taps-result",
    )

    # Early Reflections controls are rejected on the empty identity
    # Composition, mirroring mainEnabled/mainLevelDb (#109).
    early_empty_document = {
        "formatVersion": 2,
        "composition": {"early": {"taps": [{"stepIndex": 0}]}},
    }
    early_empty_request = workspace / "early-empty-request.json"
    early_empty_request.write_text(json.dumps(early_empty_document))
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            early_empty_request,
            "--output",
            workspace / "early-empty-result",
        ),
        "/composition/early: not applicable to the empty identity "
        "Composition",
        workspace / "early-empty-result",
    )

    # An out-of-range tap stepIndex is rejected: this Diffuser has one
    # step (index 0), so index 1 does not exist.
    early_out_of_range_document = json.loads(json.dumps(main_base_document))
    early_out_of_range_document["composition"]["early"] = {
        "taps": [{"stepIndex": 1}]
    }
    early_out_of_range_request = workspace / "early-out-of-range-request.json"
    early_out_of_range_request.write_text(
        json.dumps(early_out_of_range_document)
    )
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            early_out_of_range_request,
            "--output",
            workspace / "early-out-of-range-result",
        ),
        "/composition/early/taps/0/stepIndex: expected a Diffusion Step "
        "index within [0, stepCount)",
        workspace / "early-out-of-range-result",
    )

    # A negative decayDbPerSec is rejected (issue #112): the envelope
    # slope must be finite and at least zero.
    early_negative_decay_document = json.loads(json.dumps(main_base_document))
    early_negative_decay_document["composition"]["early"] = {
        "decayDbPerSec": -1.0,
        "taps": [{"stepIndex": 0}],
    }
    early_negative_decay_request = workspace / (
        "early-negative-decay-request.json"
    )
    early_negative_decay_request.write_text(
        json.dumps(early_negative_decay_document)
    )
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            early_negative_decay_request,
            "--output",
            workspace / "early-negative-decay-result",
        ),
        "/composition/early/decayDbPerSec: expected a finite value at "
        "least zero",
        workspace / "early-negative-decay-result",
    )

    # Early's own `downmix` bypasses the composition.stages dispatcher
    # that normally requires and checks `type` before ever parsing a
    # Downmix (PR review on #111): a requested `downmix` whose `type`
    # names a different stage is rejected rather than silently accepted
    # as a Downmix.
    early_wrong_type_document = json.loads(json.dumps(main_base_document))
    early_wrong_type_document["composition"]["early"] = {
        "taps": [{"stepIndex": 0}],
        "downmix": {
            "type": "feedback-loop",
            "strategy": "select",
            "leftChannel": 0,
            "rightChannel": 1,
        },
    }
    early_wrong_type_request = workspace / "early-wrong-type-request.json"
    early_wrong_type_request.write_text(json.dumps(early_wrong_type_document))
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            early_wrong_type_request,
            "--output",
            workspace / "early-wrong-type-result",
        ),
        "/composition/early/downmix/type: expected downmix",
        workspace / "early-wrong-type-result",
    )

    # On replay, resolved.json always serializes `type: "downmix"` for
    # every Downmix (unlike a request, where Early's own `downmix` never
    # carries `type` in the documented examples), so a resolved Early
    # `downmix` missing `type` entirely is rejected too.
    early_resolved_missing_type = json.loads(
        (early_result / "resolved.json").read_text()
    )
    del early_resolved_missing_type["composition"]["early"]["downmix"]["type"]
    early_resolved_missing_type_path = workspace / (
        "early-resolved-missing-type.json"
    )
    early_resolved_missing_type_path.write_text(
        json.dumps(early_resolved_missing_type)
    )
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            early_resolved_missing_type_path,
            "--output",
            workspace / "early-resolved-missing-type-result",
        ),
        "/composition/early/downmix/type: required field is missing",
        workspace / "early-resolved-missing-type-result",
    )

    invalid_ablation_resolved = []
    invalid_source_gain = json.loads(json.dumps(ablation_resolved))
    invalid_source_gain["composition"]["stages"][0]["sourceGain"] = 0.5
    invalid_ablation_resolved.append(
        (
            invalid_source_gain,
            "/composition/stages/0/sourceGain: "
            "expected gain derived from Split input mapping",
        )
    )
    invalid_split_gain = json.loads(json.dumps(ablation_resolved))
    invalid_split_gain["composition"]["stages"][0]["channelGain"] = 0.5
    invalid_ablation_resolved.append(
        (
            invalid_split_gain,
            "/composition/stages/0/channelGain: "
            "expected gain derived from Split normalisation",
        )
    )
    invalid_even_delays = json.loads(json.dumps(ablation_resolved))
    invalid_even_step = invalid_even_delays["composition"]["stages"][1][
        "steps"
    ][0]
    invalid_even_step["delaysSamples"][1] = 1
    invalid_even_step["delaysMs"][1] = 1000.0 / 48000.0
    invalid_even_step["bufferSizes"][1] = 1
    invalid_ablation_resolved.append(
        (
            invalid_even_delays,
            "/composition/stages/1/steps/0/delaysSamples: "
            "even requires delays distributed over the available positions",
        )
    )
    invalid_duplicate_delays = json.loads(json.dumps(ablation_resolved))
    invalid_duplicate_step = invalid_duplicate_delays["composition"]["stages"][1][
        "steps"
    ][0]
    invalid_duplicate_step["delaysSamples"][1] = invalid_duplicate_step[
        "delaysSamples"
    ][0]
    invalid_ablation_resolved.append(
        (
            invalid_duplicate_delays,
            "/composition/stages/1/steps/0/delaysSamples: "
            "expected distinct delays within the step sample budget",
        )
    )
    invalid_identity = json.loads(json.dumps(ablation_resolved))
    invalid_identity["composition"]["stages"][1]["steps"][0]["permutation"] = [
        1,
        0,
        2,
        3,
    ]
    invalid_ablation_resolved.append(
        (
            invalid_identity,
            "/composition/stages/1/steps/0/permutation: "
            "shuffle false requires the identity permutation",
        )
    )
    invalid_polarity = json.loads(json.dumps(ablation_resolved))
    invalid_polarity["composition"]["stages"][1]["steps"][0][
        "polaritySigns"
    ][0] = -1
    invalid_ablation_resolved.append(
        (
            invalid_polarity,
            "/composition/stages/1/steps/0/polaritySigns: "
            "polarity none requires all +1 signs",
        )
    )
    invalid_hadamard = json.loads(json.dumps(ablation_resolved))
    invalid_hadamard["composition"]["stages"][1]["steps"][0]["matrix"] = [
        [1.0, 0.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
        [0.0, 0.0, 0.0, 1.0],
    ]
    invalid_ablation_resolved.append(
        (
            invalid_hadamard,
            "/composition/stages/1/steps/0/matrix: "
            "expected the normalized canonical Sylvester-Hadamard matrix",
        )
    )
    invalid_downmix_gain = json.loads(json.dumps(ablation_resolved))
    invalid_downmix_gain["composition"]["stages"][2]["compensation"] = 2.0
    invalid_ablation_resolved.append(
        (
            invalid_downmix_gain,
            "/composition/stages/2/compensation: "
            "expected gain derived from Downmix normalisation",
        )
    )
    for index, (document, expected_error) in enumerate(
        invalid_ablation_resolved
    ):
        path = workspace / f"invalid-ablation-resolved-{index}.json"
        output = workspace / f"invalid-ablation-result-{index}"
        path.write_text(json.dumps(document))
        require_failure(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--resolved",
                path,
                "--output",
                output,
            ),
            expected_error,
            output,
        )

    mismatched_shape_result = workspace / "mismatched-shape-result"
    mismatched_shape = run_renderer(
        renderer,
        "--input",
        stereo_fixture,
        "--resolved",
        reference_result / "resolved.json",
        "--output",
        mismatched_shape_result,
    )
    require_failure(
        mismatched_shape,
        "invalid_configuration at /composition/stages/0/inputChannels: "
        "expected input Channel count 1, got 2",
        mismatched_shape_result,
    )

    invalid_channels_request = json.loads(reference_request.read_text())
    invalid_channels_request["composition"]["stages"][0]["channels"] = 0
    invalid_total_request = json.loads(reference_request.read_text())
    invalid_total_request["composition"]["stages"][1]["totalMs"] = 0
    sub_sample_request = json.loads(reference_request.read_text())
    sub_sample_request["composition"]["stages"][1]["totalMs"] = 0.000001
    oversized_total_request = json.loads(reference_request.read_text())
    oversized_total_request["composition"]["stages"][1]["totalMs"] = 1e300
    downmix_left_out_of_range_request = json.loads(reference_request.read_text())
    downmix_left_out_of_range_request["composition"]["stages"][2][
        "leftChannel"
    ] = 8
    downmix_right_out_of_range_request = json.loads(reference_request.read_text())
    downmix_right_out_of_range_request["composition"]["stages"][2][
        "rightChannel"
    ] = 8
    downmix_non_distinct_request = json.loads(reference_request.read_text())
    downmix_non_distinct_request["composition"]["stages"][2]["rightChannel"] = 0
    short_delay_request = json.loads(reference_request.read_text())
    short_delay_request["composition"]["stages"][1]["totalMs"] = 0.1
    # Same short budget and Channel count as the accepted uniform-random
    # request above (1 sample, 2 positions, N=4): even still rejects it,
    # proving the "too short" diagnostic is strategy-specific rather than a
    # blanket minimum uniform-random happens to slip past.
    short_even_delay_request = json.loads(reference_request.read_text())
    short_even_delay_request["composition"]["stages"][0]["channels"] = 4
    short_even_delay_request["composition"]["stages"][1]["totalMs"] = 0.02
    short_even_delay_request["composition"]["stages"][1]["step"][
        "delayStrategy"
    ] = "even"
    non_power_of_two_request = json.loads(reference_request.read_text())
    non_power_of_two_request["composition"]["stages"][0]["channels"] = 3
    unknown_mix_request = json.loads(reference_request.read_text())
    unknown_mix_request["composition"]["stages"][1]["step"]["mix"] = "unknown"
    lengths_ms_with_steps_request = json.loads(reference_request.read_text())
    lengths_ms_with_steps_request["composition"]["stages"][1]["lengthsMs"] = [1.0]
    step_override_out_of_range_request = json.loads(reference_request.read_text())
    step_override_out_of_range_request["composition"]["stages"][1][
        "stepOverrides"
    ] = [{"index": 1, "polarity": "none"}]
    step_override_duplicate_request = json.loads(reference_request.read_text())
    step_override_duplicate_request["composition"]["stages"][1]["steps"] = 2
    step_override_duplicate_request["composition"]["stages"][1][
        "stepOverrides"
    ] = [
        {"index": 0, "polarity": "none"},
        {"index": 0, "shuffle": False},
    ]
    # A doubling chain long enough to overflow the geometric weight sum
    # (2**i for i in [0, steps)) regardless of the platform's `long double`
    # range: 64-bit (where `long double` == `double`, e.g. Windows/ARM64)
    # overflows within ~1024 steps, while 80-/128-bit extended formats
    # (e.g. x86-64 Linux/macOS) need roughly 16384. 100000 steps clears
    # every known long double range, so on every platform either an
    # individual weight or their sum ends up non-finite -- exercising the
    # same "invalid weights" rejection this must reject cleanly instead of
    # reading past the end of its internal ordering array.
    doubling_weight_overflow_request = json.loads(reference_request.read_text())
    doubling_weight_overflow_request["composition"]["stages"][1] = {
        "type": "diffuser",
        "steps": 100000,
        "totalMs": 300,
        "distribution": "doubling",
    }
    # An unsupported formatVersion (neither 2 nor the archived 1) alongside
    # otherwise catastrophic parameters: the format check must still fail
    # fast and cleanly rather than the resolver hanging or crashing on a
    # billion-Channel, 30000-second Diffuser.
    unsafe_format_request = json.loads(reference_request.read_text())
    unsafe_format_request["formatVersion"] = 3
    unsafe_format_request["composition"]["stages"][0]["channels"] = 1073741824
    unsafe_format_request["composition"]["stages"][1]["totalMs"] = 30000000
    invalid_requests = [
        (
            '{"unexpected": true}',
            "invalid_configuration at /unexpected: unknown field",
        ),
        (
            '{"formatVersion": 2, "composition": {"unexpected": true}}',
            "invalid_configuration at /composition/unexpected: unknown field",
        ),
        (
            '{}',
            "invalid_configuration at /formatVersion: required field is missing",
        ),
        (
            '{"formatVersion": 1, "composition": {"stages": []}}',
            "invalid_configuration at /formatVersion: reverb configuration "
            "format 1 is unsupported by this build; use tag format-v1-final "
            "(commit 8a4e718) to render or analyze format-1 configurations",
        ),
        (
            json.dumps(unsafe_format_request),
            "invalid_configuration at /formatVersion: expected integer 2",
        ),
        (
            '{"formatVersion": 2, "seed": -1}',
            "invalid_configuration at /seed: expected unsigned 64-bit integer",
        ),
        (
            '{"formatVersion": 2, "composition": {"stages": [{}]}}',
            "invalid_configuration at /composition/stages/0/type: required field is missing",
        ),
        (
            '{"formatVersion": 2, "composition": {"stages": '
            '[{"type": "downmix", "leftChannel": 0}, {"type": "split"}]}}',
            "invalid_configuration at /composition/stages: expected [split, diffuser, downmix]",
        ),
        (
            '{"formatVersion": 2, "composition": {"stages": ['
            '{"type": "split"}, {"type": "feedback-loop"}, '
            '{"type": "diffuser"}, {"type": "downmix", "leftChannel": 0}'
            ']}}',
            "invalid_configuration at /composition/stages: expected [split, diffuser, downmix]",
        ),
        (
            '{"formatVersion": 2, "composition": {"stages": ['
            '{"type": "split"}, {"type": "diffuser"}, '
            '{"type": "diffuser"}, {"type": "feedback-loop"}, '
            '{"type": "downmix", "leftChannel": 0}'
            ']}}',
            "invalid_configuration at /composition/stages: expected [split, diffuser, downmix]",
        ),
        (
            json.dumps(invalid_channels_request),
            "invalid_configuration at /composition/stages/0/channels: "
            "expected value greater than zero",
        ),
        (
            json.dumps(downmix_left_out_of_range_request),
            "invalid_configuration at /composition/stages/2/leftChannel: "
            "expected a Channel index within [0, N)",
        ),
        (
            json.dumps(downmix_right_out_of_range_request),
            "invalid_configuration at /composition/stages/2/rightChannel: "
            "expected a Channel index within [0, N)",
        ),
        (
            json.dumps(downmix_non_distinct_request),
            "invalid_configuration at /composition/stages/2/rightChannel: "
            "expected a Channel distinct from leftChannel",
        ),
        (
            '{"formatVersion": 2, "composition": {"stages": ['
            '{"type": "split"}, {"type": "diffuser"}, {"type": "downmix"}'
            ']}}',
            "invalid_configuration at /composition/stages/2/leftChannel: "
            "required field is missing",
        ),
        (
            json.dumps(invalid_total_request),
            "invalid_configuration at /composition/stages/1/totalMs: "
            "expected value greater than zero",
        ),
        (
            json.dumps(sub_sample_request),
            "invalid_configuration at /composition/stages/1/totalMs: "
            "resolved sample budget must be at least one sample",
        ),
        (
            json.dumps(oversized_total_request),
            "invalid_configuration at /composition/stages/1/totalMs: "
            "resolved sample budget is too large",
        ),
        (
            json.dumps(short_delay_request),
            "invalid_configuration at /composition/stages/1/steps/0/lengthSamples: "
            "delay strategy requires at least one sample position per Channel",
        ),
        (
            json.dumps(short_even_delay_request),
            "invalid_configuration at /composition/stages/1/steps/0/lengthSamples: "
            "delay strategy requires at least one sample position per Channel",
        ),
        (
            json.dumps(non_power_of_two_request),
            "invalid_configuration at /composition/stages/1/steps/0/mix: "
            "hadamard requires a power-of-two Channel count",
        ),
        (
            json.dumps(unknown_mix_request),
            "invalid_configuration at /composition/stages/1/step/mix: "
            "expected hadamard, householder, or random-orthogonal",
        ),
        (
            json.dumps(lengths_ms_with_steps_request),
            "invalid_configuration at /composition/stages/1/lengthsMs: "
            "expected exactly one of lengthsMs or steps/totalMs/distribution",
        ),
        (
            json.dumps(step_override_out_of_range_request),
            "invalid_configuration at /composition/stages/1/stepOverrides/0/index: "
            "expected index less than the resolved step count",
        ),
        (
            json.dumps(step_override_duplicate_request),
            "invalid_configuration at /composition/stages/1/stepOverrides/1/index: "
            "expected distinct step indices",
        ),
        (
            json.dumps(doubling_weight_overflow_request),
            "invalid_configuration at /composition/stages/1/steps: "
            "expected a valid step count and distribution",
        ),
        (
            '{"seed": ',
            "malformed_json at /: malformed JSON:",
        ),
    ]
    for index, (contents, expected_error) in enumerate(invalid_requests):
        invalid_config = workspace / f"invalid-request-{index}.json"
        invalid_output = workspace / f"invalid-request-result-{index}"
        invalid_config.write_text(contents)
        completed = run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            invalid_config,
            "--output",
            invalid_output,
        )
        require_failure(completed, expected_error, invalid_output)

    mismatched_resolved = workspace / "mismatched-resolved.json"
    mismatched_output = workspace / "mismatched-result"
    mismatched_resolved.write_text(
        json.dumps(
            {
                "formatVersion": 2,
                "seed": 0,
                "sampleRate": 44100,
                "composition": {"stages": []},
            }
        )
    )
    mismatch = run_renderer(
        renderer,
        "--input",
        fixture,
        "--resolved",
        mismatched_resolved,
        "--output",
        mismatched_output,
    )
    require_failure(
        mismatch,
        "invalid_configuration at /sampleRate: expected input sample rate 48000, got 44100",
        mismatched_output,
    )

    oversized_resolved = workspace / "oversized-resolved.json"
    oversized_output = workspace / "oversized-result"
    oversized_resolved.write_text(
        json.dumps(
            {
                "formatVersion": 2,
                "seed": 0,
                "sampleRate": 4294967297,
                "composition": {"stages": []},
            }
        )
    )
    oversized = run_renderer(
        renderer,
        "--input",
        fixture,
        "--resolved",
        oversized_resolved,
        "--output",
        oversized_output,
    )
    require_failure(
        oversized,
        "invalid_configuration at /sampleRate: expected unsigned 32-bit integer",
        oversized_output,
    )

    conflicting_output = workspace / "conflicting-result"
    conflicting = run_renderer(
        renderer,
        "--input",
        fixture,
        "--config",
        request,
        "--resolved",
        requested_result / "resolved.json",
        "--output",
        conflicting_output,
    )
    require_failure(
        conflicting,
        "--config and --resolved are mutually exclusive",
        conflicting_output,
    )

    empty_config_output = workspace / "empty-config-result"
    empty_config = run_renderer(
        renderer,
        "--input",
        fixture,
        "--config",
        "",
        "--output",
        empty_config_output,
    )
    require_failure(
        empty_config,
        "--config requires a non-empty path",
        empty_config_output,
    )

    empty_resolved_output = workspace / "empty-resolved-result"
    empty_resolved = run_renderer(
        renderer,
        "--input",
        fixture,
        "--resolved",
        "",
        "--output",
        empty_resolved_output,
    )
    require_failure(
        empty_resolved,
        "--resolved requires a non-empty path",
        empty_resolved_output,
    )

    empty_conflicting_output = workspace / "empty-conflicting-result"
    empty_conflicting = run_renderer(
        renderer,
        "--input",
        fixture,
        "--config",
        "",
        "--resolved",
        requested_result / "resolved.json",
        "--output",
        empty_conflicting_output,
    )
    require_failure(
        empty_conflicting,
        "--config and --resolved are mutually exclusive",
        empty_conflicting_output,
    )


if __name__ == "__main__":
    main()
