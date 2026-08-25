#!/usr/bin/env python3

import argparse
import json
import math
import os
import sys
import tempfile
from pathlib import Path

import analyze_render


ANALYZER_NAME = "diffusion"
ANALYZER_VERSION = 1
ARTIFACT_NAME = f"{ANALYZER_NAME}-v{ANALYZER_VERSION}.json"


def verified_capture(render_result, metadata, capture, expected_frames):
    relative = Path(capture["path"])
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError("Stage capture path must remain inside Render Result")
    path = render_result / relative
    if analyze_render.file_sha256(path) != capture["sha256"]:
        raise ValueError(f"Stage capture SHA-256 mismatch: {relative}")
    wav = analyze_render.inspect_wav(path)
    expected = (
        capture["channels"],
        capture["sampleRate"],
        capture["frames"],
    )
    actual = (wav["channels"], wav["sampleRate"], wav["frameCount"])
    if actual != expected:
        raise ValueError(f"Stage capture manifest does not match {relative}")
    if (
        wav["sampleRate"] != metadata["sampleRate"]
        or wav["frameCount"] != expected_frames
        or capture["frames"] != expected_frames
    ):
        raise ValueError(
            f"Stage capture is not a complete finite response: {relative}"
        )
    return wav


def measured_energy(wav):
    metrics, non_finite = analyze_render.sample_metrics(
        analyze_render.decoded_samples(wav)
    )
    if non_finite:
        raise ValueError("Stage capture contains non-finite samples")
    return {
        "channelCount": wav["channels"],
        "sumOfSquares": metrics["sumOfSquares"],
    }


def orthogonality_evidence(step):
    matrix = step["matrix"]
    dimension = len(matrix)
    if dimension == 0 or any(len(row) != dimension for row in matrix):
        raise ValueError("resolved Diffusion Step matrix is not square")
    errors = []
    for row_a in range(dimension):
        for row_b in range(dimension):
            dot = math.fsum(
                matrix[row_a][column] * matrix[row_b][column]
                for column in range(dimension)
            )
            expected = 1.0 if row_a == row_b else 0.0
            errors.append(abs(dot - expected))
    maximum = max(errors)
    rms = math.sqrt(math.fsum(error * error for error in errors) / len(errors))
    return {
        "stepIndex": step["index"],
        "dimension": dimension,
        "maximumAbsoluteError": maximum,
        "rmsError": rms,
        "orthogonal": maximum <= 1e-12,
    }


def publish_artifact(render_result, contents):
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
    output = analyze_render.inspect_wav(render_result / "output.wav")
    if (
        output["sampleRate"] != metadata["sampleRate"]
        or output["channels"] != metadata["channels"]
        or output["frameCount"] != metadata["frames"]
    ):
        raise ValueError("render metadata does not match output.wav")

    stages = resolved["composition"]["stages"]
    if [stage["type"] for stage in stages] != [
        "split",
        "diffuser",
        "downmix",
    ]:
        raise ValueError("diffusion analysis requires a finite Diffuser")
    split = stages[0]
    diffuser = stages[1]
    if source["channels"] != split["inputChannels"]:
        raise ValueError("source Channel count does not match resolved Split")
    total_samples = diffuser["totalSamples"]
    if not isinstance(total_samples, int) or total_samples < 0:
        raise ValueError("resolved Diffuser totalSamples is invalid")
    expected_frames = metadata["inputFrames"] + total_samples
    if metadata["frames"] != expected_frames:
        raise ValueError(
            "render metadata does not contain the complete finite response: "
            f"expected {expected_frames} frames"
        )
    if output["frameCount"] != expected_frames:
        raise ValueError(
            "output.wav does not contain the complete finite response: "
            f"expected {expected_frames} frames"
        )
    captures = metadata.get("stageCaptures")
    if metadata.get("stageCaptureProfile") != "all-v1" or not captures:
        raise ValueError("diffusion analysis requires --capture-stages all")

    split_capture = None
    diffusion_captures = {}
    for capture in captures:
        wav = verified_capture(
            render_result, metadata, capture, expected_frames
        )
        if capture["boundary"] == "split":
            if split_capture is not None:
                raise ValueError("Stage capture manifest has duplicate Split")
            split_capture = wav
        elif capture["boundary"] == "diffusion-step":
            if capture["index"] in diffusion_captures:
                raise ValueError(
                    "Stage capture manifest has duplicate Diffusion Step "
                    f"{capture['index']}"
                )
            diffusion_captures[capture["index"]] = wav
    if split_capture is None:
        raise ValueError("Stage capture manifest is missing Split")

    split_energy = measured_energy(split_capture)
    split_sum_of_squares = split_energy["sumOfSquares"]
    step_energy = []
    orthogonality = []
    for step in diffuser["steps"]:
        index = step["index"]
        if index not in diffusion_captures:
            raise ValueError(
                f"Stage capture manifest is missing Diffusion Step {index}"
            )
        evidence = measured_energy(diffusion_captures[index])
        step_sum_of_squares = evidence["sumOfSquares"]
        evidence.update(
            {
                "index": index,
                "reference": "split",
                "ratio": (
                    step_sum_of_squares / split_sum_of_squares
                    if split_sum_of_squares
                    else 1.0
                ),
                "relativeError": (
                    abs(step_sum_of_squares - split_sum_of_squares)
                    / split_sum_of_squares
                    if split_sum_of_squares
                    else abs(step_sum_of_squares - split_sum_of_squares)
                ),
            }
        )
        step_energy.append(evidence)
        orthogonality.append(orthogonality_evidence(step))

    capture_frames = {capture["frames"] for capture in captures}
    if len(capture_frames) != 1:
        raise ValueError("Stage captures do not share one complete timeline")
    analysis = {
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
            "resolvedDiffuserTotalSamples": total_samples,
            "expectedFrames": expected_frames,
            "outputFrames": metadata["frames"],
            "tailFrames": metadata["frames"] - metadata["inputFrames"],
            "stageCaptureFrames": capture_frames.pop(),
        },
        "energy": {
            "split": split_energy,
            "diffusionSteps": step_energy,
        },
        "orthogonality": orthogonality,
    }
    return analysis


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Analyze finite diffusion evidence in a Render Result."
    )
    parser.add_argument("render_result", type=Path)
    parser.add_argument("--source", required=True, type=Path)
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    try:
        analysis = analyze(arguments.render_result, arguments.source)
        contents = (
            json.dumps(analysis, allow_nan=False, indent=2, sort_keys=True)
            + "\n"
        ).encode()
        artifact = publish_artifact(arguments.render_result, contents)
        print(
            f'{analysis["completeResponse"]["outputFrames"]} frames, '
            f'{len(analysis["energy"]["diffusionSteps"])} Diffusion Step, '
            f'{analysis["orthogonality"][0]["maximumAbsoluteError"]:.3g} '
            "maximum orthogonality error"
        )
        print(artifact)
        return 0
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"diffusion analysis failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
