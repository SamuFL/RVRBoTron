#!/usr/bin/env python3

import json
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
    channels = bits_per_sample = None
    data_chunk = None
    while offset + 8 <= len(data):
        chunk_id = data[offset : offset + 4]
        chunk_size = struct.unpack_from("<I", data, offset + 4)[0]
        chunk = data[offset + 8 : offset + 8 + chunk_size]
        if chunk_id == b"fmt ":
            _, channels, _ = struct.unpack_from("<HHI", chunk)
            bits_per_sample = struct.unpack_from("<H", chunk, 14)[0]
        elif chunk_id == b"data":
            data_chunk = chunk
        offset += 8 + chunk_size + (chunk_size % 2)

    if data_chunk is None:
        raise AssertionError(f"{path} has no audio data")
    if bits_per_sample == 32:
        samples = struct.unpack("<" + "f" * (len(data_chunk) // 4), data_chunk)
    elif bits_per_sample == 64:
        samples = struct.unpack("<" + "d" * (len(data_chunk) // 8), data_chunk)
    else:
        raise AssertionError(f"unexpected bits per sample: {bits_per_sample}")
    return channels, samples


def deinterleave(channels: int, samples):
    return [samples[channel::channels] for channel in range(channels)]


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


def base_document():
    return {
        "formatVersion": 2,
        "seed": 42,
        "composition": {
            "stages": [
                {
                    "type": "split",
                    "channels": 4,
                    "strategy": "duplicate",
                    "normalisation": "energy",
                },
                {
                    "type": "diffuser",
                    "steps": 2,
                    "totalMs": 2,
                    "distribution": "even",
                    "step": {
                        "delayStrategy": "segmented-random",
                        "mix": "hadamard",
                        "shuffle": True,
                        "polarity": "seeded-random",
                    },
                },
                {
                    "type": "downmix",
                    "strategy": "select",
                    "leftChannel": 0,
                    "rightChannel": 1,
                    "normalisation": "energy",
                },
            ],
            "early": {"taps": [{"stepIndex": 0}]},
        },
    }


def capture_by_boundary(metadata, boundary):
    matches = [
        capture
        for capture in metadata.get("stageCaptures") or []
        if capture["boundary"] == boundary
    ]
    if len(matches) > 1:
        raise AssertionError(f"more than one {boundary!r} capture: {matches}")
    return matches[0] if matches else None


def main():
    renderer = Path(sys.argv[1])
    fixture = Path(sys.argv[2])
    workspace = Path(sys.argv[3])
    sample_bits = int(sys.argv[4])
    tolerance = 2e-6 if sample_bits == 32 else 1e-12
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    # Both branches enabled: main-stereo and early-stereo are both
    # captured, neither manifested disabled, and the combined output
    # equals their sample-wise sum (issue #113's superposition
    # invariant) with energy reconciling against branch energies plus
    # their cross term.
    enabled_request = workspace / "enabled-request.json"
    enabled_request.write_text(json.dumps(base_document()))
    enabled_result = workspace / "enabled-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            enabled_request,
            "--capture-stages",
            "all",
            "--output",
            enabled_result,
        )
    )
    enabled_metadata = json.loads((enabled_result / "render.json").read_text())
    boundaries = [
        capture["boundary"] for capture in enabled_metadata["stageCaptures"]
    ]
    if boundaries != ["split", "diffusion-step", "diffusion-step", "main-stereo", "early-stereo"]:
        raise AssertionError(f"unexpected capture manifest boundaries: {boundaries}")

    main_capture = capture_by_boundary(enabled_metadata, "main-stereo")
    early_capture = capture_by_boundary(enabled_metadata, "early-stereo")
    if main_capture["disabled"] or early_capture["disabled"]:
        raise AssertionError("an enabled branch's capture was manifested disabled")
    if main_capture["channels"] != 2 or early_capture["channels"] != 2:
        raise AssertionError("a branch capture was not stereo")
    if (
        main_capture["frames"] != enabled_metadata["frames"]
        or early_capture["frames"] != enabled_metadata["frames"]
    ):
        raise AssertionError(
            "a branch capture did not share the render's own output "
            "timeline"
        )

    output_channels, output_samples = read_float_wav(enabled_result / "output.wav")
    _, main_samples = read_float_wav(enabled_result / main_capture["path"])
    _, early_samples = read_float_wav(enabled_result / early_capture["path"])
    output_left, output_right = deinterleave(output_channels, output_samples)
    main_left, main_right = deinterleave(2, main_samples)
    early_left, early_right = deinterleave(2, early_samples)

    main_energy = 0.0
    early_energy = 0.0
    cross_term = 0.0
    combined_energy = 0.0
    for frame in range(len(output_left)):
        expected_left = main_left[frame] + early_left[frame]
        expected_right = main_right[frame] + early_right[frame]
        if (
            abs(output_left[frame] - expected_left) > tolerance
            or abs(output_right[frame] - expected_right) > tolerance
        ):
            raise AssertionError(
                f"combined output did not equal the sample-wise sum of "
                f"its captured branches at frame {frame}"
            )
        main_energy += main_left[frame] ** 2 + main_right[frame] ** 2
        early_energy += early_left[frame] ** 2 + early_right[frame] ** 2
        cross_term += (
            main_left[frame] * early_left[frame]
            + main_right[frame] * early_right[frame]
        )
        combined_energy += output_left[frame] ** 2 + output_right[frame] ** 2
    expected_combined_energy = main_energy + early_energy + 2.0 * cross_term
    if abs(combined_energy - expected_combined_energy) > tolerance * max(
        1.0, combined_energy
    ):
        raise AssertionError(
            "combined energy did not reconcile with branch energies plus "
            "their cross term"
        )
    if main_energy <= 0.0 or early_energy <= 0.0:
        raise AssertionError(
            "an enabled branch's own captured signal was silent -- "
            "adjust the test fixture"
        )

    # Replay from Resolved Configuration reproduces byte-identical
    # branch captures.
    enabled_rerender = workspace / "enabled-rerender"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            enabled_result / "resolved.json",
            "--capture-stages",
            "all",
            "--output",
            enabled_rerender,
        )
    )
    if (enabled_rerender / main_capture["path"]).read_bytes() != (
        enabled_result / main_capture["path"]
    ).read_bytes():
        raise AssertionError("resolved replay changed the Main-stereo capture")
    if (enabled_rerender / early_capture["path"]).read_bytes() != (
        enabled_result / early_capture["path"]
    ).read_bytes():
        raise AssertionError("resolved replay changed the Early-stereo capture")

    # Main disabled, Early enabled: Main-stereo is correctly sized exact
    # zero and manifested disabled; output equals Early-stereo alone.
    main_disabled_document = json.loads(json.dumps(base_document()))
    main_disabled_document["composition"]["mainEnabled"] = False
    main_disabled_request = workspace / "main-disabled-request.json"
    main_disabled_request.write_text(json.dumps(main_disabled_document))
    main_disabled_result = workspace / "main-disabled-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            main_disabled_request,
            "--capture-stages",
            "all",
            "--output",
            main_disabled_result,
        )
    )
    main_disabled_metadata = json.loads(
        (main_disabled_result / "render.json").read_text()
    )
    main_disabled_capture = capture_by_boundary(main_disabled_metadata, "main-stereo")
    early_still_enabled_capture = capture_by_boundary(
        main_disabled_metadata, "early-stereo"
    )
    if main_disabled_metadata["frames"] <= 0:
        raise AssertionError(
            "render produced no frames at all -- adjust the test fixture"
        )
    if not main_disabled_capture["disabled"]:
        raise AssertionError("a disabled Main branch's capture was not manifested disabled")
    if early_still_enabled_capture["disabled"]:
        raise AssertionError(
            "an enabled Early branch's capture was manifested disabled "
            "because Main was disabled"
        )
    if main_disabled_capture["frames"] != main_disabled_metadata["frames"]:
        raise AssertionError(
            "a disabled Main branch's capture was not correctly sized to "
            "the render's own output timeline"
        )
    _, main_disabled_samples = read_float_wav(
        main_disabled_result / main_disabled_capture["path"]
    )
    if len(main_disabled_samples) != main_disabled_capture["frames"] * 2:
        raise AssertionError(
            "a disabled Main branch's capture file did not actually "
            "contain its manifested frame count"
        )
    if any(value != 0.0 for value in main_disabled_samples):
        raise AssertionError(
            "a disabled Main branch's capture was not exact zero"
        )
    _, main_disabled_output = read_float_wav(main_disabled_result / "output.wav")
    _, main_disabled_early = read_float_wav(
        main_disabled_result / early_still_enabled_capture["path"]
    )
    if list(main_disabled_output) != list(main_disabled_early):
        raise AssertionError(
            "output did not equal Early-stereo alone with Main disabled"
        )

    # Both branches disabled: output is exact silence, and both captures
    # are correctly sized exact zero and manifested disabled.
    both_disabled_document = json.loads(json.dumps(base_document()))
    both_disabled_document["composition"]["mainEnabled"] = False
    both_disabled_document["composition"]["early"]["enabled"] = False
    both_disabled_request = workspace / "both-disabled-request.json"
    both_disabled_request.write_text(json.dumps(both_disabled_document))
    both_disabled_result = workspace / "both-disabled-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            both_disabled_request,
            "--capture-stages",
            "all",
            "--output",
            both_disabled_result,
        )
    )
    both_disabled_metadata = json.loads(
        (both_disabled_result / "render.json").read_text()
    )
    both_disabled_main = capture_by_boundary(both_disabled_metadata, "main-stereo")
    both_disabled_early = capture_by_boundary(both_disabled_metadata, "early-stereo")
    if not both_disabled_main["disabled"] or not both_disabled_early["disabled"]:
        raise AssertionError(
            "both branches disabled did not manifest both captures as "
            "disabled"
        )
    if (
        both_disabled_main["frames"] != both_disabled_metadata["frames"]
        or both_disabled_early["frames"] != both_disabled_metadata["frames"]
    ):
        raise AssertionError(
            "both branches disabled did not correctly size both captures "
            "to the render's own output timeline"
        )
    _, both_disabled_output = read_float_wav(both_disabled_result / "output.wav")
    _, both_disabled_main_samples = read_float_wav(
        both_disabled_result / both_disabled_main["path"]
    )
    _, both_disabled_early_samples = read_float_wav(
        both_disabled_result / both_disabled_early["path"]
    )
    if (
        len(both_disabled_main_samples) != both_disabled_main["frames"] * 2
        or len(both_disabled_early_samples)
        != both_disabled_early["frames"] * 2
    ):
        raise AssertionError(
            "a disabled branch's capture file did not actually contain "
            "its manifested frame count"
        )
    if (
        any(value != 0.0 for value in both_disabled_output)
        or any(value != 0.0 for value in both_disabled_main_samples)
        or any(value != 0.0 for value in both_disabled_early_samples)
    ):
        raise AssertionError(
            "both branches disabled did not produce exact silence "
            "throughout output and both captures"
        )

    # No Early Reflections branch configured: only Main-stereo is
    # captured -- no early-stereo file or manifest entry at all.
    no_early_document = json.loads(json.dumps(base_document()))
    del no_early_document["composition"]["early"]
    no_early_request = workspace / "no-early-request.json"
    no_early_request.write_text(json.dumps(no_early_document))
    no_early_result = workspace / "no-early-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            no_early_request,
            "--capture-stages",
            "all",
            "--output",
            no_early_result,
        )
    )
    no_early_metadata = json.loads((no_early_result / "render.json").read_text())
    if capture_by_boundary(no_early_metadata, "early-stereo") is not None:
        raise AssertionError(
            "an early-stereo capture was published with no Early "
            "Reflections branch configured"
        )
    if (no_early_result / "captures" / "03-early-stereo.wav").exists():
        raise AssertionError(
            "an early-stereo capture file was written with no Early "
            "Reflections branch configured"
        )
    if capture_by_boundary(no_early_metadata, "main-stereo") is None:
        raise AssertionError("Main-stereo was not captured")

    # A Feedback-Loop-only Main wet path (no Diffuser, so no Early
    # Reflections are even structurally possible) still captures
    # Main-stereo correctly -- --capture-stages all must not assume a
    # Diffuser is always present.
    loop_only_document = {
        "formatVersion": 2,
        "seed": 42,
        "composition": {
            "stages": [
                {
                    "type": "split",
                    "channels": 4,
                    "strategy": "duplicate",
                    "normalisation": "energy",
                },
                {"type": "feedback-loop"},
                {
                    "type": "downmix",
                    "strategy": "select",
                    "leftChannel": 0,
                    "rightChannel": 1,
                    "normalisation": "energy",
                },
            ]
        },
    }
    loop_only_request = workspace / "loop-only-request.json"
    loop_only_request.write_text(json.dumps(loop_only_document))
    loop_only_result = workspace / "loop-only-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            loop_only_request,
            "--capture-stages",
            "all",
            "--output",
            loop_only_result,
        )
    )
    loop_only_metadata = json.loads((loop_only_result / "render.json").read_text())
    loop_only_boundaries = [
        capture["boundary"] for capture in loop_only_metadata["stageCaptures"]
    ]
    if loop_only_boundaries != ["split", "main-stereo"]:
        raise AssertionError(
            f"unexpected capture manifest for a Feedback-Loop-only "
            f"Composition: {loop_only_boundaries}"
        )


if __name__ == "__main__":
    main()
