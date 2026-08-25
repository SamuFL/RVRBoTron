#!/usr/bin/env python3

import hashlib
import json
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path


def truncate_zero_tail_frame(path):
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
    if data_offset + data_size != len(contents) or data_size < block_align:
        raise AssertionError(f"unexpected WAV layout: {path}")
    removed = contents[data_offset + data_size - block_align :]
    if any(removed):
        raise AssertionError(f"test did not truncate a zero tail frame: {path}")

    del contents[-block_align:]
    struct.pack_into("<I", contents, data_header + 4, data_size - block_align)
    struct.pack_into("<I", contents, 4, len(contents) - 8)
    path.write_bytes(contents)


def main():
    analyzer = Path(sys.argv[1])
    renderer = Path(sys.argv[2])
    fixture = Path(sys.argv[3])
    workspace = Path(sys.argv[4])
    sample_bits = int(sys.argv[5])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    request = workspace / "request.json"
    request.write_text(
        json.dumps(
            {
                "formatVersion": 1,
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
                        {"type": "downmix", "strategy": "select"},
                    ]
                },
            }
        )
    )
    render_result = workspace / "render-result"
    rendered = subprocess.run(
        [
            str(renderer),
            "render",
            "--input",
            str(fixture),
            "--config",
            str(request),
            "--capture-stages",
            "all",
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
    if analyzed.returncode != 0:
        raise AssertionError(analyzed.stderr)

    artifact = render_result / "analysis" / "diffusion-v1.json"
    analysis = json.loads(artifact.read_text())
    if analysis["source"] != {
        "filename": fixture.name,
        "sha256": hashlib.sha256(fixture.read_bytes()).hexdigest(),
        "verified": True,
    }:
        raise AssertionError(f"unexpected source provenance: {analysis}")
    if analysis["completeResponse"] != {
        "inputFrames": 32,
        "resolvedDiffuserTotalSamples": 48,
        "expectedFrames": 80,
        "outputFrames": 80,
        "tailFrames": 48,
        "stageCaptureFrames": 80,
    }:
        raise AssertionError(
            f"unexpected complete-response facts: {analysis}"
        )

    energy = analysis["energy"]
    tolerance = 2e-7 if sample_bits == 32 else 1e-14
    if abs(energy["split"]["sumOfSquares"] - 0.25) > tolerance:
        raise AssertionError(f"unexpected measured Split energy: {energy}")
    if energy["split"]["channelCount"] != 8:
        raise AssertionError("diffusion analysis did not inspect N Channels")
    step_energy = energy["diffusionSteps"][0]
    if abs(step_energy["ratio"] - 1.0) > tolerance:
        raise AssertionError(f"complete-response energy changed: {step_energy}")
    if step_energy["relativeError"] > tolerance:
        raise AssertionError(f"energy error exceeded tolerance: {step_energy}")
    if (
        step_energy["index"] != 0
        or step_energy["reference"] != "split"
    ):
        raise AssertionError("diffusion analysis lost the step index")

    orthogonality = analysis["orthogonality"]
    if orthogonality != [
        {
            "stepIndex": 0,
            "dimension": 8,
            "maximumAbsoluteError": 2.220446049250313e-16,
            "rmsError": 7.850462293418876e-17,
            "orthogonal": True,
        }
    ]:
        raise AssertionError(
            f"unexpected Hadamard orthogonality evidence: {orthogonality}"
        )

    original = artifact.read_bytes()
    modified = artifact.stat().st_mtime_ns
    time.sleep(0.01)
    repeated = subprocess.run(
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
    if repeated.returncode != 0:
        raise AssertionError(repeated.stderr)
    if artifact.read_bytes() != original or artifact.stat().st_mtime_ns != modified:
        raise AssertionError("idempotent diffusion analysis rewrote its artifact")

    wrong_source = workspace / "wrong.wav"
    wrong_source.write_bytes(fixture.read_bytes() + b"\x00")
    rejected = subprocess.run(
        [
            sys.executable,
            str(analyzer),
            str(render_result),
            "--source",
            str(wrong_source),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if rejected.returncode == 0:
        raise AssertionError("diffusion analyzer accepted wrong source")
    if "source SHA-256 does not match render metadata" not in rejected.stderr:
        raise AssertionError(f"unexpected provenance failure: {rejected.stderr}")
    if artifact.read_bytes() != original:
        raise AssertionError("failed provenance check changed analysis")

    tampered_result = workspace / "tampered-result"
    shutil.copytree(render_result, tampered_result)
    shutil.rmtree(tampered_result / "analysis")
    capture = tampered_result / "captures" / "00-split.wav"
    capture.write_bytes(capture.read_bytes() + b"\x00")
    tampered = subprocess.run(
        [
            sys.executable,
            str(analyzer),
            str(tampered_result),
            "--source",
            str(fixture),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if tampered.returncode == 0:
        raise AssertionError("diffusion analyzer accepted tampered Stage capture")
    if "Stage capture SHA-256 mismatch" not in tampered.stderr:
        raise AssertionError(f"unexpected capture failure: {tampered.stderr}")
    if (tampered_result / "analysis").exists():
        raise AssertionError("failed capture verification created analysis")

    truncated_result = workspace / "truncated-tail-result"
    shutil.copytree(render_result, truncated_result)
    shutil.rmtree(truncated_result / "analysis")
    metadata_path = truncated_result / "render.json"
    metadata = json.loads(metadata_path.read_text())
    truncate_zero_tail_frame(truncated_result / "output.wav")
    metadata["frames"] -= 1
    for capture_metadata in metadata["stageCaptures"]:
        capture_path = truncated_result / capture_metadata["path"]
        truncate_zero_tail_frame(capture_path)
        capture_metadata["frames"] -= 1
        capture_metadata["sha256"] = hashlib.sha256(
            capture_path.read_bytes()
        ).hexdigest()
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")

    truncated = subprocess.run(
        [
            sys.executable,
            str(analyzer),
            str(truncated_result),
            "--source",
            str(fixture),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if truncated.returncode == 0:
        raise AssertionError(
            "diffusion analyzer accepted a consistently truncated zero tail"
        )
    if "complete finite response" not in truncated.stderr:
        raise AssertionError(
            f"unexpected truncated-tail failure: {truncated.stderr}"
        )
    if (truncated_result / "analysis").exists():
        raise AssertionError("truncated response created analysis")


if __name__ == "__main__":
    main()
