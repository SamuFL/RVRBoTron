# Define reproducibility at two levels

RVRBoTron guarantees Repeat determinism as exact decoded-sample reproduction for the same renderer binary, precision, input, and configuration. Across macOS Apple Silicon, macOS Intel, and Windows, it instead requires Cross-platform equivalence using layered, stage-specific numerical and measurement tolerances, because compiler and architecture differences can make universal bit identity impractical once feedback compounds floating-point differences.
