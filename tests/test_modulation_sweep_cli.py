#!/usr/bin/env python3

import html
import json
import shutil
import subprocess
import sys
from pathlib import Path


def section_html(report_html, name):
    """The report's HTML for one named point -- from its own `<h2>` heading
    up to the next point's, or end of document for the last point -- so an
    assertion about one point's evidence can't be satisfied by another
    point's section instead."""
    marker = f"<h2>{html.escape(name)}"
    start = report_html.find(marker)
    if start == -1:
        raise AssertionError(f"listening report has no section for {name!r}")
    next_marker = report_html.find("<h2>", start + len(marker))
    return report_html[start:] if next_marker == -1 else report_html[start:next_marker]


def run(*arguments):
    return subprocess.run(
        list(map(str, arguments)),
        check=False,
        capture_output=True,
        text=True,
    )


def outcome(report, name):
    return next(entry for entry in report["points"] if entry["name"] == name)


def main():
    runner = Path(sys.argv[1])
    renderer = Path(sys.argv[2])
    analyzer = Path(sys.argv[3])
    fixture = Path(sys.argv[4])
    stereo_fixture = Path(sys.argv[5])
    workspace = Path(sys.argv[6])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    # A millisecond-scale tracer catalog: N=2, delayStrategy "even" (see
    # test_feedback_loop_cli.py), delays 1/2 ms -- fast enough for CI while
    # still exercising a Modulation-enabled, Damping-at-unity Feedback Loop
    # composition. The depth axis carries the "omitted"/"zero" identity-
    # invariant pair this runner checks; the target axis exercises the
    # JSON-Patch stage-insertion (add a Diffusion Step, remove the Feedback
    # Loop's own Modulation) real modulation_sweep_v1.json's own target axis
    # uses.
    reference = {
        "formatVersion": 1,
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
                    "gainMode": "per-channel",
                    "damping": {
                        "highRatio": 1.0,
                        "highHz": 4000,
                        "lowRatio": 1.0,
                        "lowHz": 200,
                    },
                    "modulation": {
                        "depthMs": 0.4,
                        "rateHz": 0.7,
                        "shape": "smoothed-random",
                        "channelFraction": 1.0,
                        "interpolation": "linear",
                    },
                },
                {"type": "downmix", "strategy": "select"},
            ]
        },
    }
    tracer_catalog = {
        "formatVersion": 1,
        "reference": reference,
        "axes": [
            {
                "name": "depth",
                "hypothesis": "tracer",
                "values": [
                    {
                        "label": "omitted",
                        "overrides": [
                            {"op": "remove", "path": "/composition/stages/1/modulation"}
                        ],
                    },
                    {
                        "label": "zero",
                        "overrides": [
                            {
                                "op": "replace",
                                "path": "/composition/stages/1/modulation/depthMs",
                                "value": 0.0,
                            }
                        ],
                    },
                ],
            },
            {
                "name": "target",
                "hypothesis": "tracer",
                "values": [
                    {
                        "label": "diffusion-step",
                        "overrides": [
                            {
                                "op": "add",
                                "path": "/composition/stages/1",
                                "value": {
                                    "type": "diffuser",
                                    "steps": 1,
                                    "totalMs": 20,
                                    "distribution": "even",
                                    "step": {
                                        "delayStrategy": "segmented-random",
                                        "mix": "hadamard",
                                        "shuffle": True,
                                        "polarity": "seeded-random",
                                        "modulation": {
                                            "depthMs": 0.1,
                                            "rateHz": 0.7,
                                            "shape": "smoothed-random",
                                            "channelFraction": 1.0,
                                            "interpolation": "linear",
                                        },
                                    },
                                },
                            },
                            {"op": "remove", "path": "/composition/stages/2/modulation"},
                        ],
                    }
                ],
            },
        ],
    }
    catalog_path = workspace / "tracer-sweep.json"
    catalog_path.write_text(json.dumps(tracer_catalog, indent=2))

    output_dir = workspace / "output"

    def run_sweep(sample):
        return run(
            sys.executable,
            runner,
            "--catalog",
            catalog_path,
            "--renderer",
            renderer,
            "--analyzer",
            analyzer,
            "--sample",
            sample,
            "--mono-impulse",
            fixture,
            "--stereo-impulse",
            stereo_fixture,
            "--output",
            output_dir,
            "--warmup-seconds",
            "0.02",
            "--measure-seconds",
            "0.02",
            "--block-size",
            "8",
        )

    # A missing sample is skipped, not failed -- no output is created.
    missing = run_sweep(workspace / "does-not-exist.wav")
    if missing.returncode != 0:
        raise AssertionError(f"missing sample should not fail the sweep: {missing.stdout}")
    if "[skipped]" not in missing.stdout or "git lfs pull" not in missing.stdout:
        raise AssertionError(f"missing sample was not explained: {missing.stdout}")
    if output_dir.exists():
        raise AssertionError("a skipped sweep unexpectedly created output")

    # An unpulled Git LFS pointer (present on disk, but pointer text rather
    # than audio) is likewise skipped, not failed, with its own distinct
    # explanation.
    lfs_pointer = workspace / "lfs-pointer.wav"
    lfs_pointer.write_bytes(
        b"version https://git-lfs.github.com/spec/v1\n"
        b"oid sha256:0000000000000000000000000000000000000000000000000000000000000000\n"
        b"size 12345\n"
    )
    pointer_result = run_sweep(lfs_pointer)
    if pointer_result.returncode != 0:
        raise AssertionError(
            f"an unpulled LFS pointer should not fail the sweep: {pointer_result.stdout}"
        )
    if "git lfs pull" not in pointer_result.stdout or "pointer" not in pointer_result.stdout:
        raise AssertionError(
            f"LFS pointer sample was not explained: {pointer_result.stdout}"
        )
    if output_dir.exists():
        raise AssertionError("a skipped (LFS pointer) sweep unexpectedly created output")

    # A sample that exists locally but is not a readable WAV file fails
    # cleanly with an explanatory message, not an unhandled traceback.
    invalid_wav = workspace / "invalid.wav"
    invalid_wav.write_bytes(b"not a wav file at all")
    invalid = run_sweep(invalid_wav)
    if invalid.returncode != 1:
        raise AssertionError(f"invalid WAV sample should fail cleanly: {invalid.stdout}")
    if "Traceback" in invalid.stderr:
        raise AssertionError(f"invalid WAV sample crashed instead of failing cleanly: {invalid.stderr}")
    if "not a readable WAV file" not in invalid.stderr:
        raise AssertionError(f"invalid WAV sample was not explained: {invalid.stderr}")

    def run_catalog(catalog_path):
        return run(
            sys.executable,
            runner,
            "--catalog",
            catalog_path,
            "--renderer",
            renderer,
            "--analyzer",
            analyzer,
            "--sample",
            fixture,
            "--mono-impulse",
            fixture,
            "--stereo-impulse",
            stereo_fixture,
            "--output",
            output_dir,
        )

    def expect_clean_catalog_failure(catalog_path, contents, expected_substrings, label):
        catalog_path.write_text(json.dumps(contents))
        result = run_catalog(catalog_path)
        if result.returncode != 1:
            raise AssertionError(f"{label} should fail cleanly: {result.stdout}")
        if "Traceback" in result.stderr:
            raise AssertionError(
                f"{label} crashed instead of failing cleanly: {result.stderr}"
            )
        for substring in expected_substrings:
            if substring not in result.stderr:
                raise AssertionError(
                    f"{label} was not explained (missing {substring!r}): "
                    f"{result.stderr}"
                )

    # A malformed catalog (an axis missing a required field) fails with a
    # descriptive message naming the field, not a bare KeyError.
    malformed_catalog = dict(tracer_catalog)
    malformed_catalog["axes"] = [{"values": []}]
    expect_clean_catalog_failure(
        workspace / "malformed-sweep.json",
        malformed_catalog,
        ("missing required field", "'name'"),
        "malformed catalog (axis missing 'name')",
    )

    # A catalog whose JSON root is not an object fails cleanly instead of
    # crashing with a bare AttributeError on `document.get(...)`.
    expect_clean_catalog_failure(
        workspace / "non-object-root.json",
        [],
        ("catalog root must be a JSON object",),
        "malformed catalog (non-object root)",
    )

    # An axis whose 'values' is a malformed override (missing 'op'/'path')
    # fails cleanly instead of crashing with a bare KeyError deep inside
    # JSON-Pointer application.
    bad_override_catalog = dict(tracer_catalog)
    bad_override_catalog["axes"] = [
        {
            "name": "bad-axis",
            "values": [{"label": "x", "overrides": [{"path": "/seed", "value": 1}]}],
        }
    ]
    expect_clean_catalog_failure(
        workspace / "bad-override-sweep.json",
        bad_override_catalog,
        ("malformed override", "'op'", "'path'"),
        "malformed catalog (override missing 'op')",
    )

    # A real (tracer) sample sweeps every point.
    first = run_sweep(fixture)
    if first.returncode != 0:
        raise AssertionError(first.stdout + first.stderr)
    for expected in (
        "[completed] reference",
        "[completed] depth/omitted",
        "[completed] depth/zero",
        "[completed] target/diffusion-step",
    ):
        if expected not in first.stdout:
            raise AssertionError(f"missing {expected!r} in stdout: {first.stdout}")
    if "identity invariant (depth: omitted vs. zero): sample=True impulse=True" not in first.stdout:
        raise AssertionError(f"identity invariant was not reported: {first.stdout}")

    sample_root = output_dir / fixture.stem
    reference_dir = sample_root / "00-reference"
    depth_omitted_dir = sample_root / "01-depth" / "01-omitted"
    depth_zero_dir = sample_root / "01-depth" / "02-zero"
    target_dir = sample_root / "02-target" / "01-diffusion-step"

    # Output is laid out per sample, per axis, and per axis value, with
    # numeric prefixes ordering it for auditioning; every point publishes
    # modulation-v1 evidence, from the impulse render only.
    for point_dir in (reference_dir, depth_omitted_dir, depth_zero_dir, target_dir):
        for artifact in (
            point_dir / "sample" / "render.json",
            point_dir / "impulse" / "render.json",
            point_dir / "impulse" / "analysis" / "modulation-v1.json",
            point_dir / "benchmark.json",
        ):
            if not artifact.exists():
                raise AssertionError(f"expected artifact missing: {artifact}")
        if (point_dir / "sample" / "analysis").exists():
            raise AssertionError(
                f"modulation analysis was published against the sample render: {point_dir}"
            )
        sample_resolved = (point_dir / "sample" / "resolved.json").read_bytes()
        impulse_resolved = (point_dir / "impulse" / "resolved.json").read_bytes()
        if sample_resolved != impulse_resolved:
            raise AssertionError(
                f"sample and impulse renders did not share a Resolved "
                f"Configuration: {point_dir}"
            )

    # depth/omitted materializes with no Modulation object at all.
    omitted_request = json.loads(
        (depth_omitted_dir / "materialized" / "sample-request.json").read_text()
    )
    if "modulation" in omitted_request["composition"]["stages"][1]:
        raise AssertionError(
            f"depth/omitted unexpectedly kept a Modulation object: {omitted_request}"
        )

    # depth/zero materializes with depthMs: 0 and every other Modulation
    # field unchanged from the Reference.
    zero_request = json.loads(
        (depth_zero_dir / "materialized" / "sample-request.json").read_text()
    )
    expected_zero = json.loads(json.dumps(reference))
    expected_zero["composition"]["stages"][1]["modulation"]["depthMs"] = 0.0
    if zero_request != expected_zero:
        raise AssertionError(
            f"depth/zero did not materialize a single-field override: "
            f"{zero_request} != {expected_zero}"
        )

    # The identity invariant: modulation omitted entirely and depthMs: 0
    # both resolve to the same DSP identity by construction, so their
    # rendered sample and impulse output must be byte-identical. Published
    # as its own artifact under the depth axis's directory, not only
    # asserted here.
    identity_invariant = json.loads(
        (sample_root / "01-depth" / "identity-invariant.json").read_text()
    )
    if identity_invariant != {
        "formatVersion": 1,
        "omittedPoint": "depth/omitted",
        "zeroPoint": "depth/zero",
        "sampleOutputIdentical": True,
        "impulseOutputIdentical": True,
    }:
        raise AssertionError(f"unexpected identity invariant: {identity_invariant}")
    if (depth_omitted_dir / "sample" / "output.wav").read_bytes() != (
        depth_zero_dir / "sample" / "output.wav"
    ).read_bytes():
        raise AssertionError("depth/omitted and depth/zero sample renders were not byte-identical")
    if (depth_omitted_dir / "impulse" / "output.wav").read_bytes() != (
        depth_zero_dir / "impulse" / "output.wav"
    ).read_bytes():
        raise AssertionError("depth/omitted and depth/zero impulse renders were not byte-identical")

    # The target axis's Diffusion Step point exercises a multi-operation
    # override (add a whole Diffuser stage, then remove the Feedback
    # Loop's own Modulation at its new, shifted index) -- the Composition
    # actually gained a Diffuser stage and lost the Feedback Loop's own
    # Modulation.
    target_request = json.loads(
        (target_dir / "materialized" / "sample-request.json").read_text()
    )
    target_stages = target_request["composition"]["stages"]
    if [stage["type"] for stage in target_stages] != [
        "split",
        "diffuser",
        "feedback-loop",
        "downmix",
    ]:
        raise AssertionError(f"target/diffusion-step did not gain a Diffuser stage: {target_stages}")
    if "modulation" in target_stages[2]:
        raise AssertionError(
            f"target/diffusion-step did not remove the Feedback Loop's own "
            f"Modulation: {target_stages[2]}"
        )
    if "modulation" not in target_stages[1]["step"]:
        raise AssertionError(
            f"target/diffusion-step did not configure the Diffusion Step's "
            f"own Modulation: {target_stages[1]}"
        )
    target_analysis = json.loads(
        (target_dir / "impulse" / "analysis" / "modulation-v1.json").read_text()
    )
    owners = {entry["owner"] for entry in target_analysis["activeModulations"]}
    if owners != {"diffusion-step[0]"}:
        raise AssertionError(
            f"target/diffusion-step reported unexpected active owners: {owners}"
        )

    # The Reference point itself carries no overrides.
    reference_request = json.loads(
        (reference_dir / "materialized" / "sample-request.json").read_text()
    )
    if reference_request != reference:
        raise AssertionError(
            f"reference point was not the unmodified Reference configuration: "
            f"{reference_request}"
        )

    report = json.loads((sample_root / "sweep-report.json").read_text())
    if report["sample"] != fixture.name:
        raise AssertionError(f"unexpected sweep report header: {report}")
    for name in ("reference", "depth/omitted", "depth/zero", "target/diffusion-step"):
        if outcome(report, name)["status"] != "completed":
            raise AssertionError(f"unexpected sweep report: {report}")

    # Every point is benchmarked, and the benchmarks aggregate into a
    # ranked cross-point comparison.
    summary = json.loads((sample_root / "benchmark-summary.json").read_text())
    if summary["referenceCase"] != "reference" or summary["blockSize"] != 8:
        raise AssertionError(f"unexpected benchmark summary header: {summary}")
    ranked = summary["rankedBySlowestMedian"]
    if {entry["case"] for entry in ranked} != {
        "reference",
        "depth/omitted",
        "depth/zero",
        "target/diffusion-step",
    }:
        raise AssertionError(f"unexpected ranked points: {summary}")
    reference_entry = next(e for e in ranked if e["case"] == "reference")
    if (
        reference_entry["deltaFromReferenceMedianSeconds"] != 0
        or reference_entry["ratioToReferenceMedian"] != 1.0
    ):
        raise AssertionError(f"Reference did not compare exactly to itself: {summary}")

    # A self-contained listening report is generated, presenting every
    # point's renders, requested Modulation/Damping parameters, all five
    # modulation-v1 measurements, the identity invariant, and benchmark
    # cost -- readable on its own, without the terminal output that
    # produced it.
    report_html_path = sample_root / "listening-report.html"
    report_html = report_html_path.read_text()
    if "http://" in report_html or "https://" in report_html:
        raise AssertionError(
            "listening report references an external URL, is not self-contained"
        )
    if "<script" in report_html:
        raise AssertionError("listening report includes a script, is not self-contained")
    for expected in (
        "reference",
        "depth/omitted",
        "depth/zero",
        "target/diffusion-step",
        # Renders: relative paths only, playable alongside the report.
        "00-reference/sample/output.wav",
        "00-reference/impulse/output.wav",
        # Requested Modulation/Damping parameters.
        "Requested Modulation",
        "Requested Damping",
        # All five modulation-v1 measurements.
        "Bounded energy",
        "Decay tilt",
        "Coherent pitch movement",
        "Output correlation",
        "RT60 deviation from nominal",
        "Full octave-band decay curve",
        # The identity invariant, published as its own section.
        "Identity invariant (depth: omitted vs. zero)",
        # Benchmark cost.
        "Median block time",
    ):
        if expected not in report_html:
            raise AssertionError(f"listening report is missing {expected!r}")

    # depth/omitted's own section reports Modulation as absent.
    omitted_section = section_html(report_html, "depth/omitted")
    if "Modulation: absent." not in omitted_section:
        raise AssertionError(
            f"depth/omitted's section did not report Modulation as absent: "
            f"{omitted_section}"
        )

    # Resumable: a second run does not re-render or re-analyze anything, but
    # the report (and the identity invariant) is regenerated as part of
    # that same run.
    reference_output_before = (
        reference_dir / "sample" / "output.wav"
    ).read_bytes()
    report_html_path.unlink()
    identity_invariant_path = sample_root / "01-depth" / "identity-invariant.json"
    identity_invariant_path.unlink()
    second = run_sweep(fixture)
    if second.returncode != 0:
        raise AssertionError(f"resumed sweep failed: {second.stdout}{second.stderr}")
    for expected in (
        "[resumed] reference",
        "[resumed] depth/omitted",
        "[resumed] depth/zero",
        "[resumed] target/diffusion-step",
    ):
        if expected not in second.stdout:
            raise AssertionError(f"resumed sweep did not report {expected!r}: {second.stdout}")
    reference_output_after = (
        reference_dir / "sample" / "output.wav"
    ).read_bytes()
    if reference_output_before != reference_output_after:
        raise AssertionError("resuming the sweep replaced completed evidence")
    if not report_html_path.exists():
        raise AssertionError(
            "listening report was not regenerated by a fully resumed sweep "
            "(no re-rendering involved)"
        )
    if not identity_invariant_path.exists():
        raise AssertionError(
            "identity invariant was not regenerated by a fully resumed sweep"
        )

    # Resumability is per-step: a point missing only its benchmark retries
    # just that step, without re-rendering.
    target_benchmark = target_dir / "benchmark.json"
    target_benchmark.unlink()
    target_resolved_before = (target_dir / "sample" / "resolved.json").read_bytes()
    third = run_sweep(fixture)
    if "[completed] target/diffusion-step" not in third.stdout:
        raise AssertionError(
            f"a point missing only its benchmark did not retry and complete: "
            f"{third.stdout}"
        )
    if not target_benchmark.exists():
        raise AssertionError("retried benchmark step did not publish its report")
    target_resolved_after = (target_dir / "sample" / "resolved.json").read_bytes()
    if target_resolved_before != target_resolved_after:
        raise AssertionError("retrying the benchmark step re-rendered the point")


if __name__ == "__main__":
    main()
