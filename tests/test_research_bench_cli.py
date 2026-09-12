#!/usr/bin/env python3

"""Research bench contract: the complete local HTTP workflow (issue #137).

Drives the real bench server against the real renderer and a committed WAV
fixture -- select a source, render an editor string, read the facts back,
and fetch the served audio -- rather than testing handler internals.
"""

import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


def spawn_kwargs():
    """Extra Popen kwargs so interrupt() can reach this child specifically.

    On Windows, delivering anything other than a hard TerminateProcess to
    a specific child (rather than every process on the caller's console)
    requires CREATE_NEW_PROCESS_GROUP at spawn time; POSIX needs nothing
    extra.
    """
    if os.name == "nt":
        return {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
    return {}


def interrupt(process):
    """Ask the launcher to shut down the way a terminal interruption would,
    rather than killing it outright -- the whole point of these tests is
    to prove the launcher's own cleanup runs.

    Windows has no deliverable SIGTERM (Popen.terminate()/os.kill() there
    is an unconditional TerminateProcess that runs no Python handler), so
    CTRL_BREAK_EVENT -- which serve.py also handles, as SIGBREAK -- is the
    only way a separate process can ask this one to shut down gracefully.
    """
    if os.name == "nt":
        process.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        process.send_signal(signal.SIGINT)


def wait_for_banner(process, timeout=60.0):
    """Read the launcher's own banner for the URL and session root it chose."""
    deadline = time.time() + timeout
    url = None
    session_root = None
    while time.time() < deadline:
        line = process.stdout.readline()
        if not line:
            if process.poll() is not None:
                raise AssertionError("bench exited before printing its banner")
            continue
        if "session:" in line:
            session_root = Path(line.split("session:", 1)[1].strip())
        if "open:" in line:
            url = line.split("open:", 1)[1].strip()
        if url is not None and session_root is not None:
            return url, session_root
    raise AssertionError("bench did not print its banner within the timeout")


def call(url, data=None, headers=None, method=None):
    request = urllib.request.Request(url, data=data, headers=headers or {}, method=method)
    try:
        with urllib.request.urlopen(request) as response:
            return response.status, response.read(), dict(response.headers)
    except urllib.error.HTTPError as error:
        return error.code, error.read(), dict(error.headers)


def json_body(payload):
    return json.loads(payload.decode("utf-8"))


IDENTITY_REQUEST = json.dumps({"formatVersion": 2}).encode("utf-8")

REVERB_REQUEST = json.dumps(
    {
        "formatVersion": 2,
        "seed": 42,
        "composition": {
            "stages": [
                {"type": "split", "channels": 4},
                {"type": "diffuser", "steps": 2, "totalMs": 8},
                {
                    "type": "downmix",
                    "strategy": "select",
                    "leftChannel": 0,
                    "rightChannel": 1,
                },
            ]
        },
    }
).encode("utf-8")


def main():
    serve = Path(sys.argv[1])
    renderer = Path(sys.argv[2])
    fixture = Path(sys.argv[3])
    workspace = Path(sys.argv[4])
    sample_bits = int(sys.argv[7])
    if sample_bits not in (32, 64):
        raise AssertionError(f"unexpected configured sample bits: {sample_bits}")

    process = subprocess.Popen(
        [sys.executable, str(serve), "--renderer", str(renderer), "--no-browser"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        **spawn_kwargs(),
    )
    try:
        url, session_root = wait_for_banner(process)
        base, _, query = url.partition("?")
        token = query.split("token=", 1)[1]

        def api(route):
            return f"{base}api/{route}?token={token}"

        # -- authorization ------------------------------------------------

        status, _, _ = call(f"{base}api/output.wav?token=wrong")
        if status != 403:
            raise AssertionError(f"a wrong token was not rejected: {status}")

        status, _, _ = call(f"{base}api/output.wav")
        if status != 403:
            raise AssertionError(f"a missing token was not rejected: {status}")

        status, _, _ = call(api("output.wav"), headers={"Origin": "http://evil.example"})
        if status != 403:
            raise AssertionError(f"a foreign Origin was not rejected: {status}")

        # Nothing has been rendered yet.
        status, _, _ = call(api("output.wav"))
        if status != 404:
            raise AssertionError(f"output was served before any render: {status}")

        # -- the page -----------------------------------------------------

        status, page, headers = call(url)
        if status != 200:
            raise AssertionError(f"page did not load: {status}")
        text = page.decode("utf-8")
        if "<title>RVRBoTron Research Bench</title>" not in text:
            raise AssertionError("page is not titled RVRBoTron Research Bench")
        if headers.get("X-Content-Type-Options") != "nosniff":
            raise AssertionError("missing nosniff header")
        if any(key.lower().startswith("access-control-allow") for key in headers):
            raise AssertionError("bench granted a CORS permission")

        # The policy must permit what the page actually does -- load its own
        # script and style, call its own API, and (style-src only, for
        # Ace's own runtime CSS injection -- see vendor/ace/VENDORING.md)
        # use inline style -- without ever allowing inline script. A page
        # whose script the policy forbids is inert.
        policy = headers.get("Content-Security-Policy", "")
        directives = {}
        for clause in policy.split(";"):
            tokens = clause.split()
            if tokens:
                directives[tokens[0]] = tokens[1:]
        for name, required in (
            ("script-src", ["'self'"]),
            ("style-src", ["'self'", "'unsafe-inline'"]),
            ("connect-src", ["'self'"]),
            ("worker-src", ["'none'"]),
            ("img-src", ["'self'", "data:"]),
        ):
            missing = [v for v in required if v not in directives.get(name, [])]
            if missing:
                raise AssertionError(f"{name} is missing {missing}: {policy}")
        script_src = directives.get("script-src", [])
        if "'unsafe-inline'" in script_src or "'unsafe-eval'" in script_src:
            raise AssertionError(f"script-src relaxes inline execution: {policy}")
        if re.search(r"<script(?![^>]*\ssrc=)[^>]*>\s*\S", text) or "<style" in text:
            raise AssertionError("page carries inline script or style the policy forbids")

        # Every asset the page references must actually be served.
        assets = re.findall(r'(?:src|href)="([^"]+)"', text)
        if not any(asset.startswith("bench.js") for asset in assets):
            raise AssertionError(f"page does not load its script: {assets}")
        for asset in assets:
            status, body, asset_headers = call(base + asset)
            if status != 200 or not body:
                raise AssertionError(f"asset {asset} was not served: {status}")
            if asset.startswith("bench.js"):
                if "javascript" not in asset_headers.get("Content-Type", ""):
                    raise AssertionError(f"script served as {asset_headers}")

        # An asset without the capability token is refused like any other read.
        status, _, _ = call(f"{base}bench.js")
        if status != 403:
            raise AssertionError(f"an untokened asset was served: {status}")

        # Vendored Ace files (issue #138) must be exactly what VENDORING.md
        # records -- that manifest's whole point is catching silent drift
        # or corruption, not just documenting a version number once.
        vendor_manifest = Path(sys.argv[6])
        manifest_text = vendor_manifest.read_text()
        vendor_hashes = dict(
            re.findall(r"\| `([^`]+)` \| `[^`]+` \| \d+ \| `([0-9a-f]{64})` \|", manifest_text)
        )
        if len(vendor_hashes) < 4:
            raise AssertionError(f"vendor manifest table looks short: {vendor_hashes}")
        for name, expected_hash in vendor_hashes.items():
            if name == "LICENSE":
                actual = (vendor_manifest.parent / name).read_bytes()
            else:
                status, actual, _ = call(f"{base}vendor/ace/{name}?token={token}")
                if status != 200:
                    raise AssertionError(f"vendored {name} was not served: {status}")
            digest = hashlib.sha256(actual).hexdigest()
            if digest != expected_hash:
                raise AssertionError(
                    f"vendored {name} does not match VENDORING.md: "
                    f"{digest} != {expected_hash}"
                )

        # -- rendering needs a source first --------------------------------

        status, body, _ = call(
            api("render"),
            data=REVERB_REQUEST,
            headers={"Content-Type": "application/json"},
        )
        if status != 400 or "Audition source" not in json_body(body)["reason"]:
            raise AssertionError(f"rendering without a source was not refused: {body}")

        # -- select the Audition source ------------------------------------

        source_bytes = fixture.read_bytes()
        status, body, _ = call(
            api("source"),
            data=source_bytes,
            headers={
                "Content-Type": "application/octet-stream",
                "X-Source-Filename": fixture.name,
            },
        )
        if status != 200:
            raise AssertionError(f"source was refused: {body}")
        facts = json_body(body)
        if facts["filename"] != fixture.name or facts["sampleRate"] != 48000:
            raise AssertionError(f"unexpected source facts: {facts}")

        # A file the renderer's WAV contract does not accept is refused
        # with its own vocabulary, not converted.
        status, body, _ = call(
            api("source"),
            data=b"not a wav at all",
            headers={"Content-Type": "application/octet-stream"},
        )
        if status != 400 or json_body(body)["category"] != "unsupported_audio":
            raise AssertionError(f"a non-WAV source was not refused: {body}")

        # Selection accepts exactly what the renderer accepts: a WAV the
        # bench refuses must be one the renderer refuses too, for the same
        # stated reason, rather than one that fails later at Render.
        workspace.mkdir(parents=True, exist_ok=True)
        truncated_path = workspace / "truncated.wav"
        truncated_path.write_bytes(source_bytes[: len(source_bytes) - 64])
        status, body, _ = call(
            api("source"),
            data=truncated_path.read_bytes(),
            headers={"Content-Type": "application/octet-stream"},
        )
        if status != 400:
            raise AssertionError("a truncated WAV was accepted at selection")
        refusal = json_body(body)
        direct = subprocess.run(
            [
                str(renderer),
                "render",
                "--input",
                str(truncated_path),
                "--config",
                str(write_request(workspace, IDENTITY_REQUEST)),
                "--output",
                str(workspace / "truncated-out"),
                "--error-format",
                "json",
            ],
            capture_output=True,
            text=True,
        )
        if direct.returncode == 0:
            raise AssertionError("the renderer accepted a WAV the bench refused")
        renderer_reason = json.loads(direct.stderr.strip().splitlines()[-1])
        if refusal["reason"] != renderer_reason["reason"]:
            raise AssertionError(
                "bench and renderer disagree on the WAV contract: "
                f"{refusal} vs {renderer_reason}"
            )

        # The bench parses WAV headers independently of the renderer, so
        # every encoding the renderer supports must survive selection with
        # the facts the committed matrix records -- otherwise the two
        # contracts drift apart silently.
        matrix = Path(sys.argv[5])
        manifest = json.loads((matrix / "manifest.json").read_text())
        if len(manifest) < 30:
            raise AssertionError(f"matrix manifest looks short: {len(manifest)}")
        for name, expected in sorted(manifest.items()):
            status, body, _ = call(
                api("source"),
                data=(matrix / name).read_bytes(),
                headers={
                    "Content-Type": "application/octet-stream",
                    "X-Source-Filename": name,
                },
            )
            if status != 200:
                raise AssertionError(f"the bench refused {name}: {body}")
            got = json_body(body)
            for key in ("channels", "sampleRate", "frames"):
                if got[key] != expected[key]:
                    raise AssertionError(
                        f"{name}: bench read {key}={got[key]}, "
                        f"matrix records {expected[key]}"
                    )

        # Restore the fixture as the active source for the renders below.
        status, _, _ = call(
            api("source"),
            data=source_bytes,
            headers={
                "Content-Type": "application/octet-stream",
                "X-Source-Filename": fixture.name,
            },
        )
        if status != 200:
            raise AssertionError("could not reselect the fixture source")

        # -- a rejected request reports the renderer's own diagnostic ------

        status, body, _ = call(
            api("render"),
            data=b'{"formatVersion": 2, "composition": {"stages": [{"type": "nope"}]}}',
            headers={"Content-Type": "application/json"},
        )
        if status != 400:
            raise AssertionError(f"an invalid request rendered: {status}")
        failure = json_body(body)
        if failure["category"] != "invalid_configuration" or "location" not in failure:
            raise AssertionError(f"renderer diagnostic was not surfaced: {failure}")

        if sample_bits == 32:
            # -- render, and read the exact audio back ---------------------

            status, body, _ = call(
                api("render"),
                data=REVERB_REQUEST,
                headers={"Content-Type": "application/json"},
            )
            if status != 200:
                raise AssertionError(f"render failed: {body}")
            result = json_body(body)
            for key in ("sourceFilename", "sampleRate", "channels", "durationSeconds"):
                if key not in result:
                    raise AssertionError(f"result facts missing {key}: {result}")
            if result["sourceFilename"] != fixture.name:
                raise AssertionError(f"wrong source reported: {result}")

            status, served, headers = call(api("output.wav"))
            if status != 200:
                raise AssertionError(f"output.wav was not served: {status}")
            if headers.get("Content-Type") != "audio/wav":
                raise AssertionError(f"output.wav served as {headers}")
            if served[:4] != b"RIFF" or served[8:12] != b"WAVE":
                raise AssertionError("served output is not a RIFF/WAVE file")

            # The served bytes must be the renderer's own output, unmodified.
            # Reproduce the same render directly and compare.
            result_dir = workspace / "direct"
            if result_dir.exists():
                shutil.rmtree(result_dir)
            completed = subprocess.run(
                [
                    str(renderer),
                    "render",
                    "--input",
                    str(fixture),
                    "--config",
                    str(write_request(workspace, REVERB_REQUEST)),
                    "--output",
                    str(result_dir),
                ],
                capture_output=True,
                text=True,
            )
            if completed.returncode != 0:
                raise AssertionError(f"reference render failed: {completed.stderr}")
            if served != (result_dir / "output.wav").read_bytes():
                raise AssertionError(
                    "served audio is not byte-identical to the renderer's own output"
                )

            # -- the source is reused across renders -------------------------

            status, body, _ = call(
                api("render"),
                data=IDENTITY_REQUEST,
                headers={"Content-Type": "application/json"},
            )
            if status != 200:
                raise AssertionError(f"second render failed: {body}")
            if json_body(body)["sourceFilename"] != fixture.name:
                raise AssertionError("source was not reused across renders")

            status, identity_served, _ = call(api("output.wav"))
            if status != 200:
                raise AssertionError("second output was not served")
            if identity_served == served:
                raise AssertionError("a new render did not replace the previous result")

            # -- a failed render keeps the previous playable result -----------

            status, _, _ = call(
                api("render"),
                data=b"{ not json",
                headers={"Content-Type": "application/json"},
            )
            if status != 400:
                raise AssertionError("malformed request text rendered")
            status, after_failure, _ = call(api("output.wav"))
            if status != 200 or after_failure != identity_served:
                raise AssertionError("a failed render disturbed the previous result")
        else:
            # A double-precision renderer's output is a contract mismatch
            # the bench refuses rather than transcodes (issue #139): this
            # build can never produce a playable result at all, so none of
            # the playback assertions above apply under it.
            status, body, _ = call(
                api("render"),
                data=REVERB_REQUEST,
                headers={"Content-Type": "application/json"},
            )
            if status != 400 or json_body(body)["category"] != "unsupported_output":
                raise AssertionError(f"a float64 render was not refused: {status} {body}")
            status, _, _ = call(api("output.wav"))
            if status != 404:
                raise AssertionError("a refused float64 render still served output")

        # -- mutation methods are restricted -------------------------------

        status, _, _ = call(api("render"), data=b"{}", method="PUT")
        if status != 405:
            raise AssertionError(f"PUT was not refused: {status}")

        status, _, _ = call(f"{base}api/render", data=b"{}", method="PUT")
        if status != 403:
            raise AssertionError(f"an untokened PUT was not rejected: {status}")

        if not session_root.exists():
            raise AssertionError("session root vanished while serving")
    finally:
        interrupt(process)
        try:
            process.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()

    # -- the session is removed on shutdown --------------------------------

    if session_root.exists():
        raise AssertionError(f"session root survived shutdown: {session_root}")


def write_request(workspace, request_bytes):
    path = workspace / "request.json"
    path.write_bytes(request_bytes)
    return path


if __name__ == "__main__":
    main()
