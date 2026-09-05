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
One energy-preserving delay, shuffle, polarity, and mixing operation that branches Echo paths.
_Avoid_: Diffuser stage

**Diffuser**:
An ordered chain of Diffusion Steps that increases echo density without feedback or decay.
_Avoid_: Feedback diffuser

**Feedback Loop**:
The circulating delay network that gives the reverb tail its size and decay.
_Avoid_: Diffuser

**Loop overlap**:
The span by which the Diffuser's `totalMs` exceeds the Feedback Loop's `delayMinMs`, during which fresh Diffuser output overwrites feedback-loop delay-line content before it has meaningfully decayed, heard as constructive energy buildup rather than clean decay.
_Avoid_: Buildup, flutter

**Damping**:
Frequency-dependent decay created by filtering on every circulation through the Feedback Loop.
_Avoid_: Output EQ

**Two-shelf damping**:
The current Damping model: one low shelf and one high shelf shape decay around the 1 kHz Reference band using low- and high-band decay ratios.
_Avoid_: Three-band damping, multiband damping

**Decay tilt**:
The slope of measured per-octave-band T30 against log-frequency, expressing how strongly decay time varies with frequency.
_Avoid_: Spectral tilt, Coloration

**Modulation**:
Seeded movement of delay times that smears fixed resonances.
_Avoid_: Chorus

**Excursion**:
The symmetric peak deviation, in milliseconds, of a modulated delay above and below its nominal resolved length.
_Avoid_: Depth, sweep width

**Interpolation margin**:
The fixed extra delay-line headroom reserved so any interpolation method can read past the Excursion bound without overrunning.
_Avoid_: Guard band, padding

**Detune product**:
The product of modulation depth and rate, which governs perceived pitch deviation.
_Avoid_: Modulation amount

**Early Reflections**:
The diffuser taps mixed in parallel before the reverb tail arrives.
_Avoid_: Pre-delay

**Downmix**:
The boundary that maps the internal Channels to stereo output.
_Avoid_: Output adapter

**Aligned**:
A signal whose Channels carry the same echo times while differing in sign or amplitude.
_Avoid_: Correlated

**Alignment score**:
The pairwise overlap of active arrival times between Channels, measured independently of amplitude sign.
_Avoid_: Correlation

**All-pass**:
Energy-preserving behavior in which energy may move between Channels and through time but is neither created nor lost.
_Avoid_: No coloration

**Echo density**:
The number of distinct arrivals per second in an impulse response.
_Avoid_: Diffusion

**Echo path**:
One structural propagation route through the Diffuser; k Diffusion Steps over N Channels create N^k paths before timing collisions or cancellation.
_Avoid_: Distinct arrival

**Distinct arrival**:
One output time containing energy from one or more Echo paths after timing collisions and cancellation.
_Avoid_: Echo path

**Coloration**:
Timbral character imposed by regularity in the reverb's phase response.
_Avoid_: Damping

**Reference band**:
The 1 kHz octave band, at which RT60 is defined and against which frequency-dependent decay is expressed as deviation.
_Avoid_: Mid band, undamped band

**RT60**:
The requested time for the Reference band, the 1 kHz octave, to decay by 60 dB.
_Avoid_: Tail length

**Tail budget**:
The resolved upper bound on the frames a render writes after its input ends, derived from RT60.
_Avoid_: Tail length, drain length, finite response

**Block-size bound**:
The resolved upper bound on legal block size, derived from the Feedback Loop's shortest resolved per-Channel delay less any modulation Excursion applied to it.
_Avoid_: Maximum block size, buffer size limit

**Correlation**:
The normalized zero-lag dot product between Channel signals: 1.0 identical, -1.0 polarity-inverted, and 0.0 linearly independent at zero lag.
_Avoid_: Alignment

**Output correlation**:
The signed normalized zero-lag similarity between the two Downmix output signals, used to detect coherent modulation surviving into the summed output.
_Avoid_: Channel decorrelation

**Requested configuration**:
The user-authored description of the desired reverb.
_Avoid_: Resolved configuration

**Resolved configuration**:
The complete, versioned record of concrete values used to construct and reproduce a reverb.
_Avoid_: Requested configuration

**Reference configuration**:
The documented experimental baseline resolved when requested stage settings are omitted, used to change one research axis at a time.
_Avoid_: Product default, preset

**Composition**:
The configured set, order, and wiring of reverb stages together with controls that apply to the complete wet path.
_Avoid_: Pipeline, graph

**Render Result**:
The immutable audio and configuration evidence produced by one render, together with append-only analyses derived from it.
_Avoid_: Output folder

**Stage capture**:
Optional immutable multi-Channel audio evidence recorded at a named Composition boundary for measurement without changing the stereo output.
_Avoid_: Output, debug dump

**DSP benchmark**:
An environment-qualified empirical measurement of processing time and DSP-owned memory around Reverb processing, excluding configuration, file I/O, and acoustic analysis.
_Avoid_: Render Result analysis, deterministic metric

**Repeat determinism**:
Exact decoded-sample reproduction for the same renderer binary, sample precision, input, and configuration.
_Avoid_: Cross-platform equivalence

**Cross-platform equivalence**:
Agreement between supported platform builds within the numerical and measurement tolerances declared for the relevant reverb stage.
_Avoid_: Repeat determinism, bit identity
