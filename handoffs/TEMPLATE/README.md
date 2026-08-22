# Handoff package template

Copy this directory to `handoffs/<YYYY-MM-DD>-<slug>/` when you need to hand
work to a later session or another agent, then fill in every `TODO`. Delete
nothing from the skeleton: an intentionally empty section should say why it is
empty, because a missing section reads as an oversight.

```powershell
Copy-Item -Recurse handoffs/TEMPLATE handoffs/2026-09-01-my-topic
```

A package is three things:

| File | Purpose |
| --- | --- |
| `HANDOFF.md` | The entrypoint. Intent, scope, constraints, definition of done. |
| `manifest.json` | Machine-readable identity, authority, and evidence index. |
| `evidence/` | Captured facts with timestamps. Never edited after capture. |

## manifest.json fields

| Field | Meaning |
| --- | --- |
| `schema_version` | `2`. Bump only if you change the field set, and update this table. |
| `package_type` | `implementation_handoff` for work to continue; `investigation` for findings only. |
| `status` | `draft` while assembling, `ready` once evidence is captured and HANDOFF.md is complete. |
| `title` | Human-readable, matches the `HANDOFF.md` H1. |
| `slug` | Kebab-case, matches the directory suffix after the date. |
| `mode` | `continuation` (extends existing work) or `new` (greenfield). |
| `created_at` | ISO 8601 with a UTC offset, e.g. `2026-09-01`. |
| `entrypoint` | Always `HANDOFF.md`. |
| `repository.branch` / `.head` | Git state **at capture time**. Record the real commit hash, not a placeholder. |
| `repository.dirty` | `true` if the worktree had uncommitted changes. Say so; do not tidy first. |
| `evidence[]` | Every file under `evidence/`, as repo-relative paths from the package root. |
| `review_protocol.modalities[]` | Which evidence classes a reviewer must exercise. See below. |
| `review_protocol.framing_change_authorized` | `false` unless the user explicitly allowed redefining the problem. |
| `authority.*` | All `false` by default. Only the user can raise these. |
| `data_review.*` | Assert what the package contains. `contains_secrets` must be verified, not assumed. |

Review modalities used so far in this repo:
`api-code-contract`, `runtime-capture-presentation`,
`interactive-input-regression`, `performance-reliability`.

## Evidence conventions

Every file starts with an H1 and a `Captured:` timestamp line. Evidence is a
snapshot, not a promise about the current tree — a later reader must re-check
mutable state. Typical set:

- `repository-state.txt` — `git status`, branch, head, untracked files.
- `validation-baseline.txt` — the exact commands run and their real output.
- `implementation-inventory.md` — the symbols and files in scope.
- `relevant-source-hashes.txt` — hashes so drift is detectable later.

## Reading an old package

Per [CONTRIBUTING.md](../../CONTRIBUTING.md): symbol names stay valid, **line numbers
drift**. Read for intent and invariants, never as current-state truth.
