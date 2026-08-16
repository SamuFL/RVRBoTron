# Stage 1 — Split

Expands the mono or stereo input into N internal channels.

*Vocabulary, ground rules, and the overall shape of the chain are in the [reverb design overview](../README.md).*

---

## What it does musically

Inaudible in itself. It creates the workspace the rest of the algorithm operates in — N channels that are not speakers, not outputs, and never listened to.

Its one audible consequence is **stereo behaviour**, decided here rather than at the output. A mono-summed split gives a tail with no memory of stereo position, so all width comes from the downmix; a stereo-preserving split carries source position into the tail, so a hard-left guitar produces a reverb that leans left. Neither is correct — mono-summed gives a homogeneous, enveloping space, stereo-preserving a more literal, positional one.

---

## What it does mathematically

An N×2 matrix (N×1 for mono input).

**The energy trap.** Copying one signal into N channels gives N times the energy, but the channels are perfectly correlated — summed coherently later, that becomes N² times the power. Left unhandled, changing N changes output level and every N-sweep becomes uninterpretable.

Convention: **scale by 1/√N.** Energy in equals energy out from the first stage, and sweeping N changes the sound without changing the level.

**A measurement baseline.** At Split, inter-channel correlation is exactly 1.0 by construction — the state the diffuser exists to destroy. How fast it falls toward 0 through successive diffusion steps is a direct measure of diffusion quality, independent of listening. Worth building the correlation probe early.

---

## Parameters

```json
"split": {
  "channels": 8,
  "strategy": "duplicate",
  "normalisation": "energy"
}
```

| Parameter | Value | Notes |
|---|---|---|
| `channels` | ≥ 1 | N. The big sweep axis; constrains matrix choice downstream. |
| `strategy` | `duplicate` | Summed to mono, copied to all channels. No position retention. |
| | `stereo-halves` | L → first N/2, R → last N/2. Strongest position retention. |
| | `stereo-interleave` | L → even, R → odd. Survives channel shuffling better. |
| `normalisation` | `energy` | 1/√N. Default. |
| | `unity` / `none` | Diagnostic. |

---

## In code

`Split`, holding a `SplitStrategy`. Strategy pattern, because new splitting strategies are an expected research output — adding one should mean one small class and one config string, with no changes elsewhere in the chain.

---

## What this forces on the architecture

Sweeping N to 20 doesn't work with Hadamard, and discovering that here settles three things:

1. **Mixing matrices become a first-class abstraction with a validity rule.** Hadamard exists only for N = 1, 2, and multiples of 4, with the fast construction limited to powers of two. Householder works for any N. RandomOrthogonal comes almost free once the abstraction exists.
2. **Validity is enforced at configuration load, loudly.** A silent fallback would produce audio that sounds plausible and is subtly wrong — the worst failure mode for a research tool.
3. **N is runtime, not compile-time.** Fixed-capacity arrays with a runtime length keep processing allocation-free.

---

## Invariants

- Energy out equals energy in under `normalisation: energy`, for any N and strategy.
- Inter-channel correlation at the output is 1.0 for `duplicate`.
- Sweeping N changes the sound, not the level.

---

## Worth sweeping early

- `channels` across 4 / 8 / 16 / 20 — confirms level independence before anything else is trusted.
- `strategy` on a wide stereo source — mono-summed against position-preserving.
