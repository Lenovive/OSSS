# Implementation handoff: Dynamic frame-rate target generation

## Agent kickoff

Read any repository instructions that exist when you begin, then read this package before editing. Re-check Git status, file timestamps, and the relevant source hashes because this repository has no commits, every project file is untracked, and source files changed concurrently while this package was being assembled. Reproduce the code-contract and runtime baselines below before changing behavior, preserving their fixtures and pass rules as the historical comparison. Finish only the target-clock, source-timeline, bounded-buffering, presentation, diagnostics, tests, and documentation scope described here; preserve the existing D3D11 optical-flow backend, click-through behavior, and unrelated work. Do not clean, reset, stash, commit, push, deploy, publish, or send anything externally. When finished, run the full validation contract and report all divergences, missed evidence classes, and any environmental failures separately from source failures.

## Package identity

| Field | Value |
| --- | --- |
| Mode | `continuation` |
| Package status | `ready` |
| Created | `2026-08-14` |
| Reconciled through | `2026-08-14` |
| Repository | `OSSS` |
| Repository root at capture | `C:\Users\user\OneDrive\Desktop\Formalities\OSSS` |
| Branch | `master` |
| Commit | `unborn` — this repository has no commits |
| Dirty at capture | `true`; every project file is untracked |
| Review protocol | Required; four modalities are defined below |
| Repository evidence | `evidence/repository-state.txt` |

The repository snapshot is evidence from package creation, not a promise that the worktree is unchanged. Source files were modified by another process or session during the audit between approximately 22:03 and 22:11 local time. Re-check mutable state before editing and treat every existing file as user-owned.

## Outcome

Finish OSSS's dynamic frame-generation targeting so the chosen output rate—normally the target monitor's precise refresh rate—owns a stable presentation timeline independent of source-frame arrival. At every output deadline, OSSS must select timestamped, unique captured frames and either interpolate at the correct fractional position or take an explicit cold-start, scene-cut, stall, multiplier-limit, or underrun fallback. A 60 FPS source targeting 144 FPS must therefore be treated as a continuously varying `2.4x` resampling problem, not as a choice between fixed `2x` and `3x` pair quotas. This matters because the desired user-visible result is even presentation pacing for awkward and fluctuating source rates without an unbounded queue or hidden latency growth.

Terminology for this task:

- **Target FPS:** the fixed requested output clock. The default is the selected window's target display refresh; a manual target remains supported.
- **Raw capture FPS:** all WGC callback frames, including compositor duplicates.
- **Unique source FPS:** content-changing source frames after duplicate classification; this is the cadence used for effective-multiplier and buffering decisions.
- **Effective multiplier:** `target FPS / unique source FPS`, reported as telemetry. It must not become an integer scheduling mode.
- **Queue target 1:** the first production policy for this milestone. Playback is delayed enough to select two known endpoints; extrapolation is not permitted.

The user supplied Lossless Scaling AFG as the mental model: a fixed presentation clock with dynamically adjusted fractional multipliers. Its proprietary model and exact scheduling tolerances are unknown and are not an implementation requirement.

## Scope

### In scope

- Replace source-pair-owned pacing with a deadline-owned output clock. The exact class names are flexible, but source cadence estimation, output deadline generation, and frame/pair selection must no longer be conflated in `FramePacer`.
- Carry the WGC compositor timestamp from `Direct3D11CaptureFrame::SystemRelativeTime()` through capture history. Record callback arrival separately so capture delay can be measured instead of changing the media timeline.
- Maintain a small timestamped GPU-frame history sufficient for queue target 1 and jitter tolerance. Do not rely on only an overwritten `latest_frame_` plus the renderer's current pair.
- Detect compositor duplicates before they advance unique source cadence or replace an interpolation endpoint. The detector must avoid a synchronous full-frame CPU readback in the hot path; a small asynchronous GPU reduction/signature is acceptable.
- Select a bracketing source pair for each delayed media time and compute `alpha = (media_time - a.time) / (b.time - a.time)`. Interpolation alphas must stay in `[0, 1]` for this milestone.
- Preserve the existing maximum-multiplier control as a safety/quality ceiling, but do not let changes in the source-rate EMA continuously resize or re-phase the output target interval. When the ceiling prevents full synthesis, select or skip target-clock slots using one documented deterministic policy; never build a backlog. Preserve the currently documented capped behavior, such as a 30 FPS source with a 6x ceiling producing at most about 180 distinct presentations per second.
- Change the default target-mode ceiling from the current `2x` behavior to a value that can actually reach common fractional cases such as 60→144. `6x` is the bounded existing maximum and the recommended default; keep explicit `2x` through `6x` selection.
- Add a frame-latency waitable flip-model swap chain, set per-swap-chain maximum frame latency to one, wait before rendering, and track actual presentation/missed-deadline information. Remove `Sleep(1)` polling as the pacing mechanism. Preserve resize flag consistency and close the waitable handle during teardown.
- Replace integer-only display-rate discovery with a rational rate from the active display path. Preserve manual 24–1000 FPS targets.
- Add diagnostics for raw capture FPS, unique source FPS, target FPS, submitted/presented FPS, effective multiplier, queue occupancy/age, missed deadlines, and capture-to-present latency. Keep successful `Present` calls distinct from confirmed display refreshes when DXGI statistics are available.
- Add deterministic scheduler/timeline tests, integration coverage, and documentation that accurately describe the new behavior and limitations.

### Out of scope

- A trained/ML interpolation model, vendor SDK integration, or replacement of `MotionInterpolator`.
- Extrapolation or user-selectable queue targets 0 and 2. Queue target 1 is intentionally fixed for this milestone because the current backend is interpolation-only.
- HDR, VRR-specific control policy, exclusive fullscreen, protected content, anti-cheat certification, multi-GPU/secondary-GPU scheduling, AMD/Intel validation, or physical display certification.
- General optical-flow quality work such as better disocclusion reconstruction, UI masks, particle handling, or thin-object recovery, except regressions caused directly by timeline selection.
- Broad UI redesign. Update labels/defaults/diagnostics only as needed to expose truthful target-mode behavior.
- Deployment, publishing, release packaging, migration, repository cleanup, committing, or pushing.

### Non-negotiable constraints

- Preserve window-level, non-injected capture and the borderless topmost output surface. It must remain click-through, non-activating, and compatible with the OS cursor path.
- Preserve Windows 10 version 2004 as the stated minimum and Windows 11 as the primary target. Optional newer WGC interfaces must remain best-effort with a safe fallback.
- Preserve the Direct3D 11, SDR, single-GPU baseline and feature-level-11 shader path.
- Continue computing optical flow once per selected unique source pair and reuse it for every output alpha that samples that pair.
- Never queue stale presentation work to catch up. Advance to the newest target-clock deadline and count skipped deadlines.
- Do not extrapolate beyond the newest known frame in this milestone. On cold start or underrun, hold/select a real frame using the documented policy.
- Scene cuts must select a real frame rather than blend unrelated images.
- Do not make the target interval chase the estimated source period. Source estimates may influence buffering, telemetry, and ceiling/fallback decisions only.
- The current worktree has no recoverable Git baseline. Never use reset, checkout, clean, stash, or bulk replacement to manage existing files.

## Definition of done

- [ ] A pure scheduler/timeline test demonstrates 60→144, 50→144, and 80→144 with target-owned deadlines, fractional alphas, no pair-quota holes, and no cumulative deadline drift beyond one scheduler tick over ten measured seconds.
- [ ] A deterministic fluctuating 55–70 FPS source still produces a flat 144 FPS deadline sequence when within the configured ceiling and without injected missed-work events.
- [ ] Source arrival jitter does not change source media timestamps or the output-clock phase.
- [ ] Raw duplicate captures do not advance unique source count, cadence estimation, pair identity, or optical-flow preparation count.
- [ ] Queue target 1 uses timestamp bracketing, has bounded history, and reports its added media delay. Cold start, underrun, source stall, scene cut, resize, and recovery behaviors are explicit and tested.
- [ ] Target deadlines remain phase-stable when the source estimate changes. A maximum-multiplier limit and an injected missed deadline never create an unbounded catch-up queue.
- [ ] Display-auto mode retains a rational refresh rate rather than rounding 59.94/143.856-style modes to an integer, and manual targets remain accepted from 24 through 1000 FPS.
- [ ] The swap chain uses `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`, maximum frame latency one, correct wait-before-render ordering, and matching flags on resize; ordinary `Sleep(1)` polling is gone from the production pacing loop.
- [ ] HUD/console diagnostics distinguish raw capture, unique source, target, submitted/presented, effective multiplier, queue state, missed deadlines, and capture-to-present latency.
- [ ] Existing motion reconstruction, scene-cut, GUI, capture/presentation, and click-through behavior do not regress.
- [ ] Focused validation passes or each failure is accurately scoped and explained.
- [ ] Broader repository gates are run, or explicitly recorded as not run with a reason.
- [ ] Before and after results are reviewed with the same preserved framing or selection; new adaptive cases are additive and do not replace the historical baselines.
- [ ] Unrelated pre-existing work remains untouched.

## Current state

### Verified now

- No `AGENTS.md` was found at the repository root, `.agents/`, or `.codex/` during package creation.
- Git reports `master` with no commits. `.gitignore`, `CMakeLists.txt`, `README.md`, `docs/`, `src/`, `tests/`, and this handoff package are untracked. There are no tracked diffs from which to reconstruct authorship or a before-state.
- `src/main.cpp:25-184` and `src/gui_main.cpp:19-568` already support manual/automatic target FPS plus a selectable maximum multiplier. `src/main.cpp:294-327` sends the target to capture, renderer, overlay, and the current pacer.
- `src/capture_session.cpp:45-63` already requests `IGraphicsCaptureSession5::MinUpdateInterval` at the output target when available. This is best effort and is not proof that unique application frames arrive at that cadence.
- `src/frame_pacer.cpp:51-58` computes `min(target FPS, estimated source FPS × maximum multiplier)`, but `src/frame_pacer.cpp:61-79` still resets pair state on each source arrival and `src/frame_pacer.cpp:90-118` still enforces a per-pair presentation quota. This is only a partial adaptive conversion.
- `src/capture_session.cpp:197-201` copies only the latest capture and timestamps it with callback `steady_clock::now()`; it does not retain `SystemRelativeTime`, unique-frame identity, or a timestamped frame queue.
- `src/main.cpp:371-410` feeds callback arrival into the pacer, asks for one alpha, calls `Present`, and otherwise sleeps for 1 ms.
- `src/renderer.cpp:311-314` uses blocking `Present(1, 0)`. `src/renderer.cpp:453-477` creates a two-buffer flip-discard chain without a frame-latency waitable object.
- `src/window_catalog.cpp:93-120` discovers refresh through integer `EnumDisplaySettingsW` frequency.
- `tests/frame_pacer_tests.cpp:33-132` checks output counts for several source/target/ceiling combinations, but it does not assert deadline spacing, source/output phase, queue behavior, duplicates, callback jitter, missed deadlines, or the key 60→144 case.
- The current Release tree builds successfully from Visual Studio Developer PowerShell, all three CTest tests pass, the CLI self-test passes on an NVIDIA GeForce RTX 5090, and the real-desktop WGC/capture/presentation smoke test passes with a 240 FPS capture interval request. Exact commands and results are in `evidence/validation-baseline.txt`.

### Reported, inferred, or not verified

- **User-reported desired model:** Lossless Scaling-style adaptive frame generation: target-owned fixed presentation slots and dynamic fractional multipliers.
- **Unknown:** the precise proprietary scheduling, motion-model, tolerance, and queue algorithms used by Lossless Scaling. Do not claim parity with them.
- **Not verified:** actual pacing against a real game at 60→144 or a fluctuating 55–70→144 source.
- **Not verified:** the current monitor's exact rational refresh, Windows build number, VRR/DRR state, or DXGI confirmed-display cadence.
- **Not verified:** capture-to-present latency, GPU headroom at 1080p/1440p, or missed-deadline rate.
- **Not verified:** AMD or Intel behavior.
- **Inferred risk:** the capture callback and render loop both use the D3D11 immediate context. Existing mutex coverage does not obviously serialize the callback's `CopyResource` against all render-loop context use; audit this while introducing frame history.

### Existing work to preserve

- Treat every file in the repository as user-owned because none is tracked. The task-owned partial target work is concentrated in `src/frame_pacer.*`, `src/main.cpp`, `src/gui_main.cpp`, `src/capture_session.*`, `src/capture_smoke.cpp`, `src/stats_overlay.*`, `README.md`, `docs/ROADMAP.md`, and `tests/frame_pacer_tests.cpp`.
- Preserve `src/motion_interpolator.*` and `tests/motion_interpolator_tests.cpp` as the established arbitrary-alpha motion backend and quality baseline.
- Preserve output-window style and input routing in `src/renderer.*` and `tests/input_passthrough_smoke.cpp` while changing swap-chain scheduling.
- `build/` and `out/` are ignored generated artifacts. Do not delete them; `out/release` is the verified build tree for this handoff. The root `build/` tree is stale/incomplete and is not the baseline.

## Behavioral contract

| Scenario | Current behavior | Required behavior | Evidence or acceptance check |
| --- | --- | --- | --- |
| 60 unique FPS → 144 target, ceiling ≥3 | Target interval is nominally supported, but pair arrival resets alpha/quota state; no dedicated test | 144 Hz target deadlines remain evenly spaced; pair and alpha are selected for each delayed media time; effective multiplier reports about 2.4x | New ten-second scheduler case plus phase-offset variants |
| 50→144 and 80→144 | Not covered | Same 144 Hz deadline clock; approximately 2.88x and 1.8x telemetry; no integer-mode switch | New deterministic scheduler cases |
| Source fluctuates 55–70 FPS | EMA changes `PresentationInterval()` whenever the computed limit changes | Target clock stays phase-stable when within ceiling; only pair/alpha and telemetry adapt | Deterministic cadence pattern with deadline-delta assertions |
| WGC produces 144 callbacks but only 60 unique images | All callbacks advance source sequence/FPS and replace the pair | Raw capture reports 144; unique source reports about 60; duplicates do not advance the interpolation timeline | Repeated-frame fixture and optical-flow preparation counter |
| Callback is delayed but compositor timestamp is stable | Callback arrival becomes media time | `SystemRelativeTime` remains source time; callback time is latency telemetry only | Same source timestamps under two arrival-jitter sequences must select the same pairs/alphas |
| Queue target 1 | No explicit queue; latest capture overwrites prior state | Small bounded timestamped history; target media time is delayed sufficiently for two known endpoints | Queue occupancy/age and exact bracketing assertions |
| Cold start or buffer underrun | First/newest frame behavior is implicit | Present/hold the newest real frame; no alpha outside `[0,1]`, no extrapolation | Unit states plus integration smoke |
| Scene cut | Motion shader selects current frame | Preserve newest-real-frame selection | Existing scene-cut test remains unchanged and passes |
| 30 FPS → 240 target with 6x ceiling | Output interval falls toward 180 FPS | Target phase remains defined at 240; deterministic ceiling policy yields no more than about 180 distinct/generated submissions and never queues catch-up work | Preserve historical 30→180 count case and add deadline/skip assertions |
| Source at or above target | Current count is capped by target | Drop surplus source frames and select the freshest valid sample for target deadlines | Existing 200→240 and 300→240 cases, strengthened with pair/phase checks |
| Source drops below 10 FPS or stalls | Period estimator clamps samples; no explicit suspension state | Suspend interpolation, hold/select real content, count the state, and resume with documented hysteresis | Deterministic stall/recovery case |
| Render work misses a deadline | Pacer skips intervals based on the next call time | Advance directly to the latest target deadline, increment missed count, and never queue stale frames | Injected-lateness scheduler test |
| 59.94/143.856-style display | Integer `dmDisplayFrequency` | Preserve rational numerator/denominator through target-clock construction | Display-rate unit seam plus live logged rational rate |

## Review protocol

Treat this setup as task state. Reproduce it before editing, hold the historical fixtures fixed, and rerun it afterward. Adaptive coverage is additive: do not weaken or replace existing tests to make the new implementation pass.

### Review invariants

- Keep existing deterministic motion fixtures, source pixels, dimensions, thresholds, and asserted relationships unchanged in `tests/motion_interpolator_tests.cpp`.
- Keep the existing scheduler simulation's 250 µs polling step, two-second warmup, one-second historical measurement window, source/target combinations, and ±3-count tolerance as a historical baseline. Add longer deadline-spacing tests separately.
- Keep the capture smoke source at 360×220 with the cyan 70×70 box moving 13 pixels per animation frame, target 240, maximum multiplier 6, and the existing pass rule of at least four captured frames and two presentations within five seconds. Add adaptive integration coverage as a separately named test/mode rather than silently changing this baseline.
- Keep the input passthrough fixture's 360×220 target at `(120,120)`, center click, black generated surface, exactly one target click, pointer restoration, and topmost/click-through behavior.
- Use the same checkout, Release configuration, Visual Studio 2022 Build Tools, Windows desktop session, and NVIDIA GeForce RTX 5090 for the immediate before/after comparison. If hardware or display changes, report it as a separate run.

### Active modalities

| Modality | Reviewed subject or claim | Exact input or fixture | Framing or selection held fixed | Environment and tools | Baseline or reference | Reproduction and review procedure | Pass rule or tolerance |
| --- | --- | --- | --- | --- | --- | --- | --- |
| API/code contract | Existing count semantics and new target-owned timeline | `tests/frame_pacer_tests.cpp`; existing six rate cases with 250 µs simulation step, 2 s warmup, 1 s measurement | Preserve all existing inputs/tolerances; add 60→144, 50→144, 80→144, jitter, duplicate, queue, stall, cap, and lateness cases separately | Release C++ tests via CTest | Current test output in `evidence/validation-baseline.txt` | Run `out\release\osss_tests.exe`, inspect event/deadline assertions, then full CTest | Historical counts remain within ±3; new target deadlines meet the Definition of done |
| Runtime capture/presentation | WGC delivery, GPU history, motion setup, swap-chain presentation | `src/capture_smoke.cpp`: 360×220 window; dark background; 70×70 cyan box; 13 px/frame; target 240; max 6 | Do not alter the historical fixture or its minimum 4 captures/2 presentations/5 s rule; add a separate adaptive mode if needed | Real Windows desktop session; Release binary; RTX 5090 | Passed at package creation | Run `out\release\osss.exe --capture-self-test` outside restricted/sandbox sessions | Existing smoke passes and reports whether 240 FPS capture interval was requested; no new capture/render error |
| Interactive UI/input regression | Generated surface remains visible and input-transparent after swap-chain changes | `tests/input_passthrough_smoke.cpp`: 360×220 green target at 120,120; black overlay; center click | Preserve position, size, color checks, one click, pointer restoration, and exact action order | Real Windows desktop; `out\release\osss_input_passthrough_smoke.exe` | Not run during package creation because it moves and clicks the user's pointer | Run only when the desktop is safe for injected pointer input; observe restoration and console result | Exactly one click reaches target; overlay stays visible; pointer is restored; exit 0 |
| Performance/reliability | Achievable target pacing, bounded queue, latency, and missed deadlines | After implementation, use a deterministic source window at 60 unique FPS for 10 s, queue 1, target 144, max 6; also run deterministic 55–70 FPS pattern | Keep source cadence pattern, duration, target, queue, window size, GPU, and foreground/background state fixed within a before/after pair | RTX 5090, current Windows desktop, Release build; record exact display rational/VRR state | Not run; current code lacks trustworthy unique-frame and present-deadline metrics | Record 2 s warmup plus 10 s measurement; capture raw/unique/source/target/submitted/confirmed/missed/latency data; run fixed 60 then fluctuating pattern | When hardware/display can accept 144 Hz: scheduled rate 144 ±1 FPS, no queue growth, no alpha outside range, no stale catch-up; report confirmed-display misses separately |

### Review sequence and cross-modal gates

1. Re-check repository state and source hashes; then reproduce the Release build, focused scheduler test, full CTest, CLI self-test, and capture smoke before editing.
2. Implement and pass the pure API/code-contract tests before using runtime FPS counters as evidence.
3. Pass the historical motion and capture integration baselines before interpreting new performance numbers.
4. Run the real-desktop adaptive timing measurement only after raw-vs-unique classification, deadline metrics, and queue metrics are trustworthy.
5. Run the pointer-moving input smoke last, when it is safe to move/click the user's cursor.

### Authorized protocol changes

None. Adding separately named adaptive tests and metrics is required, but the historical test fixtures and pass rules above remain the primary regression baseline. `review_protocol.framing_change_authorized` remains `false`.

## Implementation map

| Order | Area, file, or symbol | Required change | Rationale and invariants |
| --- | --- | --- | --- |
| 1 | `tests/frame_pacer_tests.cpp`; proposed `tests/adaptive_scheduler_tests.cpp` | Express output-clock events, pair IDs, alpha, mode, deadlines, skips, queue state, and metrics; add the required deterministic cases before deleting current behavior | Locks the user-visible timing contract and preserves historical count cases |
| 2 | `src/frame_pacer.*`; proposed `src/output_clock.*` and `src/source_timeline.*` | Split fixed rational deadline generation from source cadence/history and frame selection; remove `presentations_for_pair_` ownership | The output clock must not be reset/re-sized by each source pair |
| 3 | `src/capture_session.*` | Store `SystemRelativeTime`, callback arrival, dimensions, sequence, and bounded GPU-frame history; classify duplicates without synchronous full-frame readback; preserve `MinUpdateInterval` fallback | Unique content time, not callback jitter, is the media authority |
| 4 | `src/renderer.*` | Allow selection/preparation of the history pair required by a deadline; add waitable swap chain, latency-one wait, presentation statistics, correct resize/teardown | Wait before render, keep flow once per pair, preserve click-through window behavior |
| 5 | `src/main.cpp` | Drain/ingest captures, wait for a presentation slot, ask the selector for the target media sample, render/present once, and account for missed deadlines/fallbacks | Replaces `Sleep(1)` polling and pair-owned alpha calls with one coherent loop |
| 6 | `src/window_catalog.*` | Return an exact rational active-path refresh using display configuration APIs; retain safe manual fallback | Avoid integer rounding and long-term deadline drift |
| 7 | `src/stats_overlay.*`, `src/gui_main.cpp` | Make target-mode defaults and raw/unique/target/present/queue/latency metrics truthful; default ceiling should reach 60→144 | Current default max 2 cannot reach the headline fractional case |
| 8 | `src/capture_smoke.cpp`, `CMakeLists.txt` | Preserve historical smoke; add separately named adaptive integration coverage if useful | Maintains fixed baseline while testing the new orchestration |
| 9 | `README.md`, `docs/ROADMAP.md` | Describe fixed target clock, dynamic fractional multiplier, queue-1 latency, ceiling/fallback behavior, raw vs unique FPS, and evidence limits | Documentation currently overstates completion of target-driven pacing |

### Suggested first action

Before modifying `FramePacer`, add a scheduler test that records every deadline, selected pair, alpha, and fallback for 60→144 over ten seconds and for four source/output phase offsets. Make that test fail against the current pair-quota implementation. This establishes the central invariant—deadlines belong to the target clock—and prevents another count-only implementation from appearing correct.

## Validation contract

| Command or check | Purpose | Current status | Evidence | Required result |
| --- | --- | --- | --- | --- |
| Developer-shell `cmake --build out\release` | Compile current source with the verified Ninja Release tree | `passed` | `evidence/validation-baseline.txt` | Exit 0 with current source; rerun after all edits |
| `out\release\osss_tests.exe` | Historical scheduler/count contract | `passed` | `evidence/validation-baseline.txt` | Exit 0; historical cases retained plus new adaptive cases pass |
| `ctest --test-dir out\release --output-on-failure` | Scheduler, motion, and GUI tests | `passed` | `evidence/validation-baseline.txt` | 100% pass; no weakened tests |
| `out\release\osss.exe --self-test` | D3D11 adapter, motion setup, output/overlay styles, basic target pacing | `passed` | `evidence/validation-baseline.txt` | Exit 0 on the same hardware |
| `out\release\osss.exe --capture-self-test` in a real desktop session | WGC, GPU copy/history, interpolation, and production swap-chain integration | `passed` | `evidence/validation-baseline.txt` | Exit 0; preserve historical fixture and report capture interval request |
| `out\release\osss_input_passthrough_smoke.exe` | Pointer routing and visible click-through output regression | `not run` | Deliberately omitted because it moves/clicks the user's pointer | Run when desktop is safe; exactly one click reaches target and pointer restores |
| New deterministic adaptive scheduler suite | Deadline spacing, fractional alpha, queue, duplicates, jitter, stall, cap, missed work | `not run` | Feature/harness not yet implemented | All Definition-of-done timing assertions pass |
| New real-desktop 60→144 and 55–70→144 timing run | End-to-end dynamic targeting and latency evidence | `not run` | Trustworthy unique/present metrics do not yet exist | Meet the performance/reliability pass rule or report hardware/display limitation precisely |

Use only `passed`, `failed`, `not run`, or `not applicable` when updating statuses. Keep scheduler decisions, submitted `Present` calls, DXGI-confirmed display events, and visual/physical display claims distinct.

## Evidence index

- `evidence/repository-state.txt` — unborn branch, untracked-file scope, initializer snapshot, and final audit caveat.
- `evidence/validation-baseline.txt` — exact build/test commands, concise results, environment-scoped failure, and deliberately omitted checks.
- `evidence/implementation-inventory.md` — current implemented pieces, blocking architectural gaps, source pointers, and proposed components.
- `evidence/relevant-source-hashes.txt` — SHA-256 fingerprints for task-relevant source/docs/tests in a repository with no commit baseline.
- No attachments, full diff, build artifacts, credentials, `.env` files, or external proprietary materials are included.

## Risks, blockers, and stop conditions

- **Highest preservation risk:** there is no commit and every file is untracked. Source changed during package creation. Re-check status/hashes and stop if task files continue changing concurrently; do not overwrite another session's work.
- WGC `SystemRelativeTime` and the output clock must share an explicitly documented monotonic/QPC conversion. Do not assume arbitrary clock epochs align without verifying the Windows representation.
- Duplicate classification can falsely suppress slow/subpixel motion or noisy static regions. Test identical frames, low-amplitude movement, UI-only movement, and scene cuts; expose thresholds as named constants.
- Queue target 1 deliberately adds roughly one source-frame of media delay plus capture/presentation overhead. Report measured latency; do not describe generated FPS as native responsiveness.
- A waitable swap-chain flag cannot be added or removed through resize. Creation and `ResizeBuffers` flags/lifetime must remain consistent.
- Audit D3D11 immediate-context thread access while changing capture history. Either serialize all immediate-context use or move GPU copies to one owning thread; do not add races.
- Current `Present` counts do not prove physical display. Use DXGI statistics when available and label compositor/display limitations.
- Dynamic Refresh Rate and VRR can make a single fixed physical refresh assumption incomplete. They are out of scope, but detected state must be reported rather than silently misclassified.
- Stop and ask before adding extrapolation, an ML model, queue targets 0/2, a new dependency, changing the historical review fixtures, changing platform support, or performing any commit/push/deploy/publish/external-send action.

## Delivery contract

When implementation is complete, report:

- the delivered target-clock behavior, queue/fallback semantics, and observable 60→144/fluctuating-rate results;
- every file changed and the important clock, timestamp, duplicate, buffering, swap-chain, and ceiling-policy decisions;
- focused and broad validation commands and results, including exact failures and whether each was source-, environment-, hardware-, or display-scoped;
- before/after results under every preserved review modality and any separately authorized protocol changes;
- raw capture, unique source, target deadlines, submitted/confirmed presents, effective multiplier, queue latency, and missed-deadline evidence without conflating them;
- remaining risks, unsupported platforms, and unverified evidence classes;
- whether anything was deployed, published, committed, pushed, or sent externally. The expected answer for this handoff scope is no.
