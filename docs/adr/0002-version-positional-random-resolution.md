# Version positional random resolution

Format version 1 derives every randomized value with domain-separated SplitMix64 from the global seed, a fixed usage tag, and positional indices. Bounded integers use rejection sampling, doubles use the high 53 bits, and changing this mapping requires a configuration-version change because otherwise adding or changing one randomized feature would silently invalidate existing sweeps.

RandomOrthogonal uses the versioned stream to fill a dense matrix in [−1, 1], then applies deterministic Householder QR with a fixed sign convention. Every resolved matrix coefficient is serialized as a binary64-round-trippable number, including structured matrices, so rendering from `resolved.json` never regenerates random structure and float/double builds receive the same concrete matrix.

## Format version 1 derivation

`splitMix64(x)` adds `0x9e3779b97f4a7c15`, applies the canonical
`0xbf58476d1ce4e5b9` and `0x94d049bb133111eb` xor-shift/multiply rounds, then
the final xor shift. A positional word is:

```text
v0 = splitMix64(seed XOR usageTag)
v1 = splitMix64(v0 XOR itemIndex)
v2 = splitMix64(v1 XOR valueIndex)
word = splitMix64(v2 XOR drawIndex)
```

The first Diffusion Step uses fixed tags `0x445354455044454c` for delay,
`0x4453544550534846` for shuffle, and `0x4453544550504f4c` for polarity.
Rejection sampling increments `drawIndex` and accepts a word at or above
`(-bound) mod bound` before reducing modulo `bound`. Unit doubles use the high
53 bits scaled by 2⁻⁵³. These values and the fixed test vectors are part of
format version 1; standard-library random distributions are not used.
