#!/usr/bin/env python3

"""Research bench hardening contract (issue #139).

Exercises the bounds and safety behaviors that keep the bench a safe,
disposable local tool: size limits, single-flight rendering with no queue,
terminal interruption killing an active renderer, float64 output rejection,
and the rest of the HTTP contract (Host/Origin, methods, headers, text-safe
errors).

Drives the real bench server, like test_research_bench_cli.py, but against
tests/fixtures/fake_renderer.py rather than the real renderer: several
behaviors here (a render that stalls, one that reports float64 output, one
that fails on command) need to be driven on demand, in seconds, which the
real DSP renderer cannot promise.
"""

import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


def wait_for_banner(process, timeout=60.0):
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


def raw_request(host, port, method, path, headers, body):
    """An HTTP/1.1 request with headers we fully control -- in particular
    a Content-Length that need not match the bytes actually sent, so a
    size-limit boundary can be proven without allocating a boundary-sized
    body.
    """
    lines = [f"{method} {path} HTTP/1.1", f"Host: {host}:{port}"]
    for key, value in headers.items():
        lines.append(f"{key}: {value}")
    lines.append("Connection: close")
    head = ("\r\n".join(lines) + "\r\n\r\n").encode("ascii")
    with socket.create_connection((host, port), timeout=30) as sock:
        sock.sendall(head + body)
        sock.shutdown(socket.SHUT_WR)
        chunks = []
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
    response = b"".join(chunks)
    head_bytes, _, response_body = response.partition(b"\r\n\r\n")
    status_line, *header_lines = head_bytes.split(b"\r\n")
    status = int(status_line.split(b" ")[1])
    response_headers = {}
    for line in header_lines:
        if b":" in line:
            key, _, value = line.partition(b":")
            response_headers[key.decode("ascii").strip()] = value.decode("ascii").strip()
    return status, response_body, response_headers


def wait_for_file(path, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if path.exists():
            return
        time.sleep(0.02)
    raise AssertionError(f"{path} did not appear within {timeout}s")


def main():
    serve = Path(sys.argv[1])
    fake_renderer = Path(sys.argv[2])
    fixture = Path(sys.argv[3])
    workspace = Path(sys.argv[4])
    workspace.mkdir(parents=True, exist_ok=True)

    def launch():
        process = subprocess.Popen(
            [sys.executable, str(serve), "--renderer", str(fake_renderer), "--no-browser"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        url, session_root = wait_for_banner(process)
        base, _, query = url.partition("?")
        token = query.split("token=", 1)[1]
        host, port = base[len("http://"):].rstrip("/").split(":")
        return process, base, token, session_root, host, int(port)

    def stop(process, session_root):
        process.terminate()
        try:
            process.communicate(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
        if session_root.exists():
            raise AssertionError(f"session root survived shutdown: {session_root}")

    def select_source(base, token):
        status, body, _ = call(
            f"{base}api/source?token={token}",
            data=fixture.read_bytes(),
            headers={
                "Content-Type": "application/octet-stream",
                "X-Source-Filename": fixture.name,
            },
        )
        if status != 200:
            raise AssertionError(f"source selection failed: {body}")

    # === boundary sizes, without allocating a boundary-sized body =========

    process, base, token, session_root, host, port = launch()
    try:
        oversized_request = json.dumps({"formatVersion": 2}).encode("utf-8")
        status, body, _ = raw_request(
            host,
            port,
            "POST",
            f"/api/render?token={token}",
            {
                "Content-Type": "application/json",
                "Content-Length": str(1 * 1024 * 1024 + 1),
            },
            oversized_request,
        )
        if status != 400:
            raise AssertionError(f"an oversized request body was not refused: {status}")
        reason = json_body(body)["reason"]
        if "1 MiB" not in reason:
            raise AssertionError(f"oversized-request reason did not cite the limit: {reason}")

        status, body, _ = raw_request(
            host,
            port,
            "POST",
            f"/api/source?token={token}",
            {
                "Content-Type": "application/octet-stream",
                "Content-Length": str(1 * 1024 * 1024 * 1024 + 1),
            },
            b"tiny",
        )
        if status != 400:
            raise AssertionError(f"an oversized source body was not refused: {status}")
        reason = json_body(body)["reason"]
        if "1024 MiB" not in reason:
            raise AssertionError(f"oversized-source reason did not cite the limit: {reason}")

        # Exactly at the request limit is still refused for a different
        # reason (no source yet) -- proving the limit itself is not what
        # rejected it, i.e. the boundary is inclusive of the limit.
        at_limit = b'{"formatVersion": 2}' + b" " * (
            1 * 1024 * 1024 - len(b'{"formatVersion": 2}')
        )
        status, body, _ = call(
            f"{base}api/render?token={token}",
            data=at_limit,
            headers={"Content-Type": "application/json"},
        )
        if status != 400 or "MiB limit" in json_body(body)["reason"]:
            raise AssertionError(f"a request at exactly the limit was refused for size: {body}")
    finally:
        stop(process, session_root)

    # === Host, Origin, methods, headers, text-safe errors ==================

    process, base, token, session_root, host, port = launch()
    try:
        status, _, _ = call(
            f"{base}api/output.wav?token={token}", headers={"Host": "evil.example"}
        )
        if status != 403:
            raise AssertionError(f"a foreign Host was not rejected: {status}")

        for method in ("DELETE", "PATCH"):
            status, _, _ = call(f"{base}api/render?token={token}", data=b"{}", method=method)
            if status != 405:
                raise AssertionError(f"{method} was not refused: {status}")

        # An error response carries the same safety headers as a success,
        # and is JSON -- never markup an untrusted value could ride in on.
        status, body, headers = call(
            f"{base}api/render?token={token}",
            data=b"not json",
            headers={"Content-Type": "application/json"},
        )
        if status != 400:
            raise AssertionError(f"malformed request text rendered: {status}")
        if headers.get("X-Content-Type-Options") != "nosniff":
            raise AssertionError("error response missing nosniff header")
        if "Content-Security-Policy" not in headers:
            raise AssertionError("error response missing Content-Security-Policy")
        if headers.get("Content-Type") != "application/json":
            raise AssertionError(f"error response was not JSON: {headers}")
        json_body(body)  # must parse; a non-JSON body would raise here

        # A filename attempting traversal or markup injection is reduced
        # to a safe basename, never used as a path and never reflected as
        # anything but a plain, harmless string.
        status, body, _ = call(
            f"{base}api/source?token={token}",
            data=fixture.read_bytes(),
            headers={
                "Content-Type": "application/octet-stream",
                "X-Source-Filename": "../../<script>evil</script>.wav",
            },
        )
        if status != 200:
            raise AssertionError(f"a crafted filename was refused outright: {body}")
        served_name = json_body(body)["filename"]
        if any(character in served_name for character in "/\\<>"):
            raise AssertionError(f"filename was not sanitized: {served_name!r}")
    finally:
        stop(process, session_root)

    # === concurrency: rejected outright, never queued =======================

    process, base, token, session_root, host, port = launch()
    try:
        select_source(base, token)

        slow_request = json.dumps({"formatVersion": 2, "_sleepSeconds": 2.0}).encode("utf-8")
        results = {}

        def render_in_background():
            status, body, _ = call(
                f"{base}api/render?token={token}",
                data=slow_request,
                headers={"Content-Type": "application/json"},
            )
            results["status"] = status
            results["body"] = body

        import threading

        worker = threading.Thread(target=render_in_background)
        worker.start()
        try:
            wait_for_file(session_root / "renders" / "1" / "STARTED")

            started = time.time()
            status, body, _ = call(
                f"{base}api/render?token={token}",
                data=json.dumps({"formatVersion": 2}).encode("utf-8"),
                headers={"Content-Type": "application/json"},
            )
            elapsed = time.time() - started
            if status != 409:
                raise AssertionError(f"a concurrent render was not rejected: {status} {body}")
            if elapsed > 1.0:
                raise AssertionError(
                    f"a concurrent render was queued instead of rejected: {elapsed}s"
                )

            # Source selection is a state-mutating action too, and shares
            # the same one-at-a-time rule.
            status, body, _ = call(
                f"{base}api/source?token={token}",
                data=fixture.read_bytes(),
                headers={
                    "Content-Type": "application/octet-stream",
                    "X-Source-Filename": fixture.name,
                },
            )
            if status != 409:
                raise AssertionError(
                    f"source selection during a render was not rejected: {status} {body}"
                )
        finally:
            worker.join(timeout=10)
        if results.get("status") != 200:
            raise AssertionError(f"the in-flight render did not itself succeed: {results}")
    finally:
        stop(process, session_root)

    # === a valid long render is allowed to finish ===========================

    process, base, token, session_root, host, port = launch()
    try:
        select_source(base, token)
        status, body, _ = call(
            f"{base}api/render?token={token}",
            data=json.dumps({"formatVersion": 2, "_sleepSeconds": 3.0}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
        )
        if status != 200:
            raise AssertionError(f"a slow but valid render was cut off: {status} {body}")
    finally:
        stop(process, session_root)

    # === float64 output is rejected, not transcoded, previous kept =========

    process, base, token, session_root, host, port = launch()
    try:
        select_source(base, token)

        status, body, _ = call(
            f"{base}api/render?token={token}",
            data=json.dumps({"formatVersion": 2}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
        )
        if status != 200:
            raise AssertionError(f"the baseline float32 render failed: {body}")
        status, good_output, _ = call(f"{base}api/output.wav?token={token}")
        if status != 200:
            raise AssertionError("baseline output was not served")

        status, body, _ = call(
            f"{base}api/render?token={token}",
            data=json.dumps(
                {"formatVersion": 2, "_samplePrecision": "float64"}
            ).encode("utf-8"),
            headers={"Content-Type": "application/json"},
        )
        if status != 400:
            raise AssertionError(f"a float64 result was not rejected: {status} {body}")
        failure = json_body(body)
        if "float64" not in failure["reason"]:
            raise AssertionError(f"float64 rejection did not name the problem: {failure}")

        status, after, _ = call(f"{base}api/output.wav?token={token}")
        if status != 200 or after != good_output:
            raise AssertionError("a rejected float64 render disturbed the previous result")
    finally:
        stop(process, session_root)

    # === terminal interruption kills an active renderer =====================

    process, base, token, session_root, host, port = launch()
    try:
        select_source(base, token)

        slow_request = json.dumps({"formatVersion": 2, "_sleepSeconds": 10.0}).encode("utf-8")
        import threading

        worker = threading.Thread(
            target=lambda: call(
                f"{base}api/render?token={token}",
                data=slow_request,
                headers={"Content-Type": "application/json"},
            )
        )
        worker.start()

        pid_path = session_root / "renders" / "1" / "PID"
        wait_for_file(pid_path)
        renderer_pid = int(pid_path.read_text().strip())

        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            raise AssertionError("the launcher did not shut down promptly on interruption")

        try:
            os.kill(renderer_pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError("the active renderer survived terminal interruption")

        worker.join(timeout=15)
    finally:
        if process.poll() is None:
            process.terminate()
            process.communicate(timeout=30)
        if session_root.exists():
            raise AssertionError(f"session root survived interruption: {session_root}")


if __name__ == "__main__":
    main()
