# RVRBoTron — Overview

Ground rules, shared vocabulary, and the overall shape of the reverb.

Per-stage reasoning lives in the stage documents listed in Part III.

---

## Part I — Ground rules

**Research, not product.** The purpose is to understand algorithmic reverb by making experiments cheap, measurable, and repeatable. The DSP is written to production standards because it becomes the shipping core of the plugin; the harness around it is an instrument.

Consequence: which knobs eventually appear in the plugin is explicitly undecided. Every parameter is exposed, sweepable, and equal in status. Premature narrowing is how a research harness stops teaching you things.

**The code speaks the domain.** If a concept has a name in the field, that's its name in the code. No `Processor`, `Node`, `Handler`, `Manager`, `InputAdapter`. If a name isn't something you'd say out loud about reverb, it's wrong. Every step of the algorithm stays visible — no stage inlined into another for elegance or speed.

**The composition is data.** Which stages are present, in what order, with what settings, lives in a configuration file. Changing step count, matrix, or channel count is an edit, not a rebuild.

**Everything structural is a runtime value.** Channel count, step count, matrix choice, strategy selection. Costs some performance; buys the ability to hear N=20 in seconds. Allocation-free processing is preserved by allocating once at configuration.

**Energy is the organising invariant.** Every stage declares whether it preserves energy, and those claiming to are tested for it. Scaling is explicit and by stated convention, never incidental.

**Determinism is mandatory.** Every randomised quantity derives from an explicit seed. The same renderer binary, sample precision, input, and configuration produce exactly the same decoded samples. Supported platform builds satisfy stage-specific numerical and measurement tolerances rather than universal bit identity; see [ADR-0001](../../adr/0001-cross-platform-reproducibility.md).

**Measurement accompanies listening.** A measurement contradicting your ears is measuring the wrong thing; a preference you can't measure is an unfinished hypothesis.

**`dsp/` depends on nothing** outside the standard library. No file I/O, no JSON, no logging. This makes the iPlug2 lift a copy rather than a port.

---

## Part II — Vocabulary

| Term | Meaning |
|---|---|
| **Channel** | One of the N internal signal paths. Not a speaker, not an output. A workspace dimension. |
| **N** | Internal channel count. The most consequential structural parameter. |
| **All-pass** | Output energy equals input energy. Energy may move between channels and in time; none is created or lost. The multi-channel generalisation of flat frequency response. |
| **Orthogonal matrix** | A mixing matrix that is all-pass. All mixing matrices here are orthogonal. |
| **Echo density** | Distinct echoes per second. Around 2000–4000/s they fuse into continuous sound. |
| **Aligned** | All channels carry the same echo times, differing in sign or amplitude. The diffuser produces aligned output; the feedback loop destroys alignment. Determines whether channels may be summed. |
| **Coloration** | Timbral character imposed by the reverb itself, usually from regularity in the phase response. |
| **RT60** | Time to decay 60 dB. An input the user requests; gain is solved from it. Defined at the undamped reference band. |
| **Correlation** | Similarity between channels. 1.0 identical, 0.0 independent. Diffusion drives it toward 0. |

### Mixing matrices

A first-class abstraction with a validity rule, because matrix choice is a research axis and validity depends on N. All are normalised so MMᵀ = I.

| Matrix | Valid for | Character |
|---|---|---|
| **Householder** | any N | Mean of channels, subtracted twice from each. Cheap, mild mixing. Default in the feedback loop. |
| **Hadamard** | powers of two | Maximum mixing, N·log₂N additions. Default in the diffuser. |
| **RandomOrthogonal** | any N | Seeded. Tests whether maximum mixing is genuinely best. |

Validity is enforced at configuration load, loudly. Hadamard at N=20 is a hard error — never a silent fallback, never a matrix that quietly isn't orthogonal.

---

## Part III — The shape of the reverb

```
                                              ┌──────────────────┐
                                        ┌────►│ EarlyReflections ├────┐
                                        │     └──────────────────┘    ▼
  in ─► Split ─► Diffuser ──────────────┴────► FeedbackLoop ──────► Downmix ─► out
           │         │                              │
        N chans  DiffusionStep × k          delays, decay gain, mix
                                            + Damping + Modulation
```

Everything between Split and Downmix is multi-channel. The two halves do separate jobs:

- **Diffuser** makes the sound *diffuse*. All-pass throughout, no feedback, so it imposes no coloration.
- **FeedbackLoop** makes the sound *long-lasting*. Contains feedback, so it isn't all-pass — but needn't build echo density, because the diffuser already did.

Keeping these separate is the whole design. Asking the feedback loop to also diffuse is what makes conventional FDN reverbs a tuning problem.

Damping and Modulation are components of the FeedbackLoop, not stages in the chain; they are documented separately because their parameters are conceptually distinct.

| Stage | Role | File |
|---|---|---|
| 1. Split | mono/stereo → N channels | [`stages/01-split.md`](stages/01-split.md) |
| 2. DiffusionStep | delay → shuffle/invert → mix | [`stages/02-diffusion-step.md`](stages/02-diffusion-step.md) |
| 3. Diffuser | chain of diffusion steps | [`stages/03-diffuser.md`](stages/03-diffuser.md) |
| 4. FeedbackLoop | delays, decay gain, mix | [`stages/04-feedback-loop.md`](stages/04-feedback-loop.md) |
| 5. Damping | frequency-dependent decay | [`stages/05-damping.md`](stages/05-damping.md) |
| 6. Modulation | fractional delay movement | [`stages/06-modulation.md`](stages/06-modulation.md) |
| 7. EarlyReflections | taps out of the diffuser | [`stages/07-early-reflections.md`](stages/07-early-reflections.md) |
| 8. Downmix | N channels → stereo | [`stages/08-downmix.md`](stages/08-downmix.md) |
| 9. Composition | assembly, resolution, validation | [`stages/09-composition.md`](stages/09-composition.md) |

Each stage document follows the same shape: what it does musically, what it does mathematically, parameters, name in code, what it forces on the architecture, invariants, and what to sweep first.

---

## Part IV — Standing invariants

1. **Allocation-free processing.** All allocation at configuration; none while audio flows.
2. **Reproducibility.** Repeat renders are exact; supported platform builds satisfy the relevant stage's declared equivalence tolerances.
3. **All-pass where claimed.** Verified against random input within tolerance.
4. **Orthogonality.** Every mixing matrix satisfies MMᵀ = I.
5. **Matrix validity.** No matrix constructed for an unsupported N.
6. **Numerical hygiene.** No NaN, no denormal stalls, monotonic decay after input ceases.
7. **Dependency direction.** `dsp/` includes nothing outside the standard library.
8. **Level independence.** Output level is unchanged by N, by strategy choices, and by width.
9. **Identity at neutral.** Every optional component, at its neutral setting, produces output bit-identical to that component disabled.

---

## Part V — Conventions

**Sample type is a build parameter.** `float` by default; `double` available. Feedback compounds rounding error, so when a tail sounds grainy it's either the algorithm or the arithmetic — rendering both tells you which.

**Time is in milliseconds** everywhere. Conversion to samples happens once, at configuration.

**C++ makes sound; Python looks at sound.** Rendering is C++; measurement and plotting are Python, communicating through WAV files and JSON.

**RT60 is measured per octave band** from the first version of the analysis code. Once damping is active there is no single decay time.

**Library choices are made during implementation**, case by case. The only hard rule is that none may appear inside `dsp/`.

**Third-party code is tracked.** The forked Signalsmith code is MIT: usable and modifiable, but the copyright notice and license text must travel with any distribution. `THIRD_PARTY_LICENSES.md` from the first commit.
