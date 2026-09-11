# Vendored: Ace 1.44.0

The Research bench's JSON editor (issue #138). Three files from one official
prebuilt distribution, plus its license. Nothing here is generated or
modified from upstream; a byte for byte match is what the hashes below prove.

## Package identity

| Field | Value |
| --- | --- |
| Package | `ace-builds` |
| Version | `1.44.0` |
| Registry | https://registry.npmjs.org/ace-builds/-/ace-builds-1.44.0.tgz |
| Registry `shasum` (sha1) | `d657730f665fccf72d2945d95887e12c514a29f1` |
| Registry `integrity` (sha512) | `sha512-PFNMSYqFdEUkul2Ntud0HvA09AgY+F1ag0UYdpMH60wNI/qOA8cB8tlTgoALMEwIdUPJK2CjrIQ7OnbiSS/ugQ==` |
| Upstream repository | https://github.com/ajaxorg/ace-builds |
| Upstream tag | [`v1.44.0`](https://github.com/ajaxorg/ace-builds/releases/tag/v1.44.0) |
| Source commit | `184177de1dcc5946b093edba0b0fe1c29c2a127a` |
| Distribution variant | `src-min-noconflict/` (minified; does not overwrite a global `define`/`require`) |
| License | BSD-3-Clause, Copyright (c) 2010, Ajax.org B.V. |

The registry `shasum` and `integrity` were verified against the downloaded
tarball before extracting anything from it.

## Build provenance

The registry publish carries a [SLSA v1 build
provenance](https://slsa.dev/provenance/v1) attestation
(`https://registry.npmjs.org/-/npm/v1/attestations/ace-builds@1.44.0`),
independently confirming the same source commit:

```
buildType: https://slsa-framework.github.io/github-actions-buildtypes/workflow/v1
workflow:  ajaxorg/ace-builds .github/workflows/publish.yml @ refs/tags/v1.44.0
builder:   https://github.com/actions/runner/github-hosted
resolved:  git+https://github.com/ajaxorg/ace-builds@refs/tags/v1.44.0
           gitCommit 184177de1dcc5946b093edba0b0fe1c29c2a127a
```

That commit matches the tag above, the registry manifest's `gitHead`, and the
files vendored here: three independent sources agreeing on one artifact.

## Vendored files and per-file hashes

Every file below is the unmodified upstream file at the given path. `worker-json.js`
exists in the same distribution and is deliberately **not** vendored — Ace
workers are omitted from the bench (see `serve.py` and `bench.js`, which also
disable `useWorker` explicitly rather than relying on that omission alone).

| File here | Upstream path | Bytes | SHA-256 |
| --- | --- | --- | --- |
| `ace.js` | `src-min-noconflict/ace.js` | 475029 | `072d13e53d11e2ceccfffe1a0fa7f15cf69c5435d897df53d98c71be1c4a2e7f` |
| `mode-json.js` | `src-min-noconflict/mode-json.js` | 5561 | `010feab73fd75cfcdee30cdc71bdcc868e4f914465a59a7141c5ee4983a8afec` |
| `theme-tomorrow_night.js` | `src-min-noconflict/theme-tomorrow_night.js` | 3841 | `757fb017f0ff7a6f2b47f30a3ff7bd6211529deceebf07909d4647ae1175a38a` |
| `LICENSE` | `LICENSE` | 1490 | `850f545c5254e8c08204edb2a6d50bbb72727c38a82982c17a84bebf6064cdf8` |

Recompute with `shasum -a 256 <file>`. `research_bench_contract` asserts these
four hashes on every test run, so silent drift or corruption fails the suite
rather than shipping.

## Why these three files and no others

- `ace.js` — the editor core.
- `mode-json.js` — JSON syntax highlighting and bracket/brace behavior.
- `theme-tomorrow_night.js` — one fixed theme; the bench does not offer theme
  switching.

No `ext-*` extension (autocomplete, search box, language tools) is vendored:
the bench's editor is plain text editing with highlighting, not an IDE
surface, and the acceptance criteria for #138 rule out completion markup and
HTML tooltips/widgets. No `worker-json.js`: see above.

## Content-Security-Policy note

Ace injects its base, scrollbar, and theme CSS as inline `<style>` elements
at runtime (`ace/lib/dom`'s `importCssString`) rather than through a
`<link>`; this is internal to the vendored bundle, not a hook the integration
controls. The bench's `style-src` therefore carries `'unsafe-inline'`,
verified necessary and sufficient with a headless-browser probe (`ace.js`
was the sole source of every blocked `style-src-elem` violation under
`style-src 'self'` alone). `script-src` stays `'self'` with no inline
exception: nothing the bench ever shows — filenames, request text, renderer
diagnostics — is attacker-influenced content that could reach a style
context, so this widens no attack surface the bench actually has. Ace also
requests one `data:` image (a drag-cursor glyph); `img-src` allows `data:`
for it. Workers remain blocked by `worker-src` (inherited from `default-src
'none'`) and are never requested, since `useWorker` is set to `false` before
the JSON mode is attached.
