# RVRBoTron

RVRBoTron is a research-grade algorithmic reverb laboratory. It is designed to make reverb structures easy to configure, render, measure, compare, and reproduce while developing a production-quality DSP core for a future audio plugin.

## Project documentation

- [Configure the Composition](docs/guides/configure-the-composition.md) -- every request field, with examples
- [Render and analyze evidence](docs/guides/render-and-analyze-evidence.md) -- Render Results, captures, analyzers, benchmarks
- [Run experiments](docs/guides/run-experiments.md) -- catalogs, sweeps, and listening material
- [Reverb design overview](docs/design/reverb/README.md)
- [Stage specifications](docs/design/reverb/stages/)
- [Domain vocabulary](CONTEXT.md)
- [Engineering workflow](docs/agents/workflow.md)

Active implementation work is tracked in this repository's GitHub Issues.

## Quick start

Install CMake 3.25 or newer, Ninja, Python 3, and a C++17 compiler. Git LFS is
also required to download or add the curated listening samples. Rendering and
`tools/analyze_render.py` need only the standard library; the diffusion,
tail, and Early Tap support analyzers additionally need the packages in
`tools/requirements.txt` (`pip3 install -r tools/requirements.txt`).

### Configure, build, and test

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

The `double` preset runs the same suite with float64 DSP samples:

```bash
cmake --preset double
cmake --build --preset double
ctest --preset double
```

The `release` and `release-double` presets build optimized (`Release`)
binaries for meaningful `benchmark` timing evidence; `default`/`double` stay
Debug builds for development:

```bash
cmake --preset release
cmake --build --preset release
```

Every CI job runs the complete command-level suite and proves exact identity
from its own committed fixtures; artifacts are not transferred between
machines. Float builds run on macOS arm64, macOS Intel, and Windows x86_64.
Representative float64 builds run on macOS arm64 and Windows x86_64. CI runs
for pull requests targeting `develop` or `main` and for pushes to `main`.
The committed compatibility fixtures cover mono and stereo PCM16, PCM24,
PCM32, IEEE float32, and IEEE float64 WAV at 44.1, 48, and 96 kHz.

Regenerate those deterministic technical fixtures with:

```bash
python3 tests/fixtures/generate_wav_matrix.py
```

### Render a Requested Configuration

Create `request.json`:

```json
{
  "formatVersion": 2,
  "seed": 0,
  "composition": {"stages": []}
}
```

Render a committed technical fixture through the empty Composition:

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/matrix/identity-stereo-48000-pcm16.wav \
  --config request.json \
  --block-size 512 \
  --output build/requested-result
```

What that Render Result contains, how to replay it exactly, and how to
measure it are in
[Render and analyze evidence](docs/guides/render-and-analyze-evidence.md).

### Render the first Reference Diffusion Step

Format version 2 also accepts the ordered `[split, diffuser, downmix]` and
`[split, feedback-loop, downmix]` Composition shapes (every shape and field is
documented in [Configure the
Composition](docs/guides/configure-the-composition.md)).
The Diffuser resolves to an ordered chain of Hadamard Diffusion
Steps (`4` by default) feeding a select Downmix. Diagnostic ablations
support `normalisation: "none"`, `delayStrategy: "even"` or
`"uniform-random"`, `shuffle: false`, and `polarity: "none"`:

```json
{
  "formatVersion": 2,
  "seed": 42,
  "composition": {
    "stages": [
      {
        "type": "split",
        "channels": 8,
        "strategy": "duplicate",
        "normalisation": "energy"
      },
      {
        "type": "diffuser",
        "steps": 1,
        "totalMs": 40,
        "distribution": "even",
        "step": {
          "delayStrategy": "segmented-random",
          "mix": "hadamard",
          "shuffle": true,
          "polarity": "seeded-random"
        }
      },
      {
        "type": "downmix",
        "strategy": "select",
        "leftChannel": 0,
        "rightChannel": 1,
        "normalisation": "energy"
      }
    ]
  }
}
```

Non-empty diffusion renders are stereo and wet-only. The renderer drains
silence for the resolved finite Diffuser budget, so `render.json` distinguishes
`inputFrames` from the longer `frames`.

Capture the N-Channel Split and cumulative Diffusion Step without changing
`output.wav`:

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --config request.json \
  --capture-stages all \
  --output build/diffusion-result
```

### Configure the Composition

Every request field, its accepted values, defaults, the valid stage shapes, and
worked examples are in
[Configure the Composition](docs/guides/configure-the-composition.md).


### Render and analyze evidence

Render Result contents, replaying a Resolved configuration, Stage captures,
every analyzer, cross-platform equivalence, benchmarking, and diagnostics are
in [Render and analyze
evidence](docs/guides/render-and-analyze-evidence.md).


### Run experiments

Experiment catalogs and sweeps -- diffusion, tail, damping, modulation, and
spatial output -- plus the curated listening material they run against, live
in [Run experiments](docs/guides/run-experiments.md).
