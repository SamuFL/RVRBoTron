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

### Stability

A ratio above 1.0 means slower decay in that band, requiring a shelf that boosts. If the boosted response exceeds unity at any frequency the loop no longer contracts. Solve the shelf, verify |H(ω)| < 1 across the spectrum for every channel, and reject at load if it fails — do not silently clamp.

### Filter choice

One-pole shelves. Gentle slopes, minimal ringing, negligible phase disturbance relative to the 100ms+ delays around them. Steeper filters colour the tail and defeat the purpose. Their group delay is ignored in the RT60 solve; the error is far below the ±5% tolerance.

---

## Parameters

```json
"damping": {
  "highRatio": 0.5,
  "highHz": 4000,
  "lowRatio": 1.0,
  "lowHz": 200
}
```

| Parameter | Value | Notes |
|---|---|---|
| `highRatio` | > 0 | Decay time above `highHz`, relative to `rt60Sec`. 1.0 = none, 0.5 = half. |
| `highHz` | > 0 | Shelf corner. |
| `lowRatio` | > 0 | Decay time below `lowHz`, relative to `rt60Sec`. |
| `lowHz` | > 0 | Shelf corner. |

Ratios above 1.0 are permitted where stable — unnatural, occasionally useful, and the stability check decides.

---

## In code

```
Damping
  highShelf : per-channel one-pole
  lowShelf  : per-channel one-pole
```

Held by `FeedbackLoop`, applied after the gain. State is per-channel and cleared at configuration.

---

## What this forces on the architecture

**Global RT60 stops being meaningful.** With damping active there is no single decay time, only a curve per band. Analysis must compute RT60 per octave band from the start; a broadband number would describe nothing.

**Validation gains a solved-value check.** Unlike matrix validity, this cannot be checked from parameters alone — the shelf must be solved and its response evaluated. Validation therefore runs after derivation.

---

## Invariants

- **Identity at unity.** Both ratios at 1.0 give output bit-identical to damping disabled. The cheapest test in the project, and it catches most shelf-solving errors.
- **Ratio accuracy.** Measured per-band RT60 equals `ratio × rt60Sec` within ±10% — wider than the broadband case because band-limited decay estimates are noisier.
- **Stability.** |H(ω)| < 1 for every channel at every frequency; violations rejected at load, naming the parameter.
- **No self-oscillation.** 60s after a burst, decay remains monotonic.

---

## Worth sweeping early

- `highRatio` 1.0 → 0.2 at fixed RT60 — the decay-curve surface, and the clearest demonstration of why per-band measurement was necessary.
- `highHz` across 1k–10k at fixed ratio — separates "how much darker" from "where the darkening starts".
- `lowRatio` below 1.0 — the small-room effect.
- `highRatio` above 1.0 until validation refuses — worth knowing where the boundary is.
