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
    stereo_fixture = (
        fixture.parent / "matrix" / "identity-stereo-48000-float32.wav"
    )
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

    omitted_stages_request = workspace / "omitted-stages-request.json"
    omitted_stages_result = workspace / "omitted-stages-result"
    omitted_stages_request.write_text('{"composition": {}}\n')
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            omitted_stages_request,
            "--output",
            omitted_stages_result,
        )
    )
    if (omitted_stages_result / "output.wav").read_bytes() != (
        empty_result / "output.wav"
    ).read_bytes():
        raise AssertionError("omitted stages were not exact identity")
    if json.loads((omitted_stages_result / "resolved.json").read_text()) != {
        "formatVersion": 1,
        "seed": 0,
        "sampleRate": 48000,
        "composition": {"stages": []},
    }:
        raise AssertionError("omitted stages did not resolve to empty identity")

    reference_request = workspace / "reference-request.json"
    reference_result = workspace / "reference-result"
    reference_rerender = workspace / "reference-rerender"
    reference_request.write_text(
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
            },
            indent=2,
        )
        + "\n"
    )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            reference_request,
            "--output",
            reference_result,
        )
    )
    reference_resolved = json.loads(
        (reference_result / "resolved.json").read_text()
    )
    stages = reference_resolved["composition"]["stages"]
    if stages[0] != {
        "type": "split",
        "inputChannels": 1,
        "channels": 8,
        "strategy": "duplicate",
        "normalisation": "energy",
        "sourceGain": 1.0,
        "channelGain": 0.35355339059327373,
    }:
        raise AssertionError(f"unexpected resolved Split: {stages[0]}")
    if stages[1]["type"] != "diffuser" or stages[1]["totalSamples"] != 48:
        raise AssertionError(f"unexpected resolved Diffuser: {stages[1]}")
    if len(stages[1]["steps"]) != 1:
        raise AssertionError("reference request did not resolve one Diffusion Step")
    step = stages[1]["steps"][0]
    if step["delaysSamples"] != [5, 8, 16, 22, 27, 32, 41, 44]:
        raise AssertionError(f"unexpected segmented delays: {step}")
    if len(set(step["delaysSamples"])) != 8:
        raise AssertionError("segmented-random delays are not distinct")
    if step["permutation"] != [2, 4, 0, 3, 7, 6, 1, 5]:
        raise AssertionError(f"unexpected deterministic shuffle: {step}")
    if step["polaritySigns"] != [1, 1, -1, 1, -1, -1, 1, 1]:
        raise AssertionError(f"unexpected deterministic polarity: {step}")
    hadamard_scale = 0.35355339059327373
    expected_hadamard = [
        [hadamard_scale, hadamard_scale, hadamard_scale, hadamard_scale,
         hadamard_scale, hadamard_scale, hadamard_scale, hadamard_scale],
        [hadamard_scale, -hadamard_scale, hadamard_scale, -hadamard_scale,
         hadamard_scale, -hadamard_scale, hadamard_scale, -hadamard_scale],
        [hadamard_scale, hadamard_scale, -hadamard_scale, -hadamard_scale,
         hadamard_scale, hadamard_scale, -hadamard_scale, -hadamard_scale],
        [hadamard_scale, -hadamard_scale, -hadamard_scale, hadamard_scale,
         hadamard_scale, -hadamard_scale, -hadamard_scale, hadamard_scale],
        [hadamard_scale, hadamard_scale, hadamard_scale, hadamard_scale,
         -hadamard_scale, -hadamard_scale, -hadamard_scale, -hadamard_scale],
        [hadamard_scale, -hadamard_scale, hadamard_scale, -hadamard_scale,
         -hadamard_scale, hadamard_scale, -hadamard_scale, hadamard_scale],
        [hadamard_scale, hadamard_scale, -hadamard_scale, -hadamard_scale,
         -hadamard_scale, -hadamard_scale, hadamard_scale, hadamard_scale],
        [hadamard_scale, -hadamard_scale, -hadamard_scale, hadamard_scale,
         -hadamard_scale, hadamard_scale, hadamard_scale, -hadamard_scale],
    ]
    if step["matrix"] != expected_hadamard:
        raise AssertionError(f"unexpected normalized Hadamard: {step['matrix']}")
    if stages[2] != {
        "type": "downmix",
        "inputChannels": 8,
        "outputChannels": 2,
        "strategy": "select",
        "normalisation": "energy",
        "compensation": 2.0,
    }:
        raise AssertionError(f"unexpected resolved Downmix: {stages[2]}")

    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            reference_result / "resolved.json",
            "--output",
            reference_rerender,
        )
    )
    if (reference_rerender / "resolved.json").read_bytes() != (
        reference_result / "resolved.json"
    ).read_bytes():
        raise AssertionError("resolved diffusion rerender changed configuration")
    if (reference_rerender / "output.wav").read_bytes() != (
        reference_result / "output.wav"
    ).read_bytes():
        raise AssertionError("resolved diffusion rerender changed output")

    multi_step_request = json.loads(reference_request.read_text())
    multi_step_request["composition"]["stages"][1]["steps"] = 2
    multi_step_result = workspace / "multi-step-result"
    multi_step_rerender = workspace / "multi-step-rerender"
    multi_step_config = workspace / "multi-step-request.json"
    multi_step_config.write_text(json.dumps(multi_step_request))
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            multi_step_config,
            "--output",
            multi_step_result,
        )
    )
    multi_step_resolved = json.loads(
        (multi_step_result / "resolved.json").read_text()
    )
    multi_step_stages = multi_step_resolved["composition"]["stages"]
    multi_step_steps = multi_step_stages[1]["steps"]
    if len(multi_step_steps) != 2:
        raise AssertionError(
            f"expected two resolved Diffusion Steps, got {multi_step_steps}"
        )
    if [entry["index"] for entry in multi_step_steps] != [0, 1]:
        raise AssertionError(
            f"unexpected resolved step ordering: {multi_step_steps}"
        )
    if sum(entry["lengthSamples"] for entry in multi_step_steps) != (
        multi_step_stages[1]["totalSamples"]
    ):
        raise AssertionError(
            "resolved step sample budgets did not sum to the resolved total"
        )
    # Positional seed stability: step index 0's permutation and polarity are
    # pure functions of (seed, step index, Channel) and must be unaffected
    # by how many Diffusion Steps the Diffuser now has.
    if multi_step_steps[0]["permutation"] != step["permutation"]:
        raise AssertionError(
            "step 0 permutation changed when the step count changed"
        )
    if multi_step_steps[0]["polaritySigns"] != step["polaritySigns"]:
        raise AssertionError(
            "step 0 polarity changed when the step count changed"
        )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            multi_step_result / "resolved.json",
            "--output",
            multi_step_rerender,
        )
    )
    if (multi_step_rerender / "resolved.json").read_bytes() != (
        multi_step_result / "resolved.json"
    ).read_bytes():
        raise AssertionError("multi-step rerender changed configuration")

    lengths_ms_request = json.loads(reference_request.read_text())
    del lengths_ms_request["composition"]["stages"][1]["steps"]
    del lengths_ms_request["composition"]["stages"][1]["totalMs"]
    del lengths_ms_request["composition"]["stages"][1]["distribution"]
    lengths_ms_request["composition"]["stages"][1]["lengthsMs"] = [0.5, 1.0, 1.5]
    lengths_ms_request["composition"]["stages"][1]["stepOverrides"] = [
        {"index": 1, "polarity": "none"}
    ]
    lengths_ms_config = workspace / "lengths-ms-request.json"
    lengths_ms_config.write_text(json.dumps(lengths_ms_request))
    lengths_ms_result = workspace / "lengths-ms-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            lengths_ms_config,
            "--output",
            lengths_ms_result,
        )
    )
    lengths_ms_resolved = json.loads(
        (lengths_ms_result / "resolved.json").read_text()
    )
    lengths_ms_stages = lengths_ms_resolved["composition"]["stages"]
    lengths_ms_steps = lengths_ms_stages[1]["steps"]
    if len(lengths_ms_steps) != 3:
        raise AssertionError(
            f"expected three resolved Diffusion Steps, got {lengths_ms_steps}"
        )
    resolved_lengths = [entry["lengthSamples"] for entry in lengths_ms_steps]
    # 0.5ms : 1.0ms : 1.5ms at 48kHz apportions to 24 : 48 : 72 samples exactly.
    if resolved_lengths != [24, 48, 72]:
        raise AssertionError(
            f"unexpected lengthsMs apportionment: {resolved_lengths}"
        )
    if sum(resolved_lengths) != lengths_ms_stages[1]["totalSamples"]:
        raise AssertionError(
            "lengthsMs step sample budgets did not sum to the resolved total"
        )
    # stepOverrides only touches index 1's polarity: steps 0 and 2 keep the
    # shared seeded-random polarity, step 1 must resolve to all-positive.
    if lengths_ms_steps[1]["polaritySigns"] != [1] * 8:
        raise AssertionError(
            f"stepOverrides polarity override did not apply: {lengths_ms_steps[1]}"
        )
    if lengths_ms_steps[0]["polaritySigns"] == lengths_ms_steps[1]["polaritySigns"]:
        raise AssertionError(
            "expected step 0 and overridden step 1 polarity to differ"
        )
    if lengths_ms_steps[2]["polaritySigns"] == lengths_ms_steps[1]["polaritySigns"]:
        raise AssertionError(
            "expected step 2 and overridden step 1 polarity to differ"
        )

    memory_budget_request = json.loads(reference_request.read_text())
    memory_budget_request["composition"]["stages"][0]["channels"] = 64
    memory_budget_request["composition"]["stages"][1]["totalMs"] = 500000
    memory_budget_config = workspace / "memory-budget-request.json"
    memory_budget_config.write_text(json.dumps(memory_budget_request))
    require_failure(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            memory_budget_config,
            "--memory-budget-mib",
            "1",
            "--output",
            workspace / "memory-budget-result",
        ),
        "resolved Diffuser DSP memory footprint exceeds the configured memory budget",
        workspace / "memory-budget-result",
    )

    # Omitting steps/totalMs/distribution entirely must resolve to the
    # Reference four-step doubling chain (4 steps, 300ms total).
    default_diffuser_request = json.loads(reference_request.read_text())
    del default_diffuser_request["composition"]["stages"][1]["steps"]
    del default_diffuser_request["composition"]["stages"][1]["totalMs"]
    del default_diffuser_request["composition"]["stages"][1]["distribution"]
    default_diffuser_config = workspace / "default-diffuser-request.json"
    default_diffuser_config.write_text(json.dumps(default_diffuser_request))
    default_diffuser_result = workspace / "default-diffuser-result"
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            default_diffuser_config,
            "--output",
            default_diffuser_result,
        )
    )
    default_diffuser_resolved = json.loads(
        (default_diffuser_result / "resolved.json").read_text()
    )
    default_diffuser_stages = default_diffuser_resolved["composition"]["stages"]
    default_diffuser_steps = default_diffuser_stages[1]["steps"]
    if len(default_diffuser_steps) != 4:
        raise AssertionError(
            f"expected the Reference four-step chain by default, got "
            f"{default_diffuser_steps}"
        )
    # totalMs 300 at 48kHz is 14400 samples; doubling weights step i by 2**i.
    default_lengths = [entry["lengthSamples"] for entry in default_diffuser_steps]
    if default_lengths != [960, 1920, 3840, 7680]:
        raise AssertionError(
            f"unexpected default Reference doubling apportionment: "
            f"{default_lengths}"
        )
    if sum(default_lengths) != default_diffuser_stages[1]["totalSamples"]:
        raise AssertionError(
            "Reference default step sample budgets did not sum to the "
            "resolved total"
        )

    stereo_result = workspace / "stereo-reference-result"
    stereo_rerender = workspace / "stereo-reference-rerender"
    require_success(
        run_renderer(
            renderer,
            "--input",
            stereo_fixture,
            "--config",
            reference_request,
            "--output",
            stereo_result,
        )
    )
    stereo_resolved = json.loads(
        (stereo_result / "resolved.json").read_text()
    )
    stereo_split = stereo_resolved["composition"]["stages"][0]
    if stereo_split["sourceGain"] != 0.7071067811865475:
        raise AssertionError(
            f"stereo source selection gain was not resolved: {stereo_split}"
        )
    if stereo_split["channelGain"] != 0.35355339059327373:
        raise AssertionError(
            f"stereo Channel gain was not resolved: {stereo_split}"
        )
    require_success(
        run_renderer(
            renderer,
            "--input",
            stereo_fixture,
            "--resolved",
            stereo_result / "resolved.json",
            "--output",
            stereo_rerender,
        )
    )
    if (stereo_rerender / "resolved.json").read_bytes() != (
        stereo_result / "resolved.json"
    ).read_bytes():
        raise AssertionError("resolved stereo Split changed configuration")
    if (stereo_rerender / "output.wav").read_bytes() != (
        stereo_result / "output.wav"
    ).read_bytes():
        raise AssertionError("resolved stereo Split changed output")

    invalid_stereo_source = json.loads(json.dumps(stereo_resolved))
    invalid_stereo_source["composition"]["stages"][0]["sourceGain"] = 1.0
    invalid_stereo_source_path = workspace / "invalid-stereo-source.json"
    invalid_stereo_source_result = workspace / "invalid-stereo-source-result"
    invalid_stereo_source_path.write_text(json.dumps(invalid_stereo_source))
    require_failure(
        run_renderer(
            renderer,
            "--input",
            stereo_fixture,
            "--resolved",
            invalid_stereo_source_path,
            "--output",
            invalid_stereo_source_result,
        ),
        "/composition/stages/0/sourceGain: "
        "expected gain derived from Split input mapping",
        invalid_stereo_source_result,
    )

    ablation_request = workspace / "ablation-request.json"
    ablation_result = workspace / "ablation-result"
    ablation_rerender = workspace / "ablation-rerender"
    ablation_request.write_text(
        json.dumps(
            {
                "formatVersion": 1,
                "composition": {
                    "stages": [
                        {
                            "type": "split",
                            "channels": 4,
                            "strategy": "duplicate",
                            "normalisation": "none",
                        },
                        {
                            "type": "diffuser",
                            "steps": 1,
                            "totalMs": 0.125,
                            "distribution": "even",
                            "step": {
                                "delayStrategy": "even",
                                "mix": "hadamard",
                                "shuffle": False,
                                "polarity": "none",
                            },
                        },
                        {
                            "type": "downmix",
                            "strategy": "select",
                            "normalisation": "none",
                        },
                    ]
                },
            },
            indent=2,
        )
        + "\n"
    )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            ablation_request,
            "--output",
            ablation_result,
        )
    )
    ablation_resolved = json.loads(
        (ablation_result / "resolved.json").read_text()
    )
    ablation_stages = ablation_resolved["composition"]["stages"]
    if ablation_stages[0]["normalisation"] != "none":
        raise AssertionError("Split ablation did not round-trip")
    if ablation_stages[0]["sourceGain"] != 1.0:
        raise AssertionError("mono source selection gain changed level")
    if ablation_stages[0]["channelGain"] != 1.0:
        raise AssertionError("Split none normalisation changed level")
    ablation_step = ablation_stages[1]["steps"][0]
    if ablation_step["delayStrategy"] != "even":
        raise AssertionError("even delay strategy did not round-trip")
    if ablation_step["delaysSamples"] != [0, 2, 4, 6]:
        raise AssertionError(f"unexpected even delays: {ablation_step}")
    if ablation_step["shuffle"] is not False:
        raise AssertionError("shuffle false did not round-trip")
    if ablation_step["permutation"] != [0, 1, 2, 3]:
        raise AssertionError("shuffle false did not resolve identity")
    if ablation_step["polarity"] != "none":
        raise AssertionError("none polarity did not round-trip")
    if ablation_step["polaritySigns"] != [1, 1, 1, 1]:
        raise AssertionError("none polarity did not resolve all positive")
    if ablation_stages[2]["normalisation"] != "none":
        raise AssertionError("Downmix ablation did not round-trip")
    if ablation_stages[2]["compensation"] != 1.0:
        raise AssertionError("Downmix none normalisation compensated select")

    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--resolved",
            ablation_result / "resolved.json",
            "--output",
            ablation_rerender,
        )
    )
    if (ablation_rerender / "resolved.json").read_bytes() != (
        ablation_result / "resolved.json"
    ).read_bytes():
        raise AssertionError("resolved ablations changed configuration")
    if (ablation_rerender / "output.wav").read_bytes() != (
        ablation_result / "output.wav"
    ).read_bytes():
        raise AssertionError("resolved ablations changed output")

    single_channel_request = workspace / "single-channel-ablation-request.json"
    single_channel_result = workspace / "single-channel-ablation-result"
    single_channel_document = json.loads(ablation_request.read_text())
    single_channel_document["composition"]["stages"][0]["channels"] = 1
    single_channel_document["composition"]["stages"][1]["totalMs"] = 1
    single_channel_request.write_text(
        json.dumps(single_channel_document, indent=2) + "\n"
    )
    require_success(
        run_renderer(
            renderer,
            "--input",
            fixture,
            "--config",
            single_channel_request,
            "--output",
            single_channel_result,
        )
    )
    single_channel_stages = json.loads(
        (single_channel_result / "resolved.json").read_text()
    )["composition"]["stages"]
    if single_channel_stages[1]["steps"][0]["delaysSamples"] != [0]:
        raise AssertionError("single-Channel even delay is not deterministic")
    if single_channel_stages[2]["compensation"] != 1.0:
        raise AssertionError("single-Channel Downmix none changed select level")

    invalid_ablation_resolved = []
    invalid_source_gain = json.loads(json.dumps(ablation_resolved))
    invalid_source_gain["composition"]["stages"][0]["sourceGain"] = 0.5
    invalid_ablation_resolved.append(
        (
            invalid_source_gain,
            "/composition/stages/0/sourceGain: "
            "expected gain derived from Split input mapping",
        )
    )
    invalid_split_gain = json.loads(json.dumps(ablation_resolved))
    invalid_split_gain["composition"]["stages"][0]["channelGain"] = 0.5
    invalid_ablation_resolved.append(
        (
            invalid_split_gain,
            "/composition/stages/0/channelGain: "
            "expected gain derived from Split normalisation",
        )
    )
    invalid_even_delays = json.loads(json.dumps(ablation_resolved))
    invalid_even_step = invalid_even_delays["composition"]["stages"][1][
        "steps"
    ][0]
    invalid_even_step["delaysSamples"][1] = 1
    invalid_even_step["delaysMs"][1] = 1000.0 / 48000.0
    invalid_even_step["bufferSizes"][1] = 1
    invalid_ablation_resolved.append(
        (
            invalid_even_delays,
            "/composition/stages/1/steps/0/delaysSamples: "
            "even requires delays distributed over the available positions",
        )
    )
    invalid_identity = json.loads(json.dumps(ablation_resolved))
    invalid_identity["composition"]["stages"][1]["steps"][0]["permutation"] = [
        1,
        0,
        2,
        3,
    ]
    invalid_ablation_resolved.append(
        (
            invalid_identity,
            "/composition/stages/1/steps/0/permutation: "
            "shuffle false requires the identity permutation",
        )
    )
    invalid_polarity = json.loads(json.dumps(ablation_resolved))
    invalid_polarity["composition"]["stages"][1]["steps"][0][
        "polaritySigns"
    ][0] = -1
    invalid_ablation_resolved.append(
        (
            invalid_polarity,
            "/composition/stages/1/steps/0/polaritySigns: "
            "polarity none requires all +1 signs",
        )
    )
    invalid_hadamard = json.loads(json.dumps(ablation_resolved))
    invalid_hadamard["composition"]["stages"][1]["steps"][0]["matrix"] = [
        [1.0, 0.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
        [0.0, 0.0, 0.0, 1.0],
    ]
    invalid_ablation_resolved.append(
        (
            invalid_hadamard,
            "/composition/stages/1/steps/0/matrix: "
            "expected the normalized canonical Sylvester-Hadamard matrix",
        )
    )
    invalid_downmix_gain = json.loads(json.dumps(ablation_resolved))
    invalid_downmix_gain["composition"]["stages"][2]["compensation"] = 2.0
    invalid_ablation_resolved.append(
        (
            invalid_downmix_gain,
            "/composition/stages/2/compensation: "
            "expected gain derived from Downmix normalisation",
        )
    )
    for index, (document, expected_error) in enumerate(
        invalid_ablation_resolved
    ):
        path = workspace / f"invalid-ablation-resolved-{index}.json"
        output = workspace / f"invalid-ablation-result-{index}"
        path.write_text(json.dumps(document))
        require_failure(
            run_renderer(
                renderer,
                "--input",
                fixture,
                "--resolved",
                path,
                "--output",
                output,
            ),
            expected_error,
            output,
        )

    mismatched_shape_result = workspace / "mismatched-shape-result"
    mismatched_shape = run_renderer(
        renderer,
        "--input",
        stereo_fixture,
        "--resolved",
        reference_result / "resolved.json",
        "--output",
        mismatched_shape_result,
    )
    require_failure(
        mismatched_shape,
        "invalid_configuration at /composition/stages/0/inputChannels: "
        "expected input Channel count 1, got 2",
        mismatched_shape_result,
    )

    invalid_channels_request = json.loads(reference_request.read_text())
    invalid_channels_request["composition"]["stages"][0]["channels"] = 0
    invalid_total_request = json.loads(reference_request.read_text())
    invalid_total_request["composition"]["stages"][1]["totalMs"] = 0
    sub_sample_request = json.loads(reference_request.read_text())
    sub_sample_request["composition"]["stages"][1]["totalMs"] = 0.000001
    oversized_total_request = json.loads(reference_request.read_text())
    oversized_total_request["composition"]["stages"][1]["totalMs"] = 1e300
    short_delay_request = json.loads(reference_request.read_text())
    short_delay_request["composition"]["stages"][1]["totalMs"] = 0.1
    non_power_of_two_request = json.loads(reference_request.read_text())
    non_power_of_two_request["composition"]["stages"][0]["channels"] = 3
    lengths_ms_with_steps_request = json.loads(reference_request.read_text())
    lengths_ms_with_steps_request["composition"]["stages"][1]["lengthsMs"] = [1.0]
    step_override_out_of_range_request = json.loads(reference_request.read_text())
    step_override_out_of_range_request["composition"]["stages"][1][
        "stepOverrides"
    ] = [{"index": 1, "polarity": "none"}]
    step_override_duplicate_request = json.loads(reference_request.read_text())
    step_override_duplicate_request["composition"]["stages"][1]["steps"] = 2
    step_override_duplicate_request["composition"]["stages"][1][
        "stepOverrides"
    ] = [
        {"index": 0, "polarity": "none"},
        {"index": 0, "shuffle": False},
    ]
    unsafe_format_request = json.loads(reference_request.read_text())
    unsafe_format_request["formatVersion"] = 2
    unsafe_format_request["composition"]["stages"][0]["channels"] = 1073741824
    unsafe_format_request["composition"]["stages"][1]["totalMs"] = 30000000
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
            json.dumps(unsafe_format_request),
            "invalid_configuration at /formatVersion: expected integer 1",
        ),
        (
            '{"seed": -1}',
            "invalid_configuration at /seed: expected unsigned 64-bit integer",
        ),
        (
            '{"composition": {"stages": [{}]}}',
            "invalid_configuration at /composition/stages/0/type: required field is missing",
        ),
        (
            '{"composition": {"stages": [{"type": "downmix"}, {"type": "split"}]}}',
            "invalid_configuration at /composition/stages: expected [split, diffuser, downmix]",
        ),
        (
            json.dumps(invalid_channels_request),
            "invalid_configuration at /composition/stages/0/channels: "
            "expected value greater than zero",
        ),
        (
            json.dumps(invalid_total_request),
            "invalid_configuration at /composition/stages/1/totalMs: "
            "expected value greater than zero",
        ),
        (
            json.dumps(sub_sample_request),
            "invalid_configuration at /composition/stages/1/totalMs: "
            "resolved sample budget must be at least one sample",
        ),
        (
            json.dumps(oversized_total_request),
            "invalid_configuration at /composition/stages/1/totalMs: "
            "resolved sample budget is too large",
        ),
        (
            json.dumps(short_delay_request),
            "invalid_configuration at /composition/stages/1/steps/0/lengthSamples: "
            "delay strategy requires at least one sample position per Channel",
        ),
        (
            json.dumps(non_power_of_two_request),
            "invalid_configuration at /composition/stages/1/steps/0/mix: "
            "hadamard requires a power-of-two Channel count",
        ),
        (
            json.dumps(lengths_ms_with_steps_request),
            "invalid_configuration at /composition/stages/1/lengthsMs: "
            "expected exactly one of lengthsMs or steps/totalMs/distribution",
        ),
        (
            json.dumps(step_override_out_of_range_request),
            "invalid_configuration at /composition/stages/1/stepOverrides/0/index: "
            "expected index less than the resolved step count",
        ),
        (
            json.dumps(step_override_duplicate_request),
            "invalid_configuration at /composition/stages/1/stepOverrides/1/index: "
            "expected distinct step indices",
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
