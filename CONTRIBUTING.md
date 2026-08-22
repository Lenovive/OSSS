# Contributing to OSSS

OSSS is open on purpose. Fork it, break it, measure it, send the result back.
There is no CLA and no contributor agreement to sign: contributions are
accepted under the [MIT license](LICENSE) that covers the rest of the project.

## The short version

1. Build it (below). If the build works you already have every dependency —
   there are none beyond the Windows SDK.
2. Change something.
3. Run `ctest --preset release`. It must be green.
4. Open a pull request that says what you changed and what you measured.

## Getting a build

OSSS is Windows-only (Windows 10 2004 or later, Direct3D feature level 11.0)
and needs MSVC. Everything must run from a **Developer PowerShell for VS 2022**
so `cl`, `ninja`, and the Windows SDK headers are on the path. A plain
PowerShell fails with missing standard headers — that is environmental, not a
source error.

To bootstrap from a plain PowerShell:

```powershell
Import-Module "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools" -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"
```

Then:

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Binaries land in `out/release/` (Ninja is single-config, so there is no
`Release\` subfolder). **`out/release/` is the canonical, verified build tree**;
`out/build/` is the Debug preset. A `vs2022` multi-config preset also exists for
people who prefer the Visual Studio generator.

Warning level is `/W4 /permissive-`. The flags come from the
`osss_common_flags` INTERFACE target in [CMakeLists.txt](CMakeLists.txt) --
link it from every new target rather than repeating compile options, or the
target silently builds without `/W4` and without `UNICODE`, and neither failure
is visible until much later.

Not on Windows, or no GPU? You can still read the code and review pull
requests, and CI will build and run the GPU-less tests for you.

## What a change has to pass

A change is not finished until all of these hold. Say in the pull request which
ones you ran and which you could not — "I had no GPU, so I did not run the
motion tests" is a fine thing to write, and much better than silence.

1. `cmake --build --preset release` succeeds with **zero new warnings**. The
   warning level is `/W4 /permissive-`.
2. `ctest --preset release` is fully green (16/16).
3. `out\release\osss.exe --self-test` passes. This is **required** for any
   change touching a shader, the renderer, the overlay, or the output clock —
   shaders compile at runtime, so an HLSL error is a runtime failure that
   `ctest` cannot catch and only `--self-test` will. A change to the
   presentation loop or swap chain should also pass it with `--pacing queued`
   and `--pacing unpaced`.
4. Documentation is updated (see the ownership table below). A new or changed
   CLI flag means `--help` **and** `README.md`, in the same change.
5. The frozen fixtures below are untouched.
6. Anything you could not run is labelled environment-scoped rather than
   reported as a pass.

## Which checks are safe to run unattended

| Check | Unattended? | Notes |
| --- | --- | --- |
| `ctest --preset release` | Yes | Runs `osss_adaptive_scheduler_tests`, `osss_test_pattern_tests`, `osss_app_profile_tests`, `osss_debug_view_tests`, `osss_flow_scale_tests`, `osss_launcher_layout_tests`, `osss_output_mode_tests`, `osss_png_writer_tests`, `osss_window_catalog_tests`, `osss_pacing_mode_tests`, `osss_upscaler_tests`, `osss_ui_mask_tests`, `osss_stats_overlay_tests`, `osss_motion_tests` and `osss_interpolation_quality_tests` (both need a D3D11 device), and `osss_gui_tests` (which runs `osss_gui --self-test`; creates a window, asserts no two control rects intersect in either disclosure state, and that every tooltip is registered). |
| `out\release\osss_interpolation_quality_tests.exe --report` | Yes | The same bench with its full per-sample table. `--dump <dir>` writes observed/expected images as a PPM and a PNG of the same pixels (plus flow/confidence views for the reach ramp); `--dump-sequence <dir>` writes the two runs that are ordered in time -- the temporal sequence and the reach ramp -- at eight views each including the `error-step` flicker map, with a `viewer.html` to step, loop, and A/B-blink them, and `--dump-embed <divisor>` adds a single-file copy with the images inlined (~52 MB of PNGs and about a minute on top of the run); `--size WxH` re-measures at another resolution; `--temporal-prior on|off` and `--performance-mode` A/B the estimator (the reach section always runs both prior settings and also a dedicated performance-mode interpolator). ~30 s; prefers a hardware device, falls back to WARP. |
| `out\release\osss.exe --self-test` | Yes | Adapter, motion setup, overlay/click-through styles, resolved present mode, output clock. The only check that compiles the shaders. |
| `out\release\osss.exe --warm-shader-cache` | Yes | Compiles the motion shaders into `%LOCALAPPDATA%\OSSS\shadercache` and exits. Creates no window. Useful for timing a cold compile: delete that directory first. |
| `out\release\osss.exe --capture-self-test` | Needs a real desktop session | Opens a small window and captures it. Fails under RDP/sandbox — report as environment-scoped, not source-scoped. |
| `out\release\osss.exe --adaptive-capture-self-test` | Needs a real desktop session | ~12 s 60→144 timing run; results are hardware/display dependent. |
| `out\release\osss_input_passthrough_smoke.exe` | **No** | Moves and clicks your mouse pointer. Only run it deliberately, on an idle desktop. Deliberately not a CTest. |

Adding a CTest? Add it to this table in the same change.

## Measurement beats argument

This project is about frame timing and image quality, and both are easy to be
confidently wrong about. Two harnesses exist so a claim can be settled with
numbers:

- `out\release\osss_interpolation_quality_tests.exe --report` — the
  reference-image quality bench, per lane, against the plain crossfade the
  interpolator has to beat. `--dump <dir>` writes observed and expected images;
  `--dump-sequence <dir>` writes the time-ordered runs plus a `viewer.html` to
  step, loop, and A/B-blink them.
- `out\release\osss_test_animation.exe` — deterministic D3D9Ex, D3D10, D3D11,
  and D3D12 source windows with reference-frame scoring. See
  [docs/TEST_ANIMATIONS.md](docs/TEST_ANIMATIONS.md).

If you are changing the interpolator, a `--report` from before and after is
worth more than any prose description of the change.

Timing *means* are close to useless here — background work skews them. Compare
the fastest call.

## Frozen fixtures

These are regression baselines. Add new coverage as **separately named** tests
or modes; do not loosen or re-parameterize an existing one to make a change
pass:

- `tests/motion_interpolator_tests.cpp`: source pixels, dimensions,
  thresholds, the 16 px translation and black-to-white scene-cut cases.
- `tests/adaptive_scheduler_tests.cpp` `TestHistoricalTargetCounts`: 250 µs
  step, 2 s warmup, 1 s measurement, 240 Hz target, ±3 tolerance.
- `src/capture_smoke.cpp` historical mode: 360×220 window, 70×70 cyan box,
  13 px/frame, target 240, max 6, at least 4 captures and 2 presents in 5 s.
- `tests/input_passthrough_smoke.cpp`: 360×220 target at (120,120), centre
  click, exactly one click delivered, pointer restored.

The gates in `tests/interpolation_quality_tests.cpp` are *not* frozen — they are
floors and ceilings measured on one GPU at one resolution. Re-measure with
`--report` before moving one, and move it for a reason you can state in the
pull request. The relative columns are the load-bearing ones (gain over the
crossfade, roughness ratio to the crossfade), because absolute PSNR cannot tell
an interpolator that works from one that has silently become a crossfade.

## Adding a user-visible option

A tunable touches more files than it looks like. The full chain:

1. `src/main.cpp` — the `Options` field, the parse branch, the `--help` text,
   and the startup banner if the user should see it.
2. `src/gui_main.cpp` — the control inside the layout walk in
   `CreateLauncherControls` (ask `LauncherLayout` for its rect; do not name
   coordinates), an entry in `kTips` registered with `AddFieldTip`, and the
   command-line builder so the launcher actually passes it.
3. `README.md` — the command-line section *and* the launcher bullet list.
4. `tests/` — a case in the matching `*_tests.cpp` if any of it is testable
   without a GPU.
5. `CMakeLists.txt` — only if you added a module. Link `osss_common_flags`, and
   `osss_test_support` for a test.
6. `docs/ROADMAP.md` — if this closes or opens a gate.
7. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — the module map if you added a
   file, and this document's unattended-checks table if you added a CTest.

A flag that exists in `main.cpp` but not in `--help` and README is a bug. So is
one the launcher cannot produce.

## Which document owns what

Keep these in sync; when they disagree, the one that owns the topic wins.

| Document | Owns |
| --- | --- |
| [README.md](README.md) | User-facing behavior, every CLI flag, limitations |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Structure, ownership, threading, invariants |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Milestone status and open gates |
| [docs/TEST_ANIMATIONS.md](docs/TEST_ANIMATIONS.md) | The `osss_test_animation.exe` harness |
| [NOTICE.md](NOTICE.md) | The dependency inventory, and what adopting one requires |
| This file | How to work in the repo: build, checks, conventions |

Never overstate what is verified. If a number came from one machine, say which.

## Code conventions

- Namespace `osss::`; `kConstantName`; `[[nodiscard]]` on queries;
  `winrt::com_ptr` for COM; `UNICODE` everywhere (`wchar_t`, `L"..."`).
- One `.h`/`.cpp` pair per module, header named for the primary class.
  Header-only is fine for pure constants and validators.
- `src/` is flat but holds four groups — pipeline, launcher, test-animation
  harness, smoke support. Put a new file in the group it belongs to and keep
  the name prefix consistent.
- Shaders are raw-string literals inside the `.cpp` files. Grep for
  `ShaderSource[] = R"` to find them all. They compile at runtime, so an HLSL
  error is a runtime `InterpolatorError`, not a build failure.
- Tests include `tests/test_harness.h` and assert with `Require` /
  `RequireNear`; failures throw and `main` catches.
- Frame pacing is a first-class concern, not a tuning detail. Anything that
  changes *when* a frame reaches the display — present flags, sync interval,
  swap-chain flags, window styles that affect composition, the output clock's
  phase, maximum frame latency — must say in a comment what it does to the
  frame-time distribution.
- Interpolation alphas stay in `[0, 1]`; no extrapolation. Scene cuts, cold
  start, stall, and underrun select a real frame.
- Keep telemetry classes distinct in code and prose: *raw capture* vs *unique
  source* vs *target* vs *submitted* vs *DXGI-confirmed* vs *generated*. Never
  describe a `Present` count as physical display output, and never call a held
  or repeated real frame generated.

## Please open an issue first if you want to

- **Add a dependency or an ML model.** See [NOTICE.md](NOTICE.md) for what
  this project links today and what adopting anything else requires. The
  zero-dependency property is
  deliberate and is most of why this codebase stays readable. A
  machine-learning interpolation backend also needs a license audit of the
  model *and* its weights before it can be merged.
- **Extrapolate, or use a queue target other than 1.** Both are design
  reversals, not tweaks.
- **Change the platform minimums** (Windows 10 2004, D3D feature level 11.0).

None of these are forbidden — and a fork is always fine, which is rather the
point — but in this repository they want a discussion first.

## Commits and pull requests

- One logical change per commit. Keep the source, its tests, and its
  documentation updates together, so a commit is reviewable on its own.
- Say what changed and why in the message. "Fix bug" tells a future reader
  nothing; the reason a line exists is the part that cannot be recovered from
  the diff.
- Rebase rather than merge when updating a branch, so history stays linear.
- If `git` reports "dubious ownership" of `.git` on Windows, run:

```powershell
git config --global --add safe.directory <path to your OSSS checkout>
```

### Licensing constraints to preserve

- `LICENSE` must contain the MIT text and **nothing else**. Appending sections
  to it stops GitHub's license detector recognizing the repository as MIT, and
  it then reports the license as "Other" in the sidebar and through the API.
  The dependency inventory lives in [NOTICE.md](NOTICE.md) for exactly this
  reason.
- Do not add code from another source without checking that its license permits
  redistribution under MIT. If you do, record it in [NOTICE.md](NOTICE.md).

## Historical handoffs

`handoffs/<date>-<topic>/` holds dated engineering packages with their
evidence. Read them for intent and invariants, not as current-state truth:
symbol names stay valid, **line numbers drift**, and each package's
measurements were taken on the hardware it names. Do not edit a landed handoff;
copy `handoffs/TEMPLATE/` to start a new one.

Packages written before the project was renamed refer to it as JLSS. The name
was changed to OSSS throughout when the project was opened up; nothing else
about those measurements changed.

## Reporting a bug

Include your GPU and driver version, your Windows build, the exact command line
(or launcher settings), the target window's application and its graphics API,
and what the startup banner printed. For a visual artifact, a
`--dump-sequence` viewer or a short capture is worth a paragraph of
description. For a pacing complaint, the stats-overlay numbers or a `--report`
run beats "it feels stuttery".
