# Version positional random resolution

Format version 1 derives every randomized value with domain-separated SplitMix64 from the global seed, a fixed usage tag, and positional indices. Bounded integers use rejection sampling, doubles use the high 53 bits, and changing this mapping requires a configuration-version change because otherwise adding or changing one randomized feature would silently invalidate existing sweeps.

RandomOrthogonal uses the versioned stream to fill a dense matrix in [−1, 1], then applies deterministic Householder QR with a fixed sign convention. Every resolved matrix coefficient is serialized as a binary64-round-trippable number, including structured matrices, so rendering from `resolved.json` never regenerates random structure and float/double builds receive the same concrete matrix.
