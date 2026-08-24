# Stage 8 — Downmix

N internal channels back to a stereo output. Where the system stops being all-pass.

*Vocabulary, ground rules, and the overall shape of the chain are in the [reverb design overview](../README.md).*

---

## What it does musically

The internal multi-channel world ends here, and the choices made in compressing it decide how wide and enveloping the result feels. Easy to undo good work: a tail beautifully decorrelated across sixteen channels can be collapsed into something narrow and phasey by a careless downmix.

---

## What it does mathematically

An N×2 matrix.

**It cannot be all-pass, and that's fine.** A map from N dimensions to 2 has rank 2 at most, so energy in the other N−2 dimensions is discarded. No matrix avoids this — it's a property of leaving the multi-channel world.

The concern is not lost energy, which is just a level to compensate, but **pattern**. Regular phase relationships between channels become comb filtering when collapsed. The diffuser exists to make the phase response irregular, and this is where that pays off: the long-term spectral flatness of the downmixed impulse response is the end-to-end check that diffusion worked.

### Two source signals, two rules

The tail arrives **unaligned**, so summing decorrelated channels is safe and captures all the energy. The early reflections arrive **aligned**, so summing reinforces coherently and produces peaks — channel selection is required instead, which Stage 7 handles.

### Strategies

| Strategy | Method | Notes |
|---|---|---|
| `orthogonal-rows` | Two orthonormal rows of a mixing matrix | Maximal L/R decorrelation by construction. Default. |
| `halves` | First N/2 → L, last N/2 → R | Captures all energy; disjoint channel sets decorrelate well. |
| `alternating` | Even → L, odd → R | Behaves differently when channel shuffling is disabled. |
| `select` | Take two channels | Cheapest. Each channel holds a complete echo pattern, but only 2/N of the energy, needing √(N/2) compensation. |

### Width as a rotation

    M = (L + R)/√2      S = (L − R)/√2
    M′ = M·cos θ − S·sin θ
    S′ = M·sin θ + S·cos θ

A 2×2 rotation is orthogonal, so the image changes and the level does not — consistent with how every other energy question here is handled. θ = 0° collapses to mono, 90° is unmodified, beyond widens out of phase. Implementing width as a gain on the side channel instead would couple level and width.

### Level compensation

Each strategy captures a different fraction of internal energy, depending on N. Compensation is computed at configuration from both, so changing strategy or channel count changes the sound and not the level. Together with the 1/√N at Split and matrix normalisation in every step, this is what makes level independence hold end to end.

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
| `widthDeg` | 0–180 | 0 mono, 90 unmodified, >90 out of phase. |
| `normalisation` | `energy` / `none` | `none` is diagnostic. |

---

## In code

```
Downmix
  matrix       : N×2
  width        : 2×2 rotation
  compensation : scalar, resolved at configuration
```

---

## What this forces on the architecture

**Downmix must know the alignment of its input.** A summing strategy on an aligned signal produces plausible-sounding, comb-coloured output — a real error that would not announce itself. So each instance is configured with the alignment it expects, and configuration is rejected if a summing strategy meets an aligned source.

A full chain therefore has **two downmix instances**, one for the tail and one for the early reflections, with different valid strategy sets. Better than one instance with a hidden branch.

### Milestone 2 diagnostic Downmix

Before the Feedback Loop makes Channels unaligned, the finite Diffuser needs a deliberately narrow listening output. The Milestone 2 Downmix permits only `select`: Channel 0 to left and Channel 1 to right, compensated by √(N/2). At N=1 it duplicates Channel 0 to stereo at 1/√2. This is expected-energy diagnostic output, not an all-pass claim.

The complete Stage 8 strategies, width rotation, and user-selectable normalization remain deferred until spatial output is the active research subject. Exact N-Channel energy and Alignment evidence comes from Stage captures rather than from the diagnostic stereo output.

---

## Invariants

- **Level independence.** Output level unchanged by N, strategy, and `widthDeg`.
- **Width is a rotation.** Energy at any `widthDeg` equals energy at 90°.
- **Mono at zero.** `widthDeg: 0` gives L identical to R.
- **Decorrelation.** On unaligned input at 90°, L/R correlation is near zero.
- **No coloration.** Long-term average spectrum of the downmixed impulse response is flat within a few dB.
- **Alignment validation.** Summing strategies on aligned input are rejected at load.

---

## Worth sweeping early

- `strategy` across all four at N=8 and N=16 — measure L/R correlation and spectral flatness.
- `widthDeg` 0 / 45 / 90 / 135 — confirm energy is constant.
- `select` against `orthogonal-rows` — tests whether one channel really holds the whole pattern.
- Deliberate misuse: a summing strategy on the early path with validation disabled. The comb coloration made audible once, so it's recognisable later.
