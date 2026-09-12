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


def write_pcm_wav(path, samples, channels, sample_rate, bit_depth):
    """A PCM WAV of the given signed integer samples, for round-trip sources."""
    width = bit_depth // 8
    payload = b"".join(
        value.to_bytes(width, "little", signed=True) for value in samples
    )
    block_align = channels * width
    fmt = struct.pack(
        "<HHIIHH",
        1,
        channels,
        sample_rate,
        sample_rate * block_align,
        block_align,
        bit_depth,
    )
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(payload)) + payload
    path.write_bytes(b"RIFF" + struct.pack("<I", len(chunks) + 4) + b"WAVE" + chunks)


def exercising_samples(bit_depth, count=4096):
    """Deterministic samples spanning the depth's full range, extremes first.

    A committed impulse fixture is almost entirely zeros, so round-tripping
    one would pass even for a conversion that dropped every non-zero
    sample. These exercise sign, scale and byte packing at every magnitude.
    """
    limit = (1 << (bit_depth - 1)) - 1
    edges = [0, 1, -1, limit, -limit, limit // 2, -(limit // 2)]
    state = 0x2545F491
    values = list(edges)
    while len(values) < count:
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        values.append((state % (2 * limit + 1)) - limit)
    return values[:count]


def broken_request(workspace):
    """A request the renderer rejects, for exercising the failure path."""
    path = workspace / "broken.json"
    path.write_text(json.dumps({"formatVersion": 2, "reverbAmount": 0.7}) + "\n")
    return path


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
    # Every clipped file names its own overage: one aggregate number tells
    # you something clipped, not which file to go fix.
    for name in ("C3-rr1.wav", "C3-rr2.wav"):
        line = next(
            (
                text
                for text in completed.stdout.splitlines()
                if name in text and "dBFS" in text
            ),
            None,
        )
        if line is None:
            raise AssertionError(
                f"{name} has no clipping line of its own: {completed.stdout}"
            )

    if not (workspace / "loud" / "request.json").exists():
        raise AssertionError("a successful run did not record its request")

    # -- clipping clamps, it does not wrap -------------------------------
    # Reporting a clip while writing wrapped samples would be the worst of
    # both worlds. An all-positive source driven past full scale must come
    # back saturated at +full scale; wrapping inverts those samples instead
    # and shows up as large negatives.

    full_scale = (1 << 23) - 1
    dc_source = workspace / "dc-raw"
    dc_source.mkdir()
    write_pcm_wav(
        dc_source / "dc.wav", [int(full_scale * 0.9)] * 2000, 1, 48000, 24
    )
    dc_config = workspace / "dc.json"
    dc_config.write_text(
        json.dumps(
            {
                "formatVersion": 2,
                "composition": {
                    "stages": [
                        {"type": "split", "channels": 8},
                        {"type": "diffuser", "steps": 4, "totalMs": 10},
                        {
                            "type": "downmix",
                            "strategy": "select",
                            "leftChannel": 0,
                        },
                    ],
                    "dryDb": 12,
                    "wetDb": -120,
                    "wetOnly": False,
                },
            }
        )
        + "\n"
    )
    completed = run(
        tool, renderer, dc_source, workspace / "dc-out", dc_config, "--bit-depth", "24"
    )
    if completed.returncode != 0:
        raise AssertionError(f"the saturation run failed: {completed.stderr}")
    decoded = pcm_samples(workspace / "dc-out" / "dc.wav")
    if max(decoded) != full_scale:
        raise AssertionError(
            f"a clipped positive source did not saturate: max {max(decoded)}"
        )
    if min(decoded) < -(full_scale // 4):
        raise AssertionError(
            f"clipping wrapped instead of clamping: min {min(decoded)}"
        )

    # -- the float-to-PCM conversion is accurate at both depths ----------
    # An identity Composition returns its input unchanged, so a PCM source
    # round-trips through the renderer's float pipeline and back to PCM at
    # the same depth. Anything wrong with the scale factor, the byte order
    # or the 24-bit packing shows up here; the honest bound is one LSB
    # (see encode_pcm). The sources are noise across the full range rather
    # than a committed impulse, which is almost all zeros and would pass
    # even for a conversion that dropped every non-zero sample.

    identity_config = workspace / "identity.json"
    identity_config.write_text(json.dumps({"formatVersion": 2}) + "\n")

    for bit_depth, channels in ((16, 1), (24, 2)):
        identity_source = workspace / f"identity-raw-{bit_depth}"
        identity_source.mkdir()
        samples = exercising_samples(bit_depth)
        write_pcm_wav(
            identity_source / "note.wav", samples, channels, 48000, bit_depth
        )

        identity_dest = workspace / f"identity-out-{bit_depth}"
        completed = run(
            tool,
            renderer,
            identity_source,
            identity_dest,
            identity_config,
            "--bit-depth",
            str(bit_depth),
        )
        if completed.returncode != 0:
            raise AssertionError(
                f"the {bit_depth}-bit identity run failed: {completed.stderr}"
            )

        before = pcm_samples(identity_source / "note.wav")
        after = pcm_samples(identity_dest / "note.wav")
        if len(before) != len(after):
            raise AssertionError(
                f"{bit_depth}-bit identity changed the sample count: "
                f"{len(before)} -> {len(after)}"
            )
        if not any(before):
            raise AssertionError("the round-trip source is silent, proving nothing")
        worst = max(abs(a - b) for a, b in zip(before, after))
        if worst > 1:
            raise AssertionError(
                f"float-to-PCM{bit_depth} conversion is off by {worst} LSBs, "
                "expected at most 1"
            )

    # -- a renderer failure stops the run and surfaces its diagnostic ----

    completed = run(
        tool, renderer, source_dir, workspace / "broken", broken_request(workspace)
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
    # Refused with or without a suffix: a suffixed in-place run is not
    # idempotent (its second pass globs its own "-wet" outputs) and drops
    # the copied request among the raw sources.
    for extra in ((), ("--suffix=-wet",)):
        completed = run(tool, renderer, guarded, guarded, config, *extra)
        if completed.returncode == 0:
            raise AssertionError(
                f"rendering onto the source directory was allowed with {extra}"
            )
        if (guarded / "precious.wav").read_bytes() != before_bytes:
            raise AssertionError("the source file was modified despite the refusal")
        if sorted(path.name for path in guarded.iterdir()) != ["precious.wav"]:
            raise AssertionError(
                "the refused run still wrote into the source directory: "
                f"{sorted(path.name for path in guarded.iterdir())}"
            )

    # -- a failed rerun does not leave stale provenance ------------------
    # A folder holding request.json claims to be complete and to describe
    # the WAVs beside it. A rerun with a different request that fails
    # partway must not leave the old one standing over new audio.

    stale = workspace / "stale"
    completed = run(tool, renderer, source_dir, stale, config, "--bit-depth", "24")
    if completed.returncode != 0:
        raise AssertionError(f"seeding the stale-provenance run failed: {completed.stderr}")
    if not (stale / "request.json").exists():
        raise AssertionError("the seeding run recorded no request")

    completed = run(tool, renderer, source_dir, stale, broken_request(workspace))
    if completed.returncode == 0:
        raise AssertionError("the failing rerun was reported as success")
    if (stale / "request.json").exists():
        raise AssertionError(
            "a failed rerun left request.json claiming the folder is complete"
        )

    # -- two sources cannot claim one output name ------------------------
    # "note.wav" beside "note.WAV" both render to "note.wav", so one would
    # silently overwrite the other while the summary claimed both. Only a
    # case-sensitive filesystem can hold that pair, so this asserts where
    # it can and skips where the filesystem makes the case unreachable --
    # appending a constant --suffix cannot collide, since distinct stems
    # stay distinct.

    colliding = workspace / "colliding"
    colliding.mkdir()
    shutil.copyfile(mono, colliding / "note.wav")
    shutil.copyfile(stereo, colliding / "note.WAV")
    case_sensitive = len(list(colliding.iterdir())) == 2
    if case_sensitive:
        collision_dest = workspace / "colliding-out"
        completed = run(tool, renderer, colliding, collision_dest, config)
        if completed.returncode == 0:
            raise AssertionError("colliding output names were accepted")
        if "note.wav" not in completed.stderr or "note.WAV" not in completed.stderr:
            raise AssertionError(
                f"the collision report did not name both sources: {completed.stderr}"
            )
        if collision_dest.exists() and any(collision_dest.iterdir()):
            raise AssertionError("the refused run still rendered something")
    else:
        print(
            "skipping the name-collision case: this filesystem is "
            "case-insensitive, so note.wav and note.WAV cannot coexist"
        )

    # -- an unusable --dest-dir is refused cleanly -----------------------

    blocked = workspace / "blocked"
    blocked.write_text("I am a file, not a directory\n")
    completed = run(tool, renderer, source_dir, blocked, config)
    if completed.returncode == 0 or "Traceback" in completed.stderr:
        raise AssertionError(
            f"a --dest-dir that is a file was not refused cleanly: {completed.stderr}"
        )

    # -- uppercase extensions are sources too ----------------------------
    # Sample libraries ship ".WAV" constantly; missing them would silently
    # render nothing.

    upper = workspace / "upper"
    upper.mkdir()
    shutil.copyfile(mono, upper / "D3-rr1.WAV")
    completed = run(tool, renderer, upper, workspace / "upper-out", config)
    if completed.returncode != 0:
        raise AssertionError(f"an uppercase .WAV source was skipped: {completed.stderr}")
    if not (workspace / "upper-out" / "D3-rr1.wav").exists():
        raise AssertionError(
            f"no output for the .WAV source: "
            f"{sorted(p.name for p in (workspace / 'upper-out').iterdir())}"
        )

    # -- bad paths are refused cleanly, never as a traceback -------------

    for label, extra in (
        ("a missing source directory", (workspace / "absent", workspace / "nowhere", config)),
        ("a missing config", (source_dir, workspace / "nowhere", workspace / "absent.json")),
    ):
        completed = run(tool, renderer, *extra)
        if completed.returncode == 0:
            raise AssertionError(f"{label} was accepted")
        if "Traceback" in completed.stderr:
            raise AssertionError(f"{label} produced a traceback: {completed.stderr}")

    completed = subprocess.run(
        [
            sys.executable,
            str(tool),
            "--renderer",
            str(workspace / "absent-renderer"),
            "--source-dir",
            str(source_dir),
            "--dest-dir",
            str(workspace / "nowhere"),
            "--config",
            str(config),
        ],
        capture_output=True,
        text=True,
    )
    if completed.returncode == 0 or "Traceback" in completed.stderr:
        raise AssertionError(f"a missing renderer was not refused cleanly: {completed.stderr}")

    # -- re-rendering a folder with the request it already holds ---------
    # The tool copies the request beside its outputs, so pointing --config
    # back at that copy is a natural second run, not an error.

    completed = run(
        tool, renderer, source_dir, dest, dest / "request.json", "--bit-depth", "24"
    )
    if completed.returncode != 0:
        raise AssertionError(
            f"re-rendering with the copied request failed: {completed.stderr}"
        )


if __name__ == "__main__":
    main()
