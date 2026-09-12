#!/usr/bin/env python3

"""Bulk folder rendering contract (issue #151).

Drives tools/render_folder.py against the real renderer over a small source
folder, proving what a sampled-instrument workflow depends on: every source
comes back under its own stem, as PCM at the requested bit depth, with the
request that produced the folder beside them.

These outputs are deliberately not Render Results -- bare WAVs, no
resolved.json -- so nothing here asserts reproducibility evidence.
"""

import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

REQUEST = {
    "formatVersion": 2,
    "seed": 42,
    "composition": {
        "stages": [
            {"type": "split", "channels": 8},
            {"type": "diffuser", "steps": 4, "totalMs": 80},
            {
                "type": "feedback-loop",
                "delayMinMs": 60,
                "delayMaxMs": 120,
                "rt60Sec": 1.0,
            },
            {"type": "downmix", "strategy": "orthogonal-rows"},
        ],
        "preDelayMs": 20,
        "dryDb": 0,
        "wetDb": -3,
        "wetOnly": False,
    },
}


def inspect_wav(path):
    """Format tag, bit depth and frame count of a RIFF/WAVE file."""
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise AssertionError(f"{path} is not a RIFF/WAVE file")
    offset = 12
    fmt = None
    frames = None
    while offset + 8 <= len(data):
        chunk_id = data[offset : offset + 4]
        (size,) = struct.unpack_from("<I", data, offset + 4)
        if chunk_id == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", data, offset + 8)
        elif chunk_id == b"data":
            frames = size
        offset += 8 + size + size % 2
    if fmt is None or frames is None:
        raise AssertionError(f"{path} is missing fmt or data")
    tag, channels, sample_rate, _, block_align, bits = fmt
    return {
        "formatTag": tag,
        "channels": channels,
        "sampleRate": sample_rate,
        "sampleBits": bits,
        "frameCount": frames // block_align,
    }


def pcm_samples(path):
    """Signed integer samples of a PCM WAV, at whatever width it carries."""
    data = path.read_bytes()
    offset = 12
    bits = None
    payload = None
    while offset + 8 <= len(data):
        chunk_id = data[offset : offset + 4]
        (size,) = struct.unpack_from("<I", data, offset + 4)
        if chunk_id == b"fmt ":
            bits = struct.unpack_from("<HHIIHH", data, offset + 8)[5]
        elif chunk_id == b"data":
            payload = data[offset + 8 : offset + 8 + size]
        offset += 8 + size + size % 2
    if bits is None or payload is None:
        raise AssertionError(f"{path} is missing fmt or data")
    width = bits // 8
    return [
        int.from_bytes(payload[index : index + width], "little", signed=True)
        for index in range(0, len(payload), width)
    ]


def run(tool, renderer, source_dir, dest_dir, config, *extra):
    return subprocess.run(
        [
            sys.executable,
            str(tool),
            "--renderer",
            str(renderer),
            "--source-dir",
            str(source_dir),
            "--dest-dir",
            str(dest_dir),
            "--config",
            str(config),
            *extra,
        ],
        capture_output=True,
        text=True,
    )


def main():
    tool = Path(sys.argv[1])
    renderer = Path(sys.argv[2])
    mono = Path(sys.argv[3])
    stereo = Path(sys.argv[4])
    workspace = Path(sys.argv[5])

    shutil.rmtree(workspace, ignore_errors=True)
    workspace.mkdir(parents=True)

    # A source folder shaped like a sampled instrument's: a couple of named
    # sources, plus a non-WAV the tool must leave alone.
    source_dir = workspace / "raw"
    source_dir.mkdir()
    shutil.copyfile(mono, source_dir / "C3-rr1.wav")
    shutil.copyfile(stereo, source_dir / "C3-rr2.wav")
    (source_dir / "notes.txt").write_text("not audio\n")
    source_frames = inspect_wav(source_dir / "C3-rr1.wav")["frameCount"]

    config = workspace / "request.json"
    config.write_text(json.dumps(REQUEST, indent=2) + "\n")

    # -- a plain run, 24-bit -------------------------------------------

    dest = workspace / "rendered"
    completed = run(tool, renderer, source_dir, dest, config, "--bit-depth", "24")
    if completed.returncode != 0:
        raise AssertionError(f"render_folder failed: {completed.stderr}")

    produced = sorted(path.name for path in dest.glob("*.wav"))
    if produced != ["C3-rr1.wav", "C3-rr2.wav"]:
        raise AssertionError(f"unexpected outputs: {produced}")
    if not (dest / "request.json").exists():
        raise AssertionError("the request was not copied beside the outputs")
    if json.loads((dest / "request.json").read_text()) != REQUEST:
        raise AssertionError("the copied request does not match the one rendered")

    for name in produced:
        wav = inspect_wav(dest / name)
        if wav["formatTag"] != 1 or wav["sampleBits"] != 24:
            raise AssertionError(f"{name} is not 24-bit PCM: {wav}")
        # The reverb tail makes every output longer than its source.
        if wav["frameCount"] <= source_frames:
            raise AssertionError(
                f"{name} is not longer than its source: "
                f"{wav['frameCount']} <= {source_frames}"
            )

    if "2 rendered" not in completed.stdout:
        raise AssertionError(f"summary did not report the count: {completed.stdout}")

    # -- suffix and 16-bit ----------------------------------------------

    # A dash-leading suffix is the realistic case, and needs the equals form
    # -- argparse would otherwise read "-wet" as another flag.
    dest16 = workspace / "rendered-16"
    completed = run(
        tool,
        renderer,
        source_dir,
        dest16,
        config,
        "--bit-depth",
        "16",
        "--suffix=-wet",
    )
    if completed.returncode != 0:
        raise AssertionError(f"suffixed run failed: {completed.stderr}")
    produced = sorted(path.name for path in dest16.glob("*.wav"))
    if produced != ["C3-rr1-wet.wav", "C3-rr2-wet.wav"]:
        raise AssertionError(f"suffix was not applied: {produced}")
    if inspect_wav(dest16 / "C3-rr1-wet.wav")["sampleBits"] != 16:
        raise AssertionError("--bit-depth 16 did not produce 16-bit PCM")

    # -- a second run overwrites rather than refusing --------------------

    completed = run(tool, renderer, source_dir, dest, config, "--bit-depth", "24")
    if completed.returncode != 0:
        raise AssertionError(f"a repeat run did not overwrite: {completed.stderr}")

    # -- clipping is reported, not silent --------------------------------

    loud = workspace / "loud.json"
    loud_request = json.loads(json.dumps(REQUEST))
    loud_request["composition"]["dryDb"] = 18
    loud_request["composition"]["wetDb"] = 18
    loud.write_text(json.dumps(loud_request, indent=2) + "\n")
    completed = run(
        tool, renderer, source_dir, workspace / "loud", loud, "--bit-depth", "24"
    )
    if completed.returncode != 0:
        raise AssertionError(f"the loud run failed outright: {completed.stderr}")
    if "clipped" not in completed.stdout:
        raise AssertionError(
            f"clipping was not reported: {completed.stdout}"
        )
    if "C3-rr1.wav" not in completed.stdout:
        raise AssertionError(
            f"the clipping report did not name the file: {completed.stdout}"
        )

    # -- the float-to-PCM conversion is accurate ------------------------
    # An identity Composition returns the input unchanged, so a PCM24
    # source round-trips through the renderer's float32 pipeline and back
    # to PCM24. Anything wrong with the scale factor, the byte order, or
    # the 24-bit packing shows up here as a large difference; the honest
    # bound is one LSB (see encode_pcm).

    identity_source = workspace / "identity-raw"
    identity_source.mkdir()
    shutil.copyfile(stereo, identity_source / "note.wav")
    identity_config = workspace / "identity.json"
    identity_config.write_text(json.dumps({"formatVersion": 2}) + "\n")
    # Rendered at the source's own depth, so the comparison is like for like.
    identity_dest = workspace / "identity-out"
    completed = run(
        tool,
        renderer,
        identity_source,
        identity_dest,
        identity_config,
        "--bit-depth",
        "16",
    )
    if completed.returncode != 0:
        raise AssertionError(f"the identity run failed: {completed.stderr}")

    before = pcm_samples(identity_source / "note.wav")
    after = pcm_samples(identity_dest / "note.wav")
    if len(before) != len(after):
        raise AssertionError(
            f"identity changed the sample count: {len(before)} -> {len(after)}"
        )
    worst = max(abs(a - b) for a, b in zip(before, after))
    if worst > 1:
        raise AssertionError(
            f"float-to-PCM24 conversion is off by {worst} LSBs, expected at most 1"
        )

    # -- a renderer failure stops the run and surfaces its diagnostic ----

    broken = workspace / "broken.json"
    broken.write_text(json.dumps({"formatVersion": 2, "reverbAmount": 0.7}) + "\n")
    completed = run(
        tool, renderer, source_dir, workspace / "broken", broken
    )
    if completed.returncode == 0:
        raise AssertionError("an invalid request did not fail the run")
    combined = completed.stdout + completed.stderr
    for expected in ("invalid_configuration", "unknown field", "/reverbAmount"):
        if expected not in combined:
            raise AssertionError(
                f"the renderer diagnostic was not surfaced ({expected!r}): {combined}"
            )

    # -- rendering onto the sources is refused ---------------------------
    # Raw recorded material is unrecoverable, so overwriting it in place
    # must not be one typo away.

    guarded = workspace / "guarded"
    guarded.mkdir()
    shutil.copyfile(mono, guarded / "precious.wav")
    before_bytes = (guarded / "precious.wav").read_bytes()
    completed = run(tool, renderer, guarded, guarded, config)
    if completed.returncode == 0:
        raise AssertionError("rendering onto the source directory was allowed")
    if (guarded / "precious.wav").read_bytes() != before_bytes:
        raise AssertionError("the source file was modified despite the refusal")

    # With a suffix the outputs are distinct, so it is allowed.
    completed = run(tool, renderer, guarded, guarded, config, "--suffix=-wet")
    if completed.returncode != 0:
        raise AssertionError(
            f"a suffixed in-place render was refused: {completed.stderr}"
        )
    if (guarded / "precious.wav").read_bytes() != before_bytes:
        raise AssertionError("a suffixed run still overwrote the source")
    if not (guarded / "precious-wet.wav").exists():
        raise AssertionError("the suffixed output was not written")

    # -- a missing source directory is refused before any work -----------

    completed = run(
        tool, renderer, workspace / "absent", workspace / "nowhere", config
    )
    if completed.returncode == 0:
        raise AssertionError("a missing source directory was accepted")


if __name__ == "__main__":
    main()
