# Stage 6 — Modulation

Gentle movement of delay times, requiring fractional delays.

*Vocabulary, ground rules, and the overall shape of the chain are in the [reverb design overview](../README.md).*

---

## What it does musically

A static FDN sounds static. Fixed delays give fixed resonances, and long tails ring on them in a way the ear recognises as artificial. Moving the delays smears the modes so no frequency dominates.

Three regimes, genuinely different effects rather than points on one scale:

- **Subtle** (sub-millisecond, slow) — removes the metallic ring, adds liveliness. Not perceived as modulation at all.
- **Moderate** — audible thickness and shimmer.
- **Extreme** — chorus, detuning, supernatural width. Past realism deliberately.

Also the cheapest fix for a tail that measures well and sounds wrong: if the eigentones are clustered, moving them constantly hides the gaps.

---

## Placement

**In the diffuser** (typically one step) — the signal passes once, so detuning applies once and does not accumulate. Chorus-like thickening of the early smear.

**In the feedback loop** — the signal passes on every circulation, so detuning compounds. A tail that starts clean and grows progressively thicker. This is the default.

Inside the loop, **only some channels need modulating**; the mixing matrix distributes detuned content to the rest within a few circulations. Worth verifying rather than assuming, which is what `channelFraction` is for.

---

## What it does mathematically

### Fractional delay becomes mandatory

Fixed delays round to whole samples; a moving delay cannot, since rounding produces stepped jumps in read position, audible as clicks. The read must interpolate.

**In a feedback loop, interpolation error compounds on every circulation** — the reason interpolation choice matters far more here than in a chorus. Linear interpolation is a lowpass whose cutoff depends on the fractional read position, so inside the loop it acts as unintended damping that varies with modulation depth: the tail darkens for reasons unrelated to Stage 5. Third-order Lagrange is the default. Allpass interpolation has flat magnitude but transient artefacts when the delay length changes, which is exactly what modulation does.

Interpolation quality is therefore a research axis, measurable by sweeping depth against spectral tilt.

### Depth and rate multiply

Changing a delay length shifts pitch. For sinusoidal modulation of amplitude *A* and rate *f*, peak fractional pitch deviation is

    Δ ≈ 2π f A

so detuning is governed by the **product** of depth and rate. Doubling the rate at fixed depth doubles the detuning — counterintuitive at any control surface.

### Decorrelation and shape

Per-channel LFOs must be decorrelated in phase, ideally in rate. Channels moving together make the whole tail wobble in pitch, a distinct and usually unwanted effect.

`sine` is periodic, and at higher depths that periodicity becomes audible as regular wobble. `smoothed-random` — band-limited noise — models air movement more convincingly and has no period to lock onto.

### What it costs

The system is now time-varying, so it is not strictly energy-preserving, and the gains solved in Stage 4 against nominal delay lengths become approximate. Negligible at realistic depths.

---

## Parameters

```json
"modulation": {
  "target": "feedbackLoop",
  "depthMs": 0.4,
  "rateHz": 0.7,
  "shape": "smoothed-random",
  "channelFraction": 1.0,
  "interpolation": "lagrange3"
}
```

| Parameter | Value | Notes |
|---|---|---|
| `target` | `feedbackLoop` / `diffuser` / `both` | Compounding against one-shot. |
| `depthMs` | ≥ 0 | Peak delay excursion. 0 disables. |
| `rateHz` | > 0 | Multiplies with depth for perceived detune. |
| `shape` | `smoothed-random` / `sine` / `triangle` | |
| `channelFraction` | 0–1 | Proportion of channels modulated. |
| `interpolation` | `lagrange3` / `linear` / `allpass` | Property of the delay line. |

Under `diffuser`, which step is modulated is a per-step field, not a global one.

---

## In code

```
Modulation
  lfos         : per-channel, decorrelated phase
  depthSamples : resolved at configuration
```

Interpolation belongs to `DelayLine`, which exposes both an integer and a fractional read; unmodulated stages use the first and pay nothing.

---

## What this forces on the architecture

**Delay buffers need headroom.** Allocated length is nominal maximum delay plus peak modulation depth plus interpolation margin. Sizing from the nominal alone produces an overrun at the modulation peak — intermittent, depth-dependent, unpleasant to debug. The bound is computed at configuration from resolved values.

**LFO phases are seeded positionally**, by the Stage 2 rule, so changing `channelFraction` doesn't reshuffle channels that were already modulating.

**`DelayLine` gains a second read path** — the only component in the design serving two stages with different requirements.

---

## Invariants

- **Identity at zero.** `depthMs: 0` gives output bit-identical to modulation disabled.
- **No buffer overrun** at maximum depth, verified at the extreme of the permitted range.
- **Determinism.** Identical configuration gives identical LFO trajectories.
- **Bounded energy.** Modulation perturbs but does not grow the tail.
- **Decorrelation.** No coherent frequency modulation at the LFO rate in the summed output.

---

## Worth sweeping early

- `depthMs` at fixed rate, then `rateHz` at fixed depth — demonstrates the product relationship and shows where realism ends.
- `interpolation` at high depth against spectral tilt — quantifies linear interpolation's hidden damping. The sweep most likely to produce something worth publishing.
- `target` `diffuser` against `feedbackLoop` at matched depth.
- `channelFraction` 0.25 / 0.5 / 1.0 — tests whether the matrix really does distribute detuning, and how fast.
- `shape` `sine` against `smoothed-random` at high depth.
