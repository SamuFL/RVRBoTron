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
`render.json`.

The supported input matrix is mono or stereo PCM16, PCM24, PCM32, IEEE
float32, or IEEE float64 WAV at 44.1, 48, or 96 kHz. Default builds emit
canonical IEEE float32 WAV; `cmake --preset double` builds a renderer that
emits canonical IEEE float64 WAV.

The committed compatibility fixtures are reproducible:

```bash
python3 tests/fixtures/generate_wav_matrix.py
```

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
