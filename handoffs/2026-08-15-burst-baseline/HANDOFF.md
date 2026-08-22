# Burst-scoring baseline: one fixed configuration, native and through OSSS

## Agent kickoff

This is an evidence package, not a work order. Read [CONTRIBUTING.md](../../CONTRIBUTING.md)
and [docs/TEST_ANIMATIONS.md](../../docs/TEST_ANIMATIONS.md) first. It records
what `osss_test_animation.exe`'s burst mode reports for one fixed configuration
on one machine — the source alone, and through `osss.exe` in `motion` and
`blend` mode — so that a later burst under the **same** configuration has
something to be compared against. It also records the harness changes made in
the same session that the numbers depend on (the real/generated verdict and the
0.618-period default interval). Re-check mutable git state before editing
anything; reproduce the numbers on an idle desktop before reading a change into
them; and keep environment-scoped failures (obscured window, osss.exe not yet
attached) separate from source failures — this package documents both kinds.
Do not commit, push, reset, or publish on the user's behalf.

## Package identity

| Field | Value |
| --- | --- |
| Mode | `new` |
| Package status | `ready` |
| Created | 2026-08-15 |
| Repository | `OSSS` |
| Branch | `main` |
| Commit | `98160cecb1d5649084926fcb97add2f8326a4aa9` |
| Dirty at capture | yes — pre-existing in-progress work plus this session's edits; see `evidence/repository-state.txt` |
| Review protocol | none required; this is a baseline record. A reader re-measuring should use the `runtime-capture-presentation` modality. |
| Repository evidence | `evidence/repository-state.txt` |

The snapshot is evidence from package creation, not a promise that the worktree
is unchanged. Re-check mutable state before editing.

## Outcome

A future run of the burst harness under the configuration below can be judged
as *better*, *same*, or *worse* than a recorded reference, rather than read as
an absolute. Terms used here, kept distinct as CONTRIBUTING.md requires:

- **captured frame** — one GDI `BitBlt` of the test window's client area from
  the composited desktop.
- **unique frame** — a captured frame whose pixels differ from the previous
  capture. Only unique frames say anything about cadence.
- **real frame** — a unique frame whose best-matched instant lies within 0.5 ms
  of `k / base_fps`, i.e. a source frame, whether captured from the source or
  held/re-presented by OSSS.
- **generated frame** — a unique frame whose best-matched instant lies strictly
  between two source instants. Only OSSS can produce one.
- `generated_frames_observed` is a **lower bound**: OSSS's output clock is not
  phase-locked to the source, so a generated frame can land within the
  tolerance of a source instant and be counted as real. It is then also
  visually indistinguishable from one.

## Scope

### In scope

- The recorded configuration and its five burst reports.
- The reading of those reports, including what is environment and what is OSSS.
- The harness facts a re-run needs (osss.exe startup latency, capture cost,
  PowerShell launch pitfalls).

### Out of scope

- Any change to OSSS's pipeline. Nothing here is a defect report.
- Bursts at other rates, APIs, multipliers, targets, or on other GPUs. Each
  needs its own baseline; the numbers here do not transfer.
- The offline quality bench's numbers, which live in
  [docs/ROADMAP.md](../../docs/ROADMAP.md) and are only echoed in
  `evidence/validation-baseline.txt` for the record.

### Non-negotiable constraints

The frozen fixtures in CONTRIBUTING.md are untouched by this session. The burst
harness's schema gained fields (`source_phase_ms`, `classification`,
`generated_frames_observed`, `real_frames_observed`,
`real_frame_phase_tolerance_ms`, `verdict`); it removed none, and the schema
string is still `osss-test-animation-burst-v1`.

## Definition of done

For this package: the evidence files exist, are described below, and
`HANDOFF.md` states plainly which legs are clean and which are not. That holds.

For anyone re-measuring: `cmake --build --preset release` warning-clean,
`ctest --preset release` 7/7, `osss.exe --self-test` passing, then the three
legs of `evidence/run-burst-baseline.ps1` on an **idle desktop** with nothing
overlapping the top-left 1000x600 pixels of the primary display.

## Configuration

Everything below is fixed; change one thing and the numbers are a different
experiment.

| Item | Value |
| --- | --- |
| Source | `osss_test_animation.exe --api d3d11 --fps 60` (960x540 client, window at (32,32) on the primary display) |
| Burst | `--burst 20 --burst-at-ms 26000 --exit-after-burst`; interval left at the default 0.618 source periods = 10.30 ms |
| OSSS | `osss.exe --title "OSSS Test Animation" --target-fps 240 --max-multiplier 4 --stats-overlay off --interpolator motion` (and `blend`) launched 1.5 s after the source |
| Machine | RTX 5090, 3840x2160 @ 239 Hz primary display at 150 % scaling, Windows 11 26200 — see `evidence/environment.md` |
| Why 26 s | osss.exe brings its overlay up 10–15 s after launch (runtime HLSL compile). 26 s is cycle phase 2.0 s, first half; the temporal search reaches back to 1.75 s, clear of the 3.0 s scene cut. |

## Current state

### Verified now

All figures below are read from the JSON reports in `evidence/`; the summary
lines as printed are in `evidence/validation-baseline.txt`.

| Leg | Report | Unique / captured | Generated / real | Verdict | PSNR min / mean / max (dB) | Offset (ms) | Step (ms) | Backward |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Native, 26 s | `burst-native-26s.json` | 17 / 20 | 0 / 17 | source-frames-only | 34.13 / 34.16 / 34.18 | −29.3 … −11.3 | 16.47 … 16.82 | 0 |
| **OSSS motion, 26 s** | `burst-osss-motion-26s.json` | 20 / 20 | 17 / 3 | generated-frames-observed | 29.10 / 38.96 / 57.82 | −57.0 … −32.8 | 4.14 … 18.96 | 0 |
| **OSSS blend, 26 s** | `burst-osss-blend-26s.json` | 20 / 20 | 17 / 3 | generated-frames-observed | 26.88 / 35.41 / 55.57 | −47.0 … −31.0 | 3.52 … 18.33 | 0 |
| Native, 8 s (clean) | `burst-native-8s-clean.json` | 14 / 20 | 0 / 14 | source-frames-only | 52.99 / 55.48 / 57.14 | −25.0 … −4.0 | 16.54 … 16.83 | 0 |
| Native, 26 s (obscured) | `burst-native-26s-obscured.json` | 14 / 20 | 0 / 14 | source-frames-only | 13.89 / 13.96 / 14.04 | −28.5 … −5.0 | 16.18 … 16.96 | 0 |

Split by classification, OSSS legs only:

| Leg | Generated: n, PSNR mean (min–max), bad-pixel % mean (max), worst channel error | Real: n, PSNR mean (min), bad-pixel % mean |
| --- | --- | --- |
| motion | 17, 36.06 dB (29.10–44.07), 1.21 % (2.11 %), 242 | 3, 55.39 dB (53.60), 0.014 % |
| blend | 17, 32.71 dB (26.88–39.76), 13.28 % (20.76 %), 169 | 3, 50.74 dB (47.39), 0.109 % |

What the numbers say:

- **The verdict works.** Native legs are `source-frames-only` with 16.5–16.8 ms
  steps (60 Hz). Both OSSS legs are `generated-frames-observed`, 17 of 20
  unique frames generated — an 85 % share against osss.exe's own reported
  `generated-share=74%` (`evidence/osss-motion-stdout.txt`), the difference
  being sampling and the lower-bound nature of the count.
- **Real frames through OSSS score at native fidelity** (53.6–57.8 dB, matching
  the clean native leg's 53–57 dB), so the OSSS present path is pixel-faithful
  for real frames and the generated-frame numbers are the interpolator's, not
  the capture's.
- **Motion beats blend** by 3.4 dB on generated frames, and by 10x on bad-pixel
  share (1.2 % vs 13.3 %). The bad-pixel column separates the two far more
  clearly than PSNR does — the same observation that put the bad-pixel gates
  into the offline bench in this session.
- **The 0.618-period interval swept the phases**: generated frames landed at
  source phases from −6.5 to +7.3 ms (motion) and −8.1 to +6.5 ms (blend), i.e.
  across the whole 16.7 ms period, not on two fixed alphas.
- **Pacing was clean**: no backward steps in any leg; OSSS step range 3.5–19 ms
  is what a 240 Hz output sampled at 10.3 ms looks like when some captures land
  on the same composed frame's neighbour.
- **Presentation latency** reads as −31 … −57 ms through OSSS against
  −4 … −29 ms native; osss.exe reported ~24.7 ms queue delay and 26–31 ms
  capture-to-present, which accounts for the difference. The absolute native
  offset includes DWM and GDI capture latency and is not a OSSS figure.

Also verified in this session and recorded in `evidence/validation-baseline.txt`:
build warning-clean, ctest 7/7, `osss.exe --self-test` pass, and the offline
bench with its new bad-pixel and roughness-ratio gates passing at the same
numbers as before.

### Reported, inferred, or not verified

- **The native 26 s leg is contaminated.** Another window overlapped roughly
  the bottom 12 rows of the client area during that burst (visible as a light
  band in `burst-1-frame-*-observed.ppm` of that session, not copied here), which
  is why it sits at a flat 34.16 dB while the same source at 8 s scored 55.5 dB.
  The clean 8 s leg is included as the reference for native capture fidelity;
  it differs from the recorded configuration only in `--burst-at-ms`. **A clean
  native leg at 26 s still needs to be recorded on an idle desktop.** OSSS's
  overlay is topmost and hid the same overlap in its own legs, which is why
  those are clean and native is not.
- The `burst-native-26s-obscured.json` leg (Discord covering the lower half)
  is included deliberately as an example of what a wrong capture looks like:
  the verdict is still correct and pacing looks fine, and only PSNR gives it
  away. Do not read it as a source result.
- No pixel-level artefact typing was done. The generated frames were eyeballed
  once (a halo around the thin marker in the detail lane, smeared header bits
  which are unmasked and unscored) — that is an impression, not a measurement.
- Single GPU, single display, single resolution. Nothing here transfers.

### Existing work to preserve

- `kDefaultBurstIntervalSourcePeriods` = 0.618…; a return to 1/2 aliases the
  sampling against 4x output.
- `kRealFramePhaseToleranceMilliseconds` = 0.5, sitting between the 0.25 ms
  fine-search step and the 1.39 ms minimum generated spacing (120 FPS at 6x).
- The verdict's three states and the per-row `classification` values
  (`real` / `generated` / `repeat`).

## Behavioral contract

For a burst through OSSS at this configuration to count as *not worse* than
this baseline: verdict `generated-frames-observed`; `backward_steps` 0;
generated-frame PSNR mean not below ~36 dB and bad-pixel mean not above ~1.2 %
by more than run-to-run noise (establish that noise with two or three runs
before deciding); real frames at ≥ 53 dB. Anything with real frames below
~50 dB or a native leg below ~50 dB is an obscured or mis-scaled capture, not
an interpolation result.

## Review protocol

### Review invariants

A re-measurement must hold API, source rate, target rate, multiplier,
resolution, display, and burst schedule fixed, and must record whether the
desktop was idle.

### Active modalities

`runtime-capture-presentation` only: it is the only modality that produces
these numbers. `api-code-contract` for the harness edits is covered by
`osss_test_pattern_tests` and the build.

### Review sequence and cross-modal gates

Build → ctest → `osss.exe --self-test` → native leg (must be ≥ ~50 dB or the
desktop is not clear) → OSSS legs.

### Authorized protocol changes

None.

## Implementation map

None: this package changes nothing. The harness edits it depends on are listed
in `evidence/implementation-inventory.md`.

### Suggested first action

Re-run `evidence/run-burst-baseline.ps1 -Cases native` on an idle desktop and,
if it lands near 55 dB, replace the note above with the clean 26 s native leg.

## Validation contract

`evidence/run-burst-baseline.ps1` runs all three legs and copies each
`burst-1.json` next to the logs; it prints the summary line and the time the
OSSS overlay appeared. Failure signatures and their meaning:

| Signature | Meaning | Scope |
| --- | --- | --- |
| osss stderr `The requested capture target is not a valid window`, no banner | osss.exe was still compiling shaders when the source closed; burst too early | environment |
| native leg < 50 dB, `source-frames-only`, steps ~16.7 ms | something is covering the test window | environment |
| OSSS leg `source-frames-only` with overlay reported visible | OSSS held/repeated real frames, or the capture saw the source under the overlay | investigate — the one signature that is not automatically environment |
| `identical_to_previous` on most rows | desktop composing slower than the interval, or capture-bound | environment |

## Evidence index

| File | Contents |
| --- | --- |
| `evidence/repository-state.txt` | git status, branch, head at capture |
| `evidence/validation-baseline.txt` | commands run and their real output, including the discarded batches |
| `evidence/environment.md` | machine, display, running software, timing facts, PowerShell pitfalls |
| `evidence/implementation-inventory.md` | symbols the numbers depend on |
| `evidence/relevant-source-hashes.txt` | SHA-256 of the sources involved, for drift detection |
| `evidence/run-burst-baseline.ps1` | the runner used |
| `evidence/burst-native-26s.json` | native leg, recorded configuration, bottom rows overlapped |
| `evidence/burst-osss-motion-26s.json` | OSSS motion leg, clean |
| `evidence/burst-osss-blend-26s.json` | OSSS blend leg, clean |
| `evidence/burst-native-8s-clean.json` | native leg at 8 s, clean desktop — native fidelity reference |
| `evidence/burst-native-26s-obscured.json` | native leg with a window covering half the area — negative example |
| `evidence/osss-motion-stdout.txt`, `evidence/osss-blend-stdout.txt` | osss.exe banner and per-second stats for the two OSSS legs (single line; the stats are `\r`-separated) |
| `evidence/quality-bench-report.txt` | `osss_interpolation_quality_tests --report`, full table, this tree |

The observed/expected PPMs (40 per burst, ~1.5 MB each) were left in
`%TEMP%\OSSS\test-captures\<session>` and are not part of the package.

## Risks, blockers, and stop conditions

- The recorded desktop was in active use; the native 26 s leg is known-dirty.
  Do not "fix" that by loosening anything — re-record it.
- Runs pop windows over whatever else is on screen and take ~30 s each. Do not
  run a batch on a desktop that is not idle.
- If osss.exe's startup latency changes materially (e.g. shader caching), the
  26 s schedule is no longer load-bearing but stays valid.

## Delivery contract

Whoever re-measures reports, per leg: verdict, unique/generated/real counts,
PSNR and bad-pixel figures split by classification, offsets, steps, backward
steps, whether the overlay was up before the burst, and whether the desktop
was idle — with environment-scoped anomalies named as such.
