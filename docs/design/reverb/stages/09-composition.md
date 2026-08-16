# Stage 9 — Composition

How stages 1–8 are assembled, resolved, and validated from configuration.

*Vocabulary, ground rules, and the overall shape of the chain are in the [reverb design overview](../README.md).*

---

## What it owns

Three things no individual stage can own:

- **Assembly** — which stages exist, their settings, and the wiring between them.
- **The wet path envelope** — pre-delay and dry/wet, which apply to the chain as a whole.
- **Resolution and validation** — turning a requested configuration into concrete delay times, gains, and filter coefficients, then rejecting the impossible ones.

---

## Pre-delay and dry/wet

**Pre-delay** shifts the entire wet path later, including early reflections. It models distance from every surface at once and is the strongest cue for separating a source from its space — the reason a vocal with 30ms pre-delay sits in front of the reverb rather than inside it. A single delay before Split. Range 0–200ms.

**Dry/wet** is a mix, not a crossfade. Constant-power crossfading is wrong here: the wet signal is largely decorrelated from the dry, so their energies add rather than interfere. Independent gains, both expressed in dB, with a `wet-only` flag for send-style use.

```json
"predelayMs": 20,
"dryDb": 0,
"wetDb": -6,
"wetOnly": false
```

---

## The resolution pipeline

Configuration is processed in four phases, in this order:

1. **Parse** — read JSON into `ReverbConfig`. Structural errors only.
2. **Default** — fill omitted fields. Every field has a documented default; nothing is implicit.
3. **Derive** — compute everything the DSP actually needs: step lengths from the distribution, per-channel delay times from the seed, per-channel decay gains from RT60, shelf gains from damping ratios, downmix compensation, tap times, buffer sizes including modulation headroom.
4. **Validate** — check both parameters and derived values, then construct.

**Validation runs after derivation, not before.** Some rules can be checked from parameters directly (Hadamard requires a power-of-two N); others require solving first (damping stability depends on the computed shelf response). Splitting validation across two phases would mean two places to look when something is rejected.

Validation is total: every rejection names the offending parameter and why. A research tool that fails with "invalid configuration" is worse than one that crashes.

### Rules collected from the stages

| Rule | Source |
|---|---|
| Matrix must be valid for N | Overview |
| Damping shelf must keep \|H(ω)\| < 1 for every channel | Stage 5 |
| Summing downmix strategies are rejected on aligned input | Stage 8 |
| Tap indices must reference existing diffusion steps | Stage 7 |
| Delay buffers must cover nominal delay + modulation depth + interpolation margin | Stage 6 |
| Resolved step lengths must sum to `totalMs` | Stage 3 |

Deliberately *not* rejected: ablations. `shuffle: false`, `delayStrategy: even`, `polarity: none`, `normalisation: none` all produce bad reverbs on purpose. The instrument must be able to produce the wrong answer on request.

---

## Seeding

One global seed. Every randomised quantity derives from it positionally — a pure function of the seed and the item's index — never from a shared sequential stream.

| Quantity | Derived from |
|---|---|
| Diffusion step delays, shuffle, polarity | seed, step index |
| Feedback loop delays | seed, channel index |
| Modulation LFO phases | seed, channel index |
| Random orthogonal matrices | seed, usage site |

This is what makes the step-count sweep mean anything: step 2 gets identical delays whether the chain has three steps or thirty. Retrofitting it invalidates every render already made.

---

## resolved.json

Every render emits its fully resolved configuration beside the audio: actual step lengths, per-channel delay times in samples and milliseconds, decay gains, shelf coefficients, tap arrival times, buffer sizes, and the config format version.

Two reasons. Analysis needs to know what was built rather than what was requested — a doubling distribution over four steps is not something to recompute by hand when reading a plot weeks later. And a resolved configuration re-renders identically, which makes any past experiment reproducible without the original request file.

---

## In code

```
ReverbConfig     : POD, mirrors the JSON, requested values
ResolvedConfig   : POD, derived values, serialised to resolved.json
Reverb           : owns the stages, built from ResolvedConfig
```

`Reverb` never sees the requested configuration and never parses anything. Parsing lives in `cli/`; `dsp/` accepts `ResolvedConfig` only. This is what keeps the dependency rule intact and what makes the iPlug2 lift a copy — the plugin will resolve from its own parameter set rather than from JSON, and nothing in `dsp/` needs to change.

---

## What this forces on the architecture

**Two-phase construction throughout.** Every stage takes resolved values and allocates in `configure`. No stage computes its own delay times, gains, or buffer sizes; those arrive already decided. This is what makes the allocation-free invariant checkable, and it means a stage can be tested with hand-written resolved values and no configuration machinery at all.

**The config format is versioned from the first commit.** Format changes are certain, renders accumulate, and an unversioned `resolved.json` becomes unreadable the first time a field is renamed.

---

## Invariants

- **Round trip.** Rendering from a `resolved.json` reproduces the original output bit-identically.
- **Validation completeness.** Every rejection names a parameter and a reason.
- **Bypass identity.** `wetOnly: false, wetDb: −∞` returns the dry input unchanged, sample-aligned.
- **Pre-delay accuracy.** Wet onset is delayed by exactly `predelayMs`, within one sample.
- **Defaults are total.** An empty configuration object resolves to a valid, documented, renderable reverb.
- **Order independence.** Key order in the input JSON does not affect the resolved output.

---

## Worth sweeping early

- `predelayMs` 0–80ms on a dry vocal — the source/space separation cue, and the single most useful parameter for making the reverb usable in a mix.
- `wetDb` at fixed everything else — establishes the reference level for every later comparison.
