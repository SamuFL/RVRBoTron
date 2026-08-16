# RVRBoTron Reverb

RVRBoTron explores a configurable algorithmic reverb whose internal structure can be heard, measured, and compared reproducibly.

## Language

**Channel**:
One of the N internal signal paths between Split and Downmix. It is a workspace dimension, not a speaker or output.
_Avoid_: Speaker, output channel

**N**:
The runtime-selected number of internal Channels.
_Avoid_: Track count

**Split**:
The boundary that expands mono or stereo input into N Channels.
_Avoid_: Input adapter

**Diffusion Step**:
One energy-preserving delay, shuffle, polarity, and mixing operation that multiplies echo density.
_Avoid_: Diffuser stage

**Diffuser**:
An ordered chain of Diffusion Steps that increases echo density without feedback or decay.
_Avoid_: Feedback diffuser

**Feedback Loop**:
The circulating delay network that gives the reverb tail its size and decay.
_Avoid_: Diffuser

**Damping**:
Frequency-dependent decay created by filtering on every circulation through the Feedback Loop.
_Avoid_: Output EQ

**Modulation**:
Seeded movement of delay times that smears fixed resonances.
_Avoid_: Chorus

**Early Reflections**:
The diffuser taps mixed in parallel before the reverb tail arrives.
_Avoid_: Pre-delay

**Downmix**:
The boundary that maps the internal Channels to stereo output.
_Avoid_: Output adapter

**Aligned**:
A signal whose Channels carry the same echo times while differing in sign or amplitude.
_Avoid_: Correlated

**All-pass**:
Energy-preserving behavior in which energy may move between Channels and through time but is neither created nor lost.
_Avoid_: No coloration

**Echo density**:
The number of distinct echoes per second.
_Avoid_: Diffusion

**Coloration**:
Timbral character imposed by regularity in the reverb's phase response.
_Avoid_: Damping

**RT60**:
The requested time for the reference band to decay by 60 dB.
_Avoid_: Tail length

**Correlation**:
The measured similarity between Channels, from identical at 1.0 to independent at 0.0.
_Avoid_: Alignment

**Requested configuration**:
The user-authored description of the desired reverb.
_Avoid_: Resolved configuration

**Resolved configuration**:
The complete, versioned record of concrete values used to construct and reproduce a reverb.
_Avoid_: Requested configuration

**Composition**:
The configured set, order, and wiring of reverb stages together with controls that apply to the complete wet path.
_Avoid_: Pipeline, graph

**Render Result**:
The immutable audio and configuration evidence produced by one render, together with append-only analyses derived from it.
_Avoid_: Output folder

**Repeat determinism**:
Exact decoded-sample reproduction for the same renderer binary, sample precision, input, and configuration.
_Avoid_: Cross-platform equivalence

**Cross-platform equivalence**:
Agreement between supported platform builds within the numerical and measurement tolerances declared for the relevant reverb stage.
_Avoid_: Repeat determinism, bit identity
