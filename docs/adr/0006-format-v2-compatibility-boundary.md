# Archive format v1 instead of carrying its parser forward

Reverb configuration format version 2 intentionally stops loading Requested
and Resolved format-version-1 configurations rather than carrying two complete
configuration and validation paths through the research code. The executable
archive is annotated tag `format-v1-final`, commit
`8a4e7180d77f1281808a7d8dc45aa37e1599f9b1`, which remains able to render and
analyze version-1 work; version-2 errors point there explicitly. Existing
positionally seeded values remain unchanged in version 2 except for the two new
domain-separated Early and Main Downmix sites, and unrelated render, catalog,
and analysis artifact schema versions remain independent.
