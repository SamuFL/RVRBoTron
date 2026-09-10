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

    # duplicate Split feeds every Channel the same mono signal, so every
    # pairwise Correlation is exactly 1.0 there.
    correlation = analysis["correlation"]
    if [entry["boundary"] for entry in correlation] != ["split", "diffusion-step"]:
        raise AssertionError(f"unexpected Correlation boundaries: {correlation}")
    if correlation[0]["meanAbsoluteOffDiagonal"] != 1.0:
        raise AssertionError(
            f"duplicate Split Correlation was not fully correlated: {correlation}"
        )
    step_correlation = correlation[1]["matrix"]
    if len(step_correlation) != 8 or any(len(row) != 8 for row in step_correlation):
        raise AssertionError(f"unexpected Correlation matrix shape: {correlation}")
    if any(step_correlation[i][i] != 1.0 for i in range(8)):
        raise AssertionError(f"Correlation diagonal was not self-correlated: {correlation}")

    # Dense Hadamard mixing shares arrival support across every Channel, so
    # Alignment score is exactly 1.0 (docs/design/reverb/stages/02-diffusion-step.md).
    alignment = analysis["alignment"]
    if alignment["stepIndex"] != 0 or alignment["mean"] != 1.0 or alignment["minimum"] != 1.0:
        raise AssertionError(f"unexpected Alignment evidence: {alignment}")
    if len(alignment["pairwise"]) != 28:  # C(8,2)
        raise AssertionError(f"unexpected Alignment pairwise count: {alignment}")

    # 1 Diffusion Step over 8 Channels: 8 structural Echo paths. This
    # Reference configuration is independently known (test_reference_
    # diffusion_cli.py) to produce exactly 8 wet arrival frames, so every
    # Channel is active at exactly 8 Distinct arrivals with no collisions.
    density = analysis["density"]
    if density["echoPaths"] != 8:
        raise AssertionError(f"unexpected Echo path count: {density}")
    if density["distinctArrivalCounts"] != [8] * 8:
        raise AssertionError(f"unexpected Distinct arrival counts: {density}")
    if density["totalDistinctArrivals"] != 8 or density["bins"] != [8]:
        raise AssertionError(f"unexpected Distinct arrival density: {density}")

    # An all-pass Diffuser's combined N-Channel spectrum is flat by
    # construction; the diagnostic stereo output need not be.
    coloration = analysis["coloration"]
    combined = coloration["combined"]
    if combined["fftLength"] != 128:
        raise AssertionError(f"unexpected Coloration FFT length: {combined}")
    if combined["peakToPeakDb"] > 1e-9 or combined["rmsDb"] > 1e-9:
        raise AssertionError(
            f"all-pass Diffuser's combined spectrum was not flat: {combined}"
        )
    if abs(combined["spectralFlatness"] - 1.0) > 1e-9:
        raise AssertionError(f"unexpected combined spectral flatness: {combined}")
    stereo = coloration["stereo"]
    if stereo["fftLength"] != combined["fftLength"]:
        raise AssertionError(f"stereo Coloration FFT length diverged: {coloration}")
    if not stereo["twelfthOctaveCurve"]:
        raise AssertionError("stereo Coloration curve was empty")

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

    # A fully hand-derivable N=2 fixture (even delays [0,1], no shuffle, no
    # polarity flip, so only the Hadamard mix introduces structure) whose
    # Correlation, Alignment, density, and Coloration evidence is verified
    # against an independently worked NumPy oracle rather than trusting
    # this analyzer's own output.
    small_request = workspace / "small-request.json"
    small_result = workspace / "small-result"
    small_request.write_text(
        json.dumps(
            {
                "formatVersion": 2,
                "seed": 0,
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
                            "totalMs": 0.02,
                            "distribution": "even",
                            "step": {
                                "delayStrategy": "even",
                                "mix": "hadamard",
                                "shuffle": False,
                                "polarity": "none",
                            },
                        },
                        {"type": "downmix", "strategy": "select", "leftChannel": 0, "rightChannel": 1},
                    ]
                },
            }
        )
    )
    small_rendered = subprocess.run(
        [
            str(renderer),
            "render",
            "--input",
            str(fixture),
            "--config",
            str(small_request),
            "--capture-stages",
            "all",
            "--output",
            str(small_result),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if small_rendered.returncode != 0:
        raise AssertionError(small_rendered.stderr)
    small_analyzed = subprocess.run(
        [sys.executable, str(analyzer), str(small_result), "--source", str(fixture)],
        check=False,
        capture_output=True,
        text=True,
    )
    if small_analyzed.returncode != 0:
        raise AssertionError(small_analyzed.stderr)
    small_analysis = json.loads(
        (small_result / "analysis" / "diffusion-v1.json").read_text()
    )

    small_correlation = small_analysis["correlation"]
    if small_correlation[0]["matrix"] != [[1.0, 1.0], [1.0, 1.0]]:
        raise AssertionError(
            f"unexpected duplicate Split Correlation: {small_correlation}"
        )
    step_matrix = small_correlation[1]["matrix"]
    if (
        step_matrix[0][0] != 1.0
        or step_matrix[1][1] != 1.0
        or abs(step_matrix[0][1]) > 1e-15
        or abs(step_matrix[1][0]) > 1e-15
    ):
        raise AssertionError(
            f"unexpected Diffusion Step Correlation: {small_correlation}"
        )

    small_alignment = small_analysis["alignment"]
    if small_alignment["pairwise"] != [
        {"channelA": 0, "channelB": 1, "jaccard": 1.0}
    ] or small_alignment["mean"] != 1.0 or small_alignment["minimum"] != 1.0:
        raise AssertionError(f"unexpected small-fixture Alignment: {small_alignment}")

    small_density = small_analysis["density"]
    if small_density != {
        "stepIndex": 0,
        "activityFloorDb": -120.0,
        "echoPaths": 2,
        "distinctArrivalCounts": [2, 2],
        "totalDistinctArrivals": 2,
        "binMs": 10.0,
        "bins": [2],
    }:
        raise AssertionError(f"unexpected small-fixture density: {small_density}")

    small_coloration = small_analysis["coloration"]
    small_combined = small_coloration["combined"]
    small_stereo = small_coloration["stereo"]
    if small_combined["fftLength"] != 64 or small_stereo["fftLength"] != 64:
        raise AssertionError(f"unexpected small-fixture FFT length: {small_coloration}")
    if len(small_combined["twelfthOctaveCurve"]) != 27:
        raise AssertionError(
            f"unexpected small-fixture 1/12-octave band count: {small_combined}"
        )
    # select Downmix at N=2 with unity compensation passes the Diffusion
    # Step capture straight through to output.wav, so the diagnostic
    # stereo spectrum matches the combined N-Channel spectrum exactly here.
    for metric in ("peakToPeakDb", "rmsDb", "spectralFlatness"):
        if abs(small_combined[metric] - small_stereo[metric]) > 1e-9:
            raise AssertionError(
                f"stereo and combined Coloration diverged at N=2: {small_coloration}"
            )
        if metric == "spectralFlatness":
            if abs(small_combined[metric] - 1.0) > 1e-9:
                raise AssertionError(
                    f"all-pass Diffuser Coloration was not flat: {small_combined}"
                )
        elif small_combined[metric] > 1e-9:
            raise AssertionError(
                f"all-pass Diffuser Coloration was not flat: {small_combined}"
            )

    # Optional pairwise comparison: the same Resolved Configuration
    # rendered at two block sizes must decode to exactly equal evidence.
    compare_a = workspace / "compare-a"
    compare_b = workspace / "compare-b"
    for output, block_size in ((compare_a, 7), (compare_b, 1)):
        rendered = subprocess.run(
            [
                str(renderer),
                "render",
                "--input",
                str(fixture),
                "--config",
                str(small_request),
                "--block-size",
                str(block_size),
                "--capture-stages",
                "all",
                "--output",
                str(output),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        if rendered.returncode != 0:
            raise AssertionError(rendered.stderr)

    compared = subprocess.run(
        [sys.executable, str(analyzer), str(compare_a), "--compare", str(compare_b)],
        check=False,
        capture_output=True,
        text=True,
    )
    if compared.returncode != 0:
        raise AssertionError(
            f"block-size-independent Render Results compared unequal: "
            f"{compared.stdout} {compared.stderr}"
        )
    comparison = json.loads(compared.stdout)
    if not comparison["equal"] or comparison["blockSizes"] != [7, 1]:
        raise AssertionError(f"unexpected pairwise comparison: {comparison}")
    if sorted(entry["path"] for entry in comparison["comparisons"]) != [
        "captures/00-split.wav",
        "captures/01-diffusion-step-0.wav",
        "captures/02-main-stereo.wav",
        "output.wav",
    ]:
        raise AssertionError(f"unexpected pairwise comparison paths: {comparison}")

    corrupted_b = workspace / "compare-b-corrupted"
    shutil.copytree(compare_b, corrupted_b)
    corrupted_output = corrupted_b / "output.wav"
    corrupted_bytes = bytearray(corrupted_output.read_bytes())
    corrupted_bytes[-1] ^= 0xFF
    corrupted_output.write_bytes(bytes(corrupted_bytes))
    mismatched = subprocess.run(
        [sys.executable, str(analyzer), str(compare_a), "--compare", str(corrupted_b)],
        check=False,
        capture_output=True,
        text=True,
    )
    if mismatched.returncode == 0:
        raise AssertionError("pairwise comparison accepted a corrupted Render Result")
    mismatch_report = json.loads(mismatched.stdout)
    if mismatch_report["equal"]:
        raise AssertionError(f"pairwise comparison missed the corruption: {mismatch_report}")
    output_comparison = next(
        entry for entry in mismatch_report["comparisons"] if entry["path"] == "output.wav"
    )
    if output_comparison["equal"] or output_comparison["firstMismatch"] is None:
        raise AssertionError(
            f"pairwise comparison did not report the corrupted sample: {mismatch_report}"
        )

    same_block_size = subprocess.run(
        [sys.executable, str(analyzer), str(compare_a), "--compare", str(compare_a)],
        check=False,
        capture_output=True,
        text=True,
    )
    if same_block_size.returncode == 0:
        raise AssertionError(
            "pairwise comparison accepted two Render Results at the same "
            "block size"
        )
    if "different block sizes" not in same_block_size.stderr:
        raise AssertionError(
            f"unexpected same-block-size failure: {same_block_size.stderr}"
        )

    mismatched_provenance = subprocess.run(
        [sys.executable, str(analyzer), str(compare_a), "--compare", str(render_result)],
        check=False,
        capture_output=True,
        text=True,
    )
    if mismatched_provenance.returncode == 0:
        raise AssertionError(
            "pairwise comparison accepted Render Results with different configurations"
        )
    if "Resolved Configuration" not in mismatched_provenance.stderr:
        raise AssertionError(
            f"unexpected provenance-mismatch failure: {mismatched_provenance.stderr}"
        )


if __name__ == "__main__":
    main()
