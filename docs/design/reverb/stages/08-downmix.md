# Stage 8 — Downmix

N internal channels back to a stereo output. Where the system stops being all-pass.

*Vocabulary, ground rules, and the overall shape of the chain are in the [reverb design overview](../README.md).*

---

## What it does musically

The internal multi-channel world ends here, and the choices made in compressing it decide how wide and enveloping the result feels. Easy to undo good work: a tail beautifully decorrelated across sixteen channels can be collapsed into something narrow and phasey by a careless downmix.

---

## What it does mathematically

A 2×N matrix: two output rows over N input Channels.

**It cannot be all-pass, and that's fine.** A map from N dimensions to 2 has rank 2 at most, so energy in the other N−2 dimensions is discarded. No matrix avoids this — it's a property of leaving the multi-channel world.

The concern is not lost energy, which is just a level to compensate, but **pattern**. Regular phase relationships between channels become comb filtering when collapsed. The diffuser exists to make the phase response irregular, and this is where that pays off: the long-term spectral flatness of the downmixed impulse response is the end-to-end check that diffusion worked.

### Two source signals, two rules

The tail arrives **unaligned**, so summing decorrelated channels is safe and captures all the energy. The early reflections arrive **aligned**, so summing reinforces coherently and
produces peaks. Channel selection is the ordinary choice; coherent summing
remains an explicit measurable ablation handled by Stage 7.

### Strategies

| Strategy | Method | Notes |
|---|---|---|
| `orthogonal-rows` | Two deterministic dense orthonormal rows | Derived from the branch-specific versioned `main-downmix` or `early-downmix` RandomOrthogonal usage site. |
| `halves` | First ceil(N/2) Channels → L, remainder → R | Each non-empty group has equal coefficients `1/√groupSize`. |
| `alternating` | Even indices → L, odd indices → R | Each non-empty group has equal coefficients `1/√groupSize`. |
| `select` | Explicit left Channel and optional right Channel | An omitted right Channel duplicates the left to mono. |
| `sum-all` | The same `1/√N` row duplicated to L/R | Diagnostic Coherent Downmix ablation. |

`select` and `sum-all` support N≥1. The other strategies require N≥2.
Selected Channel indices are zero-based, explicit in Resolved Configuration,
and must be distinct when both are present.

### Width as a constant-power mid/side law

    M = (L + R)/√2      S = (L − R)/√2
    M′ = √2·cos(θ/2)·M
    S′ = √2·sin(θ/2)·S

After reconstructing L/R, 0° is mono, 90° is unchanged, and 180° is
side-only and out of phase. This preserves expected power when M and S carry
equal energy, as for decorrelated stereo; it cannot preserve every individual
signal's energy while also collapsing arbitrary stereo to mono. Actual energy
change is measured. A mono pre-width signal remains non-spatial and may change
level or cancel toward 180°.

The exact endpoint matrices over the pre-width `[L, R]` vector are:

    0°   [[1/√2,  1/√2], [ 1/√2,  1/√2]]
    90°  [[1,     0   ], [ 0,     1   ]]
    180° [[1/√2, -1/√2], [-1/√2,  1/√2]]

The resolved 90° matrix is an exact identity bypass. Exact matrices are also
used at 0° and 180°; intermediate matrices are resolved once and serialized, so
replay does not recompute trigonometry.

### Expected-power normalisation

Every strategy constructs unit-norm rows intrinsically. With
`normalisation: energy`, a common `√(N/2)` compensation restores the stereo
reference level under equal-power uncorrelated Channel input. With `none`, that
common compensation is omitted but row normalization remains. At N=1, energy
normalization duplicates the selected Channel with `1/√2` compensation.

This is an expected-power contract, not exact N→2 energy preservation. Tests use
seeded unaligned fixtures; analysis reports actual branch and output energy.

---

## Parameters

```json
"downmix": {
  "strategy": "orthogonal-rows",
  "widthDeg": 90,
  "normalisation": "energy"
}
```

| Parameter | Value | Notes |
|---|---|---|
| `strategy` | `orthogonal-rows` / `halves` / `alternating` / `select` | |
| | `sum-all` | Diagnostic Coherent Downmix ablation. |
| `leftChannel` | zero-based index | Used by `select`. |
| `rightChannel` | zero-based index or omitted | Used by `select`; omission duplicates left to mono. |
| `widthDeg` | 0–180 | 0 mono, 90 unmodified, >90 out of phase. |
| `normalisation` | `energy` / `none` | `none` omits common compensation, not unit-row construction. |

---

## In code

```
Downmix
  rows         : 2×N
  width        : 2×2 mid/side matrix
  compensation : scalar, resolved at configuration
```

---

## What this forces on the architecture

**Downmix records the Alignment expectation of its input.** Composition derives
that expectation from wiring: direct Diffuser output and taps are aligned;
Feedback Loop output is unaligned. Requested configuration cannot forge it.
Summing an aligned source is an acoustically revealing but structurally valid
ablation, so configuration permits it. Analysis records measured Alignment
score and tags aligned `sum-all`; matched sweep reports compare it with
`select`.

A full chain therefore has **two Downmix instances**, one for the Main wet path
and one for Early Reflections, with independent resolved matrices, width, and
evidence. Better than one instance with a hidden branch.

### Format-version-1 diagnostic Downmix

The old diagnostic implementation permits only implicit Channels 0/1
`select`, compensated by `√(N/2)`; at N=1 it duplicates Channel 0 at `1/√2`.
It remains reproducible at tag `format-v1-final` and is not accepted by
format-version-2 builds.

---

## Invariants

- **Expected level independence.** Seeded unaligned fixtures of fixed total
  power, with per-Channel power `1/N` as Split produces, retain expected power
  across N and strategy within declared tolerance.
- **Width endpoints.** 0° is mono, 90° is bit-identical bypass, and 180° is
  side-only and out of phase.
- **Width evidence.** Expected power is constant on the canonical decorrelated
  fixture; actual energy change is always reported.
- **Mono at zero.** `widthDeg: 0` gives L identical to R.
- **Decorrelation evidence.** Output correlation and inter-channel level
  difference are reported; no universal acoustic threshold rejects a render.
- **Spectral evidence.** Max and RMS band deviation are measured against the
  same N-Channel source's aggregate spectrum.
- **Alignment provenance.** Resolved expectation must match Composition wiring.

---

## Worth sweeping early

- `strategy` across all five at N=8 and N=16 — measure L/R correlation and spectral flatness.
- `widthDeg` 0 / 45 / 90 / 135 / 180 — compare expected and actual energy.
- `select` against `orthogonal-rows` — tests whether one channel really holds the whole pattern.
- `select` against aligned `sum-all` — the Coherent Downmix ablation, with no
  validation bypass required.
