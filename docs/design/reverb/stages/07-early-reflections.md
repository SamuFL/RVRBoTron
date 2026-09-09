# Stage 7 — EarlyReflections

Taps out of the diffuser, bridging the gap before the tail arrives.

*Vocabulary, ground rules, and the overall shape of the chain are in the [reverb design overview](../README.md).*

---

## What it does musically

The feedback loop's shortest delay is the earliest moment any tail energy exists. At 100ms that leaves a conspicuous silence between the dry sound and the reverb, and the result reads as bolted on — a wash behind the source rather than a space containing it.

Real rooms fill that window with reflections off nearby surfaces, and those carry most of the spatial information. Two perceptual facts shape the design:

- Reflections arriving within roughly 50ms **fuse with the direct sound**, registering as presence and position rather than as echoes. Past roughly 80ms they detach into audible repeats.
- The **ratio of early energy to direct sound** is a dominant distance cue. Stage
7 controls Early Reflections against the Main wet path; the final Composition
milestone adds the dry level and pre-delay needed to expose the complete
distance relationship.

The useful window is about 5–80ms, and it does the spatial work.

---

## Where the taps come from

The diffuser already generates a dense, decorrelated set of echoes on exactly this timescale, so tapping it is free.

**The tap point is the character control.** After one step the signal holds N echoes — sparse, slap-back-like. After three it holds N³ — dense and smooth, closer to ambience. Early taps give hard surfaces, late taps soft ones.

Multiple taps with individual gains shape the early envelope. With a `doubling` distribution the early steps are short, so taps after steps 1 and 2 land naturally in the 5–40ms window without separate delay times.

---

## What it does mathematically

A parallel path: tapped signal scaled and summed into the output alongside the tail, never re-entering the diffuser or loop.

### Alignment matters here

The diffuser's output is **aligned** — all N channels carry echoes at the same times, differing only in sign. Summing all N reinforces coherently and produces a peaky, comb-flavoured result.

The ordinary strategy is therefore to **take one or two Channels**. The mixing
has already distributed every echo into every Channel, so a single Channel
holds the complete pattern; two different Channels give a stereo pair whose
sign patterns differ. `sum-all` remains available as a Coherent Downmix
ablation. It is structurally valid and never rejected merely because it may
sound coloured; analysis tags and measures the experiment.

The feedback loop's output is unaligned, so summing is safe there. Two signals, two rules — which is why alignment is tracked.

### Early envelope

Early Reflections have no feedback RT60. Their **Early envelope** is shaped by
the tap positions and gains:

    tapShapeDb = tap.gainDb - decayDbPerSec × nominalSupportEndSec

`decayDbPerSec` is finite and non-negative. Zero leaves the automatic envelope
flat; signed per-tap offsets can still form arbitrary or rising envelopes. The
N-Channel taps are shaped and summed, Downmixed once, then `levelDb` is applied
once to the stereo branch. The nominal endpoint deliberately drives shaping:
changing Modulation may move possible or measured support, but never changes a
tap's resolved envelope gain.

Not all-pass and not claiming to be. The exact invariant is superposition:
combined output equals the sample-wise sum of separately captured Early
Reflections and Main wet path contributions. Their scalar energies need not add
because overlapping signals have a cross term.

### Tap support

A tap has no single arrival time. For a tap at zero-based `stepIndex`, Resolved
Configuration records:

- nominal support bounds from the summed minimum and maximum nominal delays
  through that step;
- conservative modulated support bounds including every contributing
  Modulation's Excursion and interpolation stencil reach; and
- the cumulative nominal endpoint as a plot landmark.

Analysis records measured first and last non-zero samples, plus peak and
centroid when useful. Cancellation may make measured support narrower than its
structural bound.

---

## Parameters

```json
"early": {
  "enabled": true,
  "levelDb": -6,
  "decayDbPerSec": 0,
  "taps": [
    { "stepIndex": 0, "gainDb": 0 },
    { "stepIndex": 1, "gainDb": 0 }
  ],
  "downmix": {
    "strategy": "select",
    "leftChannel": 0,
    "rightChannel": 1,
    "widthDeg": 90,
    "normalisation": "energy"
  }
}
```

| Parameter | Value | Notes |
|---|---|---|
| `enabled` | boolean | Exact branch ablation; resolved structure remains recorded. |
| `levelDb` | finite number | Early level against the Main wet path. |
| `decayDbPerSec` | finite number ≥ 0 | Automatic Early-envelope slope. |
| `taps` | list | Unique zero-based step indices and gain offsets. Empty or omitted means no branch. |
| `stepIndex` | 0…k−1 | The character control. |
| `gainDb` | finite number | This tap's offset before the branch level. |
| `downmix` | object | Dedicated aligned-source Downmix; defaults to Channels 0/1 selection. |

Pre-delay is not this: it shifts the whole wet path including the early reflections, and belongs to Stage 9.

---

## In code

```
EarlyReflections
  taps        : canonical step index, resolved shaping gain
  accumulator : one N-Channel frame
```

---

## What this forces on the architecture

**The diffuser must expose intermediate outputs.** `Diffuser` can no longer be
a black box. `EarlyReflections` owns the canonically sorted tap set and one
N-Channel accumulator. The Diffuser's process call offers the completed
post-step frame to that caller-provided accumulator alongside its normal
output. No callback registration, observers, retained per-tap audio, or
allocation in processing. Taps are read-only and must not perturb the main
path.

**Tap timing is bounded, not specified as one arrival.** Nominal and modulated
Tap support are derived from the resolved Diffusion Steps and belong in
`resolved.json`. The existing per-step Stage capture supplies individual tap
audio when analysis needs it; DSP retains only the combined branch.

---

## Invariants

- **Identity when empty.** An empty tap list gives output bit-identical to early reflections disabled.
- **Non-interference.** The diffuser's own output is bit-identical with and without taps configured.
- **Tap support.** No tap energy occurs outside its conservative resolved
  support; measured support is reported separately.
- **Superposition.** Combined stereo output is sample-identical to the sum of
  its captured Early Reflections and Main wet path branches.
- **Stereo evidence.** Two-Channel `select` reports Output correlation and
  inter-channel level difference; no acoustic threshold rejects a render.

---

## Worth sweeping early

- `stepIndex` 0 through k−1 at fixed gain — tap depth as a character control.
- `levelDb` and `decayDbPerSec` independently — branch balance against Early-envelope shape.
- `select` against `sum-all` — makes the alignment problem audible rather than theoretical.
- Early reflections off entirely at long RT60 — hear the gap before deciding how much to fill it.
