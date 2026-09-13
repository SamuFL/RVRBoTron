# Extend format v2 with a neutral Composition envelope

Format version 2 adds Pre-delay and dry/wet fields without making existing
Resolved configurations unreadable. The Resolved loader treats absent
envelope fields as the exact legacy behavior: zero Pre-delay, dry gated off,
and unity wet gain; newly emitted Resolved configurations record every
requested and derived envelope value explicitly. Neutral defaults preserve
existing decoded samples exactly, so a format-version break would add archive
and migration cost without identifying a sonic incompatibility.
