# Run the Research bench

The fastest way to hear a Composition: pick a source, edit a request, press
Render, listen. No files to create, no commands to assemble.

The bench is a **research instrument, not a plugin**. It drives the same
command-line renderer you built, plays its exact `output.wav`, and throws
everything away when you stop it.

*Every field you can put in a request — values, defaults, shapes — lives in
[Configure the Composition](configure-the-composition.md). This guide covers
the bench itself.*

---

## Before you start

```bash
cmake --preset default && cmake --build --preset default
git lfs install && git lfs pull      # curated listening samples
```

The bench needs the **float32** renderer. A `double` build starts fine but
refuses at the first render — see [What the bench will not
do](#what-the-bench-will-not-do).

---

## Launch it

From the repository root:

```bash
python3 tools/research_bench/serve.py
```

```text
RVRBoTron Research Bench
  renderer: /Users/you/RVRBoTron/build/default/rvrbotron
  session:  /var/folders/7h/…/T/rvrbotron-bench-0w6s8ahd
  open:     http://127.0.0.1:49208/?token=j4m_T69lhPAQdigR97NgbZ67gc5d_IiAHkg8XXM0IP4
Press Ctrl+C to stop; the session is removed on exit.
```

Your browser opens on that URL. Four things worth reading in that banner:

- **`renderer`** — the binary every Render actually invokes. If this is not
  the build you meant to audition, stop and relaunch with `--renderer`.
- **`session`** — the one temporary directory this run owns. Everything the
  bench writes lives there, and it is removed on exit.
- **`open`** — loopback only, carrying a capability token for this run alone.
  The token changes every launch; an old URL is dead.
- **Ctrl+C** — the only clean way to stop. It shuts down the launcher and any
  renderer still running, then removes the session.

| Flag | Use it when |
| --- | --- |
| `--renderer <path>` | Auditioning a build other than `build/default/rvrbotron` |
| `--no-browser` | You want the URL printed, not opened — remote shells, or a browser you pick yourself |

---

## When it will not start

The launcher checks the renderer before it binds a port, so a setup problem
arrives as one `research bench:` line on stderr and a non-zero exit, rather
than as a page that loads and then cannot render.

**No renderer at that path.** The path is always reported absolute, whatever
you typed:

```text
research bench: no renderer at /Users/you/RVRBoTron/build/default/rvrbotron.
Build it with `cmake --preset default && cmake --build --preset default`, or
pass --renderer <path>.
```

Build it, or point `--renderer` at the build you already have.

**Something is there, but it cannot be executed.**

```text
research bench: /Users/you/RVRBoTron/README.md is not an executable file.
```

You named a directory or an ordinary file. Check the path.

**Something executable is there, but it is not the renderer.**

```text
research bench: /bin/echo does not look like the rvrbotron renderer (no usage
line). Pass --renderer <path> to the real binary.
```

**It is the renderer, but too old to report JSON diagnostics.**

```text
research bench: <path> does not report errors as JSON, which the bench needs
to show configuration failures. Rebuild it, or pass --renderer <path> to a
current build.
```

The bench surfaces configuration failures by parsing `--error-format json`,
so a build without it could only ever report that something went wrong.

**It is there, but it never answers.** The startup probe gives up rather than
hanging the launch:

```text
research bench: <path> did not answer within 30 seconds, so it is not a
usable renderer. Pass --renderer <path> to the real binary.
```

**The operating system refused to start it** — wrong architecture, missing
execute permission, a truncated build:

```text
research bench: could not run <path>: <error>
```

The `<error>` is the operating system's own. Run the binary by itself from
the same shell — it should print its `usage: rvrbotron` line, which is
exactly what the launcher probes for — and rebuild if it does not.

---

## Choose an Audition source

Pick any WAV with **Audition source**. The browser sends the bytes; the
server is never given a filesystem path. The label beside the picker names
what the server actually holds:

```text
PianoDry.wav — 2 ch, 48000 Hz, 20 s
```

`samples/listening/` holds curated dry material once you have pulled LFS.

The bench accepts exactly what the renderer accepts. It reads the WAV header
itself, so a file the renderer would refuse is refused here at selection
rather than later at Render — but the contract is the renderer's own, and so
is the `unsupported_audio` vocabulary it is refused in (see
[diagnostics](render-and-analyze-evidence.md#diagnostics)). There is no second
contract to learn: if `--input` takes it, the bench takes it.

Refusals name the reason:

```text
unsupported_audio: WAV channel count must be mono or stereo
```

```text
unsupported_audio: unsupported WAV encoding
```

```text
unsupported_audio: truncated input WAV
```

A refused selection never replaces what Render will use — the status panel
adds `Still using <name>` so you know which source is still active.

---

## Edit the request

The editor starts on **Simple** and holds one string: the request text. That
text — not a parsed copy of it — is what gets rendered and what
`Download request.json` writes.

### The four templates

Each one is an ordinary valid request, rendered through the real renderer in
the test suite so it cannot drift into a broken example. None of them is a
product preset or a recommended sound.

| Template | What it shows |
| --- | --- |
| **Simple** | A Diffuser feeding a Feedback Loop and Downmix — the smallest useful reverb. Loaded at startup |
| **Full** | The richest valid Composition, with every applicable field written out explicitly |
| **Modulated** | Simple plus subtle Modulation at both sites that accept it: a Diffusion Step, and the Feedback Loop |
| **Spatial** | Simple plus a subtle Early Reflections branch, with the Main and Early Downmix widened |

**Full** is the one to open when you want to see the shape of everything at
once — Early Reflections, Damping, Modulation at both sites, per-step
overrides — with nothing left implicit. A contract test keeps it that way: if
the renderer gains a field, Full is required to carry it.

Switching templates replaces the editor text. It asks first **only when you
would lose something** — that is, when the current text differs from both the
template you last loaded and the text that last rendered successfully. Text
you have not touched is replaced without a prompt.

Reloading the page puts you back on Simple. Nothing is saved anywhere: no
cookies, no local storage, no autosave, no history.

> The Audition source you selected stays on the server until you stop the
> bench, but a reloaded page no longer shows its name. Reselect it if you
> want the label back.

### Format JSON

Reformats valid JSON to two-space indentation, in place. It is the only
action that ever rewrites your text, and it never parses numbers into
JavaScript doubles — a full uint64 `seed` survives it exactly.

Malformed JSON is reported by character position and the text is left
untouched. Given a missing comma:

```json
{"formatVersion": 2, "seed": 42 "composition": {}}
```

```text
Format JSON: Expected ',' or '}' at position 32
```

Render is the only validation there is. The editor does not lint, and no
guessed diagnostics are painted into the margin.

---

## Render and listen

Press **Render**. On success the facts line names what the renderer produced:

```text
PianoDry.wav — 23.1 s, 48000 Hz, 2 ch
```

The result is longer than the 20 s source because pre-delay and the tail both
extend it — exactly as a command-line render would be.

The player holds the renderer's exact `output.wav`. Nothing is mixed, gained,
normalized or loudness-matched between the renderer and your speakers.

- **Download output.wav** — the bytes just rendered. Available only after a
  success.
- **Download request.json** — the current editor text, valid or not, formatted
  or not. It is what is on screen, not what last rendered.

One render runs at a time. Render is disabled while one is in flight, a
second request is refused rather than queued, and a long render is allowed to
finish — there is no progress bar, no cancel, and no timeout cutting it short.

---

## When a render fails

The renderer's own diagnostic is surfaced verbatim in the status panel:
category, reason, and — for configuration errors — the exact JSON Pointer at
fault.

```text
invalid_configuration: unknown field
at /reverbAmount
```

```text
malformed_json: malformed JSON: [json.exception.parse_error.101] parse error
at line 1, column 21: syntax error while parsing object key - unexpected ',';
expected string literal
at /
```

```text
invalid_configuration: Channel 0's resolved delay less Excursion does not
exceed the fixed Interpolation margin
at /composition/stages/1/steps/0/modulation/depthMs
```

Those categories and pointers are the renderer's, documented under
[diagnostics](render-and-analyze-evidence.md#diagnostics). The bench adds
four of its own:

| Category | Means |
| --- | --- |
| `bench_error` | Refused before reaching the renderer — no source chosen yet, or a body over its limit |
| `busy` | Another Source or Render action is still running; this one was refused rather than queued |
| `unsupported_output` | The renderer produced float64 output, which the bench will not play |
| `render_failed` | The renderer failed without a JSON diagnostic to quote — its raw stderr is shown instead |

A failed render costs you nothing: the previous successful result stays
loaded and playable, and your text is untouched.

---

## From an audition to a Render Result

Each successful render writes a complete, ordinary [Render
Result](render-and-analyze-evidence.md) inside the session directory:

```text
source/PianoDry.wav        the copy of your Audition source
renders/request.json       the text handed to the last render attempt
renders/<n>/output.wav     the audio the player is holding
renders/<n>/render.json    provenance and frame accounting
renders/<n>/request.json   the request text you rendered
renders/<n>/resolved.json  every value the DSP actually used
```

Only the newest successful result is kept — a new success replaces the
previous one, and the whole session directory goes when you stop the bench.
Nothing about a bench render is second-class; it is just temporary.

To keep what you heard, render it again on disk:

```bash
# 1. Download request.json from the bench.
# 2. Render it with the same source and the same build.
build/default/rvrbotron render \
  --input samples/listening/PianoDry.wav \
  --config ~/Downloads/request.json \
  --output build/audition-keeper
```

The audio is **byte-identical** to what the bench played, because the bench
passes your text to that same binary unchanged. From there the result is
ordinary evidence: analyze it, replay its `resolved.json` with `--resolved`,
or hand the directory to someone else.

The one thing to write down yourself is **which source you auditioned** —
the bench holds a copy but does not tell you where it came from.

---

## What the bench will not do

Deliberate omissions, so the bench is not mistaken for the eventual plugin or
product GUI:

**No audio processing of its own.** No browser-side dry/wet mixing, gain,
normalization, limiting, clipping prevention, or loudness matching. What you
hear is the renderer's output or nothing.

**No durable state.** No accounts, cookies, local storage, autosave, named
projects, configuration library, render history, or session restoration. One
temporary session per launch, removed on exit; a crash leaves it to the
operating system's temporary-directory cleanup.

**No float64 playback or transcoding.** Point `--renderer` at a `double`
build and the first render is refused rather than converted:

```text
unsupported_output: the renderer produced float64 output, which the bench
does not play or transcode. Point --renderer at the repository's default
float32 build.
```

**Bounded input.** A request over 1 MiB, or an Audition source over 1 GiB, is
refused outright:

```text
bench_error: the request is larger than the 1 MiB limit
```

**Local only.** Loopback binding, one capability token per launch, no remote
access, no HTTPS, no hosting, no multi-user anything.

**Not an analysis tool.** No plots, meters, waveforms, A/B controls,
playlists, batch rendering, or measurements. Render a result to disk and use
[Render and analyze evidence](render-and-analyze-evidence.md) for that.

---

## Where next

Once you know what you want to hear, [Configure the
Composition](configure-the-composition.md) documents every field you can put
in that editor. Once you have rendered something worth keeping, [Render and
analyze evidence](render-and-analyze-evidence.md) covers what a Render Result
holds and how to measure it — and when one audition stops being enough,
[Run experiments](run-experiments.md) sweeps an axis instead of a setting.
