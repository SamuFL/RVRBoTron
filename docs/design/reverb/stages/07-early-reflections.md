# Stage 7 — EarlyReflections

Taps out of the diffuser, bridging the gap before the tail arrives.

*Vocabulary, ground rules, and the overall shape of the chain are in the [reverb design overview](../README.md).*

---

## What it does musically

The feedback loop's shortest delay is the earliest moment any tail energy exists. At 100ms that leaves a conspicuous silence between the dry sound and the reverb, and the result reads as bolted on — a wash behind the source rather than a space containing it.

Real rooms fill that window with reflections off nearby surfaces, and those carry most of the spatial information. Two perceptual facts shape the design:

- Reflections arriving within roughly 50ms **fuse with the direct sound**, registering as presence and position rather than as echoes. Past roughly 80ms they detach into audible repeats.
- The **ratio of early energy to direct sound** is the dominant distance cue — more early energy reads as further away, independent of tail level.

The useful window is about 5–80ms, and it does the spatial work.

---

## Where the taps come from

The diffuser already generates a dense, decorrelated set of echoes on exactly this timescale, so tapping it is free.

**The tap point is the character control.** After one step the signal holds N echoes — sparse, slap-back-like. After three it holds N³ — dense and smooth, closer to ambience. Early taps give hard surfaces, late taps soft ones.

Multiple taps with individual gains shape the early envelope. With a `doubling` distribution the early steps are short, so taps after steps 1 and 2 land naturally in the 5–40ms window without separate delay times.

---

## What it does mathematically

A parallel path: tapped signal scaled and summed into the output alongside the tail, never re-entering the diffuser or loop.

### Alignment matters here

The diffuser's output is **aligned** — all N channels carry echoes at the same times, differing only in sign. Summing all N reinforces coherently and produces a peaky, comb-flavoured result.

So don't sum. **Take one or two channels.** The mixing has already distributed every echo into every channel, so a single channel holds the complete pattern; two different channels give a decorrelated stereo pair for free, because their sign patterns differ.

The feedback loop's output is unaligned, so summing is safe there. Two signals, two rules — which is why alignment is tracked.

### Energy

Not all-pass and not claiming to be. This is a mix of dry, early, and tail with independent gains; the only invariant worth holding is that no path is counted twice.

---

## Parameters

```json
"early": {
  "taps": [
    { "afterStep": 1, "gainDb": -3 },
    { "afterStep": 2, "gainDb": -6 }
  ],
  "channelSelect": "first-two",
  "mixDb": -6
}
```

| Parameter | Value | Notes |
|---|---|---|
| `taps` | list | Step index and gain. Empty list disables. |
| `afterStep` | 1…k | The character control. |
| `gainDb` | — | Level of this tap. |
| `channelSelect` | `first-two` | Stereo pair. Default. |
| | `single` | Mono early field. |
| | `sum-all` | Diagnostic — demonstrates the coherent-summing problem. |
| `mixDb` | — | Early level against the tail. The distance cue. |

Pre-delay is not this: it shifts the whole wet path including the early reflections, and belongs to Stage 9.

---

## In code

```
EarlyReflections
  taps : step index, gain, channel selection
```

---

## What this forces on the architecture

**The diffuser must expose intermediate outputs.** `Diffuser` can no longer be a black box. The clean form: it holds a list of tap indices resolved at configuration, and its process call fills a caller-provided tap buffer alongside its normal output. No callbacks, no observers, no inversion of control — those would obscure the signal flow the code exists to make visible. Taps are read-only and must not perturb the main path.

**Tap timing is derived, not specified.** A tap after step *i* arrives at the sum of resolved step lengths up to *i*, so tap times are an output of configuration and belong in `resolved.json` for marking on plots.

---

## Invariants

- **Identity when empty.** An empty tap list gives output bit-identical to early reflections disabled.
- **Non-interference.** The diffuser's own output is bit-identical with and without taps configured.
- **Tap timing.** An impulse appears at tap *i* at the sum of resolved step lengths up to *i*, within one sample.
- **No double counting.** Total output energy equals dry plus early plus tail at their configured gains.
- **Stereo decorrelation.** Under `first-two`, the two early channels are not identical.

---

## Worth sweeping early

- `afterStep` 1 through k at fixed gain — tap depth as a character control.
- `mixDb` across a wide range on a dry vocal — the distance cue.
- `channelSelect` `first-two` against `sum-all` — makes the alignment problem audible rather than theoretical.
- Early reflections off entirely at long RT60 — hear the gap before deciding how much to fill it.
