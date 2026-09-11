# Configure the Composition

Every field you can put in a `request.json`, what it accepts, and what it
defaults to.

```bash
build/default/rvrbotron render \
  --input samples/listening/PianoDry.wav \
  --config request.json \
  --output build/result
```

Everything except `formatVersion` is optional — omit a field and the resolver
substitutes its default. Unknown fields are rejected anywhere in the request.

*Why the stages work the way they do lives in the [reverb design
corpus](../design/reverb/README.md); each table below links its stage.*

---

## Start from an example

### Identity — no processing

```json
{ "formatVersion": 2 }
```

Renders the input unchanged. `{"formatVersion": 2, "composition": {"stages": []}}`
is the same thing said explicitly.

### A diffuser

```json
{
  "formatVersion": 2,
  "seed": 42,
  "composition": {
    "stages": [
      { "type": "split", "channels": 8 },
      { "type": "diffuser", "steps": 4, "totalMs": 300 },
      { "type": "downmix", "strategy": "select", "leftChannel": 0, "rightChannel": 1 }
    ]
  }
}
```

Echo density without decay. Every value shown is already the default, so
`{"type": "split"}` and `{"type": "diffuser"}` render identically. The Downmix
is the exception: `leftChannel` must always be named, and dropping
`rightChannel` is not a shorthand — it mono-duplicates `leftChannel` instead of
picking Channel 1.

### A decaying tail

```json
{
  "formatVersion": 2,
  "composition": {
    "stages": [
      { "type": "split", "channels": 8 },
      { "type": "feedback-loop", "delayMinMs": 100, "delayMaxMs": 200, "rt60Sec": 2.4 },
      { "type": "downmix", "strategy": "select", "leftChannel": 0, "rightChannel": 1 }
    ]
  }
}
```

### Both — diffuser into loop

```json
"stages": [
  { "type": "split", "channels": 8 },
  { "type": "diffuser", "steps": 4, "totalMs": 300 },
  { "type": "feedback-loop", "rt60Sec": 2.4, "damping": { "highRatio": 0.5 } },
  { "type": "downmix", "strategy": "orthogonal-rows" }
]
```

Dense early energy feeding a damped tail. These four are the **only** valid
stage arrangements:

```text
[]                                        identity
[split, diffuser, downmix]
[split, feedback-loop, downmix]
[split, diffuser, feedback-loop, downmix]
```

### Early Reflections

```json
"composition": {
  "stages": [
    { "type": "split", "channels": 8 },
    { "type": "diffuser", "steps": 4, "totalMs": 300 },
    { "type": "downmix", "strategy": "select", "leftChannel": 0, "rightChannel": 1 }
  ],
  "early": {
    "levelDb": -6,
    "taps": [{ "stepIndex": 0 }, { "stepIndex": 1, "gainDb": -3 }]
  }
}
```

A parallel branch tapping the Diffuser, Downmixed on its own and summed into
the same stereo output. Needs a Diffuser in the Main path.

### Modulation — two sites

On the Feedback Loop, where movement compounds every circulation:

```json
{ "type": "feedback-loop", "rt60Sec": 2.4,
  "modulation": { "depthMs": 0.4, "rateHz": 0.7 } }
```

On a Diffusion Step, where the signal passes once so movement does not
compound:

```json
{ "type": "diffuser", "steps": 4,
  "step": { "modulation": { "depthMs": 0.05 } } }
```

`"modulation": {}` resolves the same baseline (`depthMs` `0.4`) at either site,
but note the depth above is much smaller. Diffusion Step delays are a fraction
of a Feedback Loop's, and a Channel whose delay cannot serve the requested
Excursion is rejected outright:

```text
invalid_configuration at /composition/stages/1/steps/0/modulation/depthMs:
Channel 0's resolved delay less Excursion does not exceed the fixed
Interpolation margin
```

If you hit that, lower `depthMs` or give the step more time (fewer `steps`, a
larger `totalMs`, or an explicit `lengthsMs`). Omitting `modulation` leaves the
stage unmodulated.

### Insert-style dry + wet

```json
"composition": {
  "stages": [ ... ],
  "preDelayMs": 20,
  "dryDb": 0,
  "wetDb": -6,
  "wetOnly": false
}
```

The default is `wetOnly: true` — wet output only, which is what every
Composition produced before the envelope existed. Set `wetOnly: false` to hear
dry and wet together.

---

## Top level

| Field | Values | Default |
| --- | --- | --- |
| `formatVersion` | `2` | *required* |
| `seed` | unsigned 64-bit integer | `0` |
| `composition.stages` | array — one of the four shapes above | `[]` (identity) |

`formatVersion` must be `2`. Version `1` fails with a message naming tag
`format-v1-final` (commit `8a4e718`), the last build that can render it — see
[ADR-0006](../adr/0006-format-v2-compatibility-boundary.md). `seed` drives
every seeded derivation: delays, shuffle, polarity, matrices, Modulation
trajectories.

## Composition envelope

Applies to the whole wet path. Every field here is rejected on an identity
Composition — there is no wet path for them to affect.
See [stage 09](../design/reverb/stages/09-composition.md).

| Field | Values | Default |
| --- | --- | --- |
| `composition.mainEnabled` | boolean | `true` |
| `composition.mainLevelDb` | finite number (dB) | `0` |
| `composition.preDelayMs` | finite number (ms), `0`–`200` | `0` |
| `composition.dryDb` | finite number (dB) | `0` |
| `composition.wetDb` | finite number (dB) | `0` |
| `composition.wetOnly` | boolean | `true` |
| `composition.early` | object, or omitted | omitted (no branch) |

- `mainEnabled: false` skips the Main Downmix and Width entirely, contributing
  exact stereo zero rather than a zero-multiplied value.
- `mainLevelDb` applies once after the Main Downmix, including Width.
- `preDelayMs` delays the wet path only; dry stays sample-aligned from frame
  zero.
- `wetDb` scales the complete Wet sum (Main plus Early) once, before dry is
  mixed in.
- `wetOnly: true` mutes dry exactly, regardless of `dryDb` — so `dryDb` stays
  legal and is preserved when you toggle the gate back off. `false` maps dry
  in: stereo channel-for-channel, mono duplicated to both channels without
  energy compensation.

## `split` stage

[Stage 01](../design/reverb/stages/01-split.md). Expands mono or stereo input
into N internal Channels.

| Field | Values | Default |
| --- | --- | --- |
| `channels` | unsigned 32-bit integer (N) | `8` |
| `strategy` | `"duplicate"` \| `"stereo-halves"` \| `"stereo-interleave"` | `"duplicate"` |
| `normalisation` | `"energy"` \| `"none"` | `"energy"` |

`"duplicate"` sums stereo to `(L + R)/√2` and feeds every Channel, discarding
stereo position. `"stereo-halves"` feeds the first half of the Channels from
the left input and the second half from the right; `"stereo-interleave"`
alternates by Channel index. Both preserve L/R and require an even N when the
source is stereo. For mono input all three resolve to the same mapping.

## `diffuser` stage

[Stage 03](../design/reverb/stages/03-diffuser.md). An ordered chain of
Diffusion Steps that builds echo density without decay.

| Field | Values | Default |
| --- | --- | --- |
| `steps` | unsigned 32-bit integer | `4` |
| `totalMs` | finite number | `300` |
| `distribution` | `"even"` \| `"doubling"` | `"doubling"` |
| `lengthsMs` | array of finite numbers, one per step | unset |
| `step` | object of per-step defaults (below) | — |
| `stepOverrides` | array of `{index, …}` | unset |

**`lengthsMs` is mutually exclusive with `steps`/`totalMs`/`distribution`.**
Use the first three to apportion one budget automatically — `"even"` splits
`totalMs` equally, `"doubling"` weights step *i* by `2^i` — or `lengthsMs` to
state every step's length yourself.

`step` sets the default for every step; `stepOverrides` changes individual
ones by zero-based `index` (unique, within `[0, steps)`), with unlisted fields
falling back to `step`:

| `step` / `stepOverrides` field | Values | Default |
| --- | --- | --- |
| `delayStrategy` | `"segmented-random"` \| `"uniform-random"` \| `"even"` | `"segmented-random"` |
| `mix` | `"hadamard"` \| `"householder"` \| `"random-orthogonal"` | `"hadamard"` |
| `shuffle` | boolean | `true` |
| `polarity` | `"seeded-random"` \| `"none"` | `"seeded-random"` |
| `modulation` | object, or omitted | omitted (off) |

`"hadamard"` mixes maximally and requires a power-of-two N; `"householder"`
and `"random-orthogonal"` accept any N. `"uniform-random"` samples each
Channel's delay with replacement, so it permits duplicate delays — the other
two strategies require N distinct positions.

A Diffuser is rejected before it allocates if its estimated DSP memory
(delay lines plus per-step mix matrices) exceeds the budget — 512 MiB by
default. Raise it for a large N or a long `totalMs`:

```bash
build/default/rvrbotron render --memory-budget-mib 2048 ...
```

## `feedback-loop` stage

[Stage 04](../design/reverb/stages/04-feedback-loop.md). Circulating delay
lines that give the tail its size and decay. This is the one stage that loses
energy on purpose, and its output is unaligned.

| Field | Values | Default |
| --- | --- | --- |
| `delayMinMs` / `delayMaxMs` | finite number > 0 | `100` / `200` |
| `delayStrategy` | `"segmented-random"` \| `"uniform-random"` \| `"even"` | `"segmented-random"` |
| `rt60Sec` | finite number > 0 | `2.4` |
| `decayMargin` | finite number > 0 | `1.5` |
| `mix` | `"hadamard"` \| `"householder"` \| `"random-orthogonal"` | `"householder"` |
| `gainMode` | `"per-channel"` \| `"uniform"` | `"per-channel"` |
| `silenceFloorDb` | finite number, or `null` | `null` (dormant) |
| `damping` | object, or omitted | omitted (off) |
| `modulation` | object, or omitted | omitted (off) |

- Delay range reads as room size; `delayMinMs` also sets the pre-tail gap.
  `delayMaxMs` must be at least `delayMinMs`.
- `rt60Sec` is the requested decay at the 1 kHz Reference band, solved into
  per-Channel gain at configuration time.
- `decayMargin` multiplies `rt60Sec` to derive the Tail budget — how many
  frames are rendered past input EOF. `1.5` ends the drain near −90 dB.
- `gainMode: "uniform"` solves one shared gain from the mean loop time
  instead of each Channel's own; measurably less accurate at a wide spread.
- `silenceFloorDb` is a reserved seam for the eventual plugin and does
  nothing here.

### `damping`

[Stage 05](../design/reverb/stages/05-damping.md). Independent low and high
shelves applied every circulation. `"damping": {}` resolves this baseline:

| Field | Values | Default |
| --- | --- | --- |
| `highRatio` | finite number > 0 | `0.5` |
| `highHz` | finite number, between `0` and Nyquist | `4000` |
| `lowRatio` | finite number > 0 | `1.0` |
| `lowHz` | finite number, between `0` and Nyquist | `200` |

Ratios are decay time relative to `rt60Sec` — `0.5` is half, `2.0` double.
A ratio of `1.0` bypasses that shelf, so unity on both renders identically to
`damping` omitted. Corners may sit in either order, including crossed.

Ratios above `1.0` boost, and are accepted only if a conservative
contraction certificate passes for every Channel at both float32 and
float64. A rejected boost is never silently clamped.

### `modulation`

[Stage 06](../design/reverb/stages/06-modulation.md). Seeded per-Channel delay
movement. Valid on the Feedback Loop and on a Diffusion Step, with the same
fields at both sites. `"modulation": {}` resolves this baseline:

| Field | Values | Default |
| --- | --- | --- |
| `depthMs` | finite number ≥ 0 | `0.4` |
| `rateHz` | finite number ≥ 0 | `0.7` |
| `shape` | `"smoothed-random"` \| `"sine"` \| `"triangle"` | `"smoothed-random"` |
| `channelFraction` | finite number in `[0, 1]` | `1.0` |
| `interpolation` | `"lagrange3"` \| `"linear"` \| `"allpass"` | `"lagrange3"` |

- `depthMs` is peak Excursion above and below each Channel's nominal delay.
  `0` is a bypass, bit-identical to omitting `modulation`.
- `rateHz` times `depthMs` is the perceived detuning. `0` freezes each
  Channel at a static offset — useful for isolating interpolation error from
  movement.
- `channelFraction` is the proportion of Channels modulated, rounded up; `0`
  disables the stage exactly like `depthMs: 0`.
- `linear` and `allpass` are deliberate ablations, kept so they can be heard
  against `lagrange3`, not because they are better.

A modulated Channel whose resolved delay is too short for its Excursion is
rejected up front, naming `modulation/depthMs`.

## `downmix` stage

[Stage 08](../design/reverb/stages/08-downmix.md). Maps the N Channels back
to stereo.

| Field | Values | Default |
| --- | --- | --- |
| `strategy` | `"select"` \| `"orthogonal-rows"` \| `"halves"` \| `"alternating"` \| `"sum-all"` | `"select"` |
| `leftChannel` | Channel index in `[0, N)` | *required for `select`* |
| `rightChannel` | Channel index in `[0, N)`, distinct from `leftChannel` | omitted (mono-duplicates `leftChannel`) |
| `normalisation` | `"energy"` \| `"none"` | `"energy"` |
| `widthDeg` | finite number in `[0, 180]` | `90` |

- `select` picks two Channels. `leftChannel` has no implicit default — name it.
- `orthogonal-rows` takes rows 0 and 1 of a seeded dense orthogonal matrix.
- `halves` puts the first `ceil(N/2)` Channels left and the rest right;
  `alternating` splits by even/odd index. Each group gets equal
  `1/sqrt(groupSize)` coefficients.
- `sum-all` duplicates one `1/sqrt(N)` row to both sides — the diagnostic
  Coherent Downmix ablation, which reinforces an aligned source rather than
  cancelling it.
- `widthDeg` is a constant-power mid/side law: `0` is mono, `90` an exact
  bypass, `180` side-only and out of phase.

`select` is the only strategy that accepts `leftChannel`/`rightChannel`; every
other strategy rejects them. `orthogonal-rows`, `halves`, and `alternating`
need N ≥ 2; `select` and `sum-all` work down to N = 1.

## `early` branch

[Stage 07](../design/reverb/stages/07-early-reflections.md). Taps the Main
path's Diffuser, shapes and sums the taps, Downmixes independently, and adds
the result into the same stereo output. Never feeds the Feedback Loop.

| Field | Values | Default |
| --- | --- | --- |
| `enabled` | boolean | `true` |
| `levelDb` | finite number (dB) | `0` |
| `decayDbPerSec` | finite number ≥ 0 | `0` |
| `taps` | array of `{stepIndex, gainDb?}` | unset (no branch) |
| `taps[].stepIndex` | unique index in `[0, steps)` | *required* |
| `taps[].gainDb` | finite number (dB) | `0` |
| `downmix` | object, or omitted | omitted (`select` Channels 0/1) |

`decayDbPerSec` attenuates later taps more, shaping the early envelope.
`levelDb` applies once after the branch's own Downmix. `downmix` takes the
same fields as the Main Downmix, except that `select` here defaults to
Channels 0/1 instead of requiring `leftChannel`.

Requires exactly one Diffuser in the Main path. Omitting `early`, `early: {}`,
and `early: {"taps": []}` all mean no branch — and with no taps the other
fields are rejected, since they could not affect sound.

---

## Rules the tables cannot show

**Shapes.** Only the four arrangements listed above resolve. Any other order or
combination is rejected at `/composition/stages`.

**Mutually exclusive.** `lengthsMs` cannot be combined with
`steps`/`totalMs`/`distribution`.

**Channel counts.** `hadamard` needs a power-of-two N.
`orthogonal-rows`/`halves`/`alternating` need N ≥ 2. Stereo input with
`stereo-halves`/`stereo-interleave` needs an even N.

**Envelope on identity.** Every `composition.*` envelope field, and `early`,
is rejected when `stages` is empty.

**Ablations are not presets.** `delayStrategy: "even"` or `"uniform-random"`,
`shuffle: false`, `polarity: "none"`, `normalisation: "none"`, `sum-all`, and
the `linear`/`allpass` interpolations exist to isolate one behaviour at a time.
They are diagnostic, not recommended settings.

**Defaults are a Reference configuration, not a product preset.** An omitted
stage setting resolves to the documented research baseline so experiments can
change one axis at a time against a fixed control. It is not a tuned or
recommended sound.

---

## What you get back

Every render writes the complete Resolved configuration to `resolved.json` —
your requested values plus everything derived from them (per-Channel delays,
gains, matrices, shelf coefficients, Modulation trajectories, the Tail budget).
Rendering it back with `--resolved` reproduces the output bit-identically.

See [Analyze the Render Result](../../README.md#analyze-the-render-result) for
what a Render Result contains and how to measure it.
