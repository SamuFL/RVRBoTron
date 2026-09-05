# Define reproducibility at two levels

RVRBoTron guarantees Repeat determinism as exact decoded-sample reproduction for the same renderer binary, precision, input, and configuration. Across macOS Apple Silicon, macOS Intel, and Windows, it instead requires Cross-platform equivalence using layered, stage-specific numerical and measurement tolerances, because compiler and architecture differences can make universal bit identity impractical once feedback compounds floating-point differences.

## What stays exact versus what is tolerance-based

Structural facts that are a deterministic function of the Resolved Configuration rather than a floating-point measurement -- the Echo-path count (`N^k`, purely Channel count and step count) and the Distinct-arrival density curve's bin count -- are never loosened by a tolerance -- they hold exactly on every supported platform. Repeat determinism on one machine and block-size independence (`analyze_diffusion.py --compare`) are likewise always exact. Every measured diffusion metric -- energy ratio, orthogonality error, Correlation, Alignment score, Coloration, and the measured Distinct-arrival counts themselves (`density.totalDistinctArrivals`, `density.bins`) -- is compared within an explicit, per-precision absolute tolerance instead, since these accumulate compiler- and architecture-dependent floating-point summation-order differences. The compact Reference impulse tracer used for cross-platform comparison is small enough, and its active samples sit far enough above the -120 dB activity floor, that its committed Distinct-arrival tolerance is currently zero -- a reasoned, reviewable choice rather than a structural guarantee, so it stays in the tolerances document rather than hardcoded.

The same split applies to the Feedback Loop's sustained tail. The octave-band count (fixed by `OCTAVE_BAND_CENTERS_HZ` and the sample rate) is compared unconditionally exact, on the same footing as the Echo-path count. Whether a band's T20/T30 fit exists at all, and the decay-envelope monotonicity flag, are discrete facts about the committed CI tracer's decay curve rather than continuous quantities -- a reviewable choice like the Distinct-arrival tolerance above, not a structural guarantee, so they too are compared unconditionally exact rather than looked up in a tolerances document. Every measured tail metric -- non-finite sample count, each band's own T20/T30 RT60 estimate, Alignment score, and Coloration -- is compared within an explicit, per-precision absolute tolerance instead.

## Committed tolerances

Each CI platform/precision job renders and analyzes the same compact Reference impulse tracer (`tests/test_diffusion_analyzer_cli.py`'s fixture composition), then `tools/extract_diffusion_equivalence.py` reduces its `analysis/diffusion-v1.json` to a compact, WAV-free summary uploaded as a CI artifact. A final CI job downloads every platform's summary and runs `tools/compare_diffusion_equivalence.py` against `tools/diffusion_tolerances_v1.json`, which fixes one absolute tolerance per metric per precision (float32, float64) with an explanatory `rationale` field. Tolerances are only tightened after reviewing real evidence from every supported CI platform, never reasoned from a single machine, and each committed value carries measured headroom above the largest cross-platform delta actually observed.

The values currently committed were reviewed this way for issue #37 (PR #48, CI run [33086622559](https://github.com/SamuFL/RVRBoTron/actions/runs/33086622559)): energy, orthogonality, Correlation, Alignment, and Distinct-arrival density all reproduced with exactly zero delta across macOS arm64, macOS Intel, and Windows x86_64 on the compact tracer, so their committed tolerances are small non-zero headroom for configurations the tracer does not exercise (Householder/RandomOrthogonal matrices, larger Channel counts) rather than a bound on any observed divergence. Only Coloration showed a measurable, non-zero cross-platform delta (at the `1e-16`-to-`1e-15` scale), because its FFT/dB computation always runs in float64 via NumPy regardless of DSP sample precision, so tiny per-platform NumPy/BLAS summation-order differences show through; its committed tolerance carries roughly two orders of magnitude of headroom above the largest such delta actually observed.

## Tail tolerances

The same workflow covers the sustained tail: each CI platform/precision job additionally analyzes the same millisecond-scale Feedback Loop tracer (`tests/test_tail_analyzer_cli.py`'s fixture composition) used by `tail_analyzer_contract`, `tools/extract_tail_equivalence.py` reduces its `analysis/tail-v1.json` to a compact, WAV-free summary, and a final CI job compares every platform's summary with `tools/compare_tail_equivalence.py` against `tools/tail_tolerances_v1.json`.

The values currently committed were reviewed this way for issue #58 (PR #69, CI run [33429129103](https://github.com/SamuFL/RVRBoTron/actions/runs/33429129103)): non-finite sample count, Alignment score, and every Coloration metric reproduced with exactly zero delta across macOS arm64, macOS Intel, and Windows x86_64 on the millisecond-scale tracer, so their committed tolerances are small non-zero headroom -- Coloration's matching diffusion's own committed values exactly, since it is the identical FFT/dB code path (`analyze_diffusion.coloration_evidence`, reused directly by `analyze_tail.py`) with the same latent per-platform risk, even though it did not manifest on this tracer/run. Only each octave band's T20/T30 RT60 estimate showed a measurable, non-zero cross-platform delta (at the same `1e-16`-to-`1e-15` scale as diffusion's Coloration, for the same NumPy-float64 reason), so its committed tolerance carries roughly five to six orders of magnitude of headroom above the largest such delta actually observed -- warranted because RT60 fits accumulate a Schroeder backward integration and linear regression over tens of thousands of samples, more numerically involved than a single Coloration summary, for configurations (longer renders, different band shapes) this tiny tracer does not exercise.

## Damping, FMA contraction, and architecture equivalence

Issue #78 added a Damping-enabled tail tracer in PR #84. CI run
[33867625534](https://github.com/SamuFL/RVRBoTron/actions/runs/33867625534)
showed a float32-only split: macOS Intel and Windows x86_64 produced
effectively identical analyzed results, while macOS arm64 differed by up to
`1.82e-7` seconds in fitted RT60, `2.79e-7` dB per segment in the late-tail
slope, and `7.61e-5` dB in Coloration peak-to-peak. Float64 remained at the
`1e-13`-to-`1e-16` scale.

This split was reproduced locally from the same Resolved Configuration by
changing only AppleClang's floating-point contraction policy. The default
arm64 build emitted `fmadd` and `fmsub` instructions for the one-pole shelf
recurrence in `FeedbackLoop::processFrame`; rebuilding with
`-ffp-contract=off` emitted no fused instructions. The two arm64 renders
first differed at frame 145, Channel 0, and their metric deltas matched the
CI arm64-versus-x86_64 deltas. The no-contraction arm64 metrics then matched
both x86_64 platforms to the analysis numerical floor: no Coloration-summary
delta and at most `8.88e-16` seconds across T30 fits.

Configuration resolution is not the source of this difference. Every
resolved gain and active shelf coefficient has the same realised float32 bit
pattern on all three platforms. Render length, Tail budget, FFT length, and
analysis segmentation also agree. The architecture split begins when arm64
contracts the shelf's multiply-add/subtract expression into fused operations;
the shelf state and outer Feedback Loop then propagate that valid initial
rounding difference. Coloration peak-to-peak exposes the largest reported
delta because it is a max-minus-min over individual FFT-bin levels, so one
deep spectral null is substantially more sensitive than its averaged RMS and
spectral-flatness companions.

RVRBoTron deliberately retains each toolchain's default floating-point
optimizations. Cross-platform equivalence therefore continues to compare
arm64 with x86_64 using evidence-based measurement tolerances rather than
disabling FMA contraction or excluding an architecture from comparison.
Keeping the cross-architecture comparison preserves coverage for genuine
architecture-specific regressions. Tail-v2 compares every platform pair and
uses a tighter same-architecture tolerance than the cross-architecture
tolerance where the float32 FMA evidence requires that distinction. This
keeps the macOS-Intel-versus-Windows x86_64 signal tight instead of letting
the arm64 FMA allowance mask an x86_64-only regression. Structural facts
remain exact in both comparison classes.
