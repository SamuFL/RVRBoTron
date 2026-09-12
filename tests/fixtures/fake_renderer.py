#!/usr/bin/env python3

"""A stand-in renderer CLI for Research bench hardening tests (issue #139).

Speaks just enough of the real contract (src/cli/main.cpp,
src/cli/RenderMetadata.cpp) for serve.py to accept it and drive it: a usage
line with no arguments, an --error-format json invalid_arguments diagnostic
when --input/--output are missing, and, on a full render invocation, a
render.json and output.wav under --output.

The real renderer takes minutes for some configurations and cannot be told
to fail or stall on demand, which several hardening behaviors (concurrency
rejection, terminal interruption, float64 output rejection) need to
exercise deterministically. This stand-in reads test-only knobs from extra
top-level keys in the --config JSON -- keys the real renderer would simply
ignore -- rather than adding any such thing to serve.py itself:

  _sleepSeconds  -- wait this long before finishing, to simulate a slow render
  _samplePrecision -- report this in render.json instead of "float32"

Before either sleeping or finishing, a PID file and a STARTED file are
written under --output, so a test can wait for the render to have actually
begun and can identify the exact process to check for after killing it.
"""

import json
import os
import sys
import time
from pathlib import Path

USAGE = "usage: rvrbotron <render|benchmark> ...\n"


def parse_options(args):
    options = {}
    index = 0
    while index < len(args):
        token = args[index]
        if token.startswith("--"):
            key = token[2:]
            if index + 1 < len(args) and not args[index + 1].startswith("--"):
                options[key] = args[index + 1]
                index += 2
            else:
                options[key] = True
                index += 1
        else:
            index += 1
    return options


def fail(category, reason, exit_code=1):
    sys.stderr.write(json.dumps({"category": category, "reason": reason}) + "\n")
    return exit_code


def write_minimal_wav(path):
    """One silent mono PCM16 frame: just enough to be a well-formed WAV."""
    frame = b"\x00\x00"
    header = (
        b"RIFF"
        + (36 + len(frame)).to_bytes(4, "little")
        + b"WAVE"
        + b"fmt "
        + (16).to_bytes(4, "little")
        + (1).to_bytes(2, "little")
        + (1).to_bytes(2, "little")
        + (48000).to_bytes(4, "little")
        + (48000 * 2).to_bytes(4, "little")
        + (2).to_bytes(2, "little")
        + (16).to_bytes(2, "little")
        + b"data"
        + len(frame).to_bytes(4, "little")
    )
    path.write_bytes(header + frame)


def main(argv):
    if not argv or argv[0] != "render":
        sys.stderr.write(USAGE)
        return 1

    options = parse_options(argv[1:])
    if "input" not in options or "output" not in options:
        return fail("invalid_arguments", "--input and --output are required", 7)

    output_dir = Path(options["output"])
    output_dir.mkdir(parents=True, exist_ok=True)

    config = {}
    config_path = options.get("config")
    if config_path:
        try:
            config = json.loads(Path(config_path).read_text())
        except ValueError:
            return fail("invalid_configuration", "malformed JSON", 3)

    (output_dir / "PID").write_text(str(os.getpid()))
    (output_dir / "STARTED").write_text("1")

    sleep_seconds = config.get("_sleepSeconds")
    if sleep_seconds:
        time.sleep(float(sleep_seconds))

    metadata = {
        "formatVersion": 1,
        "rendererVersion": "fake",
        "platform": "test",
        "architecture": "test",
        "samplePrecision": config.get("_samplePrecision", "float32"),
        "blockSize": 512,
        "configurationInput": "requested",
        "inputFilename": Path(options["input"]).name,
        "inputSha256": "0" * 64,
        "sampleRate": 48000,
        "channels": 1,
        "frames": 1,
        "inputFrames": 1,
        "preDelayFrames": 0,
        "tailBudgetFrames": 0,
    }
    (output_dir / "render.json").write_text(json.dumps(metadata))
    write_minimal_wav(output_dir / "output.wav")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
