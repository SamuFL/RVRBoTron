# RVRBoTron

RVRBoTron is a research-grade algorithmic reverb laboratory. It is designed to make reverb structures easy to configure, render, measure, compare, and reproduce while developing a production-quality DSP core for a future audio plugin.

The project is currently in the design and implementation-planning phase.

## Project documentation

- [Reverb design overview](docs/design/reverb/README.md)
- [Stage specifications](docs/design/reverb/stages/)
- [Domain vocabulary](CONTEXT.md)
- [Engineering workflow](docs/agents/workflow.md)

Active implementation work is tracked in this repository's GitHub Issues.

## First identity render

The initial tracer builds with CMake and streams a mono or stereo WAV through
an empty Composition:

```bash
cmake --preset default
cmake --build --preset default
build/default/rvrbotron render \
  --input tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --block-size 512 \
  --output build/identity-result
```

The new result directory contains `output.wav`, `resolved.json`, and
`render.json`. The optional `--block-size` selects the processing block size;
it defaults to 512 frames and the effective value is recorded in
`render.json`. Rendering builds the evidence in a temporary sibling and
publishes the directory atomically; an existing destination is never
overwritten.

`render.json` records the input filename and SHA-256, renderer version,
platform, architecture, sample precision, block size, configuration input
mode, sample rate, channel count, and frame count. It intentionally contains
no timestamp, host or user identity, full input path, or source-tree
fingerprint.

The supported input matrix is mono or stereo PCM16, PCM24, PCM32, IEEE
float32, or IEEE float64 WAV at 44.1, 48, or 96 kHz. Default builds emit
canonical IEEE float32 WAV; `cmake --preset double` builds a renderer that
emits canonical IEEE float64 WAV.

The committed compatibility fixtures are reproducible:

```bash
python3 tests/fixtures/generate_wav_matrix.py
```

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

Pass a Requested Configuration with `--config`:

```json
{
  "formatVersion": 1,
  "seed": 0,
  "composition": {"stages": []}
}
```

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --config request.json \
  --output build/requested-result
```

The Render Result also preserves the source request as `request.json`. To
rerender exactly what was resolved, use the emitted configuration directly:

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --resolved build/requested-result/resolved.json \
  --output build/resolved-result
```

## Analyze a Render Result

Analysis is a separate Python command and is never required for rendering:

```bash
python3 tools/analyze_render.py build/identity-result
```

It prints a concise summary and atomically adds
`analysis/baseline-v1.json`. The versioned artifact records frame count,
duration, sample rate, channel count, and non-finite sample count, plus
per-channel and combined peak absolute sample, RMS amplitude, and sum of
squares. Metric definitions and linear units are embedded in the artifact.

To compare the rendered audio with its source, provide the original WAV:

```bash
python3 tools/analyze_render.py \
  build/identity-result \
  --source tests/fixtures/audio/impulse-mono-pcm16-48000.wav
```

Identity comparison runs only when the source SHA-256 matches `render.json`.
It reports exact equality, differing-sample count, maximum absolute error,
and the first mismatch. Analysis artifacts are append-only: repeating
identical analysis succeeds without rewriting, while different content for
the same analyzer version is rejected.
