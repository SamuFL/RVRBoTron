#!/usr/bin/env python3

import argparse
import hashlib
import json
import math
import os
import struct
import sys
import tempfile
from pathlib import Path


ANALYZER_NAME = "baseline"
ANALYZER_VERSION = 1
ARTIFACT_NAME = f"{ANALYZER_NAME}-v{ANALYZER_VERSION}.json"


def inspect_wav(path: Path):
    file_size = path.stat().st_size
    with path.open("rb") as source:
        header = source.read(12)
        if len(header) != 12 or header[:4] != b"RIFF" or header[8:] != b"WAVE":
            raise ValueError(f"{path} is not a RIFF/WAVE file")

        format_info = None
        data_offset = None
        data_size = None
        offset = 12
        while offset + 8 <= file_size:
            source.seek(offset)
            chunk_header = source.read(8)
            chunk_id = chunk_header[:4]
            chunk_size = struct.unpack_from("<I", chunk_header, 4)[0]
            chunk_end = offset + 8 + chunk_size
            if chunk_end > file_size:
                raise ValueError(f"{path} contains a truncated WAV chunk")
            if chunk_id == b"fmt ":
                if chunk_size < 16:
                    raise ValueError(f"{path} contains a truncated fmt chunk")
                format_bytes = source.read(16)
                format_info = struct.unpack("<HHIIHH", format_bytes)
            elif chunk_id == b"data":
                data_offset = offset + 8
                data_size = chunk_size
            offset = chunk_end + chunk_size % 2

    if format_info is None or data_offset is None:
        raise ValueError(f"{path} is missing WAV format or audio data")

    format_tag, channels, sample_rate, _, block_align, sample_bits = format_info
    sample_size = sample_bits // 8
    if channels == 0 or block_align != channels * sample_size:
        raise ValueError(f"{path} has invalid channel or sample alignment")
    if data_size % block_align:
        raise ValueError(f"{path} contains a partial audio frame")
    if not (
        (format_tag == 1 and sample_bits in (16, 24, 32))
        or (format_tag == 3 and sample_bits in (32, 64))
    ):
        raise ValueError(f"{path} has an unsupported WAV encoding")

    return {
        "path": path,
        "channels": channels,
        "sampleRate": sample_rate,
        "frameCount": data_size // block_align,
        "formatTag": format_tag,
        "sampleBits": sample_bits,
        "sampleSize": sample_size,
        "blockAlign": block_align,
        "dataOffset": data_offset,
        "dataSize": data_size,
    }


def decoded_samples(wav):
    remaining = wav["dataSize"]
    block_bytes = 8192 * wav["blockAlign"]
    with wav["path"].open("rb") as source:
        source.seek(wav["dataOffset"])
        while remaining:
            chunk = source.read(min(block_bytes, remaining))
            if not chunk:
                raise ValueError(f'{wav["path"]} contains truncated audio data')
            remaining -= len(chunk)
            if wav["formatTag"] == 1:
                scale = float(1 << (wav["sampleBits"] - 1))
                for index in range(0, len(chunk), wav["sampleSize"]):
                    yield int.from_bytes(
                        chunk[index : index + wav["sampleSize"]],
                        byteorder="little",
                        signed=True,
                    ) / scale
            else:
                sample_format = "f" if wav["sampleBits"] == 32 else "d"
                for (sample,) in struct.iter_unpack(
                    "<" + sample_format,
                    chunk,
                ):
                    yield sample


def sample_metrics(samples):
    peak = 0.0
    finite_count = 0
    non_finite_count = 0

    def squared_samples():
        nonlocal peak, finite_count, non_finite_count
        for sample in samples:
            if math.isfinite(sample):
                finite_count += 1
                peak = max(peak, abs(sample))
                yield sample * sample
            else:
                non_finite_count += 1

    sum_of_squares = math.fsum(squared_samples())
    return {
        "peakAbsoluteSample": peak,
        "rmsAmplitude": (
            math.sqrt(sum_of_squares / finite_count) if finite_count else 0.0
        ),
        "sumOfSquares": sum_of_squares,
    }, non_finite_count


def baseline_analysis(wav):
    channels = wav["channels"]
    channel_metrics = []
    for channel in range(channels):
        metrics, _ = sample_metrics(
            sample
            for index, sample in enumerate(decoded_samples(wav))
            if index % channels == channel
        )
        channel_metrics.append({"channel": channel, **metrics})
    combined_metrics, non_finite_count = sample_metrics(decoded_samples(wav))

    return {
        "formatVersion": 1,
        "analyzer": ANALYZER_NAME,
        "analyzerVersion": ANALYZER_VERSION,
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
            "frameCount": wav["frameCount"],
            "durationSeconds": wav["frameCount"] / wav["sampleRate"],
            "sampleRate": wav["sampleRate"],
            "channelCount": channels,
            "nonFiniteSampleCount": non_finite_count,
        },
        "channels": channel_metrics,
        "combined": combined_metrics,
    }


def selected_precision(samples, precision):
    if precision == "float64":
        return samples
    if precision != "float32":
        raise ValueError(f"unknown render sample precision: {precision}")
    return (
        struct.unpack("<f", struct.pack("<f", sample))[0]
        for sample in samples
    )


def identity_analysis(source, rendered, precision):
    if (
        source["channels"] != rendered["channels"]
        or source["sampleRate"] != rendered["sampleRate"]
        or source["frameCount"] != rendered["frameCount"]
    ):
        raise ValueError("source audio facts do not match output.wav")

    source_samples = selected_precision(decoded_samples(source), precision)
    rendered_samples = decoded_samples(rendered)
    differing_count = 0
    maximum_error = 0.0
    first_mismatch = None
    channels = rendered["channels"]
    for index, (source_sample, rendered_sample) in enumerate(
        zip(source_samples, rendered_samples)
    ):
        if not (math.isfinite(source_sample) and math.isfinite(rendered_sample)):
            raise ValueError(
                f"identity comparison encountered non-finite sample at index {index}"
            )
        error = abs(source_sample - rendered_sample)
        if source_sample != rendered_sample:
            differing_count += 1
            maximum_error = max(maximum_error, error)
            if first_mismatch is None:
                first_mismatch = {
                    "sampleIndex": index,
                    "frame": index // channels,
                    "channel": index % channels,
                    "sourceSample": source_sample,
                    "renderedSample": rendered_sample,
                    "absoluteError": error,
                }
    return {
        "equal": differing_count == 0,
        "differingSampleCount": differing_count,
        "maximumAbsoluteError": maximum_error,
        "firstMismatch": first_mismatch,
    }


def file_sha256(path: Path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def encoded_analysis(analysis):
    return (
        json.dumps(
            analysis,
            allow_nan=False,
            indent=2,
            sort_keys=True,
        )
        + "\n"
    ).encode()


def publish_artifact(render_result: Path, contents: bytes):
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
        description="Analyze a RVRBoTron Render Result."
    )
    parser.add_argument("render_result", type=Path)
    parser.add_argument("--source", type=Path)
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    try:
        render_result = arguments.render_result
        metadata = json.loads((render_result / "render.json").read_text())
        wav = inspect_wav(render_result / "output.wav")
        if (
            wav["frameCount"] != metadata["frames"]
            or wav["sampleRate"] != metadata["sampleRate"]
            or wav["channels"] != metadata["channels"]
        ):
            raise ValueError("render metadata does not match output.wav")

        analysis = baseline_analysis(wav)
        if arguments.source is not None:
            source_hash = file_sha256(arguments.source)
            if source_hash != metadata["inputSha256"]:
                raise ValueError(
                    "source SHA-256 does not match render metadata"
                )
            source = inspect_wav(arguments.source)
            analysis["identity"] = identity_analysis(
                source,
                wav,
                metadata["samplePrecision"],
            )
        artifact = publish_artifact(render_result, encoded_analysis(analysis))
        audio = analysis["audio"]
        channel_word = "channel" if audio["channelCount"] == 1 else "channels"
        print(
            f'{audio["frameCount"]} frames, '
            f'{audio["durationSeconds"]:.9f} s, '
            f'{audio["sampleRate"]} Hz, '
            f'{audio["channelCount"]} {channel_word}, '
            f'{audio["nonFiniteSampleCount"]} non-finite samples'
        )
        print(artifact)
        return 0
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"analysis failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
