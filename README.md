# RVRBoTron

RVRBoTron is a research-grade algorithmic reverb laboratory. It is designed to make reverb structures easy to configure, render, measure, compare, and reproduce while developing a production-quality DSP core for a future audio plugin.

## Project documentation

- [Reverb design overview](docs/design/reverb/README.md)
- [Stage specifications](docs/design/reverb/stages/)
- [Domain vocabulary](CONTEXT.md)
- [Engineering workflow](docs/agents/workflow.md)

Active implementation work is tracked in this repository's GitHub Issues.

## Quick start

Install CMake 3.25 or newer, Ninja, Python 3, and a C++17 compiler. Git LFS is
also required to download or add the curated listening samples. Rendering and
`tools/analyze_render.py` need only the standard library; the diffusion,
tail, and Early Tap support analyzers additionally need the packages in
`tools/requirements.txt` (`pip3 install -r tools/requirements.txt`).

### Configure, build, and test

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

The `double` preset runs the same suite with float64 DSP samples:

```bash
cmake --preset double
cmake --build --preset double
ctest --preset double
```

The `release` and `release-double` presets build optimized (`Release`)
binaries for meaningful `benchmark` timing evidence; `default`/`double` stay
Debug builds for development:

```bash
cmake --preset release
cmake --build --preset release
```

Every CI job runs the complete command-level suite and proves exact identity
from its own committed fixtures; artifacts are not transferred between
machines. Float builds run on macOS arm64, macOS Intel, and Windows x86_64.
Representative float64 builds run on macOS arm64 and Windows x86_64. CI runs
for pull requests targeting `develop` or `main` and for pushes to `main`.
The committed compatibility fixtures cover mono and stereo PCM16, PCM24,
PCM32, IEEE float32, and IEEE float64 WAV at 44.1, 48, and 96 kHz.

Regenerate those deterministic technical fixtures with:

```bash
python3 tests/fixtures/generate_wav_matrix.py
```

### Render a Requested Configuration

Create `request.json`:

```json
{
  "formatVersion": 2,
  "seed": 0,
  "composition": {"stages": []}
}
```

Render a committed technical fixture through the empty Composition:

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/matrix/identity-stereo-48000-pcm16.wav \
  --config request.json \
  --block-size 512 \
  --output build/requested-result
```

The immutable Render Result contains:

| Path | Contents |
| --- | --- |
| `output.wav` | Canonical IEEE float32 WAV, or float64 with the `double` preset |
| `request.json` | Exact bytes of the user-authored request; present for requested renders |
| `resolved.json` | Complete Resolved Configuration used by the DSP |
| `render.json` | Input provenance, renderer facts, selected precision, block size, and audio facts |
| `captures/` | Optional manifested Stage captures from `--capture-stages all`: N-Channel Split/Diffusion-Step captures, plus the stereo Main-stereo (and, when configured, Early-stereo) branch captures (issue #113) |
| `analysis/` | Append-only, versioned analysis artifacts added after rendering |

`render.json` records the input filename and SHA-256, renderer version,
platform, architecture, sample precision, block size, configuration input
mode, sample rate, output channel count, input frame count, output frame
count, and the resolved Tail budget authorised for draining past input EOF
(0 for a Composition with no Diffuser or Feedback Loop). It intentionally
contains no timestamp, host or user identity, full input path, or
source-tree fingerprint.

Rendering builds the evidence in a temporary sibling and publishes it
atomically. Choose a fresh output path for every render because an existing
destination is never overwritten.

### Rerender the Resolved Configuration

Use the emitted configuration without resolving the request again:

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/matrix/identity-stereo-48000-pcm16.wav \
  --resolved build/requested-result/resolved.json \
  --output build/resolved-result
```

### Render the first Reference Diffusion Step

Format version 2 also accepts the ordered `[split, diffuser, downmix]` and
`[split, feedback-loop, downmix]` Composition shapes (the latter documented
in [Sustain a Response with a Feedback Loop](#sustain-a-response-with-a-feedback-loop)
below). The Diffuser resolves to an ordered chain of Hadamard Diffusion
Steps (`4` by default) feeding a select Downmix. Diagnostic ablations
support `normalisation: "none"`, `delayStrategy: "even"` or
`"uniform-random"`, `shuffle: false`, and `polarity: "none"`:

```json
{
  "formatVersion": 2,
  "seed": 42,
  "composition": {
    "stages": [
      {
        "type": "split",
        "channels": 8,
        "strategy": "duplicate",
        "normalisation": "energy"
      },
      {
        "type": "diffuser",
        "steps": 1,
        "totalMs": 40,
        "distribution": "even",
        "step": {
          "delayStrategy": "segmented-random",
          "mix": "hadamard",
          "shuffle": true,
          "polarity": "seeded-random"
        }
      },
      {
        "type": "downmix",
        "strategy": "select",
        "leftChannel": 0,
        "rightChannel": 1,
        "normalisation": "energy"
      }
    ]
  }
}
```

Non-empty diffusion renders are stereo and wet-only. The renderer drains
silence for the resolved finite Diffuser budget, so `render.json` distinguishes
`inputFrames` from the longer `frames`.

Capture the N-Channel Split and cumulative Diffusion Step without changing
`output.wav`:

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --config request.json \
  --capture-stages all \
  --output build/diffusion-result
```

The versioned `all-v2` capture profile writes canonical WAVs under
`captures/`. `render.json` manifests each boundary with its stable path,
SHA-256, sample rate, Channel count, and complete output-timeline frame count.

### Experiment with a Diffuser Configuration

Every field below is optional; the resolver substitutes the listed default
for anything omitted. Unknown fields anywhere in the request are rejected.
Render a curated listening sample (see
[Validate a Listening Sample Locally](#validate-a-listening-sample-locally))
through a modified `request.json` to hear the effect of each change:

```bash
build/default/rvrbotron render \
  --input samples/listening/PianoDry.wav \
  --config request.json \
  --output build/listening-diffusion-result
```

#### Top level

| Field | Type | Default | Notes |
| --- | --- | --- | --- |
| `formatVersion` | integer | *(required)* | Must be `2`. Missing or unsupported values fail at `/formatVersion`; version `1` fails with the exact recovery message naming tag `format-v1-final` (commit `8a4e718`), the last build able to render or analyze it. See [ADR-0006](docs/adr/0006-format-v2-compatibility-boundary.md). |
| `seed` | unsigned 64-bit integer | `0` | Drives every seeded-random derivation (delays, shuffle, polarity). |
| `composition.stages` | array | `[]` (empty Composition, exact identity) | When present, must be exactly `[split, diffuser, downmix]`, `[split, feedback-loop, downmix]`, or `[split, diffuser, feedback-loop, downmix]`. |
| `composition.mainEnabled` | boolean | `true` | The Main wet path's enablement. Not applicable, and rejected, when `composition.stages` is empty (issue #109). `false` skips Downmix/Width processing and contributes exact stereo zero. |
| `composition.mainLevelDb` | finite number (dB) | `0` | The Main wet path's level, applied once after its Downmix (including Width). Not applicable, and rejected, when `composition.stages` is empty (issue #109). Resolved Configuration additionally records the derived linear `mainGain`. |
| `composition.early` | object, or omitted | omitted (no branch) | The parallel Early Reflections branch, tapped from the Main wet path's own Diffuser (issues #111/#112). Valid only when `composition.stages` includes exactly one Diffuser; not applicable, and rejected, when `composition.stages` is empty. See [`early` branch](#early-branch) below. |
| `composition.dryDb` | finite number (dB) | `0` | The Composition's own dry level (issue #131). Not applicable, and rejected, when `composition.stages` is empty. Stays legal and preserved while `wetOnly` gates it off; Resolved Configuration additionally records the derived linear `dryGain`. |
| `composition.wetDb` | finite number (dB) | `0` | The Composition's own global wet level, applied once to the complete Wet sum (Main plus Early) before dry is mixed in (issue #131). Not applicable, and rejected, when `composition.stages` is empty. Resolved Configuration additionally records the derived linear `wetGain`. |
| `composition.wetOnly` | boolean | `true` | An exact gate on the dry path (issue #131): `true` (the default, reproducing every pre-envelope render) mutes dry regardless of `dryDb`; `false` maps dry into the mix, stereo input channel-for-channel and mono duplicated to both channels without energy compensation. Not applicable, and rejected, when `composition.stages` is empty. |

#### `split` stage

| Field | Values | Default |
| --- | --- | --- |
| `channels` | unsigned 32-bit integer (N) | `8` |
| `strategy` | `"duplicate"` \| `"stereo-halves"` \| `"stereo-interleave"` | `"duplicate"` |
| `normalisation` | `"energy"` \| `"none"` | `"energy"` |

`"duplicate"` sums stereo input to `(L + R)/√2` and distributes it to every
Channel; it discards stereo position. `"stereo-halves"` and
`"stereo-interleave"` preserve L/R independently and require an even
Channel count when the source is stereo; `"stereo-halves"` feeds the first
half of the Channels from the left input and the second half from the
right, and `"stereo-interleave"` alternates left/right by Channel index.
For mono input, every strategy resolves to the same mono duplication
mapping.

#### `diffuser` stage

| Field | Values | Default | Notes |
| --- | --- | --- | --- |
| `steps` | unsigned 32-bit integer (N) | `4` | Number of ordered Diffusion Steps sharing `totalMs`/`distribution`. Mutually exclusive with `lengthsMs`. |
| `totalMs` | finite number | `300` | Combined length of every Diffusion Step, apportioned per `distribution`. Mutually exclusive with `lengthsMs`. |
| `distribution` | `"even"` \| `"doubling"` | `"doubling"` | `"even"` gives every step an equal share of `totalMs`; `"doubling"` weights step *i* by `2^i` (each step roughly twice the previous). Mutually exclusive with `lengthsMs`. |
| `lengthsMs` | array of finite numbers (one per step, N ≥ 1) | unset | Explicit per-step lengths in milliseconds, in step order. Use instead of `steps`/`totalMs`/`distribution` for full control over each step's share. |
| `step.delayStrategy` | `"segmented-random"` \| `"uniform-random"` \| `"even"` | `"segmented-random"` | Shared default applied to every step unless overridden in `stepOverrides`. `"uniform-random"` samples each Channel's delay independently with replacement, so it permits duplicate delays and is exempt from the "N distinct positions" requirement the other two strategies enforce. |
| `step.mix` | `"hadamard"` \| `"householder"` \| `"random-orthogonal"` | `"hadamard"` | Shared default applied to every step unless overridden in `stepOverrides`. A matrix of a given type is resolved once and shared across every step that uses it. `"hadamard"` requires a power-of-two Channel count; `"householder"` and `"random-orthogonal"` accept any Channel count. |
| `step.shuffle` | boolean | `true` | Shared default applied to every step unless overridden in `stepOverrides`. |
| `step.polarity` | `"seeded-random"` \| `"none"` | `"seeded-random"` | Shared default applied to every step unless overridden in `stepOverrides`. |
| `step.modulation` | object, or omitted | omitted (disabled) | Seeded per-Channel delay-time movement scoped to that step alone -- the signal passes once, so detuning applies once and does not compound, in contrast to the Feedback Loop's own Modulation (compounds every circulation; see below). Omitted preserves that step's existing rendered output; an included empty object (`{}`) resolves the same research baseline as the Feedback Loop's own `modulation`. Shared default applied to every step unless overridden in `stepOverrides`; see the Feedback Loop's `modulation` field table below for the nested fields, shared verbatim between the two stages. |
| `stepOverrides` | array of `{index, delayStrategy?, mix?, shuffle?, polarity?, modulation?}` | unset | Sparse per-step overrides keyed by zero-based step index. Only listed fields are overridden; omitted fields fall back to the shared `step` defaults above. Each index must be unique and within `[0, stepCount)`. |

`"hadamard"` mixes maximally (`N·log₂N` additions) and is the diffuser's
default. `"householder"` subtracts twice the mean of the Channels from every
Channel — cheap, mild mixing, valid for any N. `"random-orthogonal"` is a
seeded dense orthogonal matrix with no Haar-uniformity claim: a fixed
`[-1, 1]` fill is orthogonalized via Householder QR with a fixed sign
convention, and a singular or near-singular fill is rejected outright rather
than silently repaired.

Every Diffusion Step's `delaysSamples`, `permutation`, and `polaritySigns`
are derived from `(seed, step index, Channel)`, so a given step index's
`permutation`/`polaritySigns` are stable across changes to `steps`/`totalMs`/
`lengthsMs` — only `delaysSamples` shifts when a step's own resolved length
changes. Per-step sample lengths are apportioned from `totalMs`/`lengthsMs`
using largest-remainder rounding, so they always sum exactly to the
resolved total.

The resolved Diffuser is rejected before allocation if its estimated DSP
memory footprint (delay lines plus per-step mix matrices) exceeds the
configured budget — 512 MiB by default, overridable with
`--memory-budget-mib <mebibytes>` on `render`. `--capture-stages all` writes
one canonical WAV per resolved step, named `01-diffusion-step-{i}.wav`.

A step's Modulation trajectories are seeded per step *and* per Channel, so
two modulated steps never share a trajectory — and neither does a
modulated step share one with the Feedback Loop's own Modulation, even
when both otherwise resolve identical parameters. Zero depth on a step is
the resolved bypass, bit-identical to `modulation` omitted from that
step; buffer headroom (the resolved Excursion plus the fixed
Interpolation margin) is reserved only on the Channels a step actually
modulates. A modulated Channel's resolved delay too short to serve its
own step's requested Excursion is rejected before any audio is
processed, naming that step's own `modulation/depthMs` — the same
Excursion rejection rule as the Feedback Loop's, since a short step delay
is exactly as unsafe as a short loop delay. Resolved per-step Modulation
is recorded in `resolved.json` alongside the Feedback Loop's own.

#### `downmix` stage

| Field | Values | Default |
| --- | --- | --- |
| `strategy` | `"select"` \| `"orthogonal-rows"` \| `"halves"` \| `"alternating"` \| `"sum-all"` | `"select"` |
| `leftChannel` | zero-based Channel index within `[0, N)` | required for `select`; not applicable to any other strategy |
| `rightChannel` | zero-based Channel index within `[0, N)`, distinct from `leftChannel`, or omitted | omitted (mono duplication of `leftChannel`); not applicable to any other strategy |
| `normalisation` | `"energy"` \| `"none"` | `"energy"` |
| `widthDeg` | finite number within `[0, 180]` | `90` |

`widthDeg` (issue #109) applies a constant-power mid/side Width law to the
Downmix's pre-Width `[left, right]` output: `0` collapses to mono, `90` is an
exact identity bypass, and `180` is side-only and out of phase. Resolved
Configuration records the resolved `widthMatrix`, the concrete 2x2 matrix
`widthDeg` resolves to (exact at `0`/`90`/`180`; trigonometric otherwise), so
replay never recomputes it.

`leftChannel` has no implicit default -- every `select` Downmix names its
Channel explicitly (see issue #107). `orthogonal-rows` (issue #108) instead
fills a deterministic N-by-N dense matrix from the branch-specific
RandomOrthogonal derivation (usage tag `MAINDNMX`; see
[ADR-0002](docs/adr/0002-version-positional-random-resolution.md)) and takes
its rows 0 and 1 as the left and right Downmix rows. `halves` and
`alternating` (issue #110) instead partition the N Channels into two
disjoint groups -- `halves` puts the first `ceil(N/2)` Channels left and
the remainder right; `alternating` puts even indices left and odd indices
right -- and each non-empty group gets equal `1/sqrt(groupSize)`
coefficients, so an odd N produces two unequal-size but still unit-norm
rows. `sum-all` (issue #114) instead duplicates one `1/sqrt(N)` row,
identical across every Channel, to both left and right -- the diagnostic
Coherent Downmix ablation: summing an aligned source coherently
reinforces it rather than the decorrelated cancellation-free sum an
unaligned source produces, and configuration renders it rather than
rejecting it -- see docs/design/reverb/stages/08-downmix.md's "Two source
signals, two rules". `orthogonal-rows`, `halves`,
and `alternating` each require N at least 2 and reject
`leftChannel`/`rightChannel` if either is present; `select` and `sum-all`
both support N as low as 1 (`sum-all`'s single-Channel row is `[1.0]`,
identical to `select`'s own N=1 mono duplication once compensation is
applied). Resolved Configuration records every strategy's rows as
`leftRow`/`rightRow` (unit norm) and
`effectiveLeftRow`/`effectiveRightRow` (scaled by `compensation`), plus
each row's Alignment expectation (`"aligned"` or `"unaligned"`), derived
from Composition wiring rather than settable by request: aligned for a
Diffuser-only Main wet path, unaligned when it includes a Feedback Loop.
It also records a `coherentDownmixAblation` boolean (issue #114), true
only for `sum-all` on an aligned source -- derived from `strategy` and
`alignment` together, never from the strategy name alone, so the same
`sum-all` request resolves `coherentDownmixAblation: false` once its
source includes a Feedback Loop.

`delayStrategy: "even"` or `"uniform-random"`, `shuffle: false`,
`polarity: "none"`, and `normalisation: "none"` are diagnostic ablations for
isolating one DSP behavior at a time; they are not intended as listening
presets.

#### `early` branch

| Field | Values | Default |
| --- | --- | --- |
| `enabled` | boolean | `true` |
| `levelDb` | finite number (dB) | `0` |
| `decayDbPerSec` | finite number `>= 0` | `0` |
| `taps` | array of `{stepIndex, gainDb?}` | required |
| `taps[].stepIndex` | unique zero-based Diffusion Step index within `[0, stepCount)` | *(required)* |
| `taps[].gainDb` | finite number (dB) | `0` |
| `downmix` | object, or omitted | omitted (`select` Channels 0/1, or Channel 0 duplicated at N=1) |

The parallel Early Reflections branch (issues #111/#112, docs/design/reverb/
stages/07-early-reflections.md): one or more taps on the Main wet path's own
Diffuser, shaped and summed into one N-Channel frame, Downmixed
independently of the Main Downmix, then added into the same stereo output --
never fed into a Feedback Loop. Valid only when `composition.stages`
contains exactly one Diffuser (a Diffuser-only or Diffuser-then-Feedback-Loop
Main wet path); rejected when the Main wet path has no Diffuser.

Omitting `composition.early`, an empty `early: {}`, and an explicit
`early: {"taps": []}` all mean no branch, and `enabled`/`levelDb`/
`decayDbPerSec`/`downmix` are then rejected since they could not affect
sound. A present, non-empty `taps` requires unique `stepIndex` values --
duplicates are rejected -- and resolves them sorted ascending (canonical
order), so a Requested tap-list permutation resolves and renders
identically.

Each tap's own resolved shaping gain is its `gainDb` minus `decayDbPerSec`
times its own nominal support end in seconds (the "Early envelope"): a
later tap, whose nominal support reaches further, is attenuated more at a
positive `decayDbPerSec`. Every tap's shaped N-Channel contribution is
summed into the branch's one shared accumulator before Downmix; `levelDb`
is then applied once more, after Downmix, to the combined branch.

`downmix` accepts the same fields as the Main Downmix's own `downmix` stage
above, with one difference: `strategy: "select"` defaults `leftChannel`/
`rightChannel` to Channels 0/1 (Channel 0 duplicated to mono at N=1) rather
than requiring `leftChannel` explicitly. Its resolved Alignment expectation
is always `"aligned"` (the Diffuser's own output shares one onset across
Channels), and its `orthogonal-rows` strategy draws from its own
domain-separated RandomOrthogonal derivation (usage tag `EARLDNMX`, distinct
from the Main Downmix's own `MAINDNMX`; see
[ADR-0002](docs/adr/0002-version-positional-random-resolution.md)).

Resolved Configuration records `enabled`, `levelDb`, the derived linear
`gain`, `decayDbPerSec`, canonical `taps`, and the full resolved `downmix`
object -- omitted entirely, like `mainEnabled`/`mainLevelDb`/`mainGain`,
when no branch is configured. Each resolved tap additionally records its
own `gainDb`; nominal Tap support bounds (`nominalSupportMin/MaxSamples`,
`nominalSupportMin/MaxMs`) summed from the resolved Diffuser's own
per-Channel delays through that tap's step; conservative bounds
(`conservativeSupportMin/MaxSamples`, `conservativeSupportMin/MaxMs`) --
the interval no tap energy occurs outside -- additionally widened by every
contributing step's own active Modulation Excursion and the fixed
Interpolation margin; and its resolved `shapingGainDb`/`gain`. Modulation on
a tapped step never changes its nominal support or its shaping gain, only
its conservative support.

Keep this table in sync whenever a request field, its accepted values, or its
default changes.

#### Capturing and ablating the Main and Early branches

`--capture-stages all` (issue #113) additionally publishes each branch's own
stereo contribution, captured after its own shaping, Downmix, Width, and
level -- immediately before the two are summed into `output.wav`:

| Path | Boundary | Present when |
| --- | --- | --- |
| `captures/02-main-stereo.wav` | `main-stereo` | Always, whenever `composition.stages` is non-empty |
| `captures/03-early-stereo.wav` | `early-stereo` | Only when `composition.early` is configured (with at least one tap) |

Each entry in `render.json`'s `stageCaptures` records a `disabled` flag: `true`
only for a branch that is configured but disabled (`mainEnabled: false`, or
`early.enabled: false`) -- its capture is still written, correctly sized to
the render's own output timeline, but exact zero throughout, since a
disabled branch contributes exact zero rather than a zero-multiplied value.
`disabled` is always `false` for `split`/`diffusion-step` captures, which
have no enablement of their own. A Feedback-Loop-only Main wet path (no
Diffuser) still captures `main-stereo`; it has no Diffusion Steps to capture
and cannot carry an Early Reflections branch, since Early requires a
Diffuser.

`output.wav` equals the sample-wise sum of `main-stereo` and (when present)
`early-stereo`, and each branch's own measured energy plus their cross term
reconciles with the combined signal's own measured energy -- overlapping
signals are not simply additive in energy, since `sum(a+b)^2 != sum(a^2) +
sum(b^2)` unless the two are uncorrelated.

### Sustain a Response with a Feedback Loop

The `[split, feedback-loop, downmix]` Composition shape circulates each
Channel through its own delay line, deliberately loses energy each
circulation via a per-Channel decay gain solved from a requested RT60, and
mixes the Channels orthogonally. Unlike the Diffuser, its output is
**unaligned** (Channels carry different echo times) and **not all-pass** --
the Feedback Loop is the one stage documented to lose energy on purpose.
Render a curated listening sample through it to hear a sustained,
decaying tail:

```bash
build/default/rvrbotron render \
  --input samples/listening/PianoDry.wav \
  --config request.json \
  --output build/tail-result
```

```json
{
  "formatVersion": 2,
  "seed": 0,
  "composition": {
    "stages": [
      {
        "type": "split",
        "channels": 8,
        "strategy": "duplicate",
        "normalisation": "energy"
      },
      {
        "type": "feedback-loop",
        "delayMinMs": 100,
        "delayMaxMs": 200,
        "delayStrategy": "segmented-random",
        "rt60Sec": 2.4,
        "decayMargin": 1.5,
        "mix": "householder",
        "gainMode": "per-channel",
        "silenceFloorDb": null
      },
      {
        "type": "downmix",
        "strategy": "select",
        "leftChannel": 0,
        "rightChannel": 1
      }
    ]
  }
}
```

| Field | Values | Default | Notes |
| --- | --- | --- | --- |
| `delayMinMs` / `delayMaxMs` | finite number > 0 | `100` / `200` | Room size; `delayMinMs` also sets the pre-tail gap. |
| `delayStrategy` | `"segmented-random"` \| `"uniform-random"` \| `"even"` | `"segmented-random"` | `"even"` demonstrates flutter -- avoid it for a listening preset. |
| `rt60Sec` | finite number > 0 | `2.4` | Requested decay time at the 1 kHz Reference band; solved into per-Channel gain at configuration. |
| `decayMargin` | finite number > 0 | `1.5` | Multiplies `rt60Sec` to derive the resolved Tail budget (the upper bound on frames rendered past input EOF); `1.5` places the drain's end near -90 dB. |
| `mix` | `"hadamard"` \| `"householder"` \| `"random-orthogonal"` | `"householder"` | Mild mixing is the default here, in contrast to the Diffuser's maximal Hadamard default. `"hadamard"` at a non-power-of-two Channel count is a hard error. |
| `gainMode` | `"per-channel"` \| `"uniform"` | `"per-channel"` | `"per-channel"` solves each Channel's gain from its own loop time; `"uniform"` solves one shared gain from the mean loop time across Channels instead (the reference design's approach), measurably less accurate at a wide delay spread. |
| `silenceFloorDb` | finite number, or `null`/omitted | `null` (disabled) | Reserved seam for the eventual plugin's runtime idle behavior; dormant here -- disabled output is bit-identical to a build without the field. |
| `damping` | object, or omitted | omitted (disabled) | Two-shelf damping: independent per-Channel low and high shelves applied after decay gain and before mixing, on every circulation. Omitted preserves undamped output; an included empty object (`{}`) resolves the research baseline below. See the field table underneath. |
| `modulation` | object, or omitted | omitted (disabled) | Seeded per-Channel delay-time movement, read through a fractional interpolator on every circulation. Omitted preserves existing rendered output; an included empty object (`{}`) resolves the research baseline below. See the field table underneath. |

Omitting `damping` entirely preserves existing undamped output, and an
existing `resolved.json` written before Damping existed loads back as
disabled. An included `damping` object resolves any missing nested field to
the baseline shown here:

| `damping` field | Values | Default | Notes |
| --- | --- | --- | --- |
| `highRatio` | finite number > 0 | `0.5` | Decay time above `highHz`, relative to `rt60Sec` (`0.5` = half, `2.0` = double). Above `1.0` is a boost, accepted only when the conservative one-circulation contraction certificate passes for every Channel at both float32 and float64 precision. |
| `highHz` | finite number, strictly between `0` and Nyquist | `4000` | Half-gain shelf corner: the response is halfway (in dB) between unity and the shelf plateau at this frequency. |
| `lowRatio` | finite number > 0 | `1.0` | Decay time below `lowHz`, relative to `rt60Sec`. Same boost certificate as `highRatio`. |
| `lowHz` | finite number, strictly between `0` and Nyquist | `200` | Half-gain shelf corner for the low shelf. May sit on either side of `highHz`, including a crossed or overlapping layout -- there is no required corner order. |

A ratio of `1.0` bypasses that shelf entirely (independently of the other
shelf), so explicit unity ratios for both render bit-identically to
`damping` omitted. Resolved per-Channel low- and high-shelf gain, canonical
prewarped coefficients, and expected low/reference/high decay (seconds) are
recorded in `resolved.json`. A corner/ratio combination whose solved 1 kHz
response lands far from `rt60Sec` is not rejected -- see
[ADR-0004](docs/adr/0004-validate-structure-not-acoustics.md) -- the
resolved evidence records the actual implied decay either way.

A boosted ratio (above `1.0`) is proven safe by a cheap conservative
structural bound rather than a frequency grid or complete FDN pole solve:
resolution bounds one circulation from the decay gain, each monotonic
shelf's maximum plateau magnitude, and the realized mixing matrix's
quantization error, for both float32 and float64 coefficients. Every
Channel's bound must land strictly below unity at both precisions or the
request is rejected, never clamped; the bound, margin, and precision are
recorded per Channel in `resolved.json`. This conservative certificate may
reject an overlapping boost-and-cut combination an exact modal analysis
could prove safe -- an accepted trade-off for a cheap, deterministic proof.
When a shelf boosts, the Tail budget follows the slower of `rt60Sec`, that
shelf's own conservative feedback-decay estimate, and its state-settling
time, so the renderer always drains the complete authorized response.

Omitting `modulation` entirely preserves existing rendered output, and an
existing `resolved.json` written before Modulation existed loads back as
disabled. An included `modulation` object resolves any missing nested field
to the baseline shown here:

| `modulation` field | Values | Default | Notes |
| --- | --- | --- | --- |
| `depthMs` | finite number >= 0 | `0.4` | Peak Excursion, in milliseconds, above and below each Channel's nominal resolved delay. `0` disables movement -- a resolved bypass, bit-identical to `modulation` omitted, rather than an interpolator collapsing to identity. |
| `rateHz` | finite number >= 0 | `0.7` | Multiplies with `depthMs` for perceived detuning (the Detune product). `0` freezes each Channel's fractional offset as a static per-Channel detune spread, isolating interpolation error from movement artefact. Means the same thing for every `shape`. |
| `shape` | `"smoothed-random"` / `"sine"` / `"triangle"` | `"smoothed-random"` | The per-Channel trajectory waveform. `sine`'s periodicity becomes an audible regular wobble at a larger Excursion; `smoothed-random` (band-limited noise) has no period to lock onto. `triangle` shares `sine`'s zero crossings and peak locations, so comparing the two at a fixed `rateHz` compares only the shape. |
| `channelFraction` | finite number in `[0, 1]` | `1.0` | Proportion of Channels modulated, rounded up. `0` disables Modulation for the stage, exactly like `depthMs: 0`. The modulated Channels are the first `ceil(channelFraction * N)` entries of a positionally seeded fixed permutation, independent of delay ordering -- raising the fraction only adds Channels, never reshuffling ones already selected. |
| `interpolation` | `"lagrange3"` / `"linear"` / `"allpass"` | `"lagrange3"` | The delay line's fractional-read method. `linear` (#92) is a deliberate ablation: it darkens a modulated tail as depth rises, an unintended depth-dependent lowpass. `allpass` (#93) has flat magnitude at any fixed fractional delay but carries persistent per-Channel filter state a moving delay repeatedly invalidates -- stable and bounded at every depth/rate tested, but measurably rougher (more sample-to-sample discontinuity) than the other two; see [the design doc's recorded finding](docs/design/reverb/stages/06-modulation.md#allpass-viability-inside-the-feedback-loop-issue-93). Both are made available on purpose, to be heard and compared against `lagrange3`, not hidden. |

`smoothed-random` is Catmull-Rom interpolation between per-Channel targets
drawn uniformly in `[-1, +1]`, a new target every `1/rateHz`, reproducible
from an integer target counter with no accumulated state. Every Channel
carries a fixed, undocumented-as-a-parameter +-10% seeded rate spread and a
positionally seeded phase offset so trajectories decorrelate across
Channels regardless of `shape`; trajectories are not pinned at render
start. A Channel `channelFraction` excludes keeps the cheaper integer read
path with no Channel reordering, and the Excursion rejection rule below
applies to modulated Channels only -- an excluded Channel may carry a delay
too short to ever serve the requested Excursion without being rejected.
Resolution reserves each *modulated* Channel's buffer as its nominal delay
plus `depthMs` in samples (the Excursion) plus a fixed worst-case
Interpolation margin, so a Channel's *buffer size* does not move when
`interpolation` changes -- the margin is sized for the worst case across
every method, not the configured one. `allpass` alone also carries its own
small per-Channel filter state (one persistent output sample), allocated
only for a Channel actually modulated with `allpass` chosen and counted
toward the owning stage's DSP-owned memory -- a bypassed stage or an
excluded Channel allocates none of it. A modulated Channel's resolved
delay too short to serve that Excursion plus margin is rejected before
any audio is processed
-- naming `modulation/depthMs` -- rather than overrunning intermittently at
the modulation peak; the Feedback Loop's Block-size bound is derived from
the shortest *instantaneous* per-Channel delay across every Channel, moved
or not. Resolved shape, per-Channel trajectory seeds, rates, and phases,
the per-Channel bypass mask, the resolved Excursion, and the Interpolation
margin are recorded in `resolved.json`.

Each Channel's decay gain is solved independently from that Channel's own
loop time (`gain = 10^(-3L/R)`), so every Channel decays at the same rate
regardless of its own delay -- the requested `rt60Sec` is measurable in the
output even with unequal delays. The resolved Tail budget, delays, gains,
and matrix coefficients are recorded in `resolved.json`, so a resolved
rerender reproduces the tail exactly. `render.json`'s `tailBudgetFrames`
records the upper bound actually authorised for this render.

### Analyze the Render Result

Render Result analysis is deterministic: it reads only the immutable Render
Result and never touches the clock, the machine, or system load, so
analyzing the same evidence on any machine reproduces the same published
artifact. It is a separate Python command and is never required for
rendering:

```bash
python3 tools/analyze_render.py \
  build/requested-result \
  --source tests/fixtures/audio/matrix/identity-stereo-48000-pcm16.wav
```

It prints a concise summary and atomically adds
`analysis/baseline-v1.json`. The versioned artifact records frame count,
duration, sample rate, channel count, and non-finite sample count, plus
per-channel and combined peak absolute sample, RMS amplitude, and sum of
squares. Metric definitions and linear units are embedded in the artifact.

The optional source comparison runs only when the source SHA-256 matches
`render.json`. It reports exact equality, differing-sample count, maximum
absolute error, and the first mismatch. Analysis artifacts are append-only:
repeating identical analysis succeeds without rewriting, while different
content for the same analyzer version is rejected.

For a captured finite Diffuser, add the separate diffusion artifact:

```bash
python3 tools/analyze_diffusion.py \
  build/diffusion-result \
  --source tests/fixtures/audio/impulse-mono-pcm16-48000.wav
```

`analysis/diffusion-v1.json` verifies source and Stage-capture provenance,
requires `inputFrames + resolved diffuser.totalSamples` frames in the output
and every capture, measures the actual captured Split and cumulative Diffusion
Step energies (including each step's relative error from Split), and measures
orthogonality from every serialized resolved matrix (Hadamard, Householder, or
RandomOrthogonal alike). Python does not reconstruct Split mapping or DSP
sample precision. Publication is append-only and idempotent.

The artifact also reports, using NumPy: a signed Correlation matrix at Split
and every Diffusion Step; an Alignment score (pairwise Jaccard overlap of
active-frame sets, -120 dB capture-relative activity floor) and 10 ms
Distinct-arrival density curve measured on the complete Diffuser output,
beside the theoretical N^k Echo-path count; and Coloration (unwindowed,
next-power-of-two FFT peak-to-peak/RMS dB deviation, spectral flatness, and a
compact 1/12-octave curve) for both the combined N-Channel signal and the
diagnostic stereo `output.wav`, reported separately since only the former
carries the all-pass claim.

Pass `--compare <other-render-result>` instead of `--source` to check two
Render Results of the same Resolved Configuration -- typically rendered at
different `--block-size` values -- for exact decoded equality of `output.wav`
and every Stage capture, with first-mismatch detail on failure. This mode
prints its own JSON report and does not publish an artifact.

For a Composition with an Early Reflections branch (issue #112), render with
`--capture-stages all` and add the Tap support artifact:

```bash
python3 tools/analyze_early_support.py build/early-result
```

`analysis/early-support-v1.json` reports, per resolved tap, the measured
first/last non-zero sample, peak sample, and energy-weighted centroid from
that tap's own captured Diffusion Step (-120 dB capture-relative activity
floor, the same convention `analyze_diffusion.py`'s own Alignment evidence
uses), alongside its resolved nominal and conservative Tap support bounds
and whether the measured interval fell within conservative support.
Cancellation can make measured support narrower than the structural bound;
it never rejects a render for falling outside it. Publication is
append-only and idempotent, like every other analyzer here.

For any Composition's Main and (if configured) Early Downmix, render with
`--capture-stages all` and add the spatial-output Downmix artifact (issue
#115) -- it works across every Downmix strategy and either Alignment
expectation, with or without a Feedback Loop:

```bash
python3 tools/analyze_downmix.py build/downmix-result --source tests/fixtures/audio/impulse-mono-pcm16-48000.wav
```

`analysis/downmix-v1.json` reports the Main branch's (and, when
configured, the Early branch's) resolved strategy, Alignment expectation,
and Coherent Downmix ablation tag (`coherentDownmixAblation`, issue #114)
read directly from `resolved.json` rather than re-derived; a measured
Alignment score of that Downmix's own immediate N-Channel source, only
when a Diffusion Step actually is that immediate input -- an unaligned
source (a Feedback Loop between the Diffuser and Downmix, or no Diffuser
at all) has no equivalent capture (ADR-0005), so this is reported
unavailable there rather than measured against the wrong signal. When
that source is available *and* the branch itself was enabled (issue
#113's `disabled` capture flag), it also reports a `branchEnergyRatio` --
the branch's own captured energy relative to that source's, and
`analyze_tail.py`'s own established raised-cosine octave-band filter's
spectral deviation between them -- deliberately reported as the
*combined* effect of the Downmix's row/compensation projection, Width,
and branch level together, not an isolated Width metric, since no
capture exists between Downmix and Width to separate them. A damped tail
never receives an "absolute flatness" spectral claim from this analyzer
as a result of the same Alignment-score gating. It also reports Output
correlation and inter-channel level difference on the combined stereo
output, branch energies and their cross term reconciled against combined
energy (captured immediately before summation, issue #113; reconstructed
at the render's own sample precision, not a higher-precision
approximation, since the renderer's own summation and Early's own
per-tap accumulation happen in that same precision), a peak factor, and
equal-power mono fold-down (energy loss and octave-band spectral
deviation between the folded signal and the stereo pair it was folded
from) -- reusing that same octave-band filter for every spectral
comparison here, rather than a fresh per-bin binning
scheme, so deviation is a stable quantity independent of FFT length.
None of this evidence imposes an acoustic rejection threshold. Publication
is append-only and idempotent.

For a Composition containing a Feedback Loop, add the separate tail
artifact instead -- the diffusion analyzer's all-pass, feedback-free
assumptions do not hold once the tail is present, so it is deliberately not
extended to cover this case. Schroeder backward integration assumes an
impulse response, so analyze a deterministic impulse render of the same
Resolved Configuration rather than a musical sample -- issue #59's sweep
keeps the two renders separate for exactly this reason:

```bash
build/default/rvrbotron render \
  --input tests/fixtures/audio/impulse-stereo-left-pcm16-48000.wav \
  --resolved build/tail-result/resolved.json \
  --output build/tail-impulse-result

python3 tools/analyze_tail.py \
  build/tail-impulse-result \
  --source tests/fixtures/audio/impulse-stereo-left-pcm16-48000.wav
```

Rerendering via `--resolved` requires the new source's Channel count to
match exactly what the first render resolved (the resolved Split's
`inputChannels`), so the impulse fixture here must be stereo -- matching
`samples/listening/PianoDry.wav` above -- not the mono fixture used
elsewhere in this document.

`analysis/tail-v1.json` requires the resolved Composition to be
`[split, feedback-loop, downmix]` or `[split, diffuser, feedback-loop,
downmix]`; any other shape is rejected. It verifies source provenance from
`render.json` and checks the output frame count against
`inputFrames + tailBudgetFrames` -- exactly, while the dormant
`silenceFloorDb` seam is disabled, relaxing to an upper bound once it is
enabled, keyed off the Resolved Configuration rather than a schema change.
Measurement reads only `output.wav`; no Stage captures are required.

RT60 is measured per octave band from 63 Hz to 16 kHz by Schroeder backward
integration of the band-limited stereo output's energy (a raised-cosine
band shape and zero-padded FFT filtering keep the filter a linear rather
than a circular convolution, avoiding leakage across a finite-length
render). T30 (fit over -5 to -35 dB, extrapolated to -60 dB) is the primary
fit; T20 (-5 to -25 dB) is reported beside it -- the two agree on a linear,
single-rate decay and diverge under `gainMode: uniform` at a wide delay
spread, exposing the non-linear, double-sloped decay that mode produces
rather than averaging it away. The Reference-band (1 kHz) T30 estimate is
reported against the requested `rt60Sec` with relative error and whether it
sits within the +/-5% Decay accuracy invariant.

The artifact separately reports the output's non-finite sample count and a
decay-envelope check: the *raw*, non-Schroeder-integrated broadband output
energy, coarsely windowed from input EOF onward, verified non-increasing.
This is deliberately distinct from the per-band Schroeder curves above,
which are non-increasing by construction regardless of what the render
does and so cannot themselves catch a genuine buildup.

The artifact also reports, reusing `analyze_diffusion.py`'s own evidence
functions for direct comparability: an Alignment score (near unity for an
aligned Diffuser-only render's internal Channels, materially lower once the
Feedback Loop circulates and unaligns them, measured here on the stereo
output's left and right positions) and Coloration for the stereo output.
Publication is append-only and idempotent.

### Compare Diffusion Equivalence Across Platforms

Cross-platform equivalence (Repeat determinism holds only on one machine;
see [ADR-0001](docs/adr/0001-cross-platform-reproducibility.md)) is proven
by reducing an already-analyzed Render Result's `analysis/diffusion-v1.json`
to a compact, WAV-free summary and comparing it against other platforms'
summaries within committed tolerances:

```bash
python3 tools/extract_diffusion_equivalence.py \
  build/diffusion-result \
  --output build/diffusion-equivalence.json

python3 tools/compare_diffusion_equivalence.py \
  build/diffusion-equivalence.json other-platform/diffusion-equivalence.json \
  --tolerances tools/diffusion_tolerances_v1.json
```

The extracted summary keeps energy relative error, orthogonality error,
Correlation, Alignment score, Coloration, structural Echo paths, and
measured Distinct-arrival evidence; it drops full Correlation matrices,
per-pair Alignment listings, and per-band Coloration curves. The comparator
groups summaries by sample precision, picks one baseline platform per group
(macOS arm64 when present), and requires the structural N^k Echo-path count
and Distinct-arrival density bin count -- deterministic functions of the
Resolved Configuration, not measurements -- to match exactly, while every
measured metric (including the measured Distinct-arrival counts
themselves) stays within `tools/diffusion_tolerances_v1.json`'s committed
per-precision absolute tolerance; a group with fewer than two platforms is
reported as skipped, not failed. CI runs both platform-independent steps
against the same compact Reference impulse tracer used by
`diffusion_analyzer_contract`, uploads each platform/precision's compact
summary as a build artifact, and runs a final job that downloads every
summary and compares them by precision.

### Compare Tail Equivalence Across Platforms

Tail measurement's octave-band FFT filtering, Schroeder integration, and
curve fitting all run in NumPy, so the same workflow applies to
`analysis/tail-v1.json`:

```bash
python3 tools/extract_tail_equivalence.py \
  build/tail-impulse-result \
  --output build/tail-equivalence.json

python3 tools/compare_tail_equivalence.py \
  build/tail-equivalence.json other-platform/tail-equivalence.json \
  --tolerances tools/tail_tolerances_v1.json
```

The extracted summary keeps non-finite sample count, each octave band's
T20/T30 RT60 estimate, Alignment score, and Coloration; it drops the full
per-segment decay-envelope energy series and Coloration's twelfth-octave
curve. The comparator groups summaries by sample precision, picks one
baseline platform per group (macOS arm64 when present), and requires the
octave-band count, whether each band's T20/T30 fit exists at all, and the
decay-envelope monotonicity flag -- discrete facts about the committed CI
tracer's decay curve, not continuous measurements -- to match exactly,
while every measured metric (including each band's own RT60 estimate)
stays within `tools/tail_tolerances_v1.json`'s committed per-precision
absolute tolerance; a group with fewer than two platforms is reported as
skipped, not failed, and a candidate platform missing a metric the
baseline reports (for example, a differing band count) fails that metric
explicitly rather than aborting the comparison. CI runs both
platform-independent steps against the same millisecond-scale Feedback
Loop tracer used by `tail_analyzer_contract`, uploads each
platform/precision's compact summary as a build artifact, and runs a final
job that downloads every summary and compares them by precision.

Per [ADR-0001](docs/adr/0001-cross-platform-reproducibility.md), tolerances
are only committed after reviewing real evidence from every supported CI
architecture; see that document for the CI run reviewed and each metric's
observed delta and headroom.

### Benchmark a Resolved Configuration

Benchmark evidence is environment-qualified rather than deterministic: it
depends on the specific machine, build configuration, and system load at
measurement time, so it is never asserted equal across machines or CI runs
-- CI benchmarks are smoke coverage only (the report has a sane shape and
completes), with no performance gate or cross-machine speed assertion.
Measure empirical CPU and memory cost around `Reverb::process`, excluding
configuration and file I/O, using an optimized build (`--preset release` or
`--preset release-double`):

```bash
build/release/rvrbotron benchmark \
  --resolved build/requested-result/resolved.json \
  --block-size 128 \
  --warmup-seconds 1 \
  --measure-seconds 5 \
  --json build/benchmark-report.json
```

The Composition must be non-empty and its Channel counts mono or stereo.
Deterministic nonzero seeded blocks drive the constructed `Reverb` for the
warm-up duration (discarded), then for the measured duration, timing every
block individually; neither phase touches file or JSON I/O. The terminal and
JSON reports are identical and include: median, p95, and worst block time;
the real-time budget, median/worst utilization, and missed-deadline count at
the resolved sample rate and requested block size; exact DSP-owned bytes
(object storage plus every owned-container's actual capacity, not size); a
best-effort process resident-set-size delta, labeled separately as
allocator/runtime-noisy evidence; and provenance -- renderer version,
platform, architecture, compiler, build type, Sample precision, sample rate,
block size, Channel count, step count, the mixing matrix used by every
Diffusion Step, and -- per Diffusion Step and for the Feedback Loop --
the resolved Modulation interpolation method actually exercised (`null`
when that stage's Modulation is absent or inactive), so two reports from
different `interpolation` choices stay directly comparable (#92).
Benchmarking a Debug binary prints a prominent warning to stderr but is
not refused.

### Run the Canonical Diffusion Experiment Catalog

`tools/diffusion_catalog_v1.json` is a versioned catalog: one complete
Reference configuration (N=8 duplicate Split, 4-step 300 ms doubling Hadamard
Diffuser) plus ~20 named one-axis overrides (Channel count, Split strategy,
total length, step count, distribution, delay strategy, matrix, and
shuffle/polarity ablations, expressed as JSON-Pointer add/remove/replace
operations against the Reference), and 5 listening cases mapping curated
material to a hypothesis, each paired with its quantitative counterpart's
exact Requested Configuration. Run it with (use `--preset release` for
meaningful benchmark evidence):

```bash
python3 tools/run_diffusion_catalog.py \
  --catalog tools/diffusion_catalog_v1.json \
  --renderer build/release/rvrbotron \
  --analyzer tools/analyze_diffusion.py \
  --mono-source tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --stereo-source tests/fixtures/audio/impulse-stereo-left-pcm16-48000.wav \
  --listening-dir samples/listening \
  --output manual_UATs/diffusion-catalog
```

Each quantitative case renders the deterministic impulse basis with
`--capture-stages all`, publishes `analysis/diffusion-v1.json`, and
benchmarks at 128 frames (1 s warm-up, 5 s measured); the Reference
additionally benchmarks 32/64/128/256/512. Listening cases render the
curated (Git-LFS) sample and are skipped, not failed, when the sample isn't
present locally. Every case's output directory is an ordinary immutable
Render Result: a case whose `render.json` already exists is never
re-rendered, and rendering, analysis, and benchmarking are each resumed
independently, so an interrupted or partially failing run picks up exactly
where it left off when rerun with the same command, without replacing
existing evidence. Benchmark reports
aggregate into `<output>/benchmark-summary.json` and a terminal table ranked
slowest-to-fastest with deltas from Reference; `<output>/catalog-report.json`
records every case's completed/resumed/skipped/failed status. This full run
is a local research artifact -- expect on the order of a hundred-plus MB of
audio evidence and several minutes of wall time. CI instead runs a small
tracer catalog (`tests/test_diffusion_catalog_cli.py`) that proves
materialization, pairing, resumability, and aggregation without executing
the full sweep.

### Sweep a Listening Sample Across the Tail Axes

`tools/tail_sweep_v1.json` is a versioned axis catalog: one Reference
Feedback Loop configuration (100-200 ms delays, RT60 2.4 s, Householder
mixing, per-Channel gain) plus 5 named axes -- delay range, RT60, matrix,
delay strategy, gain mode -- each swept as 1-2 named values that override
only that axis from the Reference, never combinatorially. Unlike the
diffusion catalog, the sample to sweep is a runtime argument rather than
catalog-embedded, so one curated (Git-LFS) sample is picked per run:

```bash
python3 tools/run_tail_sweep.py \
  --catalog tools/tail_sweep_v1.json \
  --renderer build/release/rvrbotron \
  --analyzer tools/analyze_tail.py \
  --sample samples/listening/PianoDry.wav \
  --mono-impulse tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --stereo-impulse tests/fixtures/audio/impulse-stereo-left-pcm16-48000.wav \
  --output manual_UATs/tail-sweep
```

Every sweep point renders both the selected sample and the deterministic
impulse matching its Channel count (mono or stereo, chosen automatically)
under an identical Resolved Configuration -- the sample render's
`resolved.json`, reused via `--resolved` for the impulse render -- because
Schroeder integration assumes an impulse response and programme material
would contaminate the decay curve with its own envelope; tail analysis
therefore runs only against the impulse render. Every point is also
benchmarked at a canonical block size, and every benchmark aggregates into
`<output>/<sample>/benchmark-summary.json` and a terminal table ranked
slowest-to-fastest with deltas from the Reference point -- the same
aggregation `run_diffusion_catalog.py` produces -- so acoustic preference
and processing cost are visible together across the whole sweep, not just
one point at a time. Output is laid out per sample, per axis, and per axis
value with numeric prefixes for auditioning in order:
`<output>/<sample>/00-reference/`, `<output>/<sample>/01-<axis>/01-<value>/`,
`02-<value>/`, and so on through every axis. As with the diffusion catalog,
every point's render, analysis, and benchmark steps are independently
resumable, and a missing or unpulled-Git-LFS sample is skipped, not failed,
with an explanatory message. This full run is a local research artifact;
CI instead runs a millisecond-scale tracer sweep
(`tests/test_tail_sweep_cli.py`) that proves the axis materialization,
paired-impulse rendering, and resumability without executing the full
sweep.

Every run also (re)generates `<output>/<sample>/listening-report.html`: one
self-contained page presenting every point's renders (playable in place
via relative `<audio>` paths -- no external resource requests, no
JavaScript), its measured decay against the requested RT60 (with a
per-band T30 breakdown behind a `<details>` toggle), and its benchmark
cost, alongside the same ranked cross-point comparison as the terminal
table -- so a tuning session is consumable by opening one file in a
browser, without reading terminal scrollback or opening a dozen JSON
files. Regeneration is unconditional and reads only already-published
evidence, so rerunning a fully resumed sweep refreshes the report without
re-rendering anything.

### Sweep Damping Over Curated Listening Samples

`tools/damping_sweep_v1.json` is a versioned axis catalog for the Two-shelf
Damping research space (issue #79): one Reference Feedback Loop
configuration (100-200 ms delays, RT60 2.4 s, Householder mixing,
per-Channel gain, and the documented Damping research baseline -- `highRatio`
0.5 at 4000 Hz, `lowRatio` 1.0 at 200 Hz) plus 4 named axes -- `high-ratio`,
`low-ratio`, `high-corner`, `low-corner` -- each swept as 1-2 named values
that override only that axis's Damping field(s) from the Reference, never
combinatorially. Ratios above `1.0` (boost) are deliberately excluded from
this listening catalog; they remain covered by `test_feedback_loop_cli.py`'s
configuration tests. Same shape and CLI as the tail sweep above -- the
sample to sweep is a runtime argument, not catalog-embedded:

```bash
python3 tools/run_damping_sweep.py \
  --catalog tools/damping_sweep_v1.json \
  --renderer build/release/rvrbotron \
  --analyzer tools/analyze_tail_v2.py \
  --sample samples/listening/PianoDry.wav \
  --mono-impulse tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --stereo-impulse tests/fixtures/audio/impulse-stereo-left-pcm16-48000.wav \
  --output manual_UATs/damping-sweep
```

Every sweep point renders both the selected sample and its matching
deterministic impulse under an identical Resolved Configuration -- the same
`--resolved`-reuse pattern the tail sweep uses -- and analyzes the impulse
with tail analysis version 2 (`analyze_tail_v2.py`, #78) rather than version
1, publishing `analysis/tail-v2.json`: Damping-aware, predicted-vs-measured
octave-band decay, the canonical low/Reference/high ratio summary, and
eventual-contraction evidence. Materialisation, per-step resumability,
one-axis-at-a-time catalog loading, and Git-LFS-aware sample matching are
shared with `run_tail_sweep.py` via `experiment_runner`; only the analyzer
and the listening-report layout differ. This full run is a local research
artifact; CI instead runs a millisecond-scale tracer sweep
(`tests/test_damping_sweep_cli.py`) that proves the same properties without
executing the full sweep.

Every run also (re)generates `<output>/<sample>/listening-report.html`,
presenting each point's playable sample and impulse renders, its requested
`highRatio`/`highHz`/`lowRatio`/`lowHz` (read from `resolved.json`), the
canonical low/Reference/high measured ratios, the full predicted-vs-measured
octave-band decay curve behind a `<details>` toggle, complete-response and
eventual-contraction status, and benchmark cost -- self-contained, playable
in a browser with no external resource requests or JavaScript. Per
[ADR-0004](docs/adr/0004-validate-structure-not-acoustics.md), a rendered 1
kHz Reference-band deviation past 10% from the requested `rt60Sec` is a
gentle one-pole shelf's real, expected transition-band behavior -- the
report flags it visibly (`significantDeviation` in `tail-v2.json`) rather
than treating it as a failed point.

### Sweep the Spatial-Output Reference Configuration

`tools/spatial_sweep_v1.json` is a versioned axis catalog for the
Early/Main Downmix research space (issue #116): one explicit Reference
composition -- N=8, the established four-step 300 ms doubling Diffuser and
100-200 ms/RT60 2.4 s/Householder/per-Channel Feedback Loop, taps at
Diffusion Steps 0 and 1 with zero offsets and envelope slope, -6 dB Early
level, Early `select` Channels 0/1 at 90 degrees, and 0 dB `orthogonal-rows`
Main output at 90 degrees -- plus 9 named axes (tap index, Early level,
Early-envelope slope, aligned `select` versus `sum-all`, Main strategy,
selected Channel pair, Early width, Main width, and N), each swept as 1-3
named values that override only that axis from the Reference, never
combinatorially. Same shape and CLI as the tail/damping sweeps above -- the
sample to sweep is a runtime argument, not catalog-embedded:

```bash
python3 tools/run_spatial_sweep.py \
  --catalog tools/spatial_sweep_v1.json \
  --renderer build/release/rvrbotron \
  --analyzer tools/analyze_downmix.py \
  --sample samples/listening/PianoDry.wav \
  --mono-impulse tests/fixtures/audio/impulse-mono-pcm16-48000.wav \
  --stereo-impulse tests/fixtures/audio/impulse-stereo-left-pcm16-48000.wav \
  --output manual_UATs/spatial-sweep
```

Every sweep point renders both the selected sample and its matching
deterministic impulse under an identical Resolved Configuration -- the same
`--resolved`-reuse pattern the tail/damping sweeps use, with
`--capture-stages all` added to the impulse render since `analyze_downmix.py`
needs Stage captures. Materialisation, per-step resumability,
one-axis-at-a-time catalog loading, and Git-LFS-aware sample matching are
shared with `run_tail_sweep.py`/`run_damping_sweep.py` via
`experiment_runner`; only the analyzer, the listening-report layout, and two
properties unique to this sweep differ:

- **Isolation.** Each axis declares a `scope` (a JSON-Pointer prefix) in the
  catalog. Every point's materialized Requested Configuration is diffed
  against the Reference, and every changed field must fall under that
  axis's own scope -- a catalog-authoring mistake that changes more than
  its one named axis fails loudly, naming the offending path, rather than
  silently confounding the sweep.
- **Determinism.** Each point's impulse render is repeated into a scratch
  directory and byte-compared (`resolved.json`, `output.wav`, every Stage
  capture) against the original; a mismatch fails the point.

Unlike `run_diffusion_catalog.py` and the tail/damping sweeps, points are
not benchmarked -- issue #116's own acceptance criteria cover spatial
measurements and listening-report evidence, not processing cost.

The Reference's Main wet path reads the Feedback Loop, so it is unaligned;
the Coherent Downmix ablation (issue #114) is instead demonstrated on Early
Reflections, which is always aligned by construction. The `coherent-downmix`
axis's `sum-all` point is automatically compared against the Reference's own
`select` Early Downmix (the matched control) once both have been analyzed,
using each point's own published `analysis/downmix-v1.json` -- peak-factor
and spectral-deviation deltas, plus a non-fatal warning, published to
`<output>/<sample>/coherent-downmix-comparison.json` and embedded in
`sweep-report.json`. Neither render is ever rejected by this comparison.

This full run is a local research artifact; CI instead runs a
millisecond-scale tracer sweep (`tests/test_spatial_sweep_cli.py`) that
proves axis materialization, isolation and determinism validation, the
Coherent Downmix comparator, and resumability without executing the full
sweep.

Every run also (re)generates `<output>/<sample>/listening-report.html`,
presenting each point's playable sample and impulse renders, both branches'
resolved strategy/Alignment expectation/Coherent Downmix ablation tag,
measured Alignment score and branch/source energy ratio where available,
peak factor, Output correlation, and mono fold-down energy loss, plus the
Coherent Downmix comparison -- self-contained, playable in a browser with no
external resource requests or JavaScript.

### Validate a Listening Sample Locally

Listening material is intentionally excluded from automated tests. After
installing Git LFS, render and analyze a curated sample through the same public
commands:

```bash
git lfs install
git lfs pull
build/default/rvrbotron render \
  --input samples/listening/DryHomePercussions.wav \
  --output build/listening-result
python3 tools/analyze_render.py \
  build/listening-result \
  --source samples/listening/DryHomePercussions.wav
```

The empty Composition should report exact identity. The other curated sounds
and their listening purposes are documented in
[`samples/listening/README.md`](samples/listening/README.md).

### Add a Listening Sample

WAVs in `samples/listening/` are tracked through Git LFS and remain separate
from `tests/fixtures/`:

```bash
git lfs install
git lfs track "samples/listening/*.wav"
git add .gitattributes samples/listening
git lfs ls-files
```

Add the sample's format, listening purpose, provenance, and redistribution
permission to `samples/listening/README.md`.

## Diagnostics

Failures are written to stderr with stable exit categories:

| Exit | Category |
| ---: | --- |
| 2 | `malformed_json` |
| 3 | `unsupported_audio` |
| 4 | `invalid_configuration` |
| 5 | `io_failure` |
| 6 | `internal_processing_failure` |
| 7 | `invalid_arguments` |

Pass `--error-format json` to receive the same category, reason, and optional
configuration location as one JSON object.
