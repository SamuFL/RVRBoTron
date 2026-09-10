# Research bench JSON editor

The Research bench needs a pleasant JSON editing surface without turning its
local Python launcher into a frontend project. The editor text must remain the
single source of truth used for rendering and downloading; formatting must
never happen implicitly.

The Research bench design session selected Ace 1.44.0 using the pinned,
worker-disabled inclusion described below.

| Option | Integration | Useful behavior | Cost and risk |
| --- | --- | --- | --- |
| Native `textarea` | No dependency or build | Exact current text through `.value` | No highlighting or bracket matching; indentation and formatting need local JavaScript |
| Ace 1.44.0 | Vendor official prebuilt scripts; no build or runtime package manager | JSON highlighting, indentation, bracket matching, optional syntax diagnostics | About 0.5 MB raw for the relevant assets; BSD-3-Clause notices required |
| CodeMirror 6 | Install packages and generate a committed browser bundle | JSON parsing and indentation, bracket matching, strong asynchronous lint integration | Adds a producer-side npm/Rollup workflow and notices for bundled MIT packages |
| JSONEditor 10.4.3 | Vendor official prebuilt JavaScript and CSS | Code and tree editing, formatting, repair, schema validation | Over 1 MB raw for the full build; Apache and bundled-dependency notices; tree mode parses and reserializes the request |

## Findings

### Native textarea

A textarea has no third-party cost and exposes its current text directly.
Browser APIs normalize line endings, so fidelity means preserving the current
editor string rather than the original file bytes. Syntax coloring and bracket
matching would require custom work and should not be recreated for this MVP.

Source: [WHATWG textarea value rules](https://html.spec.whatwg.org/multipage/form-elements.html#the-textarea-element).

### Ace

Ace publishes generated browser files for direct embedding, including a
minified no-conflict distribution. The relevant vendored set can remain
limited to `ace.js`, `mode-json.js`, optional `worker-json.js`, and the license.
JSON mode provides highlighting and structural indentation; Ace core provides
bracket matching. `setValue()` and `getValue()` allow templates, rendering,
and downloading to share one text value without parsing it.

Sources:

- [Ace prebuilt distribution](https://github.com/ajaxorg/ace-builds/blob/v1.44.0/README.md#L10-L24)
- [JSON mode](https://github.com/ajaxorg/ace/blob/v1.44.0/src/mode/json.js#L3-L56)
- [Bracket matching](https://github.com/ajaxorg/ace/blob/v1.44.0/src/editor.js#L491-L548)
- [BSD-3-Clause license](https://github.com/ajaxorg/ace/blob/v1.44.0/LICENSE#L1-L24)

## Ace 1.44.0 inclusion, security, and licensing

This follow-up was checked on 2026-09-10. It describes how Ace should be
included if selected; it does not select Ace.

### Proposed inclusion

Use the immutable
[`ace-builds@1.44.0` npm tarball](https://registry.npmjs.org/ace-builds/-/ace-builds-1.44.0.tgz),
not the `ajaxorg/ace` GitHub source archive. `ace-builds` is the official
generated distribution, and `src-min-noconflict` is its minified build for
embedding without claiming the global `require` name
([distribution README](https://github.com/ajaxorg/ace-builds/blob/v1.44.0/README.md#L10-L24)).
The GitHub release has no uploaded browser assets
([release API](https://api.github.com/repos/ajaxorg/ace/releases/tags/v1.44.0));
the npm package has a registry signature and SLSA provenance tied to
`ajaxorg/ace-builds` commit
[`184177de`](https://github.com/ajaxorg/ace-builds/commit/184177de1dcc5946b093edba0b0fe1c29c2a127a)
([npm metadata](https://registry.npmjs.org/ace-builds/1.44.0),
[attestation](https://registry.npmjs.org/-/npm/v1/attestations/ace-builds@1.44.0)).

If Ace is chosen, commit these upstream minified assets under the Research
bench's static tree, proposed as
`tools/research_bench/static/vendor/ace/1.44.0/`. Committing them makes the
bench offline after checkout and avoids npm, a CDN, or a build step at runtime.

| Tarball path | Include | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| `package/src-min-noconflict/ace.js` | Yes | 475,029 | `072d13e53d11e2ceccfffe1a0fa7f15cf69c5435d897df53d98c71be1c4a2e7f` |
| `package/src-min-noconflict/mode-json.js` | Yes | 5,561 | `010feab73fd75cfcdee30cdc71bdcc868e4f914465a59a7141c5ee4983a8afec` |
| `package/LICENSE` | Yes | 1,490 | `850f545c5254e8c08204edb2a6d50bbb72727c38a82982c17a84bebf6064cdf8` |
| `package/src-min-noconflict/worker-json.js` | Optional | 24,460 | `5164e2950cda1f100cf8cb1b4eaf381671fd2571ebd0407c31dbb7df04bf6b6b` |

Pin the package name and version, tarball URL, registry integrity
`sha512-PFNMSYqFdEUkul2Ntud0HvA09AgY+F1ag0UYdpMH60wNI/qOA8cB8tlTgoALMEwIdUPJK2CjrIQ7OnbiSS/ugQ==`,
tarball SHA-256
`a8116a1ec64f7d99a0a0e6003b8dbd0fab158a18716f3520990927bb01d90d14`,
registry `gitHead`, and the per-file hashes above in a sibling
`README.vendor.md` and `SHA256SUMS`. Browser SRI attributes may provide another
check, but the committed hashes remain the auditable source of truth.

The initial inclusion should omit the JSON worker because renderer validation
is authoritative. Ace enables workers by default, so explicitly call
`editor.session.setUseWorker(false)`. If live syntax diagnostics are later
worth the extra asset and policy surface, vendor `worker-json.js`, point
`workerPath` at that same-origin directory, and set
`ace.config.set("loadWorkerFromBlob", false)`. Otherwise Ace creates a Blob
worker that calls `importScripts`
([worker construction](https://github.com/ajaxorg/ace/blob/v1.44.0/lib/ace/worker/worker_client.js#L39-L64));
the JSON worker itself uses a recursive-descent parser rather than `eval`
([parser](https://github.com/ajaxorg/ace/blob/v1.44.0/lib/ace/mode/json/json_parse.js#L53-L62)).

Updates should be deliberate rather than automatic: review the source release
notes and source diff, verify new npm registry integrity and provenance,
compare generated files with the corresponding `ace-builds` tag, rerun
advisory queries and per-file hashing, review new HTML/dynamic-loading sinks,
and test offline loading, CSP, malicious text, worker/no-worker behavior,
large inputs, and exact render/download text. Then update the vendor metadata
and `THIRD_PARTY_LICENSES.md`.

### Threat model and controls

Loopback binding reduces exposure but does not make browser input trusted.
Treat request text, templates, renderer diagnostics, filenames, WAV bytes, and
every HTTP request as attacker-controlled.

| Risk | Relevant Ace behavior | Required control |
| --- | --- | --- |
| JSON or diagnostics become HTML/XSS | The normal text layer creates text nodes; however, optional APIs such as `annotation.html`, tooltip HTML, line-widget HTML, and completion `docHTML` accept markup ([text layer](https://github.com/ajaxorg/ace/blob/v1.44.0/src/layer/text.js#L351-L427), [gutter annotations](https://github.com/ajaxorg/ace/blob/v1.44.0/src/layer/gutter.js#L77-L97)) | Load templates with `setValue()`, read with `getValue()`, use only `annotation.text`, and render all filenames/errors with DOM `textContent`. Never interpolate these values into HTML, inline scripts, `annotation.html`, widgets, or completion HTML. Under those rules, JSON, templates, and renderer diagnostics are displayed as text and cannot execute. |
| Dynamic code or network loading | Ace can create script elements from configured module paths and can start Blob or URL workers ([module paths and loader](https://github.com/ajaxorg/ace/blob/v1.44.0/src/config.js#L63-L153), [worker construction](https://github.com/ajaxorg/ace/blob/v1.44.0/lib/ace/worker/worker_client.js#L39-L64)) | Serve only pinned same-origin assets, set explicit local paths, provide no CDN fallback, and disable the worker unless chosen. A scan of the exact proposed files found no literal `eval(` or `new Function`, but that is not a security proof. |
| CSP bypass or accidental weakening | Ace normally injects `<style>` elements and has a `useStrictCSP` switch that suppresses this behavior ([DOM CSS loader](https://github.com/ajaxorg/ace/blob/v1.44.0/src/lib/dom.js#L223-L281)) | Start from `default-src 'self'; script-src 'self'; connect-src 'self'; worker-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'`. Test whether the chosen theme requires narrowly allowing inline styles or extracting CSS and enabling `useStrictCSP`; do not claim strict-CSP support without a browser test. |
| Resource exhaustion | Ace holds the document in memory; the optional worker receives another copy and recursively parses nested JSON | Cap request size and nesting before render, debounce validation, reject stale responses, and test the chosen limits. The worker does not replace server/renderer validation. |
| Loopback request abuse | Other browser origins can attempt requests to localhost; a local endpoint can also expose filesystem or renderer capabilities | Bind only to loopback; validate `Host` and `Origin`; use an unpredictable per-launch capability/CSRF token; avoid CORS; make mutations POST-only; constrain output to a server-owned root; and return opaque render identifiers. |
| Local WAV access | A path-taking endpoint could expose arbitrary local files, while malformed or oversized audio can stress the decoder | Accept bytes only through an explicit file-selection/upload flow, never a browser-supplied filesystem path. Apply size limits and validate WAV structure server-side before rendering. |

Static responses should use correct content types and
`X-Content-Type-Options: nosniff`. Error pages must not echo untrusted values
as HTML. The server should retain no endpoint that executes a path, command,
module name, or other string supplied by the editor.

### Published advisories and provenance

Primary advisory indexes were queried on 2026-09-10 for `ace-builds`,
`ace-code`, `ace`, version `1.44.0`, and the source/build commits. No matching
published records were returned by the
[GitHub global advisory API](https://api.github.com/advisories?affects=ace-builds%401.44.0&per_page=100),
the Ace and ace-builds repository security-advisory APIs, the
[OSV query API](https://google.github.io/osv.dev/api/), or npm's advisory bulk
API. NVD keyword queries for
[`ace-builds`](https://services.nvd.nist.gov/rest/json/cves/2.0?keywordSearch=ace-builds)
and
[`ajaxorg%20ace`](https://services.nvd.nist.gov/rest/json/cves/2.0?keywordSearch=ajaxorg%20ace)
also returned no records. The broad phrase "Ace Editor" returned
[CVE-2025-3884](https://nvd.nist.gov/vuln/detail/CVE-2025-3884), but its
authoritative affected product is Cloudera Hue 4.11.0, not `ajaxorg/ace` or
`ace-builds`
([CVE record](https://cveawg.mitre.org/api/cve/CVE-2025-3884)).

This is a verified absence of matching published records in those queries,
not proof that Ace is secure. It does not cover unpublished flaws, incomplete
package mapping, future disclosures, or issues found only by code/runtime
analysis.

`npm audit` is not continuing protection for copied static files: it builds
its report from the installed dependency tree and lock data
([npm audit documentation](https://docs.npmjs.com/cli/v11/commands/npm-audit)).
Once `ace.js` is merely vendored, it is invisible to a later application-level
audit unless the project separately records and queries its package identity.
This is why the vendor manifest and update checklist are required.

The source tag resolves to
[`ajaxorg/ace@2143080`](https://github.com/ajaxorg/ace/commit/214308079cf20dc23e84be7f4164f8459cf9a0a8);
the generated package tag and npm provenance resolve to
[`ajaxorg/ace-builds@184177d`](https://github.com/ajaxorg/ace-builds/commit/184177de1dcc5946b093edba0b0fe1c29c2a127a).
The npm artifact was published by GitHub Actions and the publish workflow
checks that the generated package has no npm dependencies
([workflow](https://github.com/ajaxorg/ace-builds/blob/v1.44.0/.github/workflows/publish.yml#L17-L36)).
The attestation establishes the npm artifact's build-repository provenance,
but does not prove that the generated build came from the cited Ace source
commit; both commits/tags are unsigned, and the workflow uses floating major
action tags. That source-to-generated-build link remains a supply-chain risk
to review at every update.

### BSD-3-Clause implications

Ace's BSD-3-Clause license permits source and binary redistribution, with or
without modification. Source distributions must retain the copyright,
conditions, and disclaimer; binary distributions must reproduce them in
documentation or other accompanying material; and Ajax.org or contributor
names cannot endorse or promote RVRBoTron without permission
([license](https://github.com/ajaxorg/ace/blob/v1.44.0/LICENSE#L1-L24)).

RVRBoTron's original code can remain MIT-licensed. The Ace files remain under
BSD-3-Clause; the license does not require disclosing modifications or
relicensing the combined product. If selected:

1. Keep the unmodified upstream `LICENSE` beside the vendored assets.
2. Add the full BSD text to `THIRD_PARTY_LICENSES.md`, with Ace 1.44.0,
   copyright, tarball URL, source/build commits, and exact included files.
3. Reproduce that notice in documentation or accompanying materials for any
   binary/product distribution containing Ace.
4. Mark local modifications in vendor metadata, preserve the notice, and make
   no endorsement claim.

### Relative security burden and unresolved risks

The security-cost ordering is textarea, then Ace, then a CodeMirror 6 bundle.
A textarea adds no third-party artifact, loader, worker, or advisory tracking.
Ace adds one dependency and optional worker but no build graph. CodeMirror's
locked npm build gives `npm audit` a dependency graph during bundle
production, but the committed bundle also becomes opaque to later audits
unless its manifest and lockfile remain maintained.

If Ace remains on the shortlist, the lowest-burden proposal is the two-script,
worker-disabled inclusion above, with renderer validation authoritative. Open
questions are the accepted request/WAV size limits, the tested CSP strategy,
renderer diagnostic location format, and whether the unverified
source-to-generated-build relationship is acceptable. The final editor choice
remains open pending user decision.

## Other editor findings

### CodeMirror 6

CodeMirror 6 is distributed as ES modules. Its official deployment guide uses
npm and Rollup to produce a browser bundle; the generated bundle could then be
committed and served as a static asset. Its JSON and lint packages provide a
stronger base for structural editing and asynchronous diagnostics, but that
capability is beyond the agreed render-only validation loop.

Sources:

- [Bundling guide and size estimates](https://code.haverbeke.berlin/codemirror/website/src/branch/main/site/examples/bundle/index.md)
- [JSON language support](https://code.haverbeke.berlin/codemirror/lang-json/src/branch/main/src/json.ts)
- [Lint integration](https://code.haverbeke.berlin/codemirror/lint/src/branch/main/src/lint.ts)
- [MIT license](https://code.haverbeke.berlin/codemirror/view/src/branch/main/LICENSE)

### JSONEditor

JSONEditor has direct browser builds and many features, but its tree modes
parse and reserialize JSON. That changes whitespace, number lexemes, escape
spellings, and duplicate keys, conflicting with the request text's role as
user-authored evidence. In code mode it largely wraps Ace while shipping more
functionality and carrying broader notice obligations.

Sources:

- [Features and browser distribution](https://github.com/josdejong/jsoneditor/blob/v10.4.3/README.md#L23-L123)
- [Text and object conversion](https://github.com/josdejong/jsoneditor/blob/v10.4.3/src/js/JSONEditor.js#L231-L299)
- [Apache-2.0 obligations](https://github.com/josdejong/jsoneditor/blob/v10.4.3/LICENSE#L89-L128)
- [Bundled notices](https://github.com/josdejong/jsoneditor/blob/v10.4.3/NOTICE#L1-L17)

## Shortlist

The Research bench will use Ace 1.44.0 because pleasant editing must coexist
with no frontend build system. It will retain a native textarea only as a
conceptual fallback, not a second supported editing mode. CodeMirror 6 and
JSONEditor are not selected for this MVP.
