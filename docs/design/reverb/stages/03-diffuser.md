# Stage 3 — Diffuser

A chain of diffusion steps. Makes the sound diffuse, without feedback.

*Vocabulary, ground rules, and the overall shape of the chain are in the [reverb design overview](../README.md).*

---

## What it does musically

Owns what the individual steps don't know: how many steps there are, how long diffusion lasts in total, and how that total is distributed.

- **Total length** — how long the smear lasts before the tail takes over. 50ms reads as a thickening of the source; 500ms+ is audibly its own event.
- **Step count** — smoothness. Too few and you hear grains.
- **Distribution** — the envelope of the smear: where it is dense and where it thins.

No decay, no feedback. The diffuser changes *when* sound happens, never *how much*.

---

## What it does mathematically

A sequence of *k* all-pass steps, so the chain is all-pass by composition. Dense Hadamard diffusion preserves shared arrival support absent cancellation; Alignment score measures matrix and collision edge cases rather than assuming them away.

### Density

Each step multiplies structural Echo-path count by N:

    path rate ≈ Nᵏ / T   paths per second

Timing collisions and cancellation mean Nᵏ is not an exact count of Distinct arrivals. The diffusion analyzer measures actual arrival density from the impulse response in 10 ms bins. The path-rate equation remains a useful upper-bound design estimate before rendering; continuous sound typically emerges around 2000–4000 Distinct arrivals/s.

Note the asymmetry: density scales as a *power* of step count and only linearly in 1/T. One extra step at N=8 multiplies density by eight; halving the length only doubles it. Step count is the strong lever.

### Distribution

Step *i* of *k*, total length *T*:

| Strategy | Step length | Character |
|---|---|---|
| `even` | T/k | Dense in the middle, rough at both ends. |
| `doubling` | T·2ⁱ / (2ᵏ−1) | Short steps first. Density builds sooner, envelope is flatter. |
| `explicit` | given | Manual control. |

`doubling` is the default: short early steps reach useful density quickly so the onset is smooth, and the long final step spreads the tail end instead of stopping abruptly. The difference is subtle but visible in the echo-density curve.

Time conversion uses one resolved integer-sample budget for T. Step budgets are apportioned by largest remainder with step index as the deterministic tie-breaker, so their sample counts sum exactly to the resolved total.

### Onset

With `segmented-random`, each step's first segment starts at zero, so the shortest path through the diffuser is near-instant. The audible gap before the tail comes from the feedback loop's delays, not from here — that gap is Stage 7's business.

---

## Parameters

```json
"diffuser": {
  "steps": 4,
  "totalMs": 300,
  "distribution": "doubling",
  "step": {
    "delayStrategy": "segmented-random",
    "mix": "hadamard",
    "shuffle": true,
    "polarity": "seeded-random"
  }
}
```

| Parameter | Value | Notes |
|---|---|---|
| `steps` | ≥ 1 | *k*. The strong density lever. |
| `totalMs` | > 0 | *T*. Sum of all step lengths. |
| `distribution` | `doubling` / `even` | Used with `steps` and `totalMs`. |
| `lengthsMs` | array | Defines explicit distribution, step count, and total; mutually exclusive with `steps`, `totalMs`, and `distribution`. |
| `step` | — | Defaults inherited by every step; individual steps may override any field. |
| `stepOverrides` | array | Sparse zero-based indexed exceptions to inherited step settings. |

Step settings are given once and inherited. Repeating them *k* times would obscure which differences are deliberate.

---

## In code

```
Diffuser
  steps : ordered DiffusionStep
```

Length resolution happens at configuration; `Diffuser` holds resolved lengths, not a strategy.

---

## What this forces on the architecture

**Resolved configuration is an output artifact.** Every render emits `resolved.json` beside its WAV — actual step lengths, per-channel delay times, permutations, polarity patterns, and every resolved matrix coefficient. Analysis needs to know what was built rather than what was requested, and recomputing a doubling distribution by hand while reading a plot weeks later is exactly the friction that stops people using their own tools.

**The step-count sweep is the canonical experiment**, and it means nothing without the positional seeding rule from Stage 2.

---

## Invariants

- **All-pass** for any *k* and distribution.
- **Echo paths.** k steps create Nᵏ structural paths before timing collisions and cancellation.
- **Density.** Distinct-arrival count and density are measured from the rendered impulse response.
- **Length conservation.** Resolved step lengths sum to `totalMs`, within sample rounding.
- **Seed stability.** Increasing `steps` leaves preceding steps unchanged.
- **Alignment.** Hadamard cases have perfect arrival-support overlap absent cancellation; every matrix reports Alignment score.
- **No feedback.** Block-size independent.

---

## Worth sweeping early

- `steps` 1→6 at N=8 — the density equation made audible and plottable.
- `distribution` `even` against `doubling` at fixed *k* and *T*.
- `totalMs` across an order of magnitude at fixed *k*.
- N against `steps` at constant Nᵏ — N=4/k=6 against N=8/k=4. Same echo count, different cost. If they sound the same, channel count is purely a CPU decision.
