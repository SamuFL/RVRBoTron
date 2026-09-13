# Listening samples

This directory contains project-owner-curated material for subjective reverb
experiments. These samples are separate from the deterministic technical
fixtures under `tests/fixtures/` and are not used by automated tests or CI.

## Corpus

| File | Format | Listening purpose | Provenance |
| --- | --- | --- | --- |
| `DryHomePercussions.wav` | Stereo, 48 kHz, 24-bit PCM, 16.5 s | Hear transient smearing, Early Reflections, and rhythmic echoes. | Original recording created by the project owner. |
| `PianoDry.wav` | Stereo, 48 kHz, 24-bit PCM, 20.0 s | Hear tonal Coloration, resonances, and decay behavior. | Original recording created by the project owner. |
| `PadDry.wav` | Stereo, 48 kHz, 24-bit PCM, 42.5 s | Hear sustained spectral movement and Modulation behavior. | Original sound created by the project owner. |
| `VocalsHmmDry.wav` | Stereo, 48 kHz, 24-bit PCM, 16.0 s | Hear intelligibility, distance, and spatial cues. | Edited from [sound 439504 by drotzruhn](https://freesound.org/people/drotzruhn/sounds/439504/), released under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/). |
| `StereoWidePingPong.wav` | Stereo, 48 kHz, 24-bit PCM, 20.0 s | Hear stereo preservation, Correlation, and Downmix behavior. | Original sound created by the project owner. |

The project owner authorizes redistribution of the four original samples with
RVRBoTron. The edited vocal sample may be redistributed under CC0 1.0; its
source is credited as a courtesy.

## Adding samples

WAV files in this directory are tracked through Git LFS:

```bash
git lfs install
git add .gitattributes samples/listening
git lfs ls-files
```

Document each new sample's listening purpose, audio format, provenance, and
redistribution permission in the table above. Do not add listening samples to
the automated fixture matrix.
