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

**In the feedback loop** — the signal passes on every circulation, so detuning compounds. A tail that starts clean and grows progressively thicker. This is the interesting case.

There is no `target` field. Modulation is an optional nested object on whichever component it applies to — the Feedback Loop stage, or a Diffusion Step, in both the shared step defaults and per-step overrides like every other step field. Configuring both means two independent Modulations that may differ in every value, which is what makes compounding and one-shot movement comparable in a single render. This mirrors how `damping` nests inside the Feedback Loop; a global enum plus a per-step field would be two mechanisms for one decision.

Inside the loop, **only some channels need modulating**; the mixing matrix distributes detuned content to the rest within a few circulations. That claim is a hypothesis about the matrix, not a measured internal fact — see *What it forces on the architecture* — and `channelFraction` is what tests it at the output.

---

## What it does mathematically

### Fractional delay becomes mandatory

Fixed delays round to whole samples; a moving delay cannot, since rounding produces stepped jumps in read position, audible as clicks. The read must interpolate.

**In a feedback loop, interpolation error compounds on every circulation** — the reason interpolation choice matters far more here than in a chorus. Linear interpolation is a lowpass whose cutoff depends on the fractional read position, so inside the loop it acts as unintended damping that varies with modulation depth: the tail darkens for reasons unrelated to Stage 5. Third-order Lagrange is the default. Allpass interpolation has flat magnitude but transient artefacts when the delay length changes, which is exactly what modulation does.

Interpolation quality is therefore a research axis, measurable by sweeping depth against Decay tilt. Because that measurement is the point, `rateHz: 0` is permitted: it freezes each Channel's fractional offset, engaging interpolation fully while moving nothing. It is the only configuration that separates interpolation error from movement artefact, and the ground rules already bless diagnostic ablations of exactly this kind.

### Depth and rate multiply

Changing a delay length shifts pitch. For sinusoidal modulation of amplitude *A* and rate *f*, peak fractional pitch deviation is

    Δ ≈ 2π f A

so detuning is governed by the **product** of depth and rate — the **Detune product**. Doubling the rate at fixed depth doubles the detuning, counterintuitive at any control surface. Because the product is the quantity of interest, it gets its own one-dimensional sweep axis rather than a depth × rate grid; the catalogs stay strictly one-axis-at-a-time.

Excursion is **symmetric** about the nominal resolved delay. That keeps mean delay equal to nominal, so the gains solved in Stage 4 stay centred rather than biased, and it matches Δ ≈ 2πfA being a peak deviation about a centre.

### Decorrelation and shape

Per-channel LFOs are decorrelated in phase **and** in rate. Channels moving together make the whole tail wobble in pitch, a distinct and usually unwanted effect, and phase alone cannot prevent it for a periodic shape at a shared rate: the channels stay in fixed phase relationship forever and a coherent component survives summing. Each Channel's rate therefore carries a fixed ±10% seeded spread. That spread is a documented constant, not a parameter — a knob whose only purpose is to make an invariant true should not exist, and promoting a constant to a parameter later is cheap while the reverse is not.

`sine` is periodic, and at higher depths that periodicity becomes audible as regular wobble. `smoothed-random` — band-limited noise — models air movement more convincingly and has no period to lock onto.

Concretely, `smoothed-random` is Catmull-Rom interpolation between targets drawn uniformly in [−1, +1], a new target every 1/`rateHz`. This keeps `rateHz` meaning one thing across all three shapes, so a shape comparison at fixed rate compares the shapes and nothing else, and it makes the trajectory a pure function of an integer target counter rather than of accumulated filter state — the property cross-platform reproduction needs.

Trajectories are **not pinned** at the start of a render. Starting every Channel at the same value would make them momentarily coherent, which is the exact failure decorrelation exists to prevent, and would make `smoothed-random` behave unlike `sine`, whose decorrelated phases already start Channels apart. The delay lines start silent, so there is no onset transient to protect against.

### What it costs

The system is now time-varying, so it is not strictly energy-preserving, and the gains solved in Stage 4 against nominal delay lengths become approximate. Negligible at realistic depths — measured and reported as RT60 deviation from nominal rather than corrected.

---

## Parameters

```json
{
  "type": "feedback-loop",
  "modulation": {
    "depthMs": 0.4,
    "rateHz": 0.7,
    "shape": "smoothed-random",
    "channelFraction": 1.0,
    "interpolation": "lagrange3"
  }
}
```

| Parameter | Value | Notes |
|---|---|---|
| `depthMs` | finite, ≥ 0 | Peak Excursion above and below nominal. 0 disables. |
| `rateHz` | finite, ≥ 0 | Multiplies with depth for perceived detune. 0 freezes the offset — the interpolation-isolating ablation. |
| `shape` | `smoothed-random` / `sine` / `triangle` | |
| `channelFraction` | 0–1 | Proportion of Channels modulated, rounded up. 0 disables. |
| `interpolation` | `lagrange3` / `linear` / `allpass` | Property of the delay line. |

`modulation` is optional wherever it appears. Omission disables it and preserves existing format-version-1 behavior byte-for-byte, including the Resolved Configuration's own bytes. When the object is present, omitted fields resolve to the values above: a meaningful research baseline rather than a product default, and deliberately not a neutral one. Explicit zero depth remains available for identity experiments.

The modulated Channels are the first ⌈`channelFraction` × N⌉ of a positionally seeded permutation, so raising the fraction never reshuffles Channels that were already modulating and selection stays independent of delay ordering — which matters, since the parameter exists to test what the matrix does. Any non-zero fraction modulates at least one Channel; a fraction that silently rounded to none would be the kind of quiet no-op this project rejects elsewhere.

---

## In code

```
DelayLine
  storage      : one contiguous allocation, all N Channels
  readInteger  : unmodulated path
  readFraction : interpolated path

Modulation
  trajectories : per-channel, decorrelated phase and rate
  depthSamples : resolved at configuration
```

Interpolation belongs to `DelayLine`, which exposes both an integer and a fractional read; unmodulated stages use the first and pay nothing. `DelayLine` owns storage, wrap-around indexing and the two reads — but not read/write ordering, which stays explicit in the Feedback Loop's own loop, because that ordering is the one place the signal flow runs backwards and hiding it inside an abstraction is what the loop's implementation deliberately avoids.

`DelayLine` is constructed with a resolved length and never derives its own; sizing lives in resolution, where it is both recorded evidence and a rejection reason.

Modulation is a concrete, cohesive component, not a generic LFO or interpolation framework. Like Damping, its requested configuration, resolution, resolved evidence, DSP state and tests stay visibly separated from the mechanics of the stage that holds it.

---

## What this forces on the architecture

**`DelayLine` gains a second read path** — the only component in the design serving two stages with different requirements. It has to be extracted first: today the Feedback Loop and Diffusion Step each carry their own copy of the same delay machinery.

**Delay buffers need headroom.** Allocated length is nominal delay plus peak Excursion plus a fixed Interpolation margin. Sizing from the nominal alone produces an overrun at the modulation peak — intermittent, depth-dependent, unpleasant to debug. The margin is sized for the worst case across all three interpolation methods rather than for the configured one, so DSP-owned memory does not move when interpolation changes; otherwise an interpolation sweep would compare footprint alongside sound and cost. Headroom is added only where Modulation is configured, so unmodulated renders keep their existing buffer sizes and resolved bytes exactly.

**The Block-size bound becomes modulation-aware.** Stage 4 derives it from the shortest resolved per-Channel delay; a symmetric Excursion makes the shortest *instantaneous* delay smaller than that. The bound is re-derived against the Excursion, and any Channel whose resolved delay less Excursion does not exceed the Interpolation margin is rejected before construction, naming the responsible parameter. The same rule applies to Diffusion Steps, which have no Block-size bound but do have short delays.

**Trajectories are seeded positionally**, by the Stage 2 rule, so changing `channelFraction` doesn't reshuffle channels that were already modulating. Phase, rate spread and Channel selection each get their own usage tag, per target family.

**Identity is guaranteed by construction, not by arithmetic.** Zero depth, and Channels excluded by `channelFraction`, resolve to a bypass that skips the fractional read, the trajectory evaluation and any interpolator state entirely. Third-order Lagrange at a zero fractional offset would collapse arithmetically; allpass carries state and never would. One rule that holds for all three beats a rule with an exception.

**The Tail budget does not change.** The perturbation sits far inside the existing `decayMargin`, and leaving the budget untouched is what lets an omitted-Modulation render and a zero-depth render be compared byte for byte.

---

## Measurement

Movement gets its own analysis, published beside the tail evidence rather than folded into it: tail analysis predicts decay from *static* resolved values, and that contract is exact and worth keeping strict.

| Evidence | What it answers |
|---|---|
| Bounded energy | Did movement perturb the tail without growing it? |
| **Decay tilt** | Did interpolation quietly act as Damping? |
| Coherent pitch movement | Did movement survive into the output as pitch wobble? |
| **Output correlation** | Are the two outputs moving together? |
| RT60 deviation from nominal | How far did movement push decay from what was solved? |

Decay tilt is the slope of measured per-octave-band T30 against log-frequency. Measuring the *decay* curve rather than the static spectrum is the point: the substitution being feared is of Damping, which is frequency-dependent decay, and expressing the tilt in Damping's own ratio units makes "this interpolation method behaved like a `highRatio` of 0.85" a statement you can defend.

Everything here is reported, never enforced. A deviation past its threshold is flagged as significant; no measurement rejects a render. Only structural impossibility — a moving delay its buffer cannot serve — is a rejection. See [ADR-0004](../../../adr/0004-validate-structure-not-acoustics.md).

Measurement happens at the Downmix output, not inside the loop: there is no Feedback Loop Stage capture boundary, and per-Channel decorrelation is instead guaranteed structurally by distinct positional seeds. That trade, and what it costs, is recorded in [ADR-0005](../../../adr/0005-measure-movement-at-the-output.md).

---

## Invariants

- **Identity at zero.** `depthMs: 0` gives output bit-identical to modulation disabled, by construction rather than by arithmetic.
- **No buffer overrun** at maximum depth, verified at the extreme of the permitted range and rejected before construction where it cannot be honoured.
- **Determinism.** Identical configuration gives identical trajectories, reproducible from integer counters rather than accumulated state.
- **Bounded energy.** Modulation perturbs but does not grow the tail.
- **Decorrelation.** No coherent frequency modulation at the LFO rate in the summed output. Per-Channel trajectory decorrelation is structural, from distinct seeds, rather than measured.
- **Reference behavior is reported, not enforced.** Decay tilt, RT60 deviation, Output correlation and pitch movement are flagged past their thresholds; none rejects a configuration.

---

## Worth sweeping early

The Reference for this sweep carries Damping at unity, so measured Decay tilt is attributable to interpolation and movement alone rather than to deliberate damping.

- `depthMs` at fixed rate, then `rateHz` at fixed depth, then the Detune product varied as one axis — demonstrates the product relationship and shows where realism ends.
- `interpolation` at high depth against Decay tilt — quantifies linear interpolation's hidden damping. The sweep most likely to produce something worth publishing, and `rateHz: 0` is the control that separates the interpolation from the movement.
- Modulating a Diffusion Step against the Feedback Loop at matched depth.
- `channelFraction` 0.25 / 0.5 / 1.0 — tests whether the matrix really does distribute detuning, and how fast, by watching the output metrics converge.
- `shape` `sine` against `smoothed-random` at high depth.
- One point re-enabling the Damping baseline, so the interaction is measured deliberately in one place instead of contaminating every other axis.
