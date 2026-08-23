#!/usr/bin/env python3

import json
import shutil
import subprocess
import sys
from pathlib import Path


def run_renderer(renderer: Path, *arguments: str):
    return subprocess.run(
        [str(renderer), "render", *map(str, arguments)],
        check=False,
        capture_output=True,
        text=True,
    )


def require_success(completed):
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)


def require_failure(completed, expected_error: str, output: Path):
    if completed.returncode == 0:
        raise AssertionError("renderer unexpectedly succeeded")
    if expected_error not in completed.stderr:
        raise AssertionError(
            f"expected error {expected_error!r}, got {completed.stderr!r}"
        )
    if output.exists():
        raise AssertionError("configuration failure created a Render Result")


def main():
    renderer = Path(sys.argv[1])
    fixture = Path(sys.argv[2])
    workspace = Path(sys.argv[3])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    request = workspace / "request.json"
    requested_result = workspace / "requested-result"
    resolved_result = workspace / "resolved-result"
    request_bytes = (
        b"{\r\n"
        b'  "formatVersion": 1,\n'
        b'  "seed": 18446744073709551615,\r\n'
        b'  "composition": {"stages": []}\n'
        b"}\r\n"
    )
    request.write_bytes(request_bytes)

    requested = run_renderer(
        renderer,
        "--input",
        fixture,
        "--config",
        request,
        "--output",
        requested_result,
    )
    require_success(requested)

    if (requested_result / "request.json").read_bytes() != request_bytes:
        raise AssertionError("Render Result did not preserve the raw request")
    if (
        json.loads((requested_result / "render.json").read_text())[
            "configurationInput"
        ]
        != "requested"
    ):
        raise AssertionError("Render Result did not record requested input mode")

    resolved = json.loads((requested_result / "resolved.json").read_text())
    expected = {
        "formatVersion": 1,
        "seed": 18446744073709551615,
        "sampleRate": 48000,
        "composition": {"stages": []},
    }
    if resolved != expected:
        raise AssertionError(f"unexpected resolved request: {resolved}")

    rerendered = run_renderer(
        renderer,
        "--input",
        fixture,
        "--resolved",
        requested_result / "resolved.json",
        "--output",
        resolved_result,
    )
    require_success(rerendered)

    if (resolved_result / "request.json").exists():
        raise AssertionError("resolved render synthesized request.json")
    if (
        json.loads((resolved_result / "render.json").read_text())[
            "configurationInput"
        ]
        != "resolved"
    ):
        raise AssertionError("Render Result did not record resolved input mode")
    if (resolved_result / "resolved.json").read_bytes() != (
        requested_result / "resolved.json"
    ).read_bytes():
        raise AssertionError("resolved render changed the Resolved Configuration")
    if (resolved_result / "output.wav").read_bytes() != (
        requested_result / "output.wav"
    ).read_bytes():
        raise AssertionError("resolved rerender changed identity output")

    empty_request = workspace / "empty-request.json"
    empty_result = workspace / "empty-result"
    empty_request.write_text("{}\n")
    empty = run_renderer(
        renderer,
        "--input",
        fixture,
        "--config",
        empty_request,
        "--output",
        empty_result,
    )
    require_success(empty)
    if json.loads((empty_result / "resolved.json").read_text()) != {
        "formatVersion": 1,
        "seed": 0,
        "sampleRate": 48000,
        "composition": {"stages": []},
    }:
        raise AssertionError("empty request did not use current defaults")
    if (empty_result / "request.json").read_text() != "{}\n":
        raise AssertionError("empty raw request was not preserved")

    invalid_requests = [
        (
            '{"unexpected": true}',
            "invalid_configuration at /unexpected: unknown field",
        ),
        (
            '{"composition": {"unexpected": true}}',
            "invalid_configuration at /composition/unexpected: unknown field",
        ),
        (
            '{"formatVersion": 2}',
            "invalid_configuration at /formatVersion: expected integer 1",
        ),
        (
            '{"seed": -1}',
            "invalid_configuration at /seed: expected unsigned 64-bit integer",
        ),
        (
            '{"composition": {"stages": [{}]}}',
            "invalid_configuration at /composition/stages: expected empty array",
        ),
        (
            '{"seed": ',
            "malformed_json at /: malformed JSON:",
        ),
    ]
    for index, (contents, expected_error) in enumerate(invalid_requests):
        invalid_config = workspace / f"invalid-request-{index}.json"
        invalid_output = workspace / f"invalid-request-result-{index}"
        invalid_config.write_text(contents)
        completed = run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            invalid_config,
            "--output",
            invalid_output,
        )
        require_failure(completed, expected_error, invalid_output)

    mismatched_resolved = workspace / "mismatched-resolved.json"
    mismatched_output = workspace / "mismatched-result"
    mismatched_resolved.write_text(
        json.dumps(
            {
                "formatVersion": 1,
                "seed": 0,
                "sampleRate": 44100,
                "composition": {"stages": []},
            }
        )
    )
    mismatch = run_renderer(
        renderer,
        "--input",
        fixture,
        "--resolved",
        mismatched_resolved,
        "--output",
        mismatched_output,
    )
    require_failure(
        mismatch,
        "invalid_configuration at /sampleRate: expected input sample rate 48000, got 44100",
        mismatched_output,
    )

    oversized_resolved = workspace / "oversized-resolved.json"
    oversized_output = workspace / "oversized-result"
    oversized_resolved.write_text(
        json.dumps(
            {
                "formatVersion": 1,
                "seed": 0,
                "sampleRate": 4294967297,
                "composition": {"stages": []},
            }
        )
    )
    oversized = run_renderer(
        renderer,
        "--input",
        fixture,
        "--resolved",
        oversized_resolved,
        "--output",
        oversized_output,
    )
    require_failure(
        oversized,
        "invalid_configuration at /sampleRate: expected unsigned 32-bit integer",
        oversized_output,
    )

    conflicting_output = workspace / "conflicting-result"
    conflicting = run_renderer(
        renderer,
        "--input",
        fixture,
        "--config",
        request,
        "--resolved",
        requested_result / "resolved.json",
        "--output",
        conflicting_output,
    )
    require_failure(
        conflicting,
        "--config and --resolved are mutually exclusive",
        conflicting_output,
    )

    empty_config_output = workspace / "empty-config-result"
    empty_config = run_renderer(
        renderer,
        "--input",
        fixture,
        "--config",
        "",
        "--output",
        empty_config_output,
    )
    require_failure(
        empty_config,
        "--config requires a non-empty path",
        empty_config_output,
    )

    empty_resolved_output = workspace / "empty-resolved-result"
    empty_resolved = run_renderer(
        renderer,
        "--input",
        fixture,
        "--resolved",
        "",
        "--output",
        empty_resolved_output,
    )
    require_failure(
        empty_resolved,
        "--resolved requires a non-empty path",
        empty_resolved_output,
    )

    empty_conflicting_output = workspace / "empty-conflicting-result"
    empty_conflicting = run_renderer(
        renderer,
        "--input",
        fixture,
        "--config",
        "",
        "--resolved",
        requested_result / "resolved.json",
        "--output",
        empty_conflicting_output,
    )
    require_failure(
        empty_conflicting,
        "--config and --resolved are mutually exclusive",
        empty_conflicting_output,
    )


if __name__ == "__main__":
    main()
