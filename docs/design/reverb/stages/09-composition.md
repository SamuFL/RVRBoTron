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
3. **Derive** — compute everything the DSP actually needs: step lengths from the distribution, per-channel delay times from the seed, per-channel decay gains from RT60, shelf gains and first-order coefficients from damping ratios, float32/float64 contraction evidence, the slowest resolved decay, Downmix rows and compensation, nominal and modulated Tap support bounds, tap shaping gains, and buffer sizes including modulation headroom.
4. **Validate** — check both parameters and derived values, then construct.

**Validation runs after derivation, not before.** Some rules can be checked from parameters directly (Hadamard requires a power-of-two N); others require solving first (damping stability depends on the computed shelf response). Splitting validation across two phases would mean two places to look when something is rejected.

Validation is total: every rejection names the offending parameter and why. A research tool that fails with "invalid configuration" is worse than one that crashes.

### Ordered stage data

`composition.stages` is an ordered array of typed stage objects describing the
Main wet path. Format version 2 preserves the empty array as exact identity.
The valid serial shapes remain:

```text
[]
[split, downmix]
[split, diffuser, downmix]
[split, feedback-loop, downmix]
[split, diffuser, feedback-loop, downmix]
```

A Feedback Loop sits in series after a Diffuser and never inside it, so the four-stage shape only ever admits that one middle order.

Impossible ordering or signal dimensions are rejected rather than repaired implicitly. Stage objects use the `type` discriminator and do not require user-authored IDs while each type is unique in the Composition.

An optional `composition.early` object describes a parallel Early Reflections
branch sourced from the unique Diffuser. It contains taps, branch controls, and
its dedicated Downmix. `composition.mainEnabled` and `mainLevelDb` control the
serial branch. An Early Reflections branch is valid only when a Diffuser exists;
it may accompany either a Diffuser-only or Diffuser→Feedback Loop Main wet
path.

```json
{
  "formatVersion": 2,
  "composition": {
    "mainEnabled": true,
    "mainLevelDb": 0,
    "stages": ["split", "diffuser", "feedback-loop", "downmix"],
    "early": {
      "enabled": true,
      "levelDb": -6,
      "decayDbPerSec": 0,
      "taps": [
        {"stepIndex": 0, "gainDb": 0},
        {"stepIndex": 1, "gainDb": 0}
      ],
      "downmix": {
        "strategy": "select",
        "leftChannel": 0,
        "rightChannel": 1,
        "widthDeg": 90,
        "normalisation": "energy"
      }
    }
  }
}
```

The abbreviated strings above stand only for the existing typed stage objects.
The full spatial-output Reference remains N=8 duplicate Split, the established
four-step 300 ms doubling Diffuser and Feedback Loop settings, and their
existing Modulation omission/empty-object semantics. Its explicit experiment
baseline adds the two taps above and an `orthogonal-rows`, 90°,
energy-normalized Main Downmix. These are Reference values for one-axis sweeps,
not product defaults.

Omitting `early`, `early: {}`, and `early: {"taps": []}` all mean no branch.
When taps are omitted or empty, specifying any of `enabled`, `levelDb`,
`decayDbPerSec`, or `downmix` is rejected at `/composition/early` because those
controls cannot affect sound. Non-empty taps plus `enabled: false` preserve
resolved branch structure for exact ablation.

For a non-empty Main wet path, `mainEnabled` defaults true and `mainLevelDb`
defaults 0 dB. Main Downmix defaults to `orthogonal-rows` when its source
includes a Feedback Loop and `select` otherwise; width defaults to 90° and
normalization to `energy`. A present Early Reflections branch defaults to
enabled, 0 dB branch level, 0 dB/s envelope slope, `select` Channels 0/1 (or
Channel 0 duplicated at N=1), 90° width, and energy normalization. Both
branches may be disabled deliberately, producing wet-only silence and a
reported `all-wet-branches-disabled` fact. Branch controls are invalid on the
empty identity Composition.

### Rules collected from the stages

| Rule | Source |
|---|---|
| Matrix must be valid for N | Overview |
| Damping must preserve the 1 kHz RT60 contract and remain conservatively contractive after float32 and float64 quantisation | Stage 5 |
| Downmix Alignment expectation must match its source path | Stage 8 |
| Tap indices must reference existing diffusion steps | Stage 7 |
| Selected Downmix Channel indices must be within `[0, N)` and distinct when both are present | Stage 8 |
| Delay buffers must cover nominal delay + symmetric modulation Excursion + a worst-case Interpolation margin, and every modulated Channel's resolved delay less that Excursion must exceed the margin | Stage 6 |
| Resolved step lengths must sum to `totalMs` | Stage 3 |
| Requested gains, levels, envelope slopes, and width must be finite and in their declared structural domains | Stages 7–8 |
| Derived tap gains, Downmix rows, compensation, and width matrices must be finite | Stages 7–8 |

Deliberately *not* rejected: ablations. `shuffle: false`, `delayStrategy: even`, `polarity: none`, `normalisation: none` all produce bad reverbs on purpose. The instrument must be able to produce the wrong answer on request.

That policy includes `sum-all` on aligned Early Reflections. Analysis tags it
as a Coherent Downmix ablation; a matched sweep report warns and compares its
peak factor and spectral deviation with `select`, but configuration does not
reject it.

---

## Seeding

One global seed. Every randomised quantity derives from it positionally — a pure function of the seed and the item's index — never from a shared sequential stream.

| Quantity | Derived from |
|---|---|
| Diffusion step delays, shuffle, polarity | seed, step index |
| Feedback loop delays | seed, channel index |
| Modulation trajectory phase, rate spread, and Channel selection | seed, usage, step index, channel index |
| Modulation `smoothed-random` targets | the same, plus a target counter |
| Random orthogonal matrices | seed, usage site |
| Main `orthogonal-rows` Downmix | seed, `main-downmix` usage site |
| Early `orthogonal-rows` Downmix | seed, `early-downmix` usage site |

This is what makes the step-count sweep mean anything: step 2 gets identical delays whether the chain has three steps or thirty. Retrofitting it invalidates every render already made.

Format version 2 preserves every format-version-1 positional result for
existing quantities and adds only the two domain-separated Downmix usage sites. See
[ADR-0002](../../../adr/0002-version-positional-random-resolution.md).

---

## resolved.json

Every render emits its fully resolved configuration beside the audio: input and output Channel counts, actual step lengths, per-channel delay times in samples and milliseconds, resolved permutations and polarity, every matrix coefficient, Damping ratios and corners, per-Channel shelf gains and coefficients, the 1 kHz response result, per-precision contraction bounds and margins, shelf-state settling evidence, the slowest resolved decay, buffer sizes, and the config format version. Version 2 additionally records canonical tap indices, nominal and modulated Tap support, nominal-endpoint-based tap shaping gains, branch enablement and gains, both 2×N Downmix row sets, compensation, derived Alignment expectations, the derived Coherent Downmix ablation tag, and both 2×2 width matrices. Coefficients are serialized as binary64-round-trippable JSON numbers so one Resolved Configuration gives float and double DSP the same structure.

Format version 2 intentionally does not load Requested or Resolved reverb
configuration version 1. The exact error is:

```text
/formatVersion: reverb configuration format 1 is unsupported by this build; use tag format-v1-final (commit 8a4e718) to render or analyze format-1 configurations
```

Unrelated render, catalog, and analysis artifact schema versions remain
independent. See
[ADR-0006](../../../adr/0006-format-v2-compatibility-boundary.md).

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

Version 2 requires an explicit `formatVersion`. Missing versions, version 1,
and unsupported future versions fail at `/formatVersion`; the version-1 error
names the compatibility tag and exact commit. Rejection lands atomically with
usable version-2 parsing and resolution plus the matching README request-field
reference rather than creating a commit that can load no documented
configuration.

**Research evidence stays outside sonic configuration.** `--capture-stages all` is a renderer option recorded in `render.json`, not part of Requested or Resolved Configuration. It writes manifested multi-Channel Stage captures through an optional capture-sink seam on `Reverb`; see [ADR-0003](../../../adr/0003-capture-internal-stage-evidence.md).

Optional `early-stereo` and `main-stereo` capture boundaries occur after all
branch-local tap shaping, Downmix, width, and branch gain, immediately before
summation. Their sample-wise sum is the stereo wet output. Analysis uses them
to report both branch energies and the cross term implied by combined output.
Explicitly capturing a disabled branch writes a correctly sized zero capture;
otherwise a disabled branch produces no capture file.

**Total drain is authorised automatically.** The renderer feeds silence for the resolved Tail budget after source EOF: a Diffuser's own finite response, a Feedback Loop's Tail budget, or their sum when a Diffuser and a Feedback Loop are both present. `render.json` keeps `frames` as output length and adds `inputFrames`; Stage captures share the final output timeline.

---

## Invariants

- **Round trip.** Rendering from a `resolved.json` reproduces the original output bit-identically.
- **Validation completeness.** Every rejection names a parameter and a reason.
- **Bypass identity.** `wetOnly: false, wetDb: −∞` returns the dry input unchanged, sample-aligned.
- **Pre-delay accuracy.** Wet onset is delayed by exactly `predelayMs`, within one sample.
- **Identity remains explicit.** `formatVersion: 2` with
  `composition.stages: []` is a valid exact-identity render.
- **Order independence.** Key order in the input JSON does not affect the resolved output.
- **Tap order independence.** A unique tap set is sorted by `stepIndex`, so
  Requested list permutations resolve and render identically.
- **Branch superposition.** Combined wet output equals `early-stereo` plus
  `main-stereo` sample-for-sample.

---

## Worth sweeping early

- `predelayMs` 0–80ms on a dry vocal — the source/space separation cue, and the single most useful parameter for making the reverb usable in a mix.
- `wetDb` at fixed everything else — establishes the reference level for every later comparison.
