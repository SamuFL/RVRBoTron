# Capture internal stage evidence outside the Composition

Internal diffusion claims require N-Channel signals that a stereo Downmix cannot preserve, but capture selection does not change the sound. Stage captures are therefore opt-in renderer evidence requested with `--capture-stages all`, recorded and hashed in `render.json`, and written through an optional allocation-free capture-sink seam on the deep `Reverb` module.

Requested and Resolved Configuration remain purely sonic, ordinary and plugin processing supply no sink, and Python measures the captured C++ signals rather than reconstructing DSP. Making `output.wav` switch between stereo and internal signals or computing measurements inside C++ would blur the production interface with research evidence.
