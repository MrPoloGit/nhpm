# nhpm: Pull request format

What a PR should look like. See [CONTRIBUTING.md](../CONTRIBUTING.md) for the build/lint/test
commands and code style, and [testing.md](testing.md) for the test plan.

## Before opening

- `make lint` and `make test` pass (`make ci` runs the full pipeline locally, for every
  compiler on `PATH`)
- One logical change per PR; a formatting-only change is its own PR (same rule as commits,
  below)
- New behavior comes with tests; a bug fix comes with a test that fails before the fix and
  passes after
- `CHANGELOG.md` has a line for the change, if it's user-visible
- Docs are updated if the change touches something they describe (`features.md`,
  `file-formats.md`, `repo-structure.md`, `docs/README.md`, ...) -- a behavior change without
  its doc updated will usually just get asked for the update in review

## Description

- What changed and why -- the why matters more than the what, since the diff already shows
  what
- For a bug fix: how to reproduce the bug before the fix (the test you added is usually this)
- For a new feature: which milestone it belongs to ([nhpm-plan.md](nhpm-plan.md)), if any
- Anything deliberately left out or deferred, and why

## Commits

Same rules as [CONTRIBUTING.md](../CONTRIBUTING.md): a short imperative summary (<=72 chars),
blank line, then why; one logical change per commit. Rebase/squash stray fixup commits before
requesting review -- CI and review comments should land on a clean series, not a pile of
"fix typo"/"oops" commits.
