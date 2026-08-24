# Stage 2 — DiffusionStep

Delay, shuffle and invert, mix. The unit of diffusion.

*Vocabulary, ground rules, and the overall shape of the chain are in the [reverb design overview](../README.md).*

---

## What it does musically

A diffusion step turns a set of echoes into N times as many. One step alone doesn't sound like reverb — a click returns a short burst of taps. It's the compounding that matters: four steps at N=8 turn one echo into 4096. This is why the design needs no magic numbers. You don't tune a step to sound good; you stack enough of them that the result stops sounding like anything.

`lengthMs` is the only setting with obvious character. Short steps (a few ms) pack echoes tightly and read as bright and grainy; long steps (50ms+) spread them and read as space.

Shuffling and polarity flipping are inaudible in isolation and matter in aggregate — they stop successive steps being self-similar. Turn them off and the diffuser acquires a metallic ring.

---

## What it does mathematically

```
  in ─► per-channel delay ─► shuffle + polarity flip ─► mixing matrix ─► out
```

### All-pass by construction

Each operation preserves energy on its own: delay moves it in time, permutation between channels, polarity flip squares away, and the mixing matrix is orthogonal. A composition of energy-preserving operations is energy-preserving — so there is no gain to trim and no stability condition. This is why a random number generator can be thrown at the delay times and the result still works.

Contrast the Schroeder all-pass, which is flat by *cancellation* between a feedforward and feedback pair. Cancellation is fragile and hard to see; composition is obvious from the parts list.

### Normalisation

A matrix of ±1 is not orthogonal — its rows have norm √N, so it multiplies energy by N. The orthogonal matrix is (1/√N)·H. At N=2, input (a, b) becomes (1/√2)(a+b, a−b), and ½[(a+b)² + (a−b)²] = a² + b².

Geometrically: at each instant the N channel values form a vector in ℝᴺ, and an orthogonal matrix is a rotation or reflection, which preserves length. The ±1 pattern only makes that rotation cheap to compute. In practice, run the fast Walsh–Hadamard transform and apply the single 1/√N scaling at the end.

Skipping the scaling compounds silently — four steps at N=8 would add 36 dB.

### Echo arithmetic and alignment

One Echo path present in all N Channels branches through N per-channel delays before mixing. The step therefore creates N structural paths per incoming path. Timing collisions, zero matrix coefficients, or cancellation can make the number of Distinct arrivals smaller; that observable count belongs to analysis rather than the structural invariant.

With dense equal-magnitude Hadamard mixing, output Channels share the same set of arrival times and differ only in sign. Other valid matrices and timing collisions can reduce that overlap, so analysis reports an Alignment score rather than assuming perfect support equality for every N and matrix.

Alignment is a property of the signal, not of the stage:

- **Diffuser output: aligned.** Summing channels reinforces coherently.
- **FeedbackLoop output: unaligned.** Summing is safe.

Stages 7 and 8 depend on this distinction.

### Choosing delay times

Range [0, `lengthMs`]. `segmented-random` is the default: partition the resolved integer sample positions into N non-empty segments and pick one value from each. `even` also resolves distinct sample positions. Both reject a step too short to provide N positions. `uniform-random` deliberately samples with replacement, so clumping and collisions remain part of that comparison.

Segment ordering across channels is irrelevant, because the shuffle immediately follows.

### Why shuffle and polarity are separate from the matrix

They're mathematically redundant — both are orthogonal matrices and could be folded into one precomputed matrix with identical arithmetic. They stay separate because **a matrix of a given type is shared across steps while the shuffle and polarity pattern must differ in every step**, and folding hides that.

The distinction is load-bearing. The normalised Sylvester–Hadamard matrix is symmetric and orthogonal, hence its own inverse: applied twice it returns the input. Consecutive steps with the same matrix are structurally self-cancelling, and only the intervening delays prevent literal cancellation. The residual regularity in the phase response is the metallic ring. Varying shuffle and polarity per step breaks the symmetry.

---

## Parameters

```json
{
  "lengthMs": 40,
  "delayStrategy": "segmented-random",
  "mix": "hadamard",
  "shuffle": true,
  "polarity": "seeded-random"
}
```

| Parameter | Value | Notes |
|---|---|---|
| `lengthMs` | > 0 | Upper bound of the delay range. The main sonic control. |
| `delayStrategy` | `segmented-random` | Default. |
| | `uniform-random` / `even` | Comparison and diagnostic. |
| `mix` | `hadamard` | Default. Powers of two only. |
| | `householder` / `random-orthogonal` | Any N. RandomOrthogonal is seeded and dense, without a Haar-uniformity claim. |
| `shuffle` | `true` / `false` | `false` is an ablation. |
| `polarity` | `seeded-random` / `none` | |

No seed field — seeds are derived positionally.

---

## In code

```
DiffusionStep
  delays   : per-channel delay lines, integer length
  shuffle  : permutation
  polarity : per-channel ±1
  mix      : MixMatrix
```

Members in signal order; reading the class should read as the diagram.

**No feedback anywhere in this stage.** The whole diffuser can therefore be processed in arbitrarily large blocks without sub-block splitting — the thing that normally makes reverbs awkward to optimise, and which Schroeder-based diffusers cannot claim. Irrelevant to the harness, very relevant to the plugin.

**Delay lines are integer-length here.** Fractional reads become necessary only when Stage 6 modulates them.

---

## What this forces on the architecture

**Seeds derive positionally, not from a shared stream.** A single sequential RNG consumed as steps are constructed would mean that comparing a 3-step diffuser with a 4-step one also changes the delay times of the first three — four unrelated diffusers, and the plots would look fine. Format version 1 uses domain-separated SplitMix64 derivation from the global seed, usage tag, and item indices; see [ADR-0002](../../../adr/0002-version-positional-random-resolution.md).

RandomOrthogonal starts from a versioned, seeded dense matrix with values in [−1, 1], then applies deterministic Householder QR with a fixed sign convention. The resolved coefficient matrix is serialized; it is not regenerated when rendering from `resolved.json`.

**Ablation is a first-class feature.** `shuffle: false`, `polarity: none`, and `delayStrategy: even` exist so their contributions can be heard and measured. Validation must permit deliberately bad reverbs.

---

## Invariants

- **All-pass.** Energy out equals energy in for any N, strategy, and matrix.
- **Matrix normalisation.** MMᵀ = I, not N·I. Test on random vectors, not on the construction.
- **Echo multiplication.** Every incoming Echo path creates N structural paths; Distinct arrivals are measured after timing collisions and cancellation.
- **Alignment.** Hadamard output has identical arrival support across Channels absent cancellation; other matrix cases are measured with Alignment score.
- **Seed stability.** A step at index *i* is unaffected by how many steps precede or follow it.
- **Hadamard involution.** Applying the matrix twice returns the input — documents why per-step shuffling exists, and fails if the construction is changed to something non-symmetric.
- **No feedback.** One block and many blocks give bit-identical output.

---

## Worth sweeping early

- `lengthMs` across two orders of magnitude at fixed step count.
- `shuffle` and `polarity` ablations — the fastest way to hear structural repetition.
- `mix` across all three matrices at N=8.
- `delayStrategy: even` against `segmented-random` — coloration on purpose.
