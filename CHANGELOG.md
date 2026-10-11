# Changelog

All notable changes to RVRBoTron are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- The DSP core is consumable as a CMake subproject. Consumers link the new
  `rvrbotron::dsp` and `rvrbotron::config` alias targets.
  `RVRBOTRON_BUILD_TOOLS` and `RVRBOTRON_BUILD_TESTS` both default to on only
  when RVRBoTron is the top-level project, so a subproject fetches nothing and
  builds no tools or tests (#161).
- CI builds a minimal consumer from the commit's source archive with GCC,
  Apple Clang, and MSVC (#161).

### Changed

- The test suite is gated by `RVRBOTRON_BUILD_TESTS` instead of
  `BUILD_TESTING` (#161).
- `ResolvedConfigJson` moved out of `rvrbotron_config` into a new
  `rvrbotron_config_json` target, so `rvrbotron_config` no longer depends on
  `nlohmann_json` (#161).

### Fixed

- The DSP core compiles with MSVC: `ownedVectorBytes` is no longer
  `constexpr` (#162).
- Every curated listening sample is restored on the Git LFS server, so
  checking out a release with Git LFS succeeds again (#161).

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
