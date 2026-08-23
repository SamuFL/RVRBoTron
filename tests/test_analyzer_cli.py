#!/usr/bin/env python3

import json
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path


def main():
    analyzer = Path(sys.argv[1])
    renderer = Path(sys.argv[2])
    fixture = Path(sys.argv[3])
    stereo_fixture = Path(sys.argv[4])
    identity_source = Path(sys.argv[5])
    workspace = Path(sys.argv[6])
    sample_bits = int(sys.argv[7])
    output_sample_format = "<f" if sample_bits == 32 else "<d"
    output_sample_size = sample_bits // 8
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    render_result = workspace / "render-result"
    rendered = subprocess.run(
        [
            str(renderer),
            "render",
            "--input",
            str(fixture),
            "--block-size",
            "5",
            "--output",
            str(render_result),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if rendered.returncode != 0:
        raise AssertionError(rendered.stderr)

    analyzed = subprocess.run(
        [sys.executable, str(analyzer), str(render_result)],
        check=False,
        capture_output=True,
        text=True,
    )
    if analyzed.returncode != 0:
        raise AssertionError(analyzed.stderr)

    artifact = render_result / "analysis" / "baseline-v1.json"
    expected_summary = (
        "17 frames, 0.000354167 s, 48000 Hz, 1 channel, "
        "0 non-finite samples\n"
        f"{artifact}\n"
    )
    if analyzed.stdout != expected_summary:
        raise AssertionError(f"unexpected analysis summary: {analyzed.stdout!r}")

    analysis = json.loads(artifact.read_text())
    expected = {
        "formatVersion": 1,
        "analyzer": "baseline",
        "analyzerVersion": 1,
        "metricDefinitions": {
            "peakAbsoluteSample": (
                "maximum absolute finite decoded sample; linear amplitude"
            ),
            "rmsAmplitude": (
                "square root of sumOfSquares divided by finite sample count; "
                "linear amplitude"
            ),
            "sumOfSquares": (
                "sum of squared finite decoded samples; linear amplitude squared"
            ),
            "nonFiniteSampleCount": (
                "count of decoded samples that are NaN or infinite"
            ),
        },
        "audio": {
            "frameCount": 17,
            "durationSeconds": 17 / 48000,
            "sampleRate": 48000,
            "channelCount": 1,
            "nonFiniteSampleCount": 0,
        },
        "channels": [
            {
                "channel": 0,
                "peakAbsoluteSample": 1.0,
                "rmsAmplitude": 0.685714066775088,
                "sumOfSquares": 7.993464283344907,
            }
        ],
        "combined": {
            "peakAbsoluteSample": 1.0,
            "rmsAmplitude": 0.685714066775088,
            "sumOfSquares": 7.993464283344907,
        },
    }
    if analysis != expected:
        raise AssertionError(f"unexpected baseline analysis: {analysis}")

    original_bytes = artifact.read_bytes()
    original_modified = artifact.stat().st_mtime_ns
    time.sleep(0.01)
    repeated = subprocess.run(
        [sys.executable, str(analyzer), str(render_result)],
        check=False,
        capture_output=True,
        text=True,
    )
    if repeated.returncode != 0:
        raise AssertionError(repeated.stderr)
    if artifact.read_bytes() != original_bytes:
        raise AssertionError("idempotent analysis changed artifact bytes")
    if artifact.stat().st_mtime_ns != original_modified:
        raise AssertionError("idempotent analysis rewrote its artifact")

    collision = subprocess.run(
        [
            sys.executable,
            str(analyzer),
            str(render_result),
            "--source",
            str(fixture),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if collision.returncode == 0:
        raise AssertionError("different same-version analysis replaced artifact")
    if "analysis artifact already exists with different content" not in collision.stderr:
        raise AssertionError(f"unexpected analysis collision: {collision.stderr!r}")
    if artifact.read_bytes() != original_bytes:
        raise AssertionError("analysis collision changed artifact bytes")
    if list((render_result / "analysis").glob(".*.tmp*")):
        raise AssertionError("analysis collision left temporary state")

    identity_result = workspace / "identity-result"
    rendered = subprocess.run(
        [
            str(renderer),
            "render",
            "--input",
            str(identity_source),
            "--output",
            str(identity_result),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if rendered.returncode != 0:
        raise AssertionError(rendered.stderr)

    analyzed = subprocess.run(
        [
            sys.executable,
            str(analyzer),
            str(identity_result),
            "--source",
            str(identity_source),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if analyzed.returncode != 0:
        raise AssertionError(analyzed.stderr)
    identity_analysis = json.loads(
        (identity_result / "analysis" / "baseline-v1.json").read_text()
    )
    if identity_analysis["identity"] != {
        "equal": True,
        "differingSampleCount": 0,
        "maximumAbsoluteError": 0.0,
        "firstMismatch": None,
    }:
        raise AssertionError(
            f"unexpected identity analysis: {identity_analysis['identity']}"
        )

    wrong_source = workspace / "wrong-source.wav"
    wrong_source.write_bytes(identity_source.read_bytes() + b"\x00")
    mismatch_result = workspace / "mismatch-result"
    shutil.copytree(identity_result, mismatch_result)
    shutil.rmtree(mismatch_result / "analysis")
    mismatched = subprocess.run(
        [
            sys.executable,
            str(analyzer),
            str(mismatch_result),
            "--source",
            str(wrong_source),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if mismatched.returncode == 0:
        raise AssertionError("analyzer accepted a source with the wrong SHA-256")
    if "source SHA-256 does not match render metadata" not in mismatched.stderr:
        raise AssertionError(f"unexpected hash mismatch: {mismatched.stderr!r}")
    if (mismatch_result / "analysis").exists():
        raise AssertionError("failed identity analysis created an artifact")

    differing_result = workspace / "differing-result"
    shutil.copytree(identity_result, differing_result)
    shutil.rmtree(differing_result / "analysis")
    output = bytearray((differing_result / "output.wav").read_bytes())
    offset = 12
    while offset + 8 <= len(output):
        chunk_size = struct.unpack_from("<I", output, offset + 4)[0]
        if output[offset : offset + 4] == b"data":
            struct.pack_into(
                output_sample_format,
                output,
                offset + 8 + 3 * output_sample_size,
                0.5,
            )
            break
        offset += 8 + chunk_size + chunk_size % 2
    else:
        raise AssertionError("output WAV has no data chunk")
    (differing_result / "output.wav").write_bytes(output)
    differing = subprocess.run(
        [
            sys.executable,
            str(analyzer),
            str(differing_result),
            "--source",
            str(identity_source),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if differing.returncode != 0:
        raise AssertionError(differing.stderr)
    differing_identity = json.loads(
        (differing_result / "analysis" / "baseline-v1.json").read_text()
    )["identity"]
    if differing_identity != {
        "equal": False,
        "differingSampleCount": 1,
        "maximumAbsoluteError": 0.25,
        "firstMismatch": {
            "sampleIndex": 3,
            "frame": 3,
            "channel": 0,
            "sourceSample": 0.75,
            "renderedSample": 0.5,
            "absoluteError": 0.25,
        },
    }:
        raise AssertionError(f"unexpected mismatch analysis: {differing_identity}")

    stereo_result = workspace / "stereo-result"
    rendered = subprocess.run(
        [
            str(renderer),
            "render",
            "--input",
            str(stereo_fixture),
            "--output",
            str(stereo_result),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if rendered.returncode != 0:
        raise AssertionError(rendered.stderr)
    analyzed = subprocess.run(
        [sys.executable, str(analyzer), str(stereo_result)],
        check=False,
        capture_output=True,
        text=True,
    )
    if analyzed.returncode != 0:
        raise AssertionError(analyzed.stderr)
    stereo = json.loads(
        (stereo_result / "analysis" / "baseline-v1.json").read_text()
    )
    if stereo["channels"] != [
        {
            "channel": 0,
            "peakAbsoluteSample": 1.0,
            "rmsAmplitude": 0.685714066775088,
            "sumOfSquares": 7.993464283344907,
        },
        {
            "channel": 1,
            "peakAbsoluteSample": 1.0,
            "rmsAmplitude": 0.6643225576396319,
            "sumOfSquares": 7.502515830010654,
        },
    ]:
        raise AssertionError(f"unexpected per-channel metrics: {stereo['channels']}")
    if stereo["combined"] != {
        "peakAbsoluteSample": 1.0,
        "rmsAmplitude": 0.6751030447132096,
        "sumOfSquares": 15.49598011335556,
    }:
        raise AssertionError(f"unexpected combined metrics: {stereo['combined']}")

    non_finite_result = workspace / "non-finite-result"
    shutil.copytree(render_result, non_finite_result)
    shutil.rmtree(non_finite_result / "analysis")
    output = bytearray((non_finite_result / "output.wav").read_bytes())
    offset = 12
    while offset + 8 <= len(output):
        chunk_size = struct.unpack_from("<I", output, offset + 4)[0]
        if output[offset : offset + 4] == b"data":
            struct.pack_into(
                output_sample_format,
                output,
                offset + 8,
                float("nan"),
            )
            break
        offset += 8 + chunk_size + chunk_size % 2
    (non_finite_result / "output.wav").write_bytes(output)
    analyzed = subprocess.run(
        [sys.executable, str(analyzer), str(non_finite_result)],
        check=False,
        capture_output=True,
        text=True,
    )
    if analyzed.returncode != 0:
        raise AssertionError(analyzed.stderr)
    non_finite = json.loads(
        (non_finite_result / "analysis" / "baseline-v1.json").read_text()
    )
    if non_finite["audio"]["nonFiniteSampleCount"] != 1:
        raise AssertionError("analyzer did not count non-finite samples")


if __name__ == "__main__":
    main()
