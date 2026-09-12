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
import re
import shutil
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


def parser_accepted_fields(source, function_name):
    """The exact field allowlist one ConfigJson.cpp parser function accepts,
    read from its own rejectUnknownFields(...) call -- the parser's actual
    source of truth, not a hand-transcribed copy of it. A field the parser
    starts (or stops) accepting changes this set the moment ConfigJson.cpp
    changes, with no separate step to keep it current -- so check_full_
    field_coverage catches a newly added applicable field even before
    anyone updates this test or the template for it.
    """
    definition = re.search(
        r"^\S.*\b" + re.escape(function_name) + r"\s*\(", source, re.MULTILINE
    )
    if not definition:
        raise AssertionError(f"{function_name} not found in ConfigJson.cpp")
    call = re.search(r"rejectUnknownFields\s*\(", source[definition.end():])
    if not call:
        raise AssertionError(f"{function_name} has no rejectUnknownFields call")
    search_from = definition.end() + call.end()
    brace_start = source.index("{", search_from)
    brace_end = source.index("}", brace_start)
    fields = set(re.findall(r'"([A-Za-z0-9]+)"', source[brace_start:brace_end]))
    if not fields:
        raise AssertionError(f"{function_name}'s rejectUnknownFields call has no fields")
    return fields


def load_parser_fields(config_json_path):
    source = config_json_path.read_text()
    return {
        "top": parser_accepted_fields(source, "parseRequestedConfig"),
        "composition": parser_accepted_fields(source, "parseRequestedComposition"),
        "split": parser_accepted_fields(source, "parseRequestedSplit"),
        # lengthsMs is Diffuser's mutually exclusive alternative to
        # steps/totalMs/distribution (issue #140's own acceptance criterion
        # keeps such alternatives out of Full's JSON), so it is excluded
        # here as a documented choice, not missing by oversight.
        "diffuser": parser_accepted_fields(source, "parseRequestedDiffuser")
        - {"lengthsMs"},
        "step": parser_accepted_fields(source, "parseRequestedStep"),
        "step_override": parser_accepted_fields(source, "parseRequestedStepOverride"),
        "modulation": parser_accepted_fields(source, "parseRequestedModulation"),
        "feedback_loop": parser_accepted_fields(source, "parseRequestedFeedbackLoop"),
        "damping": parser_accepted_fields(source, "parseRequestedDamping"),
        # leftChannel/rightChannel apply only to strategy "select"; every
        # other strategy rejects them outright (parseRequestedDownmix's own
        # branch on strategy). type is required only as the stage array's
        # own discriminator -- Early's downmix, parsed by this same
        # function, never carries it (checkNestedDownmixType's requireType
        # is false there). Both are named separately so a caller can add
        # them back only where the JSON in hand actually needs them.
        "downmix": parser_accepted_fields(source, "parseRequestedDownmix")
        - {"leftChannel", "rightChannel", "type"},
        "downmix_select_only": {"leftChannel", "rightChannel"},
        "downmix_stage_only": {"type"},
        "early": parser_accepted_fields(source, "parseRequestedEarly"),
        "early_tap": parser_accepted_fields(source, "parseRequestedEarlyTap"),
    }


def check_full_field_coverage(full, fields):
    """Every field applicable to Full's shape (a Diffuser feeding a Feedback
    Loop, both with every optional feature attached, plus Early
    Reflections) must appear explicitly. `fields` comes from
    load_parser_fields, i.e. from ConfigJson.cpp itself, so a field the
    parser gains shows up as newly required here -- not just a field
    Full happens to stop setting.
    """
    assert_keys(full, fields["top"], "Full")
    composition = full["composition"]
    assert_keys(composition, fields["composition"], "Full/composition")

    split, diffuser, feedback_loop, downmix = composition["stages"]

    assert_keys(split, fields["split"], "Full/split")

    if "lengthsMs" in diffuser:
        raise AssertionError(
            "Full/diffuser sets lengthsMs alongside steps/totalMs/distribution, "
            "which are mutually exclusive"
        )
    assert_keys(diffuser, fields["diffuser"], "Full/diffuser")
    assert_keys(diffuser["step"], fields["step"], "Full/diffuser/step")
    assert_keys(
        diffuser["step"]["modulation"],
        fields["modulation"],
        "Full/diffuser/step/modulation",
    )
    if not diffuser["stepOverrides"]:
        raise AssertionError("Full/diffuser/stepOverrides is empty")
    for override in diffuser["stepOverrides"]:
        assert_keys(
            override, fields["step_override"], "Full/diffuser/stepOverrides[]"
        )
        assert_keys(
            override["modulation"],
            fields["modulation"],
            "Full/diffuser/stepOverrides[]/modulation",
        )

    assert_keys(feedback_loop, fields["feedback_loop"], "Full/feedback-loop")
    assert_keys(
        feedback_loop["damping"], fields["damping"], "Full/feedback-loop/damping"
    )
    assert_keys(
        feedback_loop["modulation"], fields["modulation"], "Full/feedback-loop/modulation"
    )

    # select is the only Downmix strategy with additional applicable
    # fields (leftChannel/rightChannel); every other strategy rejects
    # them outright, so a shape richer in fields calls for select here.
    assert_keys(
        downmix,
        fields["downmix"] | fields["downmix_select_only"] | fields["downmix_stage_only"],
        "Full/downmix",
    )
    if downmix["strategy"] != "select":
        raise AssertionError(
            "Full/downmix should use select to cover leftChannel/rightChannel, "
            f"the fields no other strategy accepts: {downmix['strategy']}"
        )

    early = composition["early"]
    assert_keys(early, fields["early"], "Full/early")
    if not early["taps"]:
        raise AssertionError("Full/early/taps is empty")
    for tap in early["taps"]:
        assert_keys(tap, fields["early_tap"], "Full/early/taps[]")
    # Early's own downmix defaults select to Channels 0/1 rather than
    # requiring them explicitly, so a non-select strategy here -- distinct
    # from the Main Downmix's select above -- demonstrates that field set
    # too, without leftChannel/rightChannel ambiguity either way.
    assert_keys(early["downmix"], fields["downmix"], "Full/early/downmix")
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
    config_json_path = Path(sys.argv[5])
    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    templates = {name: load(templates_dir, name) for name in TEMPLATE_NAMES}

    check_shapes(templates)
    check_full_field_coverage(templates["full"], load_parser_fields(config_json_path))

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
