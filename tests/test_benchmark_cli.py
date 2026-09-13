#!/usr/bin/env python3

import json
import shutil
import subprocess
import sys
from pathlib import Path


def run(*arguments: str):
    return subprocess.run(
        list(map(str, arguments)),
        check=False,
        capture_output=True,
        text=True,
    )


def main():
    renderer = Path(sys.argv[1])
    fixture = Path(sys.argv[2])
    workspace = Path(sys.argv[3])
    sample_bits = int(sys.argv[4])
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
                            "channels": 4,
                            "strategy": "duplicate",
                            "normalisation": "energy",
                        },
                        {
                            "type": "diffuser",
                            "steps": 2,
                            "totalMs": 5,
                            "distribution": "doubling",
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
    rendered = run(
        renderer,
        "render",
        "--input",
        fixture,
        "--config",
        request,
        "--output",
        render_result,
    )
    if rendered.returncode != 0:
        raise AssertionError(rendered.stderr)
    resolved = render_result / "resolved.json"

    report_path = workspace / "report.json"
    completed = run(
        renderer,
        "benchmark",
        "--resolved",
        resolved,
        "--block-size",
        "64",
        "--warmup-seconds",
        "0.02",
        "--measure-seconds",
        "0.02",
        "--json",
        report_path,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)
    stdout_report = json.loads(completed.stdout)
    file_report = json.loads(report_path.read_text())
    if stdout_report != file_report:
        raise AssertionError(
            "terminal and JSON reports diverged: "
            f"{stdout_report} != {file_report}"
        )
    report = stdout_report

    # Schema.
    expected_keys = {
        "formatVersion",
        "provenance",
        "warmupSeconds",
        "measureSeconds",
        "blockCount",
        "medianBlockSeconds",
        "p95BlockSeconds",
        "worstBlockSeconds",
        "realTimeBudgetSeconds",
        "medianUtilization",
        "worstUtilization",
        "missedDeadlineCount",
        "dspOwnedBytes",
        "residentSetSizeDeltaBytes",
        "residentSetSizeDeltaNote",
        "isDebugBuild",
    }
    if set(report) != expected_keys:
        raise AssertionError(f"unexpected report schema: {sorted(report)}")
    expected_provenance_keys = {
        "rendererVersion",
        "platform",
        "architecture",
        "compiler",
        "buildType",
        "samplePrecision",
        "sampleRate",
        "blockSize",
        "channels",
        "stepCount",
        "matrixByStep",
        "modulationInterpolationByStep",
        "feedbackLoopModulationInterpolation",
    }
    if set(report["provenance"]) != expected_provenance_keys:
        raise AssertionError(
            f"unexpected provenance schema: {sorted(report['provenance'])}"
        )

    # Provenance matches the Resolved Configuration and requested parameters.
    provenance = report["provenance"]
    if provenance["platform"] not in ("macos", "windows", "linux", "unknown"):
        raise AssertionError(f"unexpected platform: {provenance}")
    if not provenance["platform"] or not provenance["architecture"]:
        raise AssertionError(f"empty platform/architecture: {provenance}")
    if not provenance["compiler"] or not provenance["rendererVersion"]:
        raise AssertionError(f"empty compiler/rendererVersion: {provenance}")
    expected_precision = "float64" if sample_bits == 64 else "float32"
    if provenance["samplePrecision"] != expected_precision:
        raise AssertionError(f"unexpected samplePrecision: {provenance}")
    if provenance["sampleRate"] != 48000 or provenance["blockSize"] != 64:
        raise AssertionError(f"unexpected sampleRate/blockSize: {provenance}")
    if provenance["channels"] != 4 or provenance["stepCount"] != 2:
        raise AssertionError(f"unexpected channels/stepCount: {provenance}")
    if provenance["matrixByStep"] != ["hadamard", "hadamard"]:
        raise AssertionError(f"unexpected matrixByStep: {provenance}")
    # No Modulation is configured on either step or the (absent) Feedback
    # Loop, so this method's own arithmetic cost (issue #92) is not part
    # of what this particular report measures.
    if provenance["modulationInterpolationByStep"] != [None, None]:
        raise AssertionError(
            f"unexpected modulationInterpolationByStep: {provenance}"
        )
    if provenance["feedbackLoopModulationInterpolation"] is not None:
        raise AssertionError(
            f"unexpected feedbackLoopModulationInterpolation: {provenance}"
        )

    if report["warmupSeconds"] != 0.02 or report["measureSeconds"] != 0.02:
        raise AssertionError(f"requested durations did not round-trip: {report}")

    # Memory accounting: DSP-owned bytes must be nonzero (real DSP was
    # constructed) and RSS delta is present but only best-effort (may be
    # null on an unsupported platform).
    if not isinstance(report["dspOwnedBytes"], int) or report["dspOwnedBytes"] <= 0:
        raise AssertionError(f"unexpected dspOwnedBytes: {report}")
    rss_delta = report["residentSetSizeDeltaBytes"]
    if rss_delta is not None and not isinstance(rss_delta, int):
        raise AssertionError(f"unexpected residentSetSizeDeltaBytes: {report}")
    if "best-effort" not in report["residentSetSizeDeltaNote"]:
        raise AssertionError(
            f"RSS delta was not labeled as noisy evidence: {report}"
        )

    # Timing samples are nonnegative and percentile ordering is internally
    # consistent -- not a machine-specific speed threshold.
    if report["blockCount"] < 1:
        raise AssertionError(f"expected at least one measured block: {report}")
    if (
        report["medianBlockSeconds"] < 0
        or report["p95BlockSeconds"] < report["medianBlockSeconds"]
        or report["worstBlockSeconds"] < report["p95BlockSeconds"]
    ):
        raise AssertionError(f"timing percentiles are not ordered: {report}")
    if report["realTimeBudgetSeconds"] <= 0:
        raise AssertionError(f"unexpected realTimeBudgetSeconds: {report}")
    if report["medianUtilization"] < 0 or report["worstUtilization"] < (
        report["medianUtilization"] - 1e-9
    ):
        raise AssertionError(f"unexpected utilization ordering: {report}")
    if report["missedDeadlineCount"] < 0:
        raise AssertionError(f"unexpected missedDeadlineCount: {report}")

    # Debug warning: prominent but not refused, and only when it applies.
    if report["isDebugBuild"]:
        if "WARNING" not in completed.stderr or "non-optimized" not in (
            completed.stderr
        ):
            raise AssertionError(
                f"Debug build did not warn prominently: {completed.stderr!r}"
            )
    elif "WARNING" in completed.stderr:
        raise AssertionError(
            f"optimized build unexpectedly warned: {completed.stderr!r}"
        )

    modulation_request = workspace / "modulation-request.json"
    modulation_request.write_text(
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
                            "steps": 2,
                            "totalMs": 300.0,
                            "distribution": "even",
                            "step": {
                                "delayStrategy": "segmented-random",
                                "mix": "hadamard",
                                "shuffle": True,
                                "polarity": "seeded-random",
                            },
                            "stepOverrides": [
                                {
                                    "index": 0,
                                    "modulation": {
                                        "depthMs": 0.3,
                                        "rateHz": 4.0,
                                        "interpolation": "linear",
                                    },
                                },
                            ],
                        },
                        {
                            "type": "feedback-loop",
                            "delayMinMs": 40.0,
                            "delayMaxMs": 60.0,
                            "delayStrategy": "even",
                            "rt60Sec": 1.0,
                            "mix": "householder",
                            "modulation": {"depthMs": 0.4, "rateHz": 0.7},
                        },
                        {"type": "downmix", "strategy": "select", "leftChannel": 0, "rightChannel": 1},
                    ]
                },
            }
        )
    )
    modulation_render_result = workspace / "modulation-render-result"
    modulation_rendered = run(
        renderer,
        "render",
        "--input",
        fixture,
        "--config",
        modulation_request,
        "--output",
        modulation_render_result,
    )
    if modulation_rendered.returncode != 0:
        raise AssertionError(modulation_rendered.stderr)

    modulation_report_path = workspace / "modulation-report.json"
    modulation_completed = run(
        renderer,
        "benchmark",
        "--resolved",
        modulation_render_result / "resolved.json",
        "--block-size",
        "64",
        "--warmup-seconds",
        "0.02",
        "--measure-seconds",
        "0.02",
        "--json",
        modulation_report_path,
    )
    if modulation_completed.returncode != 0:
        raise AssertionError(modulation_completed.stderr)
    modulation_provenance = json.loads(modulation_completed.stdout)["provenance"]
    # Step 0 was actively modulated with linear; step 1 carries no
    # Modulation at all.
    if modulation_provenance["modulationInterpolationByStep"] != ["linear", None]:
        raise AssertionError(
            f"unexpected modulationInterpolationByStep for an active "
            f"Diffusion Step Modulation: {modulation_provenance}"
        )
    # The Feedback Loop's Modulation was configured with no explicit
    # interpolation, resolving the lagrange3 default.
    if modulation_provenance["feedbackLoopModulationInterpolation"] != "lagrange3":
        raise AssertionError(
            f"unexpected feedbackLoopModulationInterpolation for an "
            f"active Feedback Loop Modulation: {modulation_provenance}"
        )

    # Parameter handling and invalid-argument diagnostics.
    missing_resolved = run(renderer, "benchmark", "--block-size", "64")
    if missing_resolved.returncode == 0 or (
        "--resolved is required" not in missing_resolved.stderr
    ):
        raise AssertionError(
            f"missing --resolved was not rejected: {missing_resolved.stderr}"
        )

    missing_file = run(
        renderer, "benchmark", "--resolved", workspace / "does-not-exist.json"
    )
    if missing_file.returncode == 0 or "io_failure" not in missing_file.stderr:
        raise AssertionError(
            f"missing Resolved Configuration file was not rejected: "
            f"{missing_file.stderr}"
        )

    bad_block_size = run(
        renderer,
        "benchmark",
        "--resolved",
        resolved,
        "--block-size",
        "0",
    )
    if bad_block_size.returncode == 0 or (
        "--block-size requires a positive integer" not in bad_block_size.stderr
    ):
        raise AssertionError(
            f"invalid --block-size was not rejected: {bad_block_size.stderr}"
        )

    unknown_option = run(renderer, "benchmark", "--resolved", resolved, "--bogus", "1")
    if unknown_option.returncode == 0 or "unknown option" not in unknown_option.stderr:
        raise AssertionError(
            f"unknown option was not rejected: {unknown_option.stderr}"
        )

    empty_resolved = workspace / "empty-resolved.json"
    empty_resolved.write_text(
        json.dumps(
            {
                "formatVersion": 2,
                "seed": 0,
                "sampleRate": 48000,
                "composition": {"stages": []},
            }
        )
    )
    empty_composition = run(renderer, "benchmark", "--resolved", empty_resolved)
    if empty_composition.returncode == 0 or (
        "non-empty Composition" not in empty_composition.stderr
    ):
        raise AssertionError(
            f"empty Composition was not rejected: {empty_composition.stderr}"
        )

    v1_resolved = workspace / "v1-resolved.json"
    v1_resolved.write_text(
        json.dumps(
            {
                "formatVersion": 1,
                "seed": 0,
                "sampleRate": 48000,
                "composition": {"stages": [{}]},
            }
        )
    )
    v1_rejected = run(renderer, "benchmark", "--resolved", v1_resolved)
    if v1_rejected.returncode == 0 or (
        "invalid_configuration at /formatVersion: reverb configuration "
        "format 1 is unsupported by this build; use tag format-v1-final "
        "(commit 8a4e718) to render or analyze format-1 configurations"
        not in v1_rejected.stderr
    ):
        raise AssertionError(
            f"Resolved formatVersion 1 was not rejected before Composition "
            f"parsing: {v1_rejected.stderr}"
        )


if __name__ == "__main__":
    main()
