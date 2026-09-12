# RVRBoTron

A research-grade algorithmic reverb laboratory.

RVRBoTron lets you describe a reverb as data, render it, and then *measure*
what you heard. Every render writes back the complete configuration it
actually used, so any result can be reproduced exactly — or replayed on
another machine and compared within declared tolerances. The DSP core is
written to production standards, on its way to becoming an audio plugin.

If you want to hear how a reverb structure behaves and then prove it, rather
than turn knobs until it sounds nice, this is built for you.

---

## Setup

You need CMake 3.25+, Ninja, a C++17 compiler, Python 3, and Git LFS.

From the repository root:

```bash
pip3 install -r tools/requirements.txt   # NumPy — the analyzers, and the test suite
git lfs install && git lfs pull          # the curated listening samples
```

Both are needed before you go further: a third of the test suite runs the
analyzers, and the listening samples are Git LFS pointers until you pull them.

## Build and test

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

That builds the renderer at `build/default/rvrbotron` and runs the full suite.

Other presets: `double` for float64 DSP samples, and `release` /
`release-double` for optimized builds.

## Hear a reverb in one command

The quickest way in is the **Research bench** — a local page where you pick a
source, edit a request, press Render, and listen. Same renderer you just
built, nothing to write to disk first:

```bash
python3 tools/research_bench/serve.py
```

It opens a loopback URL good for that run alone, starts you on a working
request template, and plays the renderer's exact `output.wav` — nothing
mixed, gained, or normalized in between. Everything it writes lives in one
temporary session, removed when you stop it with Ctrl+C.

The full walkthrough — templates, diagnostics, limits, and how to turn an
audition into a result you can keep — is in [Run the Research
bench](docs/guides/run-the-research-bench.md).

Prefer the command line, or want evidence you can hand to someone else? Carry
on below.

## Render your first reverb

Still in the repository root, save this as `build/hall.json` — a diffuser
feeding a damped tail, with 20 ms of pre-delay and the dry signal mixed back
in:

```json
{
  "formatVersion": 2,
  "seed": 42,
  "composition": {
    "stages": [
      { "type": "split", "channels": 8 },
      { "type": "diffuser", "steps": 4, "totalMs": 80 },
      { "type": "feedback-loop", "delayMinMs": 60, "delayMaxMs": 120, "rt60Sec": 2.0,
        "damping": { "highRatio": 0.4 } },
      { "type": "downmix", "strategy": "orthogonal-rows" }
    ],
    "preDelayMs": 20,
    "dryDb": 0,
    "wetDb": -3,
    "wetOnly": false
  }
}
```

Send an impulse through it and listen to the tail:

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --config build/hall.json \
  --output build/first-reverb
```

Then try it on music — `samples/listening/` holds curated dry material:

```bash
build/default/rvrbotron render \
  --input samples/listening/PianoDry.wav \
  --config build/hall.json \
  --output build/first-reverb-music
```

Play `build/first-reverb-music/output.wav`.

## Measure what you rendered

Rendering is only half of it. Every Render Result can be analyzed, and the
analysis is deterministic — it reads the immutable result and nothing else:

```bash
python3 tools/analyze_render.py build/first-reverb-music
```

```text
1108800 frames, 23.100000000 s, 48000 Hz, 2 channels, 0 non-finite samples
build/first-reverb-music/analysis/baseline-v1.json
```

The artifact it wrote holds peak, RMS, and sum-of-squares per channel and
combined.

Notice the render is longer than the 20 s input — the pre-delay and the tail
both extend it, and `render.json` accounts for every frame. Deeper analyzers
measure RT60 per octave band, echo density, stereo image, and Modulation. All
of that is in [Render and analyze
evidence](docs/guides/render-and-analyze-evidence.md).

## What you just produced

`build/first-reverb-music/` is an immutable **Render Result**: the audio, the
exact request you wrote, the fully resolved configuration the DSP used, the
provenance and frame accounting in `render.json`, and any analysis published
afterwards.

The resolved configuration is the interesting one. It holds every value
derived from your request — per-Channel delays, decay gains, matrices, shelf
coefficients — so re-rendering it with `--resolved` gives back the same audio,
bit for bit. That is the point of the whole project: evidence you can hand to
someone else.

---

## Documentation

| Guide | Read it when |
| --- | --- |
| [Run the Research bench](docs/guides/run-the-research-bench.md) | You want to hear a Composition now — launch, audition, render, listen |
| [Configure the Composition](docs/guides/configure-the-composition.md) | You are writing a request — every field, default, and shape, with examples |
| [Render and analyze evidence](docs/guides/render-and-analyze-evidence.md) | You have a Render Result — captures, the analyzers, benchmarks, diagnostics |
| [Run experiments](docs/guides/run-experiments.md) | You want to sweep an axis — catalogs, sweeps, and listening material |

Background and process:

- [Reverb design overview](docs/design/reverb/README.md) and the
  [stage specifications](docs/design/reverb/stages/) — why each stage is built
  the way it is
- [Architecture decisions](docs/adr/) — the trade-offs that are settled
- [Domain vocabulary](CONTEXT.md) — the words this project uses precisely
- [Engineering workflow](docs/agents/workflow.md) — branches, commits, and how
  work is tracked

Active implementation work lives in this repository's GitHub Issues.

## Platform support

CI runs the complete command-level suite on macOS arm64, macOS Intel, and
Windows x86_64 for float32 builds, plus macOS arm64 and Windows x86_64 for
float64. Each job proves exact identity from its own committed fixtures;
nothing is transferred between machines.

The committed fixtures cover mono and stereo PCM16, PCM24, PCM32, IEEE
float32, and IEEE float64 WAV at 44.1, 48, and 96 kHz. Regenerate them with:

```bash
python3 tests/fixtures/generate_wav_matrix.py
```
