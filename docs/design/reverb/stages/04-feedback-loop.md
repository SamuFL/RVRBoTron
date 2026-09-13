# Stage 4 — FeedbackLoop

Delays, decay gain, mixing matrix. Makes the sound long-lasting.

*Vocabulary, ground rules, and the overall shape of the chain are in the [reverb design overview](../README.md).*

---

## What it does musically

Two controls, perceptually independent — which is the point of separating them:

- **Delay range** reads as *size*. 20–60ms gives a small, boxy room; 150–300ms gives a hall. The closest thing the design has to a room-size control.
- **RT60** reads as *decay*. How long the tail lasts, independent of how big the space feels.

The shortest delay is also the gap between the dry sound and the first tail energy. At 100ms that gap is clearly audible; filling it is Stage 7's job.

The only stage that deliberately loses energy.

---

## What it does mathematically

Per sample:

```
  delayed  ← read from each delay line
  out      ← delayed
  fed back ← mix( delayed × gain )
  write( input + fed back )
```

Read before write. The signal flow runs backwards at one point, which is what makes feedback code awkward to read compared with the diffuser's straight line — worth a comment in the source rather than an abstraction that hides it.

### Solving RT60 into gain

For a channel with loop time *L* and target decay *R*:

    dB lost per loop = −60 · L / R
    gain             = 10^(−3L/R)

**Per-channel gain, not one shared gain.** With a shared gain a 100ms channel circulates twice as often as a 200ms one and decays twice as fast; the matrix averages this only partly, so measured RT60 drifts with the delay range. Deriving gain from each channel's own loop time makes all channels decay at the same rate and makes the requested RT60 measurable in the output. `uniform` remains available for comparison — it is what the reference design does — and solves one shared gain from the *mean* loop time across channels, chosen over an extreme so the resulting RT60 error is symmetric around the requested value rather than biased toward one end of the delay range.

### Stability

An orthogonal matrix has spectral radius 1, so scaling by *g* < 1 before it guarantees contraction. Stability follows from `gain < 1` and orthogonality with no further condition — the same structural argument that made the diffusion step all-pass.

### Delays and eigentones

The loop's delays set the modal structure of the tail; gaps in mode spacing are audible as coloration on long decays.

Avoid simple integer ratios between delays — they align echoes into flutter, and `segmented-random` over [min, max] handles this without prime-number tables. And don't over-mix: too much inter-channel mixing couples the delay lines into one unit, moving eigentones together and leaving gaps. Householder mixes mildly and is the default here, in contrast to the diffuser.

### Alignment is destroyed here

Unequal long delays, circulated repeatedly, leave each channel holding a different set of echo times. The diffuser's output is aligned; this stage's is not. Stage 8 needs both rules.

---

## Parameters

```json
"feedbackLoop": {
  "delayMinMs": 100,
  "delayMaxMs": 200,
  "delayStrategy": "segmented-random",
  "rt60Sec": 2.4,
  "mix": "householder",
  "gainMode": "per-channel",
  "silenceFloorDb": null
}
```

| Parameter | Value | Notes |
|---|---|---|
| `delayMinMs` / `delayMaxMs` | > 0 | Room size. Minimum also sets the pre-tail gap. |
| `delayStrategy` | `segmented-random` / `uniform-random` / `even` | `even` demonstrates flutter. |
| `rt60Sec` | > 0 | Requested decay; solved into gain at configuration. |
| `mix` | `householder` / `hadamard` / `random-orthogonal` | |
| `gainMode` | `per-channel` / `uniform` | |
| `silenceFloorDb` | dB, or disabled (default) | Runtime idle-behaviour seam; see below. Dormant until a later milestone enables it. |

**RT60 is defined at the reference band.** Once Stage 5 adds a shelf inside the loop, high frequencies decay faster by design; `rt60Sec` refers to the undamped band and damping describes deviation from it. Without this convention the two stages fight over the same number.

---

## In code

```
FeedbackLoop
  delays : per-channel delay lines
  gains  : per-channel decay gain
  mix    : MixMatrix
```

---

## What this forces on the architecture

**Block size is bounded by the shortest loop delay.** At a 100ms minimum there is no practical constraint, but the bound is real and must be computed rather than assumed — a research configuration with a 5ms minimum is legitimate and would silently break block processing. This is also where the diffuser's feedback-free structure pays off: the constraint applies to this stage alone.

**Modulation perturbs RT60.** Stage 6 varies delay lengths at runtime, so gains solved against nominal lengths become approximate — negligible at realistic depths, not at extreme ones. Recorded rather than corrected.

**The tail is kept numerically clean by an explicit flush, not a CPU mode.** Every feedback write is compared against the smallest normal magnitude for the build's `Sample` type and zeroed below it. A CPU flush-to-zero mode would be platform-specific behavior inside the DSP, in conflict with the Cross-platform equivalence tolerances committed in [ADR-0001](../../../adr/0001-cross-platform-reproducibility.md); a plain comparison against a portable, deterministic constant instead cannot make supported platforms disagree. This flush is unconditional — always at the numerical floor — and independent of `silenceFloorDb` below.

**`silenceFloorDb` is the designed-in seam for runtime idle behavior, dormant here.** A plugin host has no input EOF and no drain to authorize; what it needs instead is a way to decide a tail has gone quiet enough to stop processing. `silenceFloorDb`, carried in the Resolved Configuration, is that seam: disabled (the default, `null`) leaves the write-path flush at its numerical floor, so a render is bit-identical to a build without the field at all. A future milestone may raise the flush to this audible threshold and let a drain terminate early once every Channel is below it — at that point the field starts changing rendered samples, which is exactly why it lives in the *Resolved* Configuration rather than as an implementation constant: a value that can change output belongs in the versioned, reproducible record, not hidden inside the DSP.

**Damping can extend the Tail budget.** Without Damping, the existing `rt60Sec × decayMargin` budget remains unchanged, including under the deliberately less-accurate `uniform` gain mode. With Damping, resolution uses the slower of the conservative feedback-decay bound and the shelves' own state-settling time. The existing `decayMargin` multiplies that resolved slowest RT60, preserving its meaning while preventing boosted bands or very low shelf corners from being truncated.

---

## Invariants

- **Decay accuracy.** Measured RT60 is within ±5% of `rt60Sec`, from 0.2s to 10s, damping disabled.
- **Stability.** `gain < 1` always decays; no channel grows over 60s.
- **Energy is lost deliberately.** The only stage that does not claim all-pass, and says so explicitly.
- **Uniform decay across channels** under `gainMode: per-channel`.
- **Unaligned output.** Echo times differ across channels.
- **No sub-normal persists.** A feedback-path magnitude is either exactly zero or at least the smallest normal value for the build's `Sample` type; never a denormal in between.
- **Identity while `silenceFloorDb` is disabled.** Output is bit-identical to a build without the field.

---

## Worth sweeping early

- Delay range at fixed `rt60Sec` — confirms size and decay are perceptually independent.
- `mix` across all three matrices at long RT60 — tests the eigentone-clustering claim, the one piece of reference advice given without a demonstration.
- `gainMode` `per-channel` against `uniform` — measure RT60 error, then listen.
- `delayStrategy: even` — flutter on purpose.
