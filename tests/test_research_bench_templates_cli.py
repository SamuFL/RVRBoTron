#!/usr/bin/env python3

"""Research bench Request templates (issue #140): executable documentation.

Each of the four committed templates renders successfully through the real
renderer, proving they cannot silently drift into invalid examples. Full
additionally carries a contract assertion that every field applicable to
its selected Composition shape is explicit -- mirroring both
docs/guides/configure-the-composition.md's field tables and
include/rvrbotron/config/ReverbConfig.h, the parser's own source of truth --
so a resolver-field change forces a look at this test, the template, and
the guide together.
"""

import json
import subprocess
import sys
from pathlib import Path

TEMPLATE_NAMES = ("simple", "full", "modulated", "spatial")


def load(templates_dir, name):
    return json.loads((templates_dir / f"{name}.json").read_text())


def render(renderer, fixture, config_path, output_dir):
    return subprocess.run(
        [
            str(renderer),
            "render",
            "--input",
            str(fixture),
            "--config",
            str(config_path),
            "--output",
            str(output_dir),
            "--error-format",
            "json",
        ],
        capture_output=True,
        text=True,
    )


def assert_keys(node, expected_keys, where):
    missing = sorted(set(expected_keys) - set(node.keys()))
    if missing:
        raise AssertionError(f"{where} is missing {missing}: {sorted(node.keys())}")


def check_full_field_coverage(full):
    """Every field applicable to Full's shape (a Diffuser feeding a Feedback
    Loop, both with every optional feature attached, plus Early
    Reflections) must appear explicitly. lengthsMs is Diffuser's mutually
    exclusive alternative to steps/totalMs/distribution (issue #140's own
    acceptance criterion keeps such alternatives out of the JSON, in the
    reference documentation instead), so its absence here is required, not
    an oversight to fix.
    """
    assert_keys(full, {"formatVersion", "seed", "composition"}, "Full")
    composition = full["composition"]
    assert_keys(
        composition,
        {
            "stages",
            "mainEnabled",
            "mainLevelDb",
            "early",
            "dryDb",
            "wetDb",
            "wetOnly",
            "preDelayMs",
        },
        "Full/composition",
    )

    split, diffuser, feedback_loop, downmix = composition["stages"]

    assert_keys(
        split, {"type", "channels", "strategy", "normalisation"}, "Full/split"
    )

    if "lengthsMs" in diffuser:
        raise AssertionError(
            "Full/diffuser sets lengthsMs alongside steps/totalMs/distribution, "
            "which are mutually exclusive"
        )
    assert_keys(
        diffuser,
        {"type", "steps", "totalMs", "distribution", "step", "stepOverrides"},
        "Full/diffuser",
    )
    assert_keys(
        diffuser["step"],
        {"delayStrategy", "mix", "shuffle", "polarity", "modulation"},
        "Full/diffuser/step",
    )
    assert_keys(
        diffuser["step"]["modulation"],
        {"depthMs", "rateHz", "shape", "channelFraction", "interpolation"},
        "Full/diffuser/step/modulation",
    )
    if not diffuser["stepOverrides"]:
        raise AssertionError("Full/diffuser/stepOverrides is empty")
    for override in diffuser["stepOverrides"]:
        assert_keys(
            override,
            {"index", "delayStrategy", "mix", "shuffle", "polarity", "modulation"},
            "Full/diffuser/stepOverrides[]",
        )
        assert_keys(
            override["modulation"],
            {"depthMs", "rateHz", "shape", "channelFraction", "interpolation"},
            "Full/diffuser/stepOverrides[]/modulation",
        )

    assert_keys(
        feedback_loop,
        {
            "type",
            "delayMinMs",
            "delayMaxMs",
            "delayStrategy",
            "rt60Sec",
            "decayMargin",
            "mix",
            "gainMode",
            "silenceFloorDb",
            "damping",
            "modulation",
        },
        "Full/feedback-loop",
    )
    assert_keys(
        feedback_loop["damping"],
        {"highRatio", "highHz", "lowRatio", "lowHz"},
        "Full/feedback-loop/damping",
    )
    assert_keys(
        feedback_loop["modulation"],
        {"depthMs", "rateHz", "shape", "channelFraction", "interpolation"},
        "Full/feedback-loop/modulation",
    )

    # select is the only Downmix strategy with additional applicable
    # fields (leftChannel/rightChannel); every other strategy rejects
    # them outright, so a shape richer in fields calls for select here.
    assert_keys(
        downmix,
        {
            "type",
            "strategy",
            "leftChannel",
            "rightChannel",
            "normalisation",
            "widthDeg",
        },
        "Full/downmix",
    )
    if downmix["strategy"] != "select":
        raise AssertionError(
            "Full/downmix should use select to cover leftChannel/rightChannel, "
            f"the fields no other strategy accepts: {downmix['strategy']}"
        )

    early = composition["early"]
    assert_keys(
        early,
        {"enabled", "levelDb", "decayDbPerSec", "taps", "downmix"},
        "Full/early",
    )
    if not early["taps"]:
        raise AssertionError("Full/early/taps is empty")
    for tap in early["taps"]:
        assert_keys(tap, {"stepIndex", "gainDb"}, "Full/early/taps[]")
    # Early's own downmix defaults select to Channels 0/1 rather than
    # requiring them explicitly, so a non-select strategy here -- distinct
    # from the Main Downmix's select above -- demonstrates that field set
    # too, without leftChannel/rightChannel ambiguity either way.
    assert_keys(
        early["downmix"],
        {"strategy", "normalisation", "widthDeg"},
        "Full/early/downmix",
    )
    if early["downmix"]["strategy"] == "select":
        raise AssertionError(
            "Full/early/downmix should use a non-select strategy to cover the "
            "field set select does not share with it"
        )


def check_shapes(templates):
    shape = ["split", "diffuser", "feedback-loop", "downmix"]
    for name in TEMPLATE_NAMES:
        stage_types = [
            stage["type"] for stage in templates[name]["composition"]["stages"]
        ]
        if stage_types != shape:
            raise AssertionError(
                f"{name} does not use the split+diffuser+feedback-loop+downmix "
                f"shape: {stage_types}"
            )

    simple = templates["simple"]["composition"]
    if "early" in simple or any(
        "modulation" in stage or "step" in stage or "stepOverrides" in stage
        for stage in simple["stages"]
    ):
        raise AssertionError(
            "Simple must have no Early Reflections or Modulation"
        )

    modulated = templates["modulated"]["composition"]
    diffuser = modulated["stages"][1]
    feedback_loop = modulated["stages"][2]
    has_diffuser_modulation = "modulation" in diffuser.get("step", {}) or any(
        "modulation" in override
        for override in diffuser.get("stepOverrides", [])
    )
    if not has_diffuser_modulation:
        raise AssertionError("Modulated has no Diffusion-Step modulation")
    if "modulation" not in feedback_loop:
        raise AssertionError("Modulated has no Feedback-Loop modulation")

    spatial = templates["spatial"]["composition"]
    if "early" not in spatial or not spatial["early"].get("taps"):
        raise AssertionError("Spatial has no Early Reflections")
    main_downmix = spatial["stages"][3]
    early_downmix = spatial["early"].get("downmix") or {}
    if not (
        main_downmix.get("widthDeg", 90) > 90
        and early_downmix.get("widthDeg", 90) > 90
    ):
        raise AssertionError(
            "Spatial does not widen both the Main and Early Downmix"
        )


def main():
    renderer = Path(sys.argv[1])
    fixture = Path(sys.argv[2])
    templates_dir = Path(sys.argv[3])
    workspace = Path(sys.argv[4])
    workspace.mkdir(parents=True, exist_ok=True)

    templates = {name: load(templates_dir, name) for name in TEMPLATE_NAMES}

    check_shapes(templates)
    check_full_field_coverage(templates["full"])

    for name in TEMPLATE_NAMES:
        output_dir = workspace / name
        completed = render(
            renderer, fixture, templates_dir / f"{name}.json", output_dir
        )
        if completed.returncode != 0:
            raise AssertionError(
                f"the {name} template failed to render: {completed.stderr}"
            )
        if not (output_dir / "output.wav").exists():
            raise AssertionError(f"the {name} template produced no output.wav")


if __name__ == "__main__":
    main()
