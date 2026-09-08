#!/usr/bin/env python3

"""Reproduces the empirical evidence behind docs/design/reverb/stages/
06-modulation.md's "Allpass viability inside the Feedback Loop" finding
(issue #93): renders a fixed set of Feedback-Loop-only Compositions at
several depth/rate/interpolation combinations, and reports each render's
peak amplitude and tail RMS (the stability table) plus, for a second set
at matched depth/rate across all three interpolation methods, the
sample-to-sample discontinuity a moving allpass-interpolated read produces
relative to lagrange3 and linear (the transient-artefact table).

This is a standalone evidence-reproduction script, not a general movement-
measurement tool -- Stage 6's own "Measurement" section already names that
as a later ticket's job (Decay tilt, Output correlation, RT60 deviation,
published beside the tail evidence). Every scenario here is deterministic
(fixed seed) and renders through the actual CLI, so a changed number on a
rerun is real evidence of a regression, not sampling noise.
"""

import argparse
import json
import math
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import analyze_render


STABILITY_SCENARIOS = [
    ("lagrange3_baseline", 0.4, 0.7, "lagrange3", 3.0),
    ("linear_baseline", 0.4, 0.7, "linear", 3.0),
    ("allpass_baseline", 0.4, 0.7, "allpass", 3.0),
    ("allpass_moderate", 2.0, 3.0, "allpass", 3.0),
    ("allpass_extreme", 8.0, 6.0, "allpass", 3.0),
    ("allpass_fast_rate", 1.0, 20.0, "allpass", 3.0),
]

# A long render at an aggressive depth/rate, checking decay stays
# monotonic (no runaway growth) far past the six scenarios above.
LONG_RENDER = ("allpass_long", 10.0, 8.0, "allpass", 20.0)

TRANSIENT_SCENARIOS = [
    ("lagrange3", 2.0, 3.0),
    ("linear", 2.0, 3.0),
    ("allpass", 2.0, 3.0),
]

# allpass only, at higher depth and higher rate than TRANSIENT_SCENARIOS'
# matched-comparison point, showing the jump statistic grows with either
# axis rather than being a fixed artefact of the method alone.
TRANSIENT_ALLPASS_EXTRA = [
    ("allpass_deeper", 10.0, 3.0),
    ("allpass_faster", 2.0, 15.0),
]

SEED = 7
SPLIT_CHANNELS = 2
DELAY_MIN_MS = 40.0
DELAY_MAX_MS = 60.0


def request(depth_ms, rate_hz, interpolation, rt60_sec):
    return {
        "formatVersion": 1,
        "seed": SEED,
        "composition": {
            "stages": [
                {
                    "type": "split",
                    "channels": SPLIT_CHANNELS,
                    "strategy": "duplicate",
                    "normalisation": "energy",
                },
                {
                    "type": "feedback-loop",
                    "delayMinMs": DELAY_MIN_MS,
                    "delayMaxMs": DELAY_MAX_MS,
                    "delayStrategy": "even",
                    "rt60Sec": rt60_sec,
                    "decayMargin": 2.0 if rt60_sec > 10.0 else 1.5,
                    "mix": "householder",
                    "modulation": {
                        "depthMs": depth_ms,
                        "rateHz": rate_hz,
                        "interpolation": interpolation,
                    },
                },
                {"type": "downmix", "strategy": "select"},
            ]
        },
    }


def render(renderer, fixture, workspace, name, depth_ms, rate_hz, interpolation, rt60_sec):
    request_path = workspace / f"{name}-request.json"
    request_path.write_text(json.dumps(request(depth_ms, rate_hz, interpolation, rt60_sec)))
    result = workspace / f"{name}-result"
    if result.exists():
        shutil.rmtree(result)
    completed = subprocess.run(
        [
            str(renderer), "render",
            "--input", str(fixture),
            "--config", str(request_path),
            "--output", str(result),
        ],
        capture_output=True, text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"{name}: render failed: {completed.stderr}")
    return result / "output.wav"


def channel_zero(wav_path):
    wav = analyze_render.inspect_wav(wav_path)
    all_samples = list(analyze_render.decoded_samples(wav))
    return wav["sampleRate"], all_samples[0 :: wav["channels"]]


def peak_and_tail_rms(samples, tail_fraction=0.1):
    peak = max(abs(s) for s in samples) if samples else 0.0
    tail_start = int(len(samples) * (1.0 - tail_fraction))
    tail = samples[tail_start:]
    rms = math.sqrt(sum(s * s for s in tail) / len(tail)) if tail else 0.0
    return peak, rms


def jump_statistics(samples, start_fraction=0.05, end_fraction=0.6):
    start = int(len(samples) * start_fraction)
    end = int(len(samples) * end_fraction)
    region = samples[start:end]
    if len(region) < 2:
        return 0.0, 0.0
    jumps = [abs(region[i] - region[i - 1]) for i in range(1, len(region))]
    max_jump = max(jumps)
    mean_square_jump = sum(j * j for j in jumps) / len(jumps)
    return max_jump, mean_square_jump


def decay_windows(samples, sample_rate, window_seconds=5.0):
    window = int(sample_rate * window_seconds)
    windows = []
    for start in range(0, len(samples), window):
        segment = samples[start : start + window]
        if not segment:
            continue
        peak = max(abs(s) for s in segment)
        rms = math.sqrt(sum(s * s for s in segment) / len(segment))
        windows.append((start / sample_rate, peak, rms))
    return windows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    parser.add_argument("fixture", type=Path)
    parser.add_argument("workspace", type=Path)
    arguments = parser.parse_args()

    workspace = arguments.workspace
    workspace.mkdir(parents=True, exist_ok=True)

    print("## Stability: peak amplitude and tail RMS\n")
    print(f"{'scenario':20s} {'interp':10s} {'depthMs':>8s} {'rateHz':>7s} "
          f"{'peak':>12s} {'rms_tail':>12s}")
    for name, depth_ms, rate_hz, interpolation, rt60_sec in STABILITY_SCENARIOS:
        wav_path = render(
            arguments.renderer, arguments.fixture, workspace,
            name, depth_ms, rate_hz, interpolation, rt60_sec,
        )
        _, samples = channel_zero(wav_path)
        peak, rms_tail = peak_and_tail_rms(samples)
        print(f"{name:20s} {interpolation:10s} {depth_ms:8.2f} {rate_hz:7.2f} "
              f"{peak:12.6f} {rms_tail:12.3e}")

    print(f"\n## Long render ({LONG_RENDER[3]}, {LONG_RENDER[1]} ms / "
          f"{LONG_RENDER[2]} Hz, {LONG_RENDER[4]}s RT60): decay by 5s window\n")
    name, depth_ms, rate_hz, interpolation, rt60_sec = LONG_RENDER
    wav_path = render(
        arguments.renderer, arguments.fixture, workspace,
        name, depth_ms, rate_hz, interpolation, rt60_sec,
    )
    sample_rate, samples = channel_zero(wav_path)
    print(f"frames={len(samples)} duration_sec={len(samples) / sample_rate:.1f}")
    for t, peak, rms in decay_windows(samples, sample_rate):
        print(f"  t={t:6.1f}s  peak={peak:.6e}  rms={rms:.6e}")

    print("\n## Transient artefact: sample-to-sample discontinuity at "
          "matched depth/rate\n")
    print(f"{'interpolation':15s} {'depthMs':>8s} {'rateHz':>7s} "
          f"{'max|jump|':>12s} {'mean_sq_jump':>14s}")
    for interpolation, depth_ms, rate_hz in TRANSIENT_SCENARIOS:
        wav_path = render(
            arguments.renderer, arguments.fixture, workspace,
            f"transient_{interpolation}", depth_ms, rate_hz, interpolation, 3.0,
        )
        _, samples = channel_zero(wav_path)
        max_jump, mean_square_jump = jump_statistics(samples)
        print(f"{interpolation:15s} {depth_ms:8.2f} {rate_hz:7.2f} "
              f"{max_jump:12.6f} {mean_square_jump:14.4e}")
    for name, depth_ms, rate_hz in TRANSIENT_ALLPASS_EXTRA:
        wav_path = render(
            arguments.renderer, arguments.fixture, workspace,
            f"transient_{name}", depth_ms, rate_hz, "allpass", 3.0,
        )
        _, samples = channel_zero(wav_path)
        max_jump, mean_square_jump = jump_statistics(samples)
        print(f"{'allpass (' + name.split('_')[1] + ')':15s} {depth_ms:8.2f} "
              f"{rate_hz:7.2f} {max_jump:12.6f} {mean_square_jump:14.4e}")


if __name__ == "__main__":
    main()
