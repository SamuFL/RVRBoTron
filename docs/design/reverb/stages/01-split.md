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

For mono and stereo `duplicate`, distribute the selected mono signal at 1/√N. Stereo first selects `(L + R)/√2`; this intentionally discards the Side signal, so L = −R cancels. No rank-reducing stereo-to-mono map can preserve arbitrary stereo energy.

For stereo-preserving strategies, N must be even. Each input feeds N/2 Channels at √(2/N), preserving the energy of L and R independently. The invariant is therefore precise: Split preserves the energy of the signal selected by its strategy and keeps level independent of N. It does not claim that strategies which discard source dimensions preserve them.

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
| | `stereo-halves` | L → first N/2, R → last N/2. Even N only. Strongest position retention. |
| | `stereo-interleave` | L → even, R → odd. Even N only. Survives channel shuffling better. |
| `normalisation` | `energy` | Strategy-specific energy scaling. Default. |
| | `none` | Diagnostic. |

For mono input, every strategy resolves to the same mono duplication mapping. Keeping the requested strategy valid makes one experiment catalog reusable across mono and stereo material.

---

## In code

`Split`, holding a `SplitStrategy`. Strategy pattern, because new splitting strategies are an expected research output — adding one should mean one small class and one config string, with no changes elsewhere in the chain.

---

## What this forces on the architecture

Sweeping N to 20 doesn't work with Hadamard, and discovering that here settles three things:

1. **Mixing matrices become a first-class abstraction with a validity rule.** RVRBoTron's fast Hadamard implementation exists only for powers of two. Householder and RandomOrthogonal work for any N.
2. **Validity is enforced at configuration load, loudly.** A silent fallback would produce audio that sounds plausible and is subtly wrong — the worst failure mode for a research tool.
3. **N is runtime, not compile-time.** Fixed-capacity arrays with a runtime length keep processing allocation-free.

---

## Invariants

- Energy out equals the strategy-selected signal energy under `normalisation: energy`, for every valid N and strategy.
- Inter-channel correlation at the output is 1.0 for `duplicate`.
- Sweeping N at a fixed strategy changes the sound, not the level.

---

## Worth sweeping early

- `channels` across 4 / 8 / 16 / 20 — confirms level independence before anything else is trusted.
- `strategy` on a wide stereo source — mono-summed against position-preserving.
