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

One echo present in all N channels: the per-channel delays move it to N different times, then the matrix distributes every channel into every other, so each output channel holds all N times. One echo in, N out, per channel.

The output channels therefore share the **same set of echo times**. With Hadamard every entry has equal magnitude, so they differ only in sign. The echoes are **aligned across channels** again — which is why the next step's delay does the same job as this one's.

Alignment is a property of the signal, not of the stage:

- **Diffuser output: aligned.** Summing channels reinforces coherently.
- **FeedbackLoop output: unaligned.** Summing is safe.

Stages 7 and 8 depend on this distinction.

### Choosing delay times

Range [0, `lengthMs`]. `segmented-random` is the default: divide the range into N equal segments, pick one random value in each, one per channel. Approximately even spread with no regular pattern — velvet-noise tap placement applied across channels. Even spacing produces comb coloration; uniform random clumps.

Segment ordering across channels is irrelevant, because the shuffle immediately follows.

### Why shuffle and polarity are separate from the matrix

They're mathematically redundant — both are orthogonal matrices and could be folded into one precomputed matrix with identical arithmetic. They stay separate because **the mixing matrix is the same in every step while the shuffle and polarity pattern must differ in every step**, and folding hides that.

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
| | `householder` / `random-orthogonal` | Any N. |
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

**Seeds derive positionally, not from a stream.** A single sequential RNG consumed as steps are constructed would mean that comparing a 3-step diffuser with a 4-step one also changes the delay times of the first three — four unrelated diffusers, and the plots would look fine. Each step's randomness must be a pure function of the global seed and the step index. Cheap now; retrofitting invalidates every existing render.

**Ablation is a first-class feature.** `shuffle: false`, `polarity: none`, and `delayStrategy: even` exist so their contributions can be heard and measured. Validation must permit deliberately bad reverbs.

---

## Invariants

- **All-pass.** Energy out equals energy in for any N, strategy, and matrix.
- **Matrix normalisation.** MMᵀ = I, not N·I. Test on random vectors, not on the construction.
- **Echo multiplication.** An aligned impulse produces exactly N distinct echo times per output channel.
- **Alignment.** Output echo times are identical across channels; with Hadamard only the sign differs.
- **Seed stability.** A step at index *i* is unaffected by how many steps precede or follow it.
- **Hadamard involution.** Applying the matrix twice returns the input — documents why per-step shuffling exists, and fails if the construction is changed to something non-symmetric.
- **No feedback.** One block and many blocks give bit-identical output.

---

## Worth sweeping early

- `lengthMs` across two orders of magnitude at fixed step count.
- `shuffle` and `polarity` ablations — the fastest way to hear structural repetition.
- `mix` across all three matrices at N=8.
- `delayStrategy: even` against `segmented-random` — coloration on purpose.
