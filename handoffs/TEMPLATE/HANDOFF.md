# Implementation handoff: TODO title

## Agent kickoff

TODO. One paragraph, addressed to the agent that picks this up. State: read
repository instructions first, read this package before editing, re-check mutable
git state, reproduce the baselines below before changing behavior, finish only
the scope described here, and report environmental failures separately from
source failures. Name explicitly anything the agent must not do (commit, push,
reset, publish).

## Package identity

| Field | Value |
| --- | --- |
| Mode | `continuation` / `new` |
| Package status | `draft` / `ready` |
| Created | TODO ISO 8601 |
| Repository | `OSSS` |
| Branch | TODO |
| Commit | TODO hash at capture |
| Dirty at capture | TODO |
| Review protocol | TODO required? which modalities? |
| Repository evidence | `evidence/repository-state.txt` |

The snapshot is evidence from package creation, not a promise that the worktree
is unchanged. Re-check mutable state before editing.

## Outcome

TODO. What must be true when this is done, in user-visible terms, and why it
matters. Define any term whose meaning is load-bearing — in this repo especially
the telemetry classes (raw capture / unique source / target / submitted /
DXGI-confirmed), which must never be conflated.

## Scope

### In scope

TODO.

### Out of scope

TODO. Being explicit here is what stops scope creep.

### Non-negotiable constraints

TODO. Repo-wide invariants that apply, for example: alphas stay in `[0, 1]` with
no extrapolation; no third-party dependencies; frozen historical fixtures in
CONTRIBUTING.md must not be loosened; `/W4 /permissive-` stays warning-clean.

## Definition of done

TODO. The exact commands that must pass and the exact claims that must hold.
Start from the "What a change has to pass" section of
[CONTRIBUTING.md](../../CONTRIBUTING.md) and
add anything specific to this work.

## Current state

### Verified now

TODO. Only what you personally ran or read, with the evidence file that backs it.

### Reported, inferred, or not verified

TODO. Keep this section honest and non-empty when it should be.

### Existing work to preserve

TODO. Behavior a well-meaning refactor would break.

## Behavioral contract

TODO. Input/output, ownership, and timing guarantees the implementation must
satisfy. Tables work well here.

## Review protocol

### Review invariants

TODO.

### Active modalities

TODO. Choose from `api-code-contract`, `runtime-capture-presentation`,
`interactive-input-regression`, `performance-reliability`, and say what each one
must exercise.

### Review sequence and cross-modal gates

TODO.

### Authorized protocol changes

TODO. Default: none.

## Implementation map

| # | Files | Change | Why |
| --- | --- | --- | --- |
| 1 | TODO | TODO | TODO |

### Suggested first action

TODO. One concrete starting move.

## Validation contract

TODO. Commands, expected output, and which failures are environment-scoped.

## Evidence index

| File | Contents |
| --- | --- |
| `evidence/repository-state.txt` | git status, branch, head, untracked files at capture |
| `evidence/validation-baseline.txt` | commands run and their real output |
| `evidence/implementation-inventory.md` | symbols and files in scope |
| `evidence/relevant-source-hashes.txt` | hashes for later drift detection |

## Risks, blockers, and stop conditions

TODO. What should make the agent stop and ask rather than proceed. See the
"Please open an issue first" section of
[CONTRIBUTING.md](../../CONTRIBUTING.md).

## Delivery contract

TODO. What the finishing agent must report: results per modality, divergences,
missed evidence classes, and environmental versus source failures kept separate.
