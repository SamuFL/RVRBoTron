# Engineering Workflow

RVRBoTron follows the issue-led git-flow established in RVRSE.

## Plan work

The design corpus under `docs/design/reverb/` records architectural intent and experiment hypotheses. GitHub Issues record active scope.

For a new implementation effort:

1. Run `/grill-with-docs` to stress-test the relevant design and capture resolved vocabulary or durable trade-offs.
2. Run `/to-spec` to publish the agreed behavior and testing seams as a GitHub issue.
3. Run `/to-tickets` to split the spec into issue-sized tracer bullets with explicit blocking edges.
4. Work the unblocked issue frontier. Run `/implement` from a feature branch and use `/tdd` at the agreed seams.
5. Complete `/code-review` before committing and opening or updating the pull request.

## Branches and pull requests

| Branch | Purpose | Destination |
| --- | --- | --- |
| `main` | Production-ready, tagged releases | — |
| `develop` | Integration branch | `main` through `release/*` |
| `feature/<issue>-<slug>` | One implementation issue | `develop` |
| `release/<version>` | Release stabilization; fixes only | `main`, then sync to `develop` |
| `hotfix/<issue>-<slug>` | Urgent fix based on `main` | `main`, then sync to `develop` |

Create implementation branches from `develop`. Never commit implementation work directly to `main` or `develop`. Use pull requests, keep one issue as the primary scope, and include `Closes #<issue>` when the PR completes it.

Keep `main` linear and release-only. Stable releases use annotated `vX.Y.Z` tags. A rolling `develop-latest` prerelease may be introduced when automated builds exist.

## Commits

Include the active GitHub issue or PR reference in each normal commit subject:

```text
Implement energy-normalized Split (#12)
```

Keep user-facing documentation and changelog updates in the same change that alters behavior.

## Land the plane

A working session is complete when:

1. Relevant tests, analysis, and builds pass.
2. The issue or pull request records the outcome and follow-up work.
3. Changes are committed and pushed.
4. `git status` confirms the branch is up to date with its upstream.

Create follow-up issues rather than expanding the active issue silently.

## Repository safeguards

Configure the versioned hooks once per clone:

```bash
git config core.hooksPath hooks
```

The hooks require issue references in normal commits, reject common staged-file hazards, block direct pushes to `main`, and warn on direct pushes to `develop`.
