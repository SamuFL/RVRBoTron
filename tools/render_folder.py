#!/usr/bin/env python3

"""Applies one Requested configuration across a folder of WAVs (issue #151).

The sampled-instrument workflow: find a reverb you like in the Research
bench, download its request.json, then render every note and round robin in
a raw sample folder through it into a destination folder.

This is production tooling, not an experiment. What it writes are bare WAVs
-- no resolved.json, no render.json -- so they are deliberately **not**
Render Results and carry no reproducibility claim. The request copied beside
them answers "which patch made this library?"; it is a convenience, not
evidence, since it pins neither the renderer build nor the resolved DSP
parameters. Use the renderer directly, or a sweep, when you want evidence.

Two consequences of the renderer's own contract are worth knowing here:

- It always writes IEEE float32 (src/io/WavStream.cpp), so this tool
  converts to PCM at --bit-depth for samplers that will not load float.
- It preserves no metadata: `smpl` loop points, cue markers and LIST/INFO
  tags in a source do not survive the render. Fine for one-shots; not for
  looped sustains.
"""

import argparse
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

from experiment_runner import run_command

BIT_DEPTHS = (16, 24)


def parse_arguments(argv):
    parser = argparse.ArgumentParser(
        description=(
            "Render every WAV in a folder through one Requested configuration."
        )
    )
    parser.add_argument("--source-dir", required=True, type=Path)
    parser.add_argument("--dest-dir", required=True, type=Path)
    parser.add_argument(
        "--config",
        required=True,
        type=Path,
        help="a Requested configuration, e.g. the request.json the bench wrote",
    )
    parser.add_argument("--renderer", required=True, type=Path)
    parser.add_argument(
        "--suffix",
        default="",
        help=(
            "appended to each source's stem; empty by default so filenames "
            "pass through unchanged for instrument builders that parse them. "
            "A suffix starting with a dash needs the equals form, as in "
            "--suffix=-wet, or argparse reads it as another flag"
        ),
    )
    parser.add_argument(
        "--bit-depth", type=int, default=24, choices=BIT_DEPTHS
    )
    return parser.parse_args(argv)


def read_float_wav(path):
    """Decode the renderer's own output: canonical IEEE float, fmt + data."""
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path} is not a RIFF/WAVE file")
    offset = 12
    format_info = None
    payload = None
    while offset + 8 <= len(data):
        chunk_id = data[offset : offset + 4]
        (size,) = struct.unpack_from("<I", data, offset + 4)
        if chunk_id == b"fmt ":
            format_info = struct.unpack_from("<HHIIHH", data, offset + 8)
        elif chunk_id == b"data":
            payload = data[offset + 8 : offset + 8 + size]
        offset += 8 + size + size % 2
    if format_info is None or payload is None:
        raise ValueError(f"{path} is missing WAV format or audio data")
    format_tag, channels, sample_rate, _, _, sample_bits = format_info
    if format_tag != 3 or sample_bits not in (32, 64):
        raise ValueError(f"{path} is not the IEEE float WAV the renderer writes")
    dtype = "<f4" if sample_bits == 32 else "<f8"
    return np.frombuffer(payload, dtype=dtype).astype(np.float64), channels, sample_rate


def encode_pcm(samples, bit_depth):
    """Scale float samples to little-endian PCM of the requested width.

    Scales by 2^(bits-1) - 1 rather than 2^(bits-1), which cannot overflow
    at exactly +1.0. The asymmetry against a decoder dividing by 2^(bits-1)
    costs at most one LSB -- -144 dBFS at 24-bit, -96 at 16 -- and the
    contract test pins that bound by round-tripping an identity render.
    """
    peak = float(np.max(np.abs(samples))) if samples.size else 0.0
    clipped = np.clip(samples, -1.0, 1.0)
    full_scale = (1 << (bit_depth - 1)) - 1
    scaled = np.rint(clipped * full_scale).astype("<i4")
    if bit_depth == 16:
        return scaled.astype("<i2").tobytes(), peak
    # 24-bit: keep the low three bytes of each little-endian word.
    return scaled.view(np.uint8).reshape(-1, 4)[:, :3].tobytes(), peak


def write_pcm_wav(path, payload, channels, sample_rate, bit_depth):
    block_align = channels * (bit_depth // 8)
    fmt = struct.pack(
        "<HHIIHH",
        1,  # PCM
        channels,
        sample_rate,
        sample_rate * block_align,
        block_align,
        bit_depth,
    )
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(payload)) + payload
    if len(payload) % 2:
        chunks += b"\x00"
    path.write_bytes(
        b"RIFF" + struct.pack("<I", len(chunks) + 4) + b"WAVE" + chunks
    )


def renderer_failure(stderr_text):
    """The renderer's own JSON diagnostic, as one line to print.

    --error-format json puts category, reason and (for configuration
    errors) the exact JSON Pointer on stderr; anything unparseable is
    surfaced verbatim rather than swallowed.
    """
    text = (stderr_text or "").strip()
    for line in reversed(text.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                parsed = json.loads(line)
            except ValueError:
                continue
            detail = f"{parsed.get('category', 'render_failed')}: " + parsed.get(
                "reason", "render failed"
            )
            if parsed.get("location"):
                detail += f"\n  at {parsed['location']}"
            return detail
    return text or "render failed"


def render_one(renderer, source, config, destination, bit_depth):
    """Render one source and write it as PCM. Returns its peak amplitude."""
    with tempfile.TemporaryDirectory() as scratch:
        result_dir = Path(scratch) / "result"
        completed = run_command(
            renderer,
            "render",
            "--input",
            source,
            "--config",
            config,
            "--output",
            result_dir,
            "--error-format",
            "json",
        )
        if completed.returncode != 0:
            raise RuntimeError(renderer_failure(completed.stderr))
        samples, channels, sample_rate = read_float_wav(result_dir / "output.wav")

    if not np.all(np.isfinite(samples)):
        raise RuntimeError(
            "the render produced non-finite samples, which cannot be written as "
            "PCM -- report this, it is a renderer bug rather than a level problem"
        )
    payload, peak = encode_pcm(samples, bit_depth)
    write_pcm_wav(destination, payload, channels, sample_rate, bit_depth)
    return peak


def main(argv=None):
    arguments = parse_arguments(argv if argv is not None else sys.argv[1:])

    if not arguments.source_dir.is_dir():
        sys.stderr.write(f"no source directory at {arguments.source_dir}\n")
        return 1
    if not arguments.config.is_file():
        sys.stderr.write(f"no Requested configuration at {arguments.config}\n")
        return 1
    # Rendering into the source folder under the same names overwrites the
    # raw material in place, which for recorded samples is unrecoverable.
    # A suffix makes the outputs distinct, so that stays allowed.
    if (
        not arguments.suffix
        and arguments.dest_dir.resolve() == arguments.source_dir.resolve()
    ):
        sys.stderr.write(
            "--dest-dir is the source directory and --suffix is empty, which "
            "would overwrite the sources with their own renders. Choose a "
            "different --dest-dir, or pass a --suffix.\n"
        )
        return 1

    sources = sorted(arguments.source_dir.glob("*.wav"))
    if not sources:
        sys.stderr.write(f"no *.wav files in {arguments.source_dir}\n")
        return 1

    arguments.dest_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(arguments.config, arguments.dest_dir / "request.json")

    clipped = []
    for index, source in enumerate(sources, start=1):
        destination = (
            arguments.dest_dir / f"{source.stem}{arguments.suffix}.wav"
        )
        print(f"[{index}/{len(sources)}] {source.name} -> {destination.name}")
        try:
            peak = render_one(
                arguments.renderer,
                source,
                arguments.config,
                destination,
                arguments.bit_depth,
            )
        except (RuntimeError, ValueError) as error:
            # Fail fast: in a uniformly recorded folder a failure is
            # systematic, so continuing would collect the same error once
            # per remaining file.
            sys.stderr.write(f"{source.name}: {error}\n")
            sys.stderr.write(
                f"stopped after {index - 1} of {len(sources)} rendered\n"
            )
            return 1
        if peak > 1.0:
            clipped.append((destination.name, peak))

    print(f"\n{len(sources)} rendered into {arguments.dest_dir}")
    if clipped:
        worst = max(20.0 * np.log10(peak) for _, peak in clipped)
        names = ", ".join(name for name, _ in clipped)
        print(
            f"warning: {len(clipped)} clipped (max +{worst:.1f} dBFS): {names}\n"
            "Lower dryDb/wetDb in the request and render again if that matters."
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
