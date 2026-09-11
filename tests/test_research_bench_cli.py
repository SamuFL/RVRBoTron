#!/usr/bin/env python3

"""Research bench contract: the complete local HTTP workflow (issue #137).

Drives the real bench server against the real renderer and a committed WAV
fixture -- select a source, render an editor string, read the facts back,
and fetch the served audio -- rather than testing handler internals.
"""

import json
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


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
    request = urllib.request.Request(
        url, data=data, headers=headers or {}, method=method
    )
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

    process = subprocess.Popen(
        [
            sys.executable,
            str(serve),
            "--renderer",
            str(renderer),
            "--no-browser",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        url, session_root = wait_for_banner(process)
        base, _, query = url.partition("?")
        token = query.split("token=", 1)[1]

        def api(route):
            return "{}api/{}?token={}".format(base, route, token)

        # -- authorization ------------------------------------------------

        status, _, _ = call("{}api/output.wav?token=wrong".format(base))
        if status != 403:
            raise AssertionError("a wrong token was not rejected: {}".format(status))

        status, _, _ = call("{}api/output.wav".format(base))
        if status != 403:
            raise AssertionError("a missing token was not rejected: {}".format(status))

        status, _, _ = call(
            api("output.wav"), headers={"Origin": "http://evil.example"}
        )
        if status != 403:
            raise AssertionError("a foreign Origin was not rejected: {}".format(status))

        # Nothing has been rendered yet.
        status, _, _ = call(api("output.wav"))
        if status != 404:
            raise AssertionError(
                "output was served before any render: {}".format(status)
            )

        # -- the page -----------------------------------------------------

        status, page, headers = call(url)
        if status != 200:
            raise AssertionError("page did not load: {}".format(status))
        text = page.decode("utf-8")
        if "<title>RVRBoTron Research Bench</title>" not in text:
            raise AssertionError("page is not titled RVRBoTron Research Bench")
        if headers.get("X-Content-Type-Options") != "nosniff":
            raise AssertionError("missing nosniff header")
        if "Content-Security-Policy" not in headers:
            raise AssertionError("missing content security policy")
        if any(key.lower().startswith("access-control-allow") for key in headers):
            raise AssertionError("bench granted a CORS permission")

        # -- rendering needs a source first --------------------------------

        status, body, _ = call(
            api("render"),
            data=REVERB_REQUEST,
            headers={"Content-Type": "application/json"},
        )
        if status != 400 or "Audition source" not in json_body(body)["reason"]:
            raise AssertionError(
                "rendering without a source was not refused: {}".format(body)
            )

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
            raise AssertionError("source was refused: {}".format(body))
        facts = json_body(body)
        if facts["filename"] != fixture.name or facts["sampleRate"] != 48000:
            raise AssertionError("unexpected source facts: {}".format(facts))

        # A file the renderer's WAV contract does not accept is refused
        # with its own vocabulary, not converted.
        status, body, _ = call(
            api("source"),
            data=b"not a wav at all",
            headers={"Content-Type": "application/octet-stream"},
        )
        if status != 400 or json_body(body)["category"] != "unsupported_audio":
            raise AssertionError("a non-WAV source was not refused: {}".format(body))

        # -- a rejected request reports the renderer's own diagnostic ------

        status, body, _ = call(
            api("render"),
            data=b'{"formatVersion": 2, "composition": {"stages": [{"type": "nope"}]}}',
            headers={"Content-Type": "application/json"},
        )
        if status != 400:
            raise AssertionError("an invalid request rendered: {}".format(status))
        failure = json_body(body)
        if failure["category"] != "invalid_configuration" or "location" not in failure:
            raise AssertionError(
                "renderer diagnostic was not surfaced: {}".format(failure)
            )

        # -- render, and read the exact audio back -------------------------

        status, body, _ = call(
            api("render"),
            data=REVERB_REQUEST,
            headers={"Content-Type": "application/json"},
        )
        if status != 200:
            raise AssertionError("render failed: {}".format(body))
        result = json_body(body)
        for key in ("sourceFilename", "sampleRate", "channels", "durationSeconds"):
            if key not in result:
                raise AssertionError("result facts missing {}: {}".format(key, result))
        if result["sourceFilename"] != fixture.name:
            raise AssertionError("wrong source reported: {}".format(result))

        status, served, headers = call(api("output.wav"))
        if status != 200:
            raise AssertionError("output.wav was not served: {}".format(status))
        if headers.get("Content-Type") != "audio/wav":
            raise AssertionError("output.wav served as {}".format(headers))
        if served[:4] != b"RIFF" or served[8:12] != b"WAVE":
            raise AssertionError("served output is not a RIFF/WAVE file")

        # The served bytes must be the renderer's own output, unmodified.
        # Reproduce the same render directly and compare.
        reference = Path(sys.argv[4])
        reference.mkdir(parents=True, exist_ok=True)
        request_path = reference / "request.json"
        request_path.write_bytes(REVERB_REQUEST)
        result_dir = reference / "direct"
        if result_dir.exists():
            import shutil

            shutil.rmtree(result_dir)
        completed = subprocess.run(
            [
                str(renderer),
                "render",
                "--input",
                str(fixture),
                "--config",
                str(request_path),
                "--output",
                str(result_dir),
            ],
            capture_output=True,
            text=True,
        )
        if completed.returncode != 0:
            raise AssertionError("reference render failed: {}".format(completed.stderr))
        if served != (result_dir / "output.wav").read_bytes():
            raise AssertionError(
                "served audio is not byte-identical to the renderer's own output"
            )

        # -- the source is reused across renders ---------------------------

        status, body, _ = call(
            api("render"),
            data=IDENTITY_REQUEST,
            headers={"Content-Type": "application/json"},
        )
        if status != 200:
            raise AssertionError("second render failed: {}".format(body))
        if json_body(body)["sourceFilename"] != fixture.name:
            raise AssertionError("source was not reused across renders")

        status, identity_served, _ = call(api("output.wav"))
        if status != 200:
            raise AssertionError("second output was not served")
        if identity_served == served:
            raise AssertionError("a new render did not replace the previous result")

        # -- a failed render keeps the previous playable result ------------

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

        # -- mutation methods are restricted -------------------------------

        status, _, _ = call(api("render"), data=b"{}", method="PUT")
        if status != 405:
            raise AssertionError("PUT was not refused: {}".format(status))

        if not session_root.exists():
            raise AssertionError("session root vanished while serving")
    finally:
        process.terminate()
        try:
            process.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()

    # -- the session is removed on shutdown --------------------------------

    if session_root.exists():
        raise AssertionError(
            "session root survived shutdown: {}".format(session_root)
        )


if __name__ == "__main__":
    main()
