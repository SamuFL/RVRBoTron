#!/usr/bin/env python3

"""RVRBoTron Research Bench: a local, disposable listening loop.

Pick an Audition source, edit a Requested configuration, render it through
the unchanged C++ renderer, and play the result -- without writing files or
constructing commands by hand (issue #137).

This is a research instrument, not the eventual plugin or product GUI. It
binds loopback only, carries a capability token in its own URL, and keeps
exactly one temporary session that it removes on shutdown.

Python 3 standard library only: no web framework, no package manager, no
runtime dependency beyond what rendering already needs.
"""

import argparse
import atexit
import json
import os
import re
import secrets
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote, urlparse

HOST = "127.0.0.1"
STATIC_DIR = Path(__file__).resolve().parent / "static"

# The page's own script and style are served as same-origin files so the
# Content-Security-Policy below can stay at 'self' with no inline exception.
# Each carries the capability token, like every other request.
TOKEN_PLACEHOLDER = "__BENCH_TOKEN__"
STATIC_FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/bench.css": ("bench.css", "text/css; charset=utf-8"),
    "/bench.js": ("bench.js", "text/javascript; charset=utf-8"),
    "/vendor/ace/ace.js": ("vendor/ace/ace.js", "text/javascript; charset=utf-8"),
    "/vendor/ace/mode-json.js": (
        "vendor/ace/mode-json.js",
        "text/javascript; charset=utf-8",
    ),
    "/vendor/ace/theme-tomorrow_night.js": (
        "vendor/ace/theme-tomorrow_night.js",
        "text/javascript; charset=utf-8",
    ),
}

# style-src carries 'unsafe-inline': Ace injects its base, scrollbar, and
# theme CSS as inline <style> elements at runtime (ace/lib/dom's own
# importCssString), not through a <link> the integration controls --
# confirmed the sole source of every blocked style-src-elem violation with a
# headless-browser probe before this was added. Nothing the bench ever
# shows (filenames, request text, renderer diagnostics) reaches a style
# context, so this widens no attack surface the bench has; script-src stays
# 'self' with no inline exception. img-src allows data: for the two
# indentation-guide glyphs the fixed Ace theme embeds. worker-src is spelled
# out (default-src 'none' already
# covers it) because Ace workers are a deliberate omission: see bench.js,
# which also disables useWorker explicitly rather than relying on this alone.
CONTENT_SECURITY_POLICY = (
    "default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
    "connect-src 'self'; media-src 'self'; img-src 'self' data:; "
    "worker-src 'none'; form-action 'none'; base-uri 'none'; "
    "frame-ancestors 'none'"
)

# The renderer's own WAV contract (src/io/WavStream.cpp): mono or stereo
# RIFF/WAVE, PCM16/24/32 or IEEE float32/64. The bench converts nothing, so
# it accepts exactly this and rejects the rest with the same vocabulary.
WAV_FORMAT_PCM = 1
WAV_FORMAT_IEEE_FLOAT = 3

# A crude guard so one stray selection cannot exhaust memory or fill the
# session root. Issue #139 owns the researcher-facing limit contract.
MAX_REQUEST_BYTES = 1 * 1024 * 1024
MAX_SOURCE_BYTES = 1 * 1024 * 1024 * 1024

# Long enough for a cold start, short enough that a binary which never
# answers is reported rather than waited on.
PROBE_TIMEOUT_SECONDS = 30


class BenchError(Exception):
    """A failure to report to the page as text, never as markup."""

    def __init__(
        self,
        reason,
        category="bench_error",
        location=None,
        status=HTTPStatus.BAD_REQUEST,
    ):
        super().__init__(reason)
        self.reason = reason
        self.category = category
        self.location = location
        self.status = status

    def payload(self):
        body = {"category": self.category, "reason": self.reason}
        if self.location is not None:
            body["location"] = self.location
        return body


def default_renderer_path(repository_root):
    """The default float32 build, where `cmake --preset default` puts it."""
    name = "rvrbotron.exe" if os.name == "nt" else "rvrbotron"
    return repository_root / "build" / "default" / name


def probe_renderer(path, arguments):
    """Run one startup probe, reporting every way it can fail actionably.

    A binary that cannot be started, or that never answers, must produce a
    message the researcher can act on rather than a traceback.
    """
    try:
        return subprocess.run(
            [str(path)] + list(arguments),
            capture_output=True,
            text=True,
            timeout=PROBE_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired:
        raise BenchError(
            f"{path} did not answer within {PROBE_TIMEOUT_SECONDS} seconds, so it "
            f"is not a usable renderer. Pass --renderer <path> to the real binary."
        )
    except OSError as error:
        raise BenchError(f"could not run {path}: {error}")


def verify_renderer(path):
    """Fail at startup, actionably, rather than at the first render.

    Probes the binary with no arguments: the real CLI answers with its own
    usage line, which distinguishes a working renderer from a missing file,
    a non-executable one, or some unrelated binary at that path. A second
    probe exercises `--error-format json`, which the bench depends on to
    report configuration errors, so an older build fails here rather than
    at the researcher's first Render.
    """
    if not path.exists():
        raise BenchError(
            f"no renderer at {path}. Build it with `cmake --preset default && "
            f"cmake --build --preset default`, or pass --renderer <path>."
        )
    if not path.is_file() or not os.access(str(path), os.X_OK):
        raise BenchError(f"{path} is not an executable file.")
    probe = probe_renderer(path, [])
    usage = probe.stderr + probe.stdout
    if "usage: rvrbotron" not in usage:
        raise BenchError(
            f"{path} does not look like the rvrbotron renderer (no usage line). "
            f"Pass --renderer <path> to the real binary."
        )
    diagnostic = probe_renderer(path, ["render", "--error-format", "json"])
    if renderer_failure(diagnostic.stderr).category != "invalid_arguments":
        raise BenchError(
            f"{path} does not report errors as JSON, which the bench needs to "
            f"show configuration failures. Rebuild it, or pass --renderer "
            f"<path> to a current build."
        )


def inspect_wav(data):
    """Read a WAV header from the bytes the browser supplied.

    Mirrors the renderer's own header contract (src/io/WavStream.cpp) so a
    file accepted here is a file the renderer will accept, rather than one
    that is refused later with a different message. Returns the Audition
    source facts, or raises BenchError with the renderer's own vocabulary.
    """
    file_size = len(data)
    if file_size < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise BenchError("malformed input WAV", category="unsupported_audio")

    truncated = BenchError("truncated input WAV", category="unsupported_audio")
    malformed = BenchError("malformed input WAV", category="unsupported_audio")

    (riff_size,) = struct.unpack_from("<I", data, 4)
    riff_end = riff_size + 8
    if riff_end > file_size:
        raise truncated
    if riff_end < 12:
        raise malformed

    format_tag = channels = sample_rate = byte_rate = None
    block_align = bits = data_size = None
    offset = 12
    while offset + 8 <= riff_end:
        chunk_id = data[offset : offset + 4]
        (chunk_size,) = struct.unpack_from("<I", data, offset + 4)
        chunk_data = offset + 8
        chunk_end = chunk_data + chunk_size
        padded_end = chunk_end + (chunk_size % 2)
        if chunk_end > riff_end:
            raise malformed
        if padded_end > file_size:
            raise truncated
        if chunk_id == b"fmt ":
            if chunk_size < 16:
                raise malformed
            format_tag, channels, sample_rate, byte_rate, block_align, bits = (
                struct.unpack_from("<HHIIHH", data, chunk_data)
            )
        elif chunk_id == b"data":
            data_size = chunk_size
        offset = padded_end

    if format_tag is None or data_size is None or offset != riff_end:
        raise malformed
    if channels not in (1, 2):
        raise BenchError(
            "WAV channel count must be mono or stereo",
            category="unsupported_audio",
        )
    supported = (format_tag == WAV_FORMAT_PCM and bits in (16, 24, 32)) or (
        format_tag == WAV_FORMAT_IEEE_FLOAT and bits in (32, 64)
    )
    if not supported:
        raise BenchError("unsupported WAV encoding", category="unsupported_audio")

    expected_block_align = channels * (bits // 8)
    if (
        sample_rate == 0
        or block_align != expected_block_align
        or byte_rate != sample_rate * expected_block_align
        or data_size % expected_block_align != 0
    ):
        raise malformed

    frames = data_size // expected_block_align
    return {
        "channels": channels,
        "sampleRate": sample_rate,
        "frames": frames,
        "durationSeconds": round(frames / sample_rate, 3),
    }


def safe_display_name(raw):
    """A filename is untrusted text from the browser.

    Kept only for display and for naming one file inside the session root,
    so it is reduced to a basename of safe characters. Never used to reach
    outside the session, and never interpreted as a path by any endpoint.
    """
    if not raw:
        return "source.wav"
    base = os.path.basename(raw.replace("\\", "/"))
    cleaned = re.sub(r"[^A-Za-z0-9._-]", "_", base)
    return cleaned[:128] or "source.wav"


class Session:
    """One server process owns one temporary session root.

    Everything the bench writes -- the Audition source and the newest
    Render Result -- lives beneath it, and it is removed on shutdown.
    """

    def __init__(self, renderer):
        self.renderer = renderer
        self.root = Path(tempfile.mkdtemp(prefix="rvrbotron-bench-"))
        self.lock = threading.Lock()
        self.source_path = None
        self.source_name = None
        self.result_dir = None
        self.render_count = 0
        # Guards active_process and shutting_down, which a
        # terminal-interruption handler on the main thread reads and sets
        # while a renderer call (on a worker thread) still owns the
        # process -- separate from the lock above, which only ever one
        # thread holds at a time. Spawning the child and publishing it to
        # active_process happen inside the same critical section as the
        # shutting_down check (issue #139 review) so an interruption that
        # lands between Popen() returning and active_process being set
        # cannot slip through and leave the child orphaned: either it is
        # published before shutdown starts checking, or shutdown has
        # already refused to let a new child start.
        self.process_lock = threading.Lock()
        self.active_process = None
        self.shutting_down = False

    def resolve_within(self, path):
        """Guard every temporary path against escaping the session root."""
        candidate = Path(path).resolve()
        root = self.root.resolve()
        if candidate != root and root not in candidate.parents:
            raise BenchError("path escapes the session root")
        return candidate

    def store_source(self, data, display_name):
        facts = inspect_wav(data)
        name = safe_display_name(display_name)
        source_dir = self.resolve_within(self.root / "source")
        if source_dir.exists():
            shutil.rmtree(source_dir, ignore_errors=True)
        source_dir.mkdir(parents=True)
        path = self.resolve_within(source_dir / name)
        path.write_bytes(data)
        self.source_path = path
        self.source_name = name
        return dict(facts, filename=name)

    def render(self, request_text):
        if self.source_path is None:
            raise BenchError("choose an Audition source first")

        self.render_count += 1
        render_dir = self.resolve_within(self.root / "renders" / str(self.render_count))
        render_dir.parent.mkdir(parents=True, exist_ok=True)

        # The editor's text is the request. It is written through byte for
        # byte -- never parsed and re-serialized here -- so what the
        # renderer validates is exactly what is on screen.
        request_path = self.resolve_within(render_dir.parent / "request.json")
        request_path.write_bytes(request_text)

        try:
            completed = self._run_renderer(
                [
                    str(self.renderer),
                    "render",
                    "--input",
                    str(self.source_path),
                    "--config",
                    str(request_path),
                    "--output",
                    str(render_dir),
                    "--error-format",
                    "json",
                ]
            )
        except BenchError:
            # Shutdown won the race with this render's own start (issue
            # #139 review): no child was spawned, so there is nothing to
            # preserve here either.
            shutil.rmtree(render_dir, ignore_errors=True)
            raise
        if completed.returncode != 0:
            # A failed render leaves the previous playable result alone.
            shutil.rmtree(render_dir, ignore_errors=True)
            raise renderer_failure(completed.stderr)

        metadata = json.loads((render_dir / "render.json").read_text())
        # The bench plays exactly what the renderer wrote (issue #128): a
        # float64 build's output is a contract mismatch, not something to
        # transcode. Caught here, from the renderer's own metadata, rather
        # than by inspecting the WAV bytes it just wrote.
        if metadata.get("samplePrecision") == "float64":
            shutil.rmtree(render_dir, ignore_errors=True)
            raise BenchError(
                "the renderer produced float64 output, which the bench "
                "does not play or transcode. Point --renderer at the "
                "repository's default float32 build.",
                category="unsupported_output",
            )
        previous = self.result_dir
        self.result_dir = render_dir
        if previous is not None and previous != render_dir:
            shutil.rmtree(previous, ignore_errors=True)
        frames = metadata["frames"]
        sample_rate = metadata["sampleRate"]
        return {
            "sourceFilename": self.source_name,
            "sampleRate": sample_rate,
            "channels": metadata["channels"],
            "frames": frames,
            "durationSeconds": round(frames / sample_rate, 3),
        }

    def _run_renderer(self, arguments):
        """subprocess.run, but with the live process reachable for a kill.

        Exposing the Popen object between start and completion is the only
        difference from subprocess.run: terminate_active_render (issue
        #139) needs it to end a renderer that is still running when the
        launcher is interrupted, rather than leaving it as an orphan.

        Spawning and publishing the child happen inside the same
        process_lock critical section terminate_active_render uses to read
        and refuse further spawns: an interruption arriving mid-spawn
        either lands before this runs (shutting_down is already set, so
        nothing is spawned) or after (active_process is already published,
        so it is found and killed) -- never in a gap where neither is true.
        """
        with self.process_lock:
            if self.shutting_down:
                raise BenchError("the bench is shutting down", category="busy")
            process = subprocess.Popen(
                arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
            )
            self.active_process = process
        try:
            stdout, stderr = process.communicate()
        finally:
            with self.process_lock:
                self.active_process = None
        return subprocess.CompletedProcess(arguments, process.returncode, stdout, stderr)

    def terminate_active_render(self):
        """End whatever renderer child is running, and refuse any further
        one from starting; a no-op beyond that if none is running.

        Terminal interruption must not leave a renderer process behind
        (issue #139): there is no queue or timeout to wait it out instead.
        """
        with self.process_lock:
            self.shutting_down = True
            process = self.active_process
        if process is None or process.poll() is not None:
            return
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()

    def read_output(self):
        """The newest result's bytes, read while the lock is held.

        A render completing between finding the file and opening it would
        otherwise delete the previous result underneath this reader.
        """
        with self.lock:
            if self.result_dir is None:
                return None
            path = self.resolve_within(self.result_dir / "output.wav")
            return path.read_bytes() if path.exists() else None

    def close(self):
        self.terminate_active_render()
        shutil.rmtree(self.root, ignore_errors=True)


def renderer_failure(stderr_text):
    """Turn the renderer's own diagnostic into a BenchError.

    `--error-format json` gives category, reason, and (for configuration
    errors) the exact location, which the page shows verbatim as text.
    """
    text = (stderr_text or "").strip()
    for line in reversed(text.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                parsed = json.loads(line)
            except ValueError:
                continue
            return BenchError(
                parsed.get("reason", "render failed"),
                category=parsed.get("category", "render_failed"),
                location=parsed.get("location"),
            )
    return BenchError(text or "render failed", category="render_failed")


def load_static_files(token):
    """Read the page once, with the token woven into its asset links."""
    served = {}
    for route, (name, content_type) in STATIC_FILES.items():
        text = (STATIC_DIR / name).read_text(encoding="utf-8")
        text = text.replace(TOKEN_PLACEHOLDER, quote(token, safe=""))
        served[route] = (text.encode("utf-8"), content_type)
    return served


def build_handler(session, token, port, static_files):
    allowed_hosts = {f"{HOST}:{port}", f"localhost:{port}"}
    allowed_origins = {f"http://{HOST}:{port}", f"http://localhost:{port}"}

    class Handler(BaseHTTPRequestHandler):
        server_version = "RVRBoTronBench"
        sys_version = ""
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):  # keep the terminal for the researcher
            pass

        # -- helpers ---------------------------------------------------

        def _send(self, status, body, content_type, extra=None):
            if isinstance(body, str):
                body = body.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("Cache-Control", "no-store")
            # No Access-Control-Allow-* header is ever sent: another origin
            # must not be able to read this server's responses.
            self.send_header("Content-Security-Policy", CONTENT_SECURITY_POLICY)
            for key, value in (extra or {}).items():
                self.send_header(key, value)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def _send_json(self, status, payload):
            self._send(status, json.dumps(payload), "application/json")

        def _reject(self, status, reason):
            self._send_json(status, {"reason": reason})

        def _authorized(self):
            """Loopback, same-origin, and capability-token checks.

            Every request -- reads included -- must carry the token, so a
            URL that was never shared cannot be driven by another page.
            """
            if self.headers.get("Host") not in allowed_hosts:
                return False
            request_origin = self.headers.get("Origin")
            if request_origin is not None and request_origin not in allowed_origins:
                return False
            supplied = parse_qs(urlparse(self.path).query).get("token", [""])[0]
            return secrets.compare_digest(supplied, token)

        def _body(self, limit, description):
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                raise BenchError("missing or invalid Content-Length")
            # A negative length would slip past the limit below and turn
            # rfile.read into a read-to-EOF that blocks this handler.
            if length < 0:
                raise BenchError("Content-Length cannot be negative")
            if length > limit:
                raise BenchError(
                    f"{description} is larger than the "
                    f"{limit // (1024 * 1024)} MiB limit"
                )
            return self.rfile.read(length) if length else b""

        # -- routes ----------------------------------------------------

        def do_GET(self):
            if not self._authorized():
                self._reject(HTTPStatus.FORBIDDEN, "not authorized")
                return
            route = urlparse(self.path).path
            if route in static_files:
                body, content_type = static_files[route]
                self._send(HTTPStatus.OK, body, content_type)
            elif route == "/api/output.wav":
                audio = session.read_output()
                if audio is None:
                    self._reject(HTTPStatus.NOT_FOUND, "nothing rendered yet")
                    return
                self._send(
                    HTTPStatus.OK,
                    audio,
                    "audio/wav",
                    {"Content-Disposition": 'attachment; filename="output.wav"'},
                )
            else:
                self._reject(HTTPStatus.NOT_FOUND, "no such route")

        def _exclusive(self, action):
            """Run one state-mutating action at a time; reject, never queue.

            Issue #139 rules out a render queue, parallel execution, and
            waiting out a timeout: a Source or Render request that arrives
            while another is still in flight is refused immediately,
            matching the Render button the page disables meanwhile.
            "Immediately" means before reading the new request's body, not
            just before acting on it -- callers must defer their _body()
            read into `action` itself, or a concurrent request would sit
            uploading up to a gigabyte before ever being told no.
            """
            if not session.lock.acquire(blocking=False):
                raise BenchError(
                    "another action is already running; wait for it to finish",
                    category="busy",
                    status=HTTPStatus.CONFLICT,
                )
            try:
                return action()
            finally:
                session.lock.release()

        def do_POST(self):
            if not self._authorized():
                self._reject(HTTPStatus.FORBIDDEN, "not authorized")
                return
            route = urlparse(self.path).path
            try:
                if route == "/api/source":
                    facts = self._exclusive(
                        lambda: session.store_source(
                            self._body(MAX_SOURCE_BYTES, "the Audition source"),
                            self.headers.get("X-Source-Filename", ""),
                        )
                    )
                    self._send_json(HTTPStatus.OK, facts)
                elif route == "/api/render":
                    facts = self._exclusive(
                        lambda: session.render(
                            self._body(MAX_REQUEST_BYTES, "the request")
                        )
                    )
                    self._send_json(HTTPStatus.OK, facts)
                else:
                    self._reject(HTTPStatus.NOT_FOUND, "no such route")
            except BenchError as error:
                self._send_json(error.status, error.payload())

        def _reject_method(self):
            """The bench has no route that answers PUT, DELETE, or PATCH.

            Authorization is still checked first, so an unauthenticated
            caller learns nothing the other routes would not tell it.
            """
            if not self._authorized():
                self._reject(HTTPStatus.FORBIDDEN, "not authorized")
                return
            self._reject(HTTPStatus.METHOD_NOT_ALLOWED, "method not allowed")

        do_PUT = _reject_method
        do_DELETE = _reject_method
        do_PATCH = _reject_method

    return Handler


def parse_arguments(argv):
    parser = argparse.ArgumentParser(
        description="Launch the RVRBoTron Research Bench on loopback."
    )
    parser.add_argument(
        "--renderer",
        type=Path,
        default=None,
        help="path to the rvrbotron renderer (default: build/default/rvrbotron)",
    )
    parser.add_argument(
        "--no-browser",
        action="store_true",
        help="print the URL instead of opening a browser",
    )
    return parser.parse_args(argv)


def main(argv=None):
    arguments = parse_arguments(argv or sys.argv[1:])
    repository_root = Path(__file__).resolve().parents[2]
    renderer = (arguments.renderer or default_renderer_path(repository_root)).resolve()

    try:
        verify_renderer(renderer)
    except BenchError as error:
        sys.stderr.write(f"research bench: {error.reason}\n")
        return 1

    token = secrets.token_urlsafe(32)
    session = Session(renderer)
    atexit.register(session.close)

    static_files = load_static_files(token)
    # Bind first: the handler validates Host and Origin against the actual
    # port, which the OS only assigns once bound.
    server = ThreadingHTTPServer((HOST, 0), BaseHTTPRequestHandler)
    port = server.server_address[1]
    server.RequestHandlerClass = build_handler(session, token, port, static_files)

    url = f"http://{HOST}:{port}/?token={token}"
    sys.stdout.write("RVRBoTron Research Bench\n")
    sys.stdout.write(f"  renderer: {renderer}\n")
    sys.stdout.write(f"  session:  {session.root}\n")
    sys.stdout.write(f"  open:     {url}\n")
    sys.stdout.write("Press Ctrl+C to stop; the session is removed on exit.\n")
    sys.stdout.flush()

    if not arguments.no_browser:
        webbrowser.open(url)

    def shutdown(*_):
        # Kill any renderer in flight before waiting on it: server.shutdown
        # only stops accepting new requests, and would otherwise sit behind
        # a worker thread that is itself sitting in process.communicate().
        session.terminate_active_render()
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)
    # Windows has no deliverable SIGTERM -- os.kill/Popen.terminate there
    # is an unconditional TerminateProcess that runs no Python handler at
    # all -- so a controlling process asking this launcher to shut down
    # gracefully (rather than a user's own console Ctrl+C, which already
    # arrives as SIGINT on every platform) has only CTRL_BREAK_EVENT to
    # send, which Python surfaces as SIGBREAK.
    if hasattr(signal, "SIGBREAK"):
        signal.signal(signal.SIGBREAK, shutdown)
    try:
        server.serve_forever()
    finally:
        server.server_close()
        session.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
