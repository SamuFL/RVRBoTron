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

The initial tracer builds with CMake and renders a mono PCM16 WAV through an
empty Composition:

```bash
cmake --preset default
cmake --build --preset default
build/default/rvrbotron render \
  --input tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --output build/identity-result
```

The new result directory contains `output.wav`, `resolved.json`, and
`render.json`.

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
