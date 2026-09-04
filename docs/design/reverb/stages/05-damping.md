# Stage 5 — Damping

Shelving filters inside the feedback loop, giving frequency-dependent decay.

*Vocabulary, ground rules, and the overall shape of the chain are in the [reverb design overview](../README.md).*

---

## What it does musically

Real spaces absorb high frequencies faster than low ones. A tail that decays evenly across the spectrum sounds glassy; one that darkens as it fades sounds like a room.

This is the most effective control for making the reverb sit in a mix rather than on top of it — bright tails fight vocals and cymbals, damped tails sink underneath them. Low-frequency damping is the half people forget: small rooms shed bass quickly, and an undamped low end turns long tails to mud.

---

## Structural note

Damping is not a stage. It is a component of the feedback loop, placed immediately after the decay gain, and it has no meaning outside the loop.

A filter after the reverb output changes the tail's tonality once. A filter inside the loop changes the *rate of decay per band*, because the signal passes through it on every circulation. Only the second is damping.

---

## What it does mathematically

The decay gain sets level lost per loop; a filter after it makes that loss frequency-dependent, and therefore decay time frequency-dependent. Since RT60 is inversely proportional to dB lost per loop, for a reference loss *g* and an additional shelf *S*:

    RT60_band / RT60_ref = |g| / |g + S|

At g = −1.5 dB and S = −1.5 dB, highs decay in half the time.

### Parameterise by ratio, not shelf gain

The user asks for a **decay ratio** and the code solves the shelf:

    S = |g| · (1/ratio − 1),  applied negatively

Specifying shelf decibels instead would make the resulting decay time depend on the reference RT60 — the same damping setting behaving differently at 1s and 8s, which is the coupled-control problem that makes reverbs feel untunable. Because decay gain is per-channel, the solved shelf gain is per-channel too.

The ratios describe the low- and high-frequency shelf plateaus relative to the undamped Reference-band loss. One-pole transitions are gradual, so the two shelves can influence each other and the 1 kHz octave slightly. Resolution does not run a coupled compensation solve: the rendered octave-band curve is accepted within the measurement tolerances below.

`lowHz` and `highHz` are half-gain frequencies: a shelf whose plateau is −6 dB is approximately −3 dB at its corner. The exact deterministic coefficient form is chosen and documented with the implementation; the observable contract is a prewarped first-order digital shelf with this half-gain convention. Its resolved coefficients are part of the Resolved Configuration.

### Stability

A ratio above 1.0 means slower decay in that band, requiring a shelf that boosts. Stability uses a cheap conservative structural proof rather than a frequency grid or complete FDN pole solve. For both float32 and float64 realised coefficients, resolution bounds one circulation from the quantised matrix error, decay gain, and each monotonic shelf's maximum plateau magnitude. Every Channel's bound must remain strictly below unity. This may reject an overlapping boost-and-cut combination that an exact modal analysis could prove safe; safe rejection is preferred to a more complex or sampled stability claim.

Shelf corners may appear anywhere strictly between 0 Hz and Nyquist, including overlapping or crossed layouts; there is no conventional corner order to enforce. Every rejection names the responsible Damping parameter and reason; values are never clamped.

The 1 kHz center response is solved and recorded (`expectedReferenceRt60Sec`) but its distance from `rt60Sec` is not itself a rejection reason (see ADR-0004): a gentle one-pole shelf's transition band is wide by design, so even the research-baseline default (`highRatio` 0.5 at 4 kHz) leaks enough into 1 kHz to imply an RT60 more than 5% from `rt60Sec`, at any loop time or `rt60Sec` scale -- a real, verified property of the filter, not a bug (ADR-0004 documents the three-way check that confirmed this). The render is stable and does exactly what was requested either way, so nothing here is unsafe or silently substituted; a future analysis or report layer flags a deviation past 10% as significant rather than resolution rejecting it.

### Filter choice

One-pole shelves. Gentle slopes, minimal ringing, negligible phase disturbance relative to the 100ms+ delays around them. Steeper filters colour the tail and defeat the purpose. Their group delay is ignored in the RT60 solve; the error is far below the ±5% tolerance.

---

## Parameters

```json
{
  "type": "feedback-loop",
  "damping": {
    "highRatio": 0.5,
    "highHz": 4000,
    "lowRatio": 1.0,
    "lowHz": 200
  }
}
```

| Parameter | Value | Notes |
|---|---|---|
| `highRatio` | finite, > 0 | Decay time above `highHz`, relative to `rt60Sec`. 1.0 = none, 0.5 = half. |
| `highHz` | finite, 0 < value < Nyquist | Half-gain shelf corner. |
| `lowRatio` | finite, > 0 | Decay time below `lowHz`, relative to `rt60Sec`. |
| `lowHz` | finite, 0 < value < Nyquist | Half-gain shelf corner. |

Ratios above 1.0 are permitted where stable — unnatural, occasionally useful, and the stability check decides.

`damping` is optional inside the Feedback Loop configuration. Omission disables it and preserves existing format-version-1 behavior. When the object is present, omitted fields resolve to the values above: a meaningful research baseline rather than a product default. Explicit unity ratios remain available for identity experiments.

---

## In code

```
Damping
  highShelf : per-channel one-pole
  lowShelf  : per-channel one-pole
```

Held by `FeedbackLoop`, applied after the gain and before mixing. State is per-channel and cleared at configuration.

This is a concrete, cohesive component, not a generic filter graph or strategy framework. Requested Damping configuration, resolution, resolved coefficients and evidence, DSP state, and tests stay visibly separated from Feedback Loop mechanics so a future refinement or replacement remains local without prebuilding an abstraction for it now.

Damping allocates its per-Channel state at configuration and nothing while audio flows. Its state and coefficient storage count toward the Feedback Loop's DSP-owned memory.

---

## What this forces on the architecture

**Global RT60 stops being meaningful.** With damping active there is no single decay time, only a curve per band. Analysis must compute RT60 per octave band from the start; a broadband number would describe nothing.

**Validation gains a solved-value check.** Unlike matrix validity, this cannot be checked from parameters alone — the shelf must be solved and its response evaluated. Validation therefore runs after derivation.

**Tail budget follows the slowest resolved decay.** Resolution records the slower of the conservative feedback-decay bound and the shelves' state-settling time, then applies the Feedback Loop's existing `decayMargin`.

**Damping is not shimmer.** Shelves create no pitch-shifted energy or new frequencies. A stable ratio above 1.0 can make a tail brighten as it decays, but shimmer requires a separate pitch-shifted feedback path.

---

## Invariants

- **Identity at unity.** Both ratios at 1.0 give output bit-identical to damping disabled. The cheapest test in the project, and it catches most shelf-solving errors.
- **Reference accuracy is reported, not enforced.** Resolution records the solved 1 kHz center's implied RT60 and the rendered octave-band T30 measures it; a deviation past 10% from `rt60Sec` is flagged as significant in analysis and reports, but no Damping request is rejected for missing the Reference band alone (see ADR-0004) -- a gentle one-pole shelf's wide transition band makes even the research-baseline default land a few percent off.
- **Ratio accuracy.** Tail analysis derives each Channel's expected octave-band decay from its loop time, gain, and shelf response. Under `per-channel` gain mode, measured RT60 follows the narrow common target within ±10%; under `uniform`, it falls within the predicted per-Channel range plus the same tolerance.
- **Stability.** The conservative one-circulation bound is strictly below unity after both float32 and float64 coefficient quantisation.
- **No self-oscillation.** A long burst response remains finite and has no sustained or growing late-energy trend. Local increases from modal beating are permitted.
- **Complete response.** The Tail budget covers the slowest resolved feedback or filter-state decay before applying `decayMargin`.

---

## Worth sweeping early

- `highRatio` 1.0 → 0.2 at fixed RT60 — the decay-curve surface, and the clearest demonstration of why per-band measurement was necessary.
- `highHz` across 1k–10k at fixed ratio — separates "how much darker" from "where the darkening starts".
- `lowRatio` below 1.0 — the small-room effect.
