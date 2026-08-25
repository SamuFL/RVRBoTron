#!/usr/bin/env python3

import hashlib
import json
import os
import platform
import signal
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path


def with_non_finite_sample(source: Path, destination: Path):
    contents = bytearray(source.read_bytes())
    offset = 12
    while offset + 8 <= len(contents):
        chunk_size = struct.unpack_from("<I", contents, offset + 4)[0]
        if contents[offset : offset + 4] == b"data":
            sample_offset = offset + 8 + 6 * 4
            contents[sample_offset : sample_offset + 4] = struct.pack(
                "<f", float("nan")
            )
            destination.write_bytes(contents)
            return
        offset += 8 + chunk_size + chunk_size % 2
    raise AssertionError("fixture has no data chunk")


def run_renderer(renderer: Path, *arguments):
    return subprocess.run(
        [str(renderer), "render", *map(str, arguments)],
        check=False,
        capture_output=True,
        text=True,
    )


def require_json_error(
    renderer: Path,
    expected_code: int,
    expected_category: str,
    *arguments,
    expected_location=None,
):
    completed = run_renderer(
        renderer,
        *arguments,
        "--error-format",
        "json",
    )
    if completed.returncode != expected_code:
        raise AssertionError(
            f"expected exit {expected_code}, got {completed.returncode}: "
            f"{completed.stderr}"
        )
    diagnostic = json.loads(completed.stderr)
    expected_keys = {"category", "exitCode", "reason"}
    if expected_location is not None:
        expected_keys.add("location")
    if set(diagnostic) != expected_keys:
        raise AssertionError(f"unexpected diagnostic fields: {diagnostic}")
    if diagnostic["category"] != expected_category:
        raise AssertionError(f"unexpected diagnostic category: {diagnostic}")
    if diagnostic["exitCode"] != expected_code:
        raise AssertionError(f"unexpected diagnostic exit code: {diagnostic}")
    if expected_location is not None:
        if diagnostic["location"] != expected_location:
            raise AssertionError(f"unexpected diagnostic location: {diagnostic}")
    if not diagnostic["reason"]:
        raise AssertionError("diagnostic reason is empty")
    return diagnostic


def write_sparse_pcm16_wav(path: Path, frame_count: int):
    data_size = frame_count * 2
    fmt = struct.pack("<HHIIHH", 1, 1, 48000, 96000, 2, 16)
    riff_size = 4 + 8 + len(fmt) + 8 + data_size
    with path.open("wb") as output:
        output.write(b"RIFF" + struct.pack("<I", riff_size) + b"WAVE")
        output.write(b"fmt " + struct.pack("<I", len(fmt)) + fmt)
        output.write(b"data" + struct.pack("<I", data_size))
        output.truncate(44 + data_size)


def main():
    renderer = Path(sys.argv[1])
    fixture = Path(sys.argv[2])
    workspace = Path(sys.argv[3])
    sample_bits = int(sys.argv[4])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    invalid_input = workspace / "non-finite.wav"
    with_non_finite_sample(fixture, invalid_input)
    result = workspace / "failed-result"
    completed = subprocess.run(
        [
            str(renderer),
            "render",
            "--input",
            str(invalid_input),
            "--block-size",
            "5",
            "--output",
            str(result),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode == 0:
        raise AssertionError("renderer unexpectedly succeeded")
    if "non-finite input sample" not in completed.stderr:
        raise AssertionError(completed.stderr)
    if result.exists():
        raise AssertionError("failed render published a Render Result")
    if {path.name for path in workspace.iterdir()} != {"non-finite.wav"}:
        raise AssertionError("failed render left temporary state")

    result = workspace / "successful-result"
    completed = subprocess.run(
        [
            str(renderer),
            "render",
            "--input",
            str(fixture),
            "--block-size",
            "5",
            "--output",
            str(result),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)
    if completed.stdout != f"{result}\n":
        raise AssertionError(f"unexpected success output: {completed.stdout!r}")

    machine = platform.machine().lower()
    architecture = {
        "amd64": "x86_64",
        "x86_64": "x86_64",
        "arm64": "arm64",
        "aarch64": "arm64",
    }.get(machine, machine)
    system = platform.system()
    platform_name = {
        "Darwin": "macos",
        "Windows": "windows",
        "Linux": "linux",
    }.get(system, system.lower())
    metadata = json.loads((result / "render.json").read_text())
    expected_metadata = {
        "formatVersion": 1,
        "rendererVersion": "0.1.0",
        "platform": platform_name,
        "architecture": architecture,
        "samplePrecision": f"float{sample_bits}",
        "blockSize": 5,
        "configurationInput": "defaults",
        "inputFilename": fixture.name,
        "inputSha256": hashlib.sha256(fixture.read_bytes()).hexdigest(),
        "sampleRate": 48000,
        "channels": 1,
        "frames": 17,
        "inputFrames": 17,
    }
    if metadata != expected_metadata:
        raise AssertionError(f"unexpected render metadata: {metadata}")

    malformed_config = workspace / "malformed.json"
    malformed_config.write_text('{"seed":')
    malformed_result = workspace / "malformed-result"
    malformed = run_renderer(
        renderer,
        "--input",
        fixture,
        "--config",
        malformed_config,
        "--output",
        malformed_result,
    )
    if malformed.returncode != 2:
        raise AssertionError(f"unexpected malformed JSON exit: {malformed}")
    if not malformed.stderr.startswith("malformed_json at /: malformed JSON:"):
        raise AssertionError(f"unexpected human diagnostic: {malformed.stderr!r}")
    if malformed_result.exists():
        raise AssertionError("malformed JSON published a Render Result")

    malformed_json = require_json_error(
        renderer,
        2,
        "malformed_json",
        "--input",
        fixture,
        "--config",
        malformed_config,
        "--output",
        workspace / "malformed-json-result",
        expected_location="/",
    )
    if not malformed_json["reason"].startswith("malformed JSON:"):
        raise AssertionError(f"unexpected malformed JSON reason: {malformed_json}")

    unsupported_input = workspace / "unsupported.wav"
    unsupported_input.write_bytes(b"not a WAV file")
    require_json_error(
        renderer,
        3,
        "unsupported_audio",
        "--input",
        unsupported_input,
        "--output",
        workspace / "unsupported-result",
    )

    invalid_config = workspace / "invalid.json"
    invalid_config.write_text('{"unexpected": true}')
    require_json_error(
        renderer,
        4,
        "invalid_configuration",
        "--input",
        fixture,
        "--config",
        invalid_config,
        "--output",
        workspace / "invalid-result",
        expected_location="/unexpected",
    )

    require_json_error(
        renderer,
        5,
        "io_failure",
        "--input",
        workspace / "missing.wav",
        "--output",
        workspace / "missing-result",
    )

    require_json_error(
        renderer,
        6,
        "internal_processing_failure",
        "--input",
        fixture,
        "--block-size",
        str((1 << 63) - 1),
        "--output",
        workspace / "internal-result",
    )

    require_json_error(
        renderer,
        7,
        "invalid_arguments",
        "--input",
        fixture,
        "--unknown",
        "value",
        "--output",
        workspace / "argument-result",
    )

    existing_result = workspace / "existing-result"
    existing_result.mkdir()
    require_json_error(
        renderer,
        5,
        "io_failure",
        "--input",
        fixture,
        "--output",
        existing_result,
    )
    if any(existing_result.iterdir()):
        raise AssertionError("renderer changed an existing destination")

    collision_result = workspace / "collision-result"
    stale_temporary = workspace / ".collision-result.rvrbotron-tmp-0"
    stale_temporary.mkdir()
    (stale_temporary / "owner.txt").write_text("not this renderer\n")
    collision = run_renderer(
        renderer,
        "--input",
        fixture,
        "--output",
        collision_result,
    )
    if collision.returncode != 0:
        raise AssertionError(collision.stderr)
    if (stale_temporary / "owner.txt").read_text() != "not this renderer\n":
        raise AssertionError("renderer changed temporary state it did not own")
    shutil.rmtree(stale_temporary)

    if os.name != "nt":
        long_input = workspace / "long-input.wav"
        write_sparse_pcm16_wav(long_input, 32 * 1024 * 1024)
        interrupted_result = workspace / "interrupted-result"
        process = subprocess.Popen(
            [
                str(renderer),
                "render",
                "--input",
                str(long_input),
                "--output",
                str(interrupted_result),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        temporary_pattern = ".interrupted-result.rvrbotron-tmp-*"
        deadline = time.monotonic() + 5
        while not list(workspace.glob(temporary_pattern)):
            if process.poll() is not None:
                raise AssertionError("render finished before interruption")
            if time.monotonic() >= deadline:
                process.terminate()
                raise AssertionError("renderer did not create temporary state")
            time.sleep(0.001)

        process.send_signal(signal.SIGINT)
        stdout, stderr = process.communicate(timeout=10)
        if process.returncode != 6:
            raise AssertionError(
                f"unexpected interruption exit {process.returncode}: "
                f"{stdout!r} {stderr!r}"
            )
        if stderr != "internal_processing_failure: render interrupted\n":
            raise AssertionError(f"unexpected interruption diagnostic: {stderr!r}")
        if interrupted_result.exists():
            raise AssertionError("interrupted render published a Render Result")
        if list(workspace.glob(temporary_pattern)):
            raise AssertionError("interrupted render left temporary state")
        long_input.unlink()

    if list(workspace.glob(".*.rvrbotron-tmp-*")):
        raise AssertionError("renderer-owned temporary state remains")


if __name__ == "__main__":
    main()
