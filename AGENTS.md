# RVRBoTron agent instructions

## Project context

Read `CONTEXT.md` before naming domain concepts. When planning or changing reverb behavior, read `docs/design/reverb/README.md` and every stage document relevant to the signal path being changed. Treat these design documents as architectural intent, not as an active implementation ticket.

RVRBoTron is a research instrument whose DSP becomes the production plugin core. Preserve deterministic, measurable experiments and production-grade DSP; leave product parameter choices open until experiments support them.

## Working agreement

Follow `docs/agents/workflow.md` for issue-led planning, git-flow branches, commits, pull requests, and session completion. Active implementation scope lives in GitHub Issues rather than in this file or the design documents.

Use the versioned hooks in `hooks/`; configure each clone with `git config core.hooksPath hooks`.

## Agent skills

### Issue tracker

Issues and specs are tracked in this repository's GitHub Issues. See `docs/agents/issue-tracker.md`.

### Triage labels

Triage uses the canonical `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, and `wontfix` labels. See `docs/agents/triage-labels.md`.

### Domain docs

Domain documentation uses a single-context layout. See `docs/agents/domain.md`.

### Planning and implementation

Use the Matt Pocock skills in the sequence described by `docs/agents/workflow.md` when turning the design corpus into implementation work.
