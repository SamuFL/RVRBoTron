#!/usr/bin/env python3

import hashlib
import json
import math
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path


def append_zero_tail_frames(path, count):
    """Grow a canonical WAV's data chunk by `count` all-zero frames,
    mirroring test_diffusion_analyzer_cli.py's truncate_zero_tail_frame in
    reverse -- used to construct an output.wav whose frame count matches a
    deliberately inflated render.json without touching real DSP output."""
    contents = bytearray(path.read_bytes())
    if contents[:4] != b"RIFF" or contents[8:12] != b"WAVE":
        raise AssertionError(f"not a RIFF/WAVE file: {path}")

    block_align = None
    data_header = None
    data_offset = None
    data_size = None
    offset = 12
    while offset + 8 <= len(contents):
        chunk_id = contents[offset : offset + 4]
        chunk_size = struct.unpack_from("<I", contents, offset + 4)[0]
        chunk_data = offset + 8
        if chunk_id == b"fmt ":
            block_align = struct.unpack_from("<H", contents, chunk_data + 12)[0]
        elif chunk_id == b"data":
            data_header = offset
            data_offset = chunk_data
            data_size = chunk_size
            break
        offset = chunk_data + chunk_size + chunk_size % 2

    if None in (block_align, data_header, data_offset, data_size):
        raise AssertionError(f"missing WAV format or data chunk: {path}")

    zeros = bytes(block_align * count)
    contents[data_offset + data_size : data_offset + data_size] = zeros
    struct.pack_into("<I", contents, data_header + 4, data_size + len(zeros))
    struct.pack_into("<I", contents, 4, len(contents) - 8)
    path.write_bytes(contents)


def run_renderer(renderer, request_path, output, block_size=32):
    completed = subprocess.run(
        [
            str(renderer),
            "render",
            "--input",
            str(FIXTURE),
            "--config",
            str(request_path),
            "--output",
            str(output),
            "--block-size",
            str(block_size),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)
    return completed


def run_analyzer(analyzer, render_result, source=None):
    return subprocess.run(
        [
            sys.executable,
            str(analyzer),
            str(render_result),
            "--source",
            str(source or FIXTURE),
        ],
        check=False,
        capture_output=True,
        text=True,
    )


def feedback_loop_request(channels=2, **overrides):
    loop = {
        "type": "feedback-loop",
        "delayMinMs": 1.0,
        "delayMaxMs": 2.0,
        "delayStrategy": "even",
        "rt60Sec": 1.0,
        "mix": "householder",
    }
    loop.update(overrides)
    return {
        "formatVersion": 2,
        "seed": 7,
        "composition": {
            "stages": [
                {
                    "type": "split",
                    "channels": channels,
                    "strategy": "duplicate",
                    "normalisation": "energy",
                },
                loop,
                {"type": "downmix", "strategy": "select", "leftChannel": 0, "rightChannel": 1},
            ]
        },
    }


def diffuser_only_request():
    return {
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
                {"type": "downmix", "strategy": "select", "leftChannel": 0, "rightChannel": 1},
            ]
        },
    }


def main():
    global FIXTURE
    tail_analyzer = Path(sys.argv[1])
    diffusion_analyzer = Path(sys.argv[2])
    renderer = Path(sys.argv[3])
    FIXTURE = Path(sys.argv[4])
    workspace = Path(sys.argv[5])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    # N=2, delayStrategy "even" (see test_feedback_loop_cli.py): resolved
    # delays land exactly on 48/96 samples (1/2 ms at 48 kHz), a
    # millisecond-scale loop fast enough for CI while still producing a
    # dense, well-resolved octave-band decay across the whole 1.5 s Tail
    # budget.
    request_path = workspace / "request.json"
    request_path.write_text(json.dumps(feedback_loop_request()))
    render_result = workspace / "render-result"
    run_renderer(renderer, request_path, render_result)

    analyzed = run_analyzer(tail_analyzer, render_result)
    if analyzed.returncode != 0:
        raise AssertionError(analyzed.stderr)

    artifact = render_result / "analysis" / "tail-v1.json"
    analysis = json.loads(artifact.read_text())

    if analysis["source"] != {
        "filename": FIXTURE.name,
        "sha256": hashlib.sha256(FIXTURE.read_bytes()).hexdigest(),
        "verified": True,
    }:
        raise AssertionError(f"unexpected source provenance: {analysis}")

    expected_tail_budget = math.ceil(1.0 * 1.5 * 48000)
    expected_frames = 32 + expected_tail_budget
    if analysis["completeResponse"] != {
        "inputFrames": 32,
        "tailBudgetFrames": expected_tail_budget,
        "expectedFrames": expected_frames,
        "outputFrames": expected_frames,
        "frameCountCheck": "exact",
        "silenceFloorEnabled": False,
    }:
        raise AssertionError(f"unexpected complete-response facts: {analysis}")

    if analysis["nonFiniteSampleCount"] != 0:
        raise AssertionError(f"unexpected non-finite samples: {analysis}")

    decay = analysis["decay"]
    if decay["referenceBandHz"] != 1000.0 or decay["requestedRt60Sec"] != 1.0:
        raise AssertionError(f"unexpected decay header: {decay}")
    if decay["measuredRt60Sec"] is None:
        raise AssertionError(f"Reference band RT60 fit failed: {decay}")
    if decay["relativeError"] > 0.05 or not decay["withinAccuracyInvariant"]:
        raise AssertionError(
            f"Reference band RT60 exceeded the +/-5% accuracy invariant: {decay}"
        )

    expected_centers = [63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0]
    bands = decay["bands"]
    if [band["centerHz"] for band in bands] != expected_centers:
        raise AssertionError(f"unexpected octave-band centers: {bands}")
    for band in bands:
        if not (band["lowHz"] < band["centerHz"] < band["highHz"]):
            raise AssertionError(f"unexpected octave-band edges: {band}")

    reference_band = next(band for band in bands if band["centerHz"] == 1000.0)
    if reference_band["t20"] is None or reference_band["t30"] is None:
        raise AssertionError(f"Reference band T20/T30 fit failed: {reference_band}")

    # The raw (non-Schroeder-integrated) output envelope genuinely declines
    # after input EOF, distinct from the per-band Schroeder curve, which is
    # non-increasing by construction regardless of what the render does.
    decay_envelope = analysis["decayEnvelope"]
    if decay_envelope["monotonic"] is not True:
        raise AssertionError(
            f"decay envelope was not monotonic after input ceased: {decay_envelope}"
        )

    # Householder N=2 unaligns the two internal Channels this stage
    # circulates; the "select" Downmix passes them straight through as
    # left/right, so the Alignment score measured on the stereo output here
    # is materially below the Diffuser's own near-unity evidence
    # (test_diffusion_analyzer_cli.py's Hadamard fixture measures exactly
    # 1.0 on internal Channels, which this reuses the same Jaccard scoring
    # for).
    alignment = analysis["alignment"]
    if not (0.0 <= alignment["score"] < 0.9):
        raise AssertionError(
            f"Alignment score was not materially below the Diffuser's "
            f"near-unity baseline: {alignment}"
        )

    coloration = analysis["coloration"]
    if not coloration.get("twelfthOctaveCurve"):
        raise AssertionError(f"Coloration curve was empty: {coloration}")

    # Idempotent: repeating the analysis does not rewrite the artifact.
    original = artifact.read_bytes()
    modified = artifact.stat().st_mtime_ns
    time.sleep(0.01)
    repeated = run_analyzer(tail_analyzer, render_result)
    if repeated.returncode != 0:
        raise AssertionError(repeated.stderr)
    if artifact.read_bytes() != original or artifact.stat().st_mtime_ns != modified:
        raise AssertionError("idempotent tail analysis rewrote its artifact")

    # A source that does not match render.json's recorded SHA-256 is
    # rejected without changing the published artifact.
    wrong_source = workspace / "wrong.wav"
    wrong_source.write_bytes(FIXTURE.read_bytes() + b"\x00")
    rejected = run_analyzer(tail_analyzer, render_result, source=wrong_source)
    if rejected.returncode == 0:
        raise AssertionError("tail analyzer accepted wrong source")
    if "source SHA-256 does not match render metadata" not in rejected.stderr:
        raise AssertionError(f"unexpected provenance failure: {rejected.stderr}")
    if artifact.read_bytes() != original:
        raise AssertionError("failed provenance check changed analysis")

    # A Composition without a Feedback Loop is rejected by the tail
    # analyzer; the diffusion analyzer, unextended, still rejects a
    # Composition that contains one.
    diffuser_only_path = workspace / "diffuser-only-request.json"
    diffuser_only_path.write_text(json.dumps(diffuser_only_request()))
    diffuser_only_result = workspace / "diffuser-only-result"
    diffuser_only_render = subprocess.run(
        [
            str(renderer),
            "render",
            "--input",
            str(FIXTURE),
            "--config",
            str(diffuser_only_path),
            "--capture-stages",
            "all",
            "--output",
            str(diffuser_only_result),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if diffuser_only_render.returncode != 0:
        raise AssertionError(diffuser_only_render.stderr)

    rejected_composition = run_analyzer(tail_analyzer, diffuser_only_result)
    if rejected_composition.returncode == 0:
        raise AssertionError(
            "tail analyzer accepted a Composition without a Feedback Loop"
        )
    if "tail analysis requires a Feedback Loop" not in rejected_composition.stderr:
        raise AssertionError(
            f"unexpected composition rejection: {rejected_composition.stderr}"
        )
    if (diffuser_only_result / "analysis" / "tail-v1.json").exists():
        raise AssertionError("rejected composition published a tail analysis")

    rejected_by_diffusion = subprocess.run(
        [
            sys.executable,
            str(diffusion_analyzer),
            str(render_result),
            "--source",
            str(FIXTURE),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if rejected_by_diffusion.returncode == 0:
        raise AssertionError(
            "diffusion analyzer accepted a Composition containing a "
            "Feedback Loop"
        )
    if "diffusion analysis requires a finite Diffuser" not in rejected_by_diffusion.stderr:
        raise AssertionError(
            f"unexpected diffusion-analyzer rejection: {rejected_by_diffusion.stderr}"
        )

    # gainMode: uniform (#56) solves one shared gain from the mean loop time
    # across Channels; at N=8 with a wide delay spread this produces a
    # non-linear, double-sloped decay that T20 and T30 disagree on, rather
    # than the near-single-rate decay gainMode: per-channel produces from
    # the same delays (stage 04's "Solving RT60 into gain"). N=8's milder
    # Householder mixing (vs. N=2's full swap-negate) keeps each Channel's
    # own decay rate visible in the selected output Channel instead of
    # averaging it away.
    def gain_mode_reference_divergence(name, gain_mode):
        request = workspace / f"{name}-request.json"
        request.write_text(
            json.dumps(
                feedback_loop_request(
                    channels=8, delayMaxMs=32.0, gainMode=gain_mode
                )
            )
        )
        result = workspace / f"{name}-result"
        run_renderer(renderer, request, result)
        analyzed = run_analyzer(tail_analyzer, result)
        if analyzed.returncode != 0:
            raise AssertionError(analyzed.stderr)
        analysis = json.loads((result / "analysis" / "tail-v1.json").read_text())
        reference = next(
            band for band in analysis["decay"]["bands"] if band["centerHz"] == 1000.0
        )
        if reference["t20"] is None or reference["t30"] is None:
            raise AssertionError(f"{name} T20/T30 fit failed: {reference}")
        return abs(reference["t20"]["rt60Sec"] - reference["t30"]["rt60Sec"])

    per_channel_divergence = gain_mode_reference_divergence(
        "wide-per-channel", "per-channel"
    )
    uniform_divergence = gain_mode_reference_divergence("wide-uniform", "uniform")
    if uniform_divergence <= 5 * per_channel_divergence:
        raise AssertionError(
            "gainMode: uniform at a wide delay spread did not diverge T20 "
            f"from T30 materially more than gainMode: per-channel at the "
            f"same delays: uniform={uniform_divergence} vs "
            f"per-channel={per_channel_divergence}"
        )

    # An explicit silenceFloorDb (dormant this milestone -- see stage 04)
    # relaxes the frame-count check to an upper bound, keyed off the
    # Resolved Configuration rather than a schema change.
    silence_floor_path = workspace / "silence-floor-request.json"
    silence_floor_path.write_text(
        json.dumps(feedback_loop_request(silenceFloorDb=-90.0))
    )
    silence_floor_result = workspace / "silence-floor-result"
    run_renderer(renderer, silence_floor_path, silence_floor_result)
    silence_floor_analyzed = run_analyzer(tail_analyzer, silence_floor_result)
    if silence_floor_analyzed.returncode != 0:
        raise AssertionError(silence_floor_analyzed.stderr)
    silence_floor_analysis = json.loads(
        (silence_floor_result / "analysis" / "tail-v1.json").read_text()
    )
    if silence_floor_analysis["completeResponse"]["frameCountCheck"] != "upper-bound":
        raise AssertionError(
            f"silenceFloorDb did not relax the frame-count check to an "
            f"upper bound: {silence_floor_analysis['completeResponse']}"
        )
    if silence_floor_analysis["completeResponse"]["silenceFloorEnabled"] is not True:
        raise AssertionError(
            f"silenceFloorEnabled was not reported: "
            f"{silence_floor_analysis['completeResponse']}"
        )

    # The frame-count check splits two distinct failures (review fix): a
    # render.json that disagrees with output.wav's actual frame count fails
    # on that mismatch, never blamed on the Tail budget.
    mismatch_result = workspace / "silence-floor-mismatch-result"
    shutil.copytree(silence_floor_result, mismatch_result)
    mismatch_metadata_path = mismatch_result / "render.json"
    mismatch_metadata = json.loads(mismatch_metadata_path.read_text())
    mismatch_metadata["frames"] += 1
    mismatch_metadata_path.write_text(json.dumps(mismatch_metadata))
    mismatch_analyzed = run_analyzer(tail_analyzer, mismatch_result)
    if mismatch_analyzed.returncode == 0:
        raise AssertionError(
            "tail analyzer accepted a render.json/output.wav frame-count "
            "mismatch"
        )
    if "does not match output.wav" not in mismatch_analyzed.stderr:
        raise AssertionError(f"unexpected mismatch failure: {mismatch_analyzed.stderr}")
    if "Tail budget" in mismatch_analyzed.stderr:
        raise AssertionError(
            "an output.wav/render.json mismatch was misreported as a Tail "
            f"budget failure: {mismatch_analyzed.stderr}"
        )

    # Conversely, a render.json that agrees with output.wav but exceeds the
    # Tail budget upper bound fails with its own distinct message, not the
    # output.wav-mismatch one.
    exceeded_result = workspace / "silence-floor-exceeded-result"
    shutil.copytree(silence_floor_result, exceeded_result)
    append_zero_tail_frames(exceeded_result / "output.wav", 1)
    exceeded_metadata_path = exceeded_result / "render.json"
    exceeded_metadata = json.loads(exceeded_metadata_path.read_text())
    exceeded_metadata["frames"] += 1
    exceeded_metadata_path.write_text(json.dumps(exceeded_metadata))
    exceeded_analyzed = run_analyzer(tail_analyzer, exceeded_result)
    if exceeded_analyzed.returncode == 0:
        raise AssertionError(
            "tail analyzer accepted a render exceeding the Tail budget "
            "upper bound"
        )
    if "exceeds the Tail budget upper bound" not in exceeded_analyzed.stderr:
        raise AssertionError(
            f"unexpected Tail-budget-exceeded failure: {exceeded_analyzed.stderr}"
        )
    if "does not match output.wav" in exceeded_analyzed.stderr:
        raise AssertionError(
            "a Tail budget overflow was misreported as an output.wav "
            f"mismatch: {exceeded_analyzed.stderr}"
        )


if __name__ == "__main__":
    main()
