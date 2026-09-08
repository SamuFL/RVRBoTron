#!/usr/bin/env python3

"""Smoke coverage for tools/evaluate_allpass_interpolation.py (issue #93):
confirms the script that reproduces docs/design/reverb/stages/
06-modulation.md's "Allpass viability" evidence runs end to end and
produces the expected report shape. Exact numeric values are not asserted
here -- they are environment-qualified DSP evidence, not a CI gate (the
same reasoning as benchmark_contract's own smoke-only coverage) -- only
that every section renders, every allpass render stays finite, and the
transient-artefact ordering (allpass rougher than both other methods at
matched depth/rate) holds on this machine too.
"""

import re
import subprocess
import sys
from pathlib import Path


def main():
    script = Path(sys.argv[1])
    renderer = Path(sys.argv[2])
    fixture = Path(sys.argv[3])
    workspace = Path(sys.argv[4])

    completed = subprocess.run(
        [sys.executable, str(script), str(renderer), str(fixture), str(workspace)],
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr)

    output = completed.stdout
    for heading in (
        "## Stability: peak amplitude and tail RMS",
        "## Long render",
        "## Transient artefact: sample-to-sample discontinuity",
    ):
        if heading not in output:
            raise AssertionError(f"missing expected section {heading!r}: {output}")

    # Every reported peak/rms/jump value is a finite, non-negative number:
    # a NaN or negative figure here would mean the evaluation itself is
    # broken, not just that this render happened to sound different. The
    # alternation also matches Python's own non-finite spellings (`nan`,
    # `inf`, `-inf`) -- the decimal-only pattern this started as would
    # silently drop those tokens instead of catching them below (PR #101
    # review).
    numbers = re.findall(r"[-+]?(?:\d*\.\d+(?:[eE][-+]?\d+)?|nan|inf)", output)
    if not numbers:
        raise AssertionError(f"no numeric evidence was reported: {output}")
    for token in numbers:
        value = float(token)
        if not (value == value) or value in (float("inf"), float("-inf")):
            raise AssertionError(f"non-finite value {token!r} in report: {output}")
        if value < 0:
            raise AssertionError(f"unexpected negative value {token!r}: {output}")

    # The transient-artefact table's own ordering claim: allpass carries a
    # larger mean-square sample-to-sample jump than lagrange3 or linear at
    # the same depthMs/rateHz.
    transient_section = output.split(
        "## Transient artefact: sample-to-sample discontinuity"
    )[1]
    rows = {}
    for line in transient_section.splitlines():
        match = re.match(
            r"^(\S+(?: \(\S+\))?)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([\d.eE+-]+)$",
            line.strip(),
        )
        if match:
            rows[match.group(1)] = float(match.group(5))
    if "lagrange3" not in rows or "linear" not in rows or "allpass" not in rows:
        raise AssertionError(
            f"transient artefact table did not include all three baseline "
            f"methods: {rows}"
        )
    if not (rows["allpass"] > rows["lagrange3"] and rows["allpass"] > rows["linear"]):
        raise AssertionError(
            f"allpass did not carry more mean-square sample-to-sample "
            f"jump than lagrange3 and linear at matched depth/rate: {rows}"
        )


if __name__ == "__main__":
    main()
