# Changelog

All notable changes to RVRBoTron are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-09-13

First public release. Everything below arrived across the six development
milestones that preceded it; the release itself adds no behavior.

### Reverb structure

- **Diffusion.** Multi-step Diffuser with positionally stable delays,
  selectable delay strategies, and Hadamard, Householder, or
  RandomOrthogonal mixing matrices. Split strategies are compared without
  changing level.
- **Sustained tail.** Feedback Loop composed with the Diffuser, with
  selectable gain modes, block size bounded by the shortest loop delay, and
  a tail that stays numerically clean as it drains.
- **Frequency-dependent decay.** Damping darkens the tail over time, with
  low-frequency cleanup, a protected Reference band, and bounded boosted
  decay.
- **Modulation.** Seeded time-varying delay on the Feedback Loop and on
  Diffusion Steps, with selectable waveform shapes, partial-Channel
  modulation, and a choice of linear or allpass interpolation.
- **Spatial output.** Stereo Main output with deterministic orthogonal
  Downmix rows, halves and alternating Downmix strategies, Width, and a
  canonical multi-tap Early Reflections envelope.
- **Composition envelope.** Dry and wet are mixed through one envelope, with
  the wet path delayed before Split.

### Evidence and reproducibility

- Every render writes a Render Result: the requested configuration, the
  fully resolved configuration, and build provenance beside the audio.
  Results are written atomically.
- `--resolved` replays a resolved configuration exactly, on the same machine
  or another, compared within declared tolerances.
- Analyzers for diffusion, tail, frequency-dependent decay, modulation, and
  spatial output, each reproducible from a Render Result alone.
- Empirical benchmarking of the resolved DSP.
- Experiment catalogs and sweeps, including sweeps of curated listening
  samples across a single axis.

### Tools

- **Research bench** (`tools/research_bench/serve.py`) — a local page that
  renders and plays the renderer's exact output, started from a Request
  template, bounded to a single temporary session.
- **Folder renderer** (`tools/render_folder.py`) — applies one Requested
  configuration across a folder of WAVs for sampled-instrument workflows,
  converting to PCM and reporting clipping.

### Documentation

- Guides for running the Research bench, configuring the Composition,
  rendering and analyzing evidence, and running experiments.
- Architecture decision records for the settled trade-offs, stage
  specifications for the DSP design, and a domain vocabulary in
  `CONTEXT.md`.

### Configuration format

- Reverb configuration is at **format v2**. Format-1 Requested and Resolved
  configurations are deliberately rejected rather than carried forward
  (ADR-0006); the error points at the annotated tag `format-v1-final`, which
  remains able to render and analyze format-1 work.
- Format v2 adds Pre-delay and dry/wet fields without invalidating existing
  Resolved configurations: the loader reads absent envelope fields as the
  exact legacy behavior (ADR-0007).

[1.0.0]: https://github.com/SamuFL/RVRBoTron/releases/tag/v1.0.0
