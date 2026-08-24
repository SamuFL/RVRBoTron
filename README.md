# RVRBoTron

RVRBoTron is a research-grade algorithmic reverb laboratory. It is designed to make reverb structures easy to configure, render, measure, compare, and reproduce while developing a production-quality DSP core for a future audio plugin.

## Project documentation

- [Reverb design overview](docs/design/reverb/README.md)
- [Stage specifications](docs/design/reverb/stages/)
- [Domain vocabulary](CONTEXT.md)
- [Engineering workflow](docs/agents/workflow.md)

Active implementation work is tracked in this repository's GitHub Issues.

## Quick start

Install CMake 3.25 or newer, Ninja, Python 3, and a C++17 compiler. Git LFS is
also required to download or add the curated listening samples.

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
  "formatVersion": 1,
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

The immutable Render Result contains:

| Path | Contents |
| --- | --- |
| `output.wav` | Canonical IEEE float32 WAV, or float64 with the `double` preset |
| `request.json` | Exact bytes of the user-authored request; present for requested renders |
| `resolved.json` | Complete Resolved Configuration used by the DSP |
| `render.json` | Input provenance, renderer facts, selected precision, block size, and audio facts |
| `analysis/` | Append-only, versioned analysis artifacts added after rendering |

`render.json` records the input filename and SHA-256, renderer version,
platform, architecture, sample precision, block size, configuration input
mode, sample rate, channel count, and frame count. It intentionally contains
no timestamp, host or user identity, full input path, or source-tree
fingerprint.

Rendering builds the evidence in a temporary sibling and publishes it
atomically. Choose a fresh output path for every render because an existing
destination is never overwritten.

### Rerender the Resolved Configuration

Use the emitted configuration without resolving the request again:

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/matrix/identity-stereo-48000-pcm16.wav \
  --resolved build/requested-result/resolved.json \
  --output build/resolved-result
```

### Analyze the Render Result

Analysis is a separate Python command and is never required for rendering:

```bash
python3 tools/analyze_render.py \
  build/requested-result \
  --source tests/fixtures/audio/matrix/identity-stereo-48000-pcm16.wav
```

It prints a concise summary and atomically adds
`analysis/baseline-v1.json`. The versioned artifact records frame count,
duration, sample rate, channel count, and non-finite sample count, plus
per-channel and combined peak absolute sample, RMS amplitude, and sum of
squares. Metric definitions and linear units are embedded in the artifact.

The optional source comparison runs only when the source SHA-256 matches
`render.json`. It reports exact equality, differing-sample count, maximum
absolute error, and the first mismatch. Analysis artifacts are append-only:
repeating identical analysis succeeds without rewriting, while different
content for the same analyzer version is rejected.

### Validate a Listening Sample Locally

Listening material is intentionally excluded from automated tests. After
installing Git LFS, render and analyze a curated sample through the same public
commands:

```bash
git lfs install
git lfs pull
build/default/rvrbotron render \
  --input samples/listening/DryHomePercussions.wav \
  --output build/listening-result
python3 tools/analyze_render.py \
  build/listening-result \
  --source samples/listening/DryHomePercussions.wav
```

The empty Composition should report exact identity. The other curated sounds
and their listening purposes are documented in
[`samples/listening/README.md`](samples/listening/README.md).

### Add a Listening Sample

WAVs in `samples/listening/` are tracked through Git LFS and remain separate
from `tests/fixtures/`:

```bash
git lfs install
git lfs track "samples/listening/*.wav"
git add .gitattributes samples/listening
git lfs ls-files
```

Add the sample's format, listening purpose, provenance, and redistribution
permission to `samples/listening/README.md`.

## Diagnostics

Failures are written to stderr with stable exit categories:

| Exit | Category |
| ---: | --- |
| 2 | `malformed_json` |
| 3 | `unsupported_audio` |
| 4 | `invalid_configuration` |
| 5 | `io_failure` |
| 6 | `internal_processing_failure` |
| 7 | `invalid_arguments` |

Pass `--error-format json` to receive the same category, reason, and optional
configuration location as one JSON object.
