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
    channels = sample_rate = bits = None
    samples = None
    while offset + 8 <= len(data):
        chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
        chunk = data[offset + 8 : offset + 8 + chunk_size]
        if data[offset : offset + 4] == b"fmt ":
            _, channels, sample_rate = struct.unpack_from("<HHI", chunk)
            bits = struct.unpack_from("<H", chunk, 14)[0]
        elif data[offset : offset + 4] == b"data":
            sample_format = "f" if bits == 32 else "d"
            samples = struct.unpack(
                "<" + sample_format * (len(chunk) // (bits // 8)), chunk
            )
        offset += 8 + chunk_size + chunk_size % 2
    if samples is None:
        raise AssertionError(f"{path} has no audio data")
    return channels, sample_rate, bits, samples


def run(renderer: Path, *arguments):
    return subprocess.run(
        [str(renderer), "render", *map(str, arguments)],
        check=False,
        capture_output=True,
        text=True,
    )


def run_ok(renderer: Path, *arguments):
    completed = run(renderer, *arguments)
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)
    return completed


def find_stage(resolved, stage_type):
    return next(
        stage
        for stage in resolved["composition"]["stages"]
        if stage["type"] == stage_type
    )


def main():
    renderer = Path(sys.argv[1])
    fixture = Path(sys.argv[2])
    workspace = Path(sys.argv[3])
    sample_bits = int(sys.argv[4])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    # N=2, delayStrategy "even" throughout (deterministic, no
    # positional-random draws) so the Feedback Loop's resolved delays are
    # hand-derivable exactly as in test_feedback_loop_cli.py, and are
    # identical to a loop-only render's -- proving loop time is unaffected
    # by the Diffuser placed in front of it.
    loop_only_request = {
        "formatVersion": 2,
        "seed": 7,
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
                {"type": "downmix", "strategy": "select"},
            ]
        },
    }
    diffuser_loop_request = {
        "formatVersion": 2,
        "seed": 7,
        "composition": {
            "stages": [
                loop_only_request["composition"]["stages"][0],
                {
                    "type": "diffuser",
                    "steps": 1,
                    "totalMs": 0.1,
                    "distribution": "even",
                    "step": {
                        "delayStrategy": "even",
                        "mix": "hadamard",
                        "shuffle": True,
                        "polarity": "seeded-random",
                    },
                },
                loop_only_request["composition"]["stages"][1],
                {"type": "downmix", "strategy": "select"},
            ]
        },
    }

    loop_only_path = workspace / "loop-only-request.json"
    loop_only_path.write_text(json.dumps(loop_only_request, indent=2) + "\n")
    diffuser_loop_path = workspace / "diffuser-loop-request.json"
    diffuser_loop_path.write_text(
        json.dumps(diffuser_loop_request, indent=2) + "\n"
    )

    loop_only_result = workspace / "loop-only-result"
    # Resolved delays here are 48/96 samples, below the default 512-frame
    # block size, so a legal --block-size (#53) must be requested explicitly.
    run_ok(
        renderer,
        "--input",
        fixture,
        "--config",
        loop_only_path,
        "--output",
        loop_only_result,
        "--block-size",
        "32",
    )
    result = workspace / "result"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--config",
        diffuser_loop_path,
        "--output",
        result,
        "--block-size",
        "32",
    )

    channels, sample_rate, bits, samples = read_float_wav(result / "output.wav")
    if (channels, sample_rate, bits) != (2, 48000, sample_bits):
        raise AssertionError(
            "Diffuser-into-loop render did not emit canonical stereo"
        )

    # The four-stage Composition shape is accepted, and each stage lands at
    # its expected position.
    resolved = json.loads((result / "resolved.json").read_text())
    stage_types = [stage["type"] for stage in resolved["composition"]["stages"]]
    if stage_types != ["split", "diffuser", "feedback-loop", "downmix"]:
        raise AssertionError(f"unexpected resolved stage order: {stage_types}")

    diffuser = find_stage(resolved, "diffuser")
    loop = find_stage(resolved, "feedback-loop")

    # Loop time for a Channel remains that Channel's own feedback delay,
    # unaffected by Diffuser settings: the loop's resolved fields here are
    # identical to the loop-only render's.
    loop_only_resolved = json.loads(
        (loop_only_result / "resolved.json").read_text()
    )
    loop_only_loop = find_stage(loop_only_resolved, "feedback-loop")
    for field in (
        "delayStrategy",
        "delayMinSamples",
        "delayMaxSamples",
        "delaysSamples",
        "delaysMs",
        "bufferSizes",
        "rt60Sec",
        "decayMargin",
        "tailBudgetSamples",
        "blockSizeBoundSamples",
        "gains",
        "mix",
        "matrix",
    ):
        if loop[field] != loop_only_loop[field]:
            raise AssertionError(
                f"Feedback Loop field {field!r} changed when a Diffuser was "
                f"placed in front of it: {loop[field]} != "
                f"{loop_only_loop[field]}"
            )

    # Total drain is the Diffuser's finite response plus the loop's Tail
    # budget, and render metadata (resolved.json's per-stage fields plus
    # render.json's combined budget) distinguishes the two.
    metadata = json.loads((result / "render.json").read_text())
    expected_combined_tail = diffuser["totalSamples"] + loop["tailBudgetSamples"]
    if metadata["tailBudgetFrames"] != expected_combined_tail:
        raise AssertionError(
            f"unexpected combined tailBudgetFrames: "
            f"{metadata['tailBudgetFrames']} != {expected_combined_tail} "
            f"(diffuser totalSamples={diffuser['totalSamples']}, loop "
            f"tailBudgetSamples={loop['tailBudgetSamples']})"
        )
    if metadata["frames"] != metadata["inputFrames"] + metadata["tailBudgetFrames"]:
        raise AssertionError(
            f"renderer did not drain exactly the combined Tail budget: "
            f"{metadata}"
        )
    if len(samples) != metadata["frames"] * 2:
        raise AssertionError("output.wav frame count does not match render.json")

    # An exact resolved rerender reproduces output.wav bit-identically.
    rerendered = workspace / "rerendered"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--resolved",
        result / "resolved.json",
        "--output",
        rerendered,
        "--block-size",
        "32",
    )
    if (rerendered / "output.wav").read_bytes() != (result / "output.wav").read_bytes():
        raise AssertionError("resolved rerender changed Diffuser-into-loop output")
    if (rerendered / "resolved.json").read_bytes() != (
        result / "resolved.json"
    ).read_bytes():
        raise AssertionError("resolved rerender changed resolved.json")

    # Existing three-stage diffusion renders are unchanged: a
    # [split, diffuser, downmix] render using the same Diffuser settings
    # produces the same Diffuser stage fields as the four-stage render
    # above (the Feedback Loop is only appended after it).
    diffusion_only_request = json.loads(json.dumps(diffuser_loop_request))
    diffusion_only_request["composition"]["stages"] = [
        diffuser_loop_request["composition"]["stages"][0],
        diffuser_loop_request["composition"]["stages"][1],
        {"type": "downmix", "strategy": "select"},
    ]
    diffusion_only_path = workspace / "diffusion-only-request.json"
    diffusion_only_path.write_text(json.dumps(diffusion_only_request, indent=2))
    diffusion_only_result = workspace / "diffusion-only-result"
    run_ok(
        renderer,
        "--input",
        fixture,
        "--config",
        diffusion_only_path,
        "--output",
        diffusion_only_result,
    )
    diffusion_only_resolved = json.loads(
        (diffusion_only_result / "resolved.json").read_text()
    )
    diffusion_only_diffuser = find_stage(diffusion_only_resolved, "diffuser")
    for field in ("totalSamples", "steps"):
        if diffusion_only_diffuser[field] != diffuser[field]:
            raise AssertionError(
                f"three-stage Diffuser field {field!r} differs from the "
                f"four-stage chain's Diffuser: {diffusion_only_diffuser[field]} "
                f"!= {diffuser[field]}"
            )

    # Every other stage ordering involving both a Diffuser and a Feedback
    # Loop is rejected, naming the offending position.
    wrong_orderings = {
        "loop-before-diffuser": [
            diffuser_loop_request["composition"]["stages"][0],
            diffuser_loop_request["composition"]["stages"][2],
            diffuser_loop_request["composition"]["stages"][1],
            {"type": "downmix", "strategy": "select"},
        ],
        "downmix-not-last": [
            diffuser_loop_request["composition"]["stages"][0],
            diffuser_loop_request["composition"]["stages"][1],
            {"type": "downmix", "strategy": "select"},
            diffuser_loop_request["composition"]["stages"][2],
        ],
        "split-not-first": [
            diffuser_loop_request["composition"]["stages"][1],
            diffuser_loop_request["composition"]["stages"][0],
            diffuser_loop_request["composition"]["stages"][2],
            {"type": "downmix", "strategy": "select"},
        ],
    }
    for name, stages in wrong_orderings.items():
        bad_request = json.loads(json.dumps(diffuser_loop_request))
        bad_request["composition"]["stages"] = stages
        bad_path = workspace / f"{name}-request.json"
        bad_path.write_text(json.dumps(bad_request, indent=2))
        bad_result = workspace / f"{name}-result"
        bad = run(
            renderer,
            "--input",
            fixture,
            "--config",
            bad_path,
            "--output",
            bad_result,
        )
        if bad.returncode == 0:
            raise AssertionError(f"{name}: renderer accepted an invalid shape")
        if "/composition/stages" not in bad.stderr:
            raise AssertionError(
                f"{name}: rejection did not name the offending position: "
                f"{bad.stderr}"
            )
        if bad_result.exists():
            raise AssertionError(f"{name}: rejected configuration created a Render Result")


if __name__ == "__main__":
    main()
