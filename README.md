# OSSS — Open Source Super Scaler

[![CI](https://github.com/Lenovive/OSSS/actions/workflows/ci.yml/badge.svg)](https://github.com/Lenovive/OSSS/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Written primarily by Claude](https://img.shields.io/badge/written%20primarily%20by-Claude-d97757)](#authorship)

> **Written primarily by Claude.** Most of this codebase — the optical-flow
> estimator, the pacing loop, the launcher, the test harnesses, and these docs
> — was written by Anthropic's Claude, directed and reviewed by
> [@Lenovive](https://github.com/Lenovive). See [Authorship](#authorship).

OSSS (Open Source Super Scaler) is an experimental, window-level
frame-generation and upscaling pipeline for Windows. It captures an ordinary
window, estimates screen-space motion on the GPU, and fills a chosen output-FPS
target without injecting code into the target.

It is MIT-licensed, has **no third-party dependencies** of any kind, and is
built to be read, forked, and tinkered with: C++20, CMake, and raw D3D11/HLSL,
with every shader inline in the source file that uses it.

New here? [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) explains the data flow
and who owns which decision; [CONTRIBUTING.md](CONTRIBUTING.md) covers building
and the definition of done for a change.

## Current state

- Captures a visible window through Windows Graphics Capture.
- Keeps a bounded history of up to eight unique captured frames on the GPU with
  Direct3D 11.
- Presents through a borderless, click-through output window without injecting
  code into the target process.
- Owns presentation with a fixed rational target clock. Automatic mode retains
  the active display path's numerator/denominator; manual targets remain
  available from 24 through 1000 FPS.
- Uses the WGC compositor timestamp as source media time and records callback
  arrival separately for latency diagnostics.
- Classifies exact compositor duplicates with a small asynchronous GPU
  signature before they can advance unique-source cadence or pair identity.
- Uses queue target 1: playback is delayed by about one estimated source period
  plus an adaptive jitter budget whose default floor is 8 ms, then each target deadline selects two known
  timestamped endpoints and a fractional alpha in `[0, 1]`. It never
  extrapolates.
- Treats the selectable `2x` through `6x` multiplier as a ceiling, not a pacing
  mode. The default is `6x`, which covers common fractional cases such as
  60→144 (`2.4x`). When that ceiling binds, production defaults to an even
  cadence; `--ceiling-pacing spread` retains the historical distributed gate
  for comparison.
- Uses a flip-model frame-latency waitable swap chain with maximum frame latency
  one by default. Late work is counted and skipped instead of queued for
  catch-up. `--pacing` selects between that default (`paced`), a render-ahead
  variant with maximum frame latency two that still hands frames over on the
  deadline grid (`queued`), and a free-running loop with no deadline grid at
  all (`unpaced`); see
  [Pacing modes](#pacing-modes-unpaced-paced-and-queued).
- Records non-blocking GPU timestamp-query p50/p95 telemetry for source-pair
  flow and output fusion, and reuses a bounded history-texture pool to avoid
  per-frame allocation churn.
- Requests capture updates at the output-target cadence on Windows builds that
  expose the high-rate capture-session interval control.
- Uses a vendor-neutral Direct3D 11/HLSL motion backend by default:
  - bidirectional coarse-to-fine optical flow over a mip-chained luma pyramid,
    refined to a quarter pixel and then to the vertex of the match-error curve;
  - one flow estimate per source pair, reused for every generated position;
  - forward/backward consistency and confidence-weighted warped-frame fusion,
    biased toward the better-supported side where the two warps disagree;
  - scene-cut rejection and static-pixel protection;
  - optional user-defined UI/HUD mask rectangles that always show the newest
    real frame, are excluded from optical-flow matching evidence, and do not
    count toward scene-cut detection;
  - optional automatic detection of static overlays, which masks regions that
    hold still while the scene around them moves.
- Falls back to temporal blending if motion setup or estimation fails, and
  exposes the blend path explicitly for A/B comparisons.
- Preserves the operating-system cursor instead of interpolating it.
- Includes deterministic D3D9Ex, D3D10, D3D11, and D3D12 source animations at
  every 10 FPS step from 10 through 120, with reference-frame scoring.

This is classical reduced-resolution optical flow, not a trained interpolation
model. It is now motion-aware and substantially reduces simple double images,
but difficult disocclusions, fine particles, transparency, and very fast motion
still expose artifacts. The interchangeable interpolation boundary leaves room
for a later neural backend without replacing capture or presentation.

One limit is worth stating precisely, because it is a property of local motion
estimation rather than of this implementation. A repeating texture displaced by
more than half its own period is indistinguishable from the same texture
displaced by the smaller amount in the opposite direction. OSSS resolves that
tie toward the smaller displacement, which is correct whenever the source rate
samples the motion finely enough. Below that rate a periodic region reconstructs
sharply but in the wrong phase. Raising the source frame rate is the fix; no
amount of filtering recovers the information.

`osss_interpolation_quality_tests` measures all of this against analytically
generated ground truth. See
[docs/TEST_ANIMATIONS.md](docs/TEST_ANIMATIONS.md#reference-image-quality-bench).
Its `--dump-sequence <dir>` writes the runs that are ordered in time as PNGs
with a viewer to step, loop, and blink them, including a per-frame flicker map;
that is the tool for an artifact you can see but not score.

## Requirements

- Windows 10 version 2004 or newer; Windows 11 is the primary target.
- A GPU and driver exposing Direct3D feature level 11.0 plus typed UAV access
  to `R32G32B32A32_FLOAT` textures.
- Visual Studio 2022 Build Tools with the Desktop C++ workload.
- CMake 3.25 or newer.
- A visible, restored target running windowed or borderless.

## Build

From a Developer PowerShell for Visual Studio 2022, using the presets in
`CMakePresets.json`:

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
```

That configures a Ninja Release tree in `out\release\`, which is the canonical
build location used for all validation below. A `debug` preset (`out\build\`)
and a `vs2022` multi-config preset (`out\vs2022\`) are also defined:

```powershell
cmake --preset vs2022
cmake --build --preset vs2022-release
ctest --preset vs2022-release
```

Verify the build without needing a target window. This checks the adapter,
compiles and initializes the motion shaders, validates the overlay and
click-through window styles, and exercises the output clock:

```powershell
.\out\release\osss.exe --self-test
```

It is the only check that compiles the HLSL, so run it after editing a shader.

Run the short-lived end-to-end Windows Graphics Capture check after building:

```powershell
.\out\release\osss.exe --capture-self-test          # Ninja presets
.\out\vs2022\Release\osss.exe --capture-self-test   # Visual Studio preset
```

It opens a small animated window, captures at least four changing GPU frames,
copies them through the history pipeline, presents the interpolated result with
the production swap chain, and closes automatically.

Run the additive adaptive timing check from a real desktop session:

```powershell
.\out\release\osss.exe --adaptive-capture-self-test          # Ninja presets
.\out\vs2022\Release\osss.exe --adaptive-capture-self-test   # Visual Studio preset
```

That check preserves the historical smoke fixture separately, then measures a
60 FPS animated source against a fixed 144 FPS clock after a two-second warmup.
It validates bounded history, duplicate isolation, fractional selection, alpha
bounds, and flow reuse. Submitted and DXGI-confirmed rates, missed slots, queue
delay, and capture-to-present latency are reported as environment-dependent
evidence rather than being conflated with the scheduler's requested rate.

For an explicit input-routing check, run the interactive smoke test:

```powershell
.\out\release\osss_input_passthrough_smoke.exe
```

It briefly moves the system pointer, clicks a generated-frame surface over a
separate target UI thread, verifies that the surface remains visible while the
target receives the click, and restores the pointer.

## Run

### Settings GUI

Launch the native local settings window:

```powershell
.\out\release\osss_gui.exe
```

The launcher is two tiers. Five settings are visible; the rest are behind one
**Advanced settings** disclosure, grouped as Pacing / Quality / Output shape and
HUD masks / Diagnostics and profiles. Whether that disclosure is open is
remembered in `%LOCALAPPDATA%\OSSS\launcher.txt`, beside the profiles file.
Every selectable option carries a hover tooltip saying what it does and when to
pick it, reachable from the control, its label, or the `(?)` beside the label.
The launcher follows the system app theme (light or dark) and offers no switch
of its own.

Visible without expanding:

- a dropdown of visible, restored target windows plus a Refresh button;
- an output-FPS target that defaults to the target display refresh rate;
- a 2x through 20x maximum interpolation limit, defaulting to 6x;
- Motion-aware or Temporal-blend interpolation;
- a default-on source/output FPS overlay toggle.

Behind **Advanced settings**:

- *Pacing* — a present mode of Auto, Tearing / VRR, or VSync, defaulting to Auto
  (see [Present modes and frame pacing](#present-modes-and-frame-pacing)); a
  pacing mode of Paced, Queued, or Unpaced, defaulting to Paced (see
  [Pacing modes](#pacing-modes-unpaced-paced-and-queued)); an adaptive
  queue-buffer floor from 0 through 32 ms, defaulting to 8 ms; and a ceiling
  pacing of Even or Spread, defaulting to Even;
- *Quality* — a flow-scale setting of Auto, Quality, Performance, or Ultra
  performance, defaulting to Auto; an upscale setting of Auto, Off, or Always,
  defaulting to Auto; an optional cheaper motion-search checkbox, off by
  default; and a temporal-prior checkbox, on by default, that seeds each pair's
  motion search from the previous pair (see
  [How far the motion search reaches](#how-far-the-motion-search-reaches));
- *Output shape and HUD masks* — an output shape of Overlay or Fullscreen,
  defaulting to Overlay; an optional HUD mask field listing static UI rectangles
  to keep out of interpolation (see [UI/HUD masks](#uihud-masks)), which reports
  how many regions parsed, or the typo, as you type; and an optional automatic
  static-HUD detection checkbox, off by default;
- *Diagnostics and profiles* — a diagnostic view selector of Off, Flow field,
  Confidence, or Fallback reason, defaulting to Off; and an optional
  per-application profile checkbox, off by default, with a Save button that
  writes the current settings as the selected target's profile.

Below both tiers, a status panel reports state as data rather than as one long
sentence: a coloured dot and a headline for the state, the resolved values on
one line, and the rule relating them on a second. Start and Stop sit in a
footer band that stays pinned; when the expanded settings do not fit the work
area, only the settings region scrolls.

On **Start**, the launcher minimizes itself and hands the foreground back to
the target window. That is deliberate: `osss.exe` keeps its overlay hidden
while the target does not own the foreground, so a launcher that stayed on top
would leave nothing visible until you switched to the game yourself. Stop stays
reachable from the taskbar, and a global stop shortcut is also registered — see
[Stopping a session](#stopping-a-session).

Opening the launcher also starts a background `osss.exe --warm-shader-cache`.
The motion shaders are compiled from inline HLSL at runtime, and the first
compile after an update takes several seconds; doing it while you pick a window
means a session started from the launcher normally never waits for one. The
compiled bytecode is cached under `%LOCALAPPDATA%\OSSS\shadercache`, so later
starts read it instead of recompiling.

It starts the sibling `osss.exe` without a console window and requests a clean
shutdown through a local Windows event.

### Dev test animations

Use `Dev > Test animation` in the settings GUI to launch a deterministic source
through Direct3D 9Ex, 10, 11, or 12 at 10, 20, 30, ... 110, or 120 FPS. The
launcher refreshes and selects the new source window. Start frame generation as
usual, then press `S` in the test window to save the visible output, align it to
the analytically correct frame, and report pixel variance separately from its
temporal offset. Press `O` to open the evidence folder.

The `Dev` menu covers the API/rate matrix only. To exercise UI masking, launch
the animation from a terminal with `--hud-overlay`, which adds static-position
HUD panels that step at source cadence and prints the matching `--ui-mask`
argument:

```powershell
.\out\release\osss_test_animation.exe --api d3d11 --fps 60 --hud-overlay
```

See [docs/TEST_ANIMATIONS.md](docs/TEST_ANIMATIONS.md) for the pattern rationale,
metrics, command-line reference exporter/comparator, the HUD-overlay fixture,
and capture caveats.

The compact HUD follows the target's upper-left corner and is topmost,
click-through, and non-activating. It reports:

- `RAW`: all WGC callbacks over the last interval;
- `UNIQUE`: content-changing frames after GPU duplicate classification;
- `SUBMITTED`: successful swap-chain `Present` calls;
- `DISPLAY`: DXGI-confirmed present progression when frame statistics are
  available, otherwise `n/a`;
- required, allowed, and realized multipliers;
- `FRAMES`: the generated/native split of the last interval's submitted
  presents, where *generated* counts only interpolated frames and *native*
  counts real source frames, including ones held or repeated into a slot;
- bounded queue occupancy, policy delay, capture-to-present age, missed target
  slots, duplicate captures, and capture-queue drops;
- `PACING`: the present-interval distribution — p50, p95, max, and the
  percentage of intervals landing within 15% of the target period.

`PACING` answers a different question from every rate above it. The rates say
how many frames were submitted; `PACING` says whether they were evenly spaced,
and those are independent. A submitted rate sitting exactly on target is
perfectly compatible with intervals alternating between half and one-and-a-half
periods, and that combination is what judder *is*. Read `on-time` first: if the
rate looks right and `on-time` is low, the pacing is wrong, and
`--present-mode` is the setting that governs it.

The interval is measured from present to present, so it is what OSSS is
answerable for — deadline to handover. It is not scan-out; `DISPLAY` remains the
only DXGI-side evidence, and neither is physical proof. The measurement resets
whenever the overlay hides, because the interval spanning an alt-tab is the
length of the alt-tab rather than a pacing failure. Under `--pacing unpaced`
there is no deadline grid: the HUD labels the row `PACING (free-run)`, omits
`on-time`, and shows `MISSED --`, because there is no target period for an
interval to be on time against and no slot to miss.

The console additionally reports p50/p95 media-to-ingest, flow-GPU, and
fusion-GPU timings, the same pacing figures as `pacing-p50/p95/max`,
`pacing-on-time` and `pacing-mae` (mean absolute deviation from the target
period), and `clock-phase-fixes`, the number of vblank phase corrections the
output clock has applied. Under `--pacing queued` it also prints
`render-ahead=` and `served-late=` — how many of the interval's presents were
of a frame drawn ahead of its deadline versus served at the deadline because
nothing was prepared — so the mode's claim can be checked rather than assumed.
Its `modes-i` and `modes-total` fields use this order:
`no_frame/cold_start/interpolate/real_frame/hold/underrun/ceiling_hold/stalled`.

The HUD split is instantaneous — it covers the same one-second interval as the
rate row. The console line carries the session-cumulative equivalent as
`generated-share=`, alongside `interpolated=`, a raw count of presents that went
through the interpolator.

Those two are not the same figure, and the difference is large. `interpolated=`
runs at 96-99% of submitted presents whenever generation is active at all: an
output slot practically never lands on a source timestamp, so nearly every slot
blends — including slots at alpha 0.999 that are visually the real frame. It
measures interpolator work, not frame-generation uplift.

The split is the smaller of two bounds. `submitted - unique` is the uplift the
source did not supply, but a capture stall inflates it, because hold, cold
start, underrun, and stall all submit a *real* frame and keep submitted up while
unique collapses. The interpolated count bounds it from the other side, since a
hold never interpolates. Taking the minimum makes a stalled source report 0%
generated rather than 100%.

On supported Windows builds, OSSS requests WGC delivery up to the output target;
the OS and target application still decide when new surfaces exist. The ceiling
permits at most:

```text
min(output target FPS, unique source FPS x maximum multiplier)
```

That expression controls deterministic target-slot admission; it does not resize
or re-phase the target interval. Submitted FPS may still be lower when GPU work
or the display misses a slot. Neither successful `Present` calls nor optional
DXGI statistics are physical proof that every generated image was visibly
scanned out.

When the required multiplier is at or near unity — the measured source rate
already meets the output target — OSSS presents the nearer real frame instead of
synthesizing one. Interpolating there cannot add smoothness, and a synthesized
frame measurably softens thin detail and adds latency.

The threshold is 1.15, chosen from both sides. Capture-rate jitter puts a matched
source and target anywhere in roughly 0.92 to 1.11, so a tighter bound would
leave real unity cases synthesizing; and the smallest ratio genuinely worth
interpolating is a 120 FPS source on a 144 Hz display at 1.20, which must stay
above the threshold. Ratios above 1.15 interpolate exactly as before.

Because the comparison uses the *captured* source rate, capture throughput
determines whether the guard engages: if capture falls behind a fast source, the
required multiplier rises and interpolation resumes even though the source itself
was already at the target rate.

For the Visual Studio preset, launch `.\out\vs2022\Release\osss_gui.exe`.

### Command line

List candidate windows:

```powershell
.\out\release\osss.exe --list-windows
```

`--title` takes a case-insensitive fragment of either the window title or the
executable file name, so `--title vlc` finds a window titled
`movie.mkv - VLC media player`. An executable match wins outright: if any window
matches by executable name, windows that matched only by title are dropped. That
is what keeps `--title mygame` from being made ambiguous by the terminal you
launched OSSS from, whose own title is the command line you typed. A fragment
naming a document rather than an app still falls through to titles. OSSS never
offers its own windows as a target.

It must resolve to exactly one window; when it still matches more than one -- two
windows of the same application, say -- OSSS lists the candidates and you pass
`--hwnd` instead.

Target the selected window's display refresh, allowing up to 6x interpolation:

```powershell
.\out\release\osss.exe --title "Game title" --target-fps auto --max-multiplier 6 --interpolator motion --stats-overlay on
```

The queue buffer floor trades latency for resilience to capture/GPU jitter. It
defaults to 8 ms, grows by 2 ms after an underrun, and decays slowly after
clean source frames; it is capped at 32 ms. Set it explicitly when measuring a
heavy game:

```powershell
.\out\release\osss.exe --title "Game title" --target-fps auto --buffer 16
```

The buffer affects only how far behind the compositor OSSS selects known media;
the WGC compositor timestamp remains the media clock. `media-to-ingest-p50` and
`media-to-ingest-p95` in the runtime telemetry report the measured availability
tail without feeding callback timing back into media timestamps.

`motion` is the default. Run the old temporal baseline against the same target
for an A/B comparison:

```powershell
.\out\release\osss.exe --title "Game title" --target-fps 240 --max-multiplier 6 --interpolator blend
```

The FPS overlay is also enabled by default for command-line sessions. Pass
`--stats-overlay off` to disable it.

### Present modes and frame pacing

`--present-mode` chooses how a finished frame reaches the display. It is the
single largest influence on frame pacing, so it is selectable and the resolved
choice is printed in the startup banner.

| Mode | Present call | Use it when |
| --- | --- | --- |
| `vsync` | `Present(1, 0)` | Fixed-refresh panel, and you would rather have judder than tearing. |
| `tearing` | `Present(0, DXGI_PRESENT_ALLOW_TEARING)` | You have G-Sync or FreeSync on, or you accept tearing to get exact pacing. |
| `auto` (default) | tearing where DXGI supports it, else vsync | Almost always. |

Under `vsync` every present is held to a vblank, so the output rate is
quantized to the display's refresh period. A target rate that is not a divisor
of the refresh rate cannot be paced evenly: a 120 FPS target on a 144 Hz panel
rounds each deadline up to the next vblank and alternates 6.9 ms and 13.9 ms
frame times indefinitely, which is visible as judder however correct the frame
*count* looks.

Under `tearing` the present returns immediately, so the rational output clock
sets handover time and OSSS keeps its own cadence instead of being dragged onto
vblank multiples.

**Scope this honestly.** The generated-frame surface must be click-through, and
on Windows that requires `WS_EX_LAYERED` — measured, not assumed: without it
`osss_input_passthrough_smoke.exe` reports that the overlay intercepted the
click. A layered window is always composed by DWM, so this swap chain can never
reach independent flip. `tearing` therefore does **not** produce literal
tearing, and it does not drive a G-Sync or FreeSync panel the way a fullscreen
exclusive swap chain would. What it removes is OSSS's *own* quantization, which
is the half of the pacing problem an overlay can control. If your frame times
under `vsync` alternate between one and two refresh periods, `tearing` fixes
that; if you want the display itself to follow the frame rate, an overlay is the
wrong shape of program for it.

`auto` prefers tearing wherever `IDXGIFactory5::CheckFeatureSupport` reports
`DXGI_FEATURE_PRESENT_ALLOW_TEARING`. If you ask for `tearing` on an adapter
that does not support it, the banner says so rather than silently using vsync.

Under `vsync` only, OSSS also phase-aligns its deadline grid to the display's
vblank raster, using the sync timestamps DXGI reports for presented frames. Only
the phase moves — the rational target rate is never altered — and the correction
is a capped fraction of the measured error, so it settles without hitching. The
alignment is skipped outright when the two grids are incommensurate, which is
exactly the case `tearing` exists to serve.

`--ceiling-pacing even` is the default. It shows each admitted frame for a
whole number of target slots when the multiplier ceiling binds, which
avoids alternating one-slot and two-slot frame durations. Use
`--ceiling-pacing spread` only when comparing against the older Bresenham-like
distribution. The output remains source-dependent; the console's `submitted`
rate is the resulting runtime rate.

### Pacing modes: unpaced, paced, and queued

`--present-mode` decides *how* a present reaches the display. `--pacing`
decides *when*, relative to the output clock, a frame is rendered and handed
over — whether the loop waits for a deadline at all, and how much work it may
have in flight ahead of one. The two are independent and every combination is
valid.

| Mode | Deadline grid | Max frame latency | Where the render sits | Extra media delay |
| --- | --- | --- | --- | --- |
| `unpaced` | none | 1 | whenever a back buffer is free | none |
| `paced` (default) | rational output clock | 1 | at the deadline | none |
| `queued` | rational output clock | 2 | one slot ahead of the deadline | one target slot |

**Read this before choosing.** Capture-to-present latency comes almost
entirely from the media queue — one source period plus `--buffer` — and that
is the same in every mode. In both a paced and a free-running loop the frame
shown at time *t* reflects media time *t − queue delay*. So `unpaced` does not
buy latency; it changes the *cadence*. Measured on a 60 FPS source into a 240
FPS target on a 239 Hz display, all three modes submitted 236 frames/s (the rate
DWM picks up a composed overlay at) with a present-interval p50 of 4.24 ms;
`unpaced` was about 1 ms lower in capture-to-present than `paced`, and
`queued` about 5 ms higher.

`unpaced` has no output clock. Whenever the swap chain has a free back buffer
the loop selects for *now*, renders, and presents. Cadence is whatever the swap
chain permits: refresh-locked under `vsync`, composition-limited for the
DWM-composed overlay under `tearing`, and genuinely free-running only for
`--output-mode fullscreen` under independent flip. Nothing is ever late and
nothing is evenly spaced either — the first slot of every source pair carries
the optical-flow pass and lands later than the ones after it. `--target-fps`
and `--max-multiplier` do not limit cadence in this mode; the generation
toggle still works and selects real frames only, presented once each as they
arrive. It exists as a measurement floor and for parity with tools that offer
it, not as something to run for its own sake.

`paced` is today's behaviour and the one every measurement in this repository
was taken with. A rational output clock owns the timeline; each slot's frame is
selected, rendered, and presented at its deadline, with maximum frame latency
one so a queued frame can never add hidden latency. A slot whose back buffer is
not free is dropped and counted in `missed`, never queued for catch-up.

`queued` keeps the same clock but renders one slot ahead: as soon as slot *k*
is handed over, slot *k+1* is selected and drawn, and at its own deadline only
`Present()` runs. Maximum frame latency two and a third back buffer make that
legal. The flow pass leaves the critical path entirely — a slow pair costs
nothing unless it exceeds a whole slot — at a price of exactly one *target*
slot of extra media delay (about 4 ms at 240 FPS, visible as `queue-delay`
growing by that much), because slot *k+1*'s bracketing source frames must
already be in hand when slot *k* is presented. That delay is taken on the media
clock, not by presenting early: frames still hand over on the deadline grid,
which is what keeps this correct under `tearing`, where an early present would
simply appear early. The console's `render-ahead=` / `served-late=` counters
show whether the render-ahead is actually happening; on the measurement above
every present was rendered ahead (`served-late=0`) under both `tearing` and
`vsync`.

Queue target — how many source periods the media clock trails live by — is not
part of this setting. It stays at 1 in every mode.

```powershell
.\out\release\osss.exe --title "Game title" --target-fps 240 --pacing queued
```

An exact window handle can be used when titles are ambiguous:

```powershell
.\out\release\osss.exe --hwnd 0x123456 --target-fps 240 --max-multiplier 3
```

Keep a top-left minimap and a bottom-right ammo counter out of interpolation:

```powershell
.\out\release\osss.exe --title "Game title" --ui-mask "0,0,0.22,0.18; 1560,940,1920,1080px"
```

The examples above use the Ninja `release` preset location. Use
`.\out\vs2022\Release\` for the Visual Studio preset.

`--flow-scale` sets the resolution motion estimation runs at, as a divisor of
the source size along each axis. Flow is low-frequency, so a coarser grid is
much cheaper in principle and loses little on large objects; what it loses is
thin structures, sharp motion boundaries, and small fast objects.

| Value | Divisor | Notes |
| --- | --- | --- |
| `auto` | 4 up to 1440p, 8 above | The default. Every other measurement in this repository was taken on this rule. |
| `quality` | 4 everywhere | Finer than `auto` above 1440p. |
| `performance` | 8 everywhere | Coarser than `auto` at 1440p and below. |
| `ultra-performance` | 16 everywhere | The cheapest flow this pipeline estimates. |

This setting changes the grid motion is estimated on, **not how far the search
reaches** — see [How far the motion search
reaches](#how-far-the-motion-search-reaches). It used to change both, which made
`quality` reach half as far as `auto` above 1440p.

```powershell
.\out\release\osss.exe --title "Game title" --flow-scale quality
```

Measured on one RTX 5090 with `osss_interpolation_quality_tests --report`, the
cost side of this trade is far smaller than the theory predicts. Flow
preparation moved only from 0.51 to 0.44 ms per pair across every setting at
960x540, and from 0.92 to 0.88 ms at 3840x2160 — because the pass is dominated
by fixed costs (pyramid build, dispatch, the outlier filter) rather than by the
per-cell search. The quality side is not small: at 960x540 `performance` costs
2.0 dB on the linear lane and 3.7 dB on thin detail, and it raises the detail
lane's temporal roughness from 0.41x the crossfade's to 10.8x — that is
visible flicker, not a rounding difference.

**At 4K the `auto` rule is measurably the wrong choice.** Its divisor of 8
fails this repository's own quality gates at 3840x2160 while `quality` passes
them, for 4% more GPU time (0.92 ms against 0.88 ms per pair): 45.03 dB against
43.26 dB on linear motion, 22.54 dB against 18.11 dB on thin detail, and a
detail roughness ratio of 0.33x the crossfade against 11.65x. If you run above
1440p, pass `--flow-scale quality`. The default is unchanged pending a second
GPU, because every number here is from one adapter.

### How far the motion search reaches

The coarse level scans a grid of candidate displacements around each cell, so
there is a largest motion it can find at all. Past that the estimate is not
merely coarse, it is wrong: the fine level only refines a few pixels either side
of what the coarse level hands it, confidence collapses, and fusion falls back to
a crossfade. On a fast pan at a high multiplier that fallback is what reads as
smearing or wobble.

Reach is a target stated in **source pixels**: 64 of them per pair at 60 FPS. Two
things would otherwise move it, and both are corrected for.

The first is the source frame rate. The motion the estimator has to find is a
velocity multiplied by the frame period, so at 30 FPS a camera covers twice the
ground per pair that it does at 60, for the same camera. A fixed radius therefore
made the same scene fail at low source rates and pass at high ones. The search
now scales with the measured period, holding reach at one velocity: a 60 FPS
source on a divisor of 8 resolves to a radius of 4 cells, exactly what the search
used before it scaled, and 30 FPS resolves to 8 while 120 resolves to 2.

The second is `--flow-scale`, and until recently it was a bug. Because the radius
was counted in flow *cells*, choosing a **finer** grid silently **halved** reach:
`--flow-scale quality` reached 32 source pixels where `auto` above 1440p reached
64. The setting you pick to get more accuracy was quietly taking away the range
that decides whether a fast pan survives at all. Reach is now divided by the cell
size, so **every `--flow-scale` reaches the same distance** and the setting trades
detail against GPU time only. Holding reach on a finer grid does cost more —
measured in Factorio at 2752×2064 and 60 FPS, a divisor of 4 costs 0.4 ms of flow
against 0.2 for a divisor of 8, and 3.3 W of board power.

Reach is clamped to 128 source pixels at the top — a cost bound, since the search
is `(2r+1)^2` comparisons per cell — and to two cells at the bottom, which keeps
enough search to seed the fine level.

The per-second telemetry line reports the result as `flow-search`, in source
pixels per source frame. To give the estimator more reach when content still
outruns it, **raise the source frame rate**. That is the direct lever: it shrinks
the motion rather than growing the search, and it costs the interpolator nothing.
Lowering `--flow-scale` no longer buys range, only speed.

`--debug-view fallback` shows where the interpolator gave up and crossfaded, and
is the direct way to tell whether the reach is the problem.

`--performance-mode on` estimates motion more cheaply at the same flow
resolution: it halves the coarse search radius, and skips the second local
search that resolves periodic detail landing in the wrong period.

```powershell
.\out\release\osss.exe --title "Game title" --performance-mode on
```

**On the one GPU measured, this is not a trade worth taking, and it is off by
default.** Warm-cache flow preparation at 960x540 goes from 0.39-0.44 to
0.35-0.36 ms per pair — about 0.05 ms, or 0.3% of a 60 FPS source frame — while
thin detail drops 7.3 dB (22.02 to 14.74) and the linear lane's temporal
roughness rises from 0.45x the crossfade's to 1.05x, meaning the output is no
longer steadier than a plain blend. At 1920x1080 the timing difference
(0.44 against 0.47 ms) is inside run-to-run noise.

The setting is kept because it may earn its keep on a weaker GPU, where the
search is a larger share of the pass — which is untested. Both of this
section's levers cut search work substantially and neither moved the wall
clock much, which points at the rest of the pass rather than the search: the
pyramid build, the six separate dispatches per pair, and the full-resolution

`--temporal-prior on` (the default) is the third lever, and the only one that
looks past the current pair. When two consecutive pairs share a frame -- the
ordinary case, which the renderer knows from the unique sequences it selects
pairs by -- the previous pair's motion field is offered to the coarse search as
nine extra candidates around the vector it found last time. The search window is
a displacement ceiling per pair, and content that leaves it is not estimated
coarsely but not found at all; a seed lets the estimate follow a pan that
accelerates out past the window one flow cell per source frame instead of
losing it the moment it crosses. It is a candidate and never a bias: selection
stays regularised toward zero, so in flat regions where every displacement ties
the smallest still wins and a wrong vector cannot perpetuate itself, and only
confident vectors are offered. Cost is about a tenth of the coarse search
(0.37 to 0.41 ms per pair at 960x540, warm).

```powershell
.\out\release\osss.exe --title "Game title" --temporal-prior off
```

What is measured, on the quality bench's accelerating-pan sequence over a
dead-leaves scene (`osss_interpolation_quality_tests.exe --report`, the
*reach* tables): in performance mode, where the window is 16 source pixels,
the first pan past it scores 21.7 dB seeded against 17.2 unseeded (14 % bad
pixels against 35 %), the next 18.8 against 15.5, and the one after 16.4
against 14.7 -- the pan is followed for three cells beyond the window and then
lost gradually rather than at once. In quality mode the same sequence is
neutral (at most +0.45 dB), because the fine level's second search from zero
compares the seeded match against a near-zero one on a cost that penalises
displacement at four times the coarse level's rate and overrides most of what
the seed found; that penalty scale is a separate finding, recorded in the bench.
On the test pattern itself the prior changes no matrix number: every pair there
is estimated from scratch, and the consecutive-pair sequence moves by less than
a cell. It does not help a moving *object* whose displacement exceeds the
window, only whole-frame motion: the backward search is seeded from the same
position in the shared frame, which for an object is offset by its own motion.
`off` exists for A/B comparison and for measuring the estimator without it.

`--output-mode` decides whether a variable-refresh display can follow OSSS at
all. A display only follows an application when that application's swap chain
drives scanout directly, which on Windows means **independent flip**: DWM
noticing that one opaque window covers a whole output and handing it the
display plane instead of compositing it.

| Value | Shape | Variable refresh |
| --- | --- | --- |
| `overlay` | Click-through, sized to the target window | Never. Layered windows are always composed. |
| `fullscreen` | Opaque, covering the monitor | Eligible. Pair with `--present-mode tearing`. |

```powershell
.\out\release\osss.exe --title "Game title" --output-mode fullscreen --present-mode tearing
```

The default stays `overlay` because `fullscreen` gives up click-through, and
that is not a small loss. Without `WS_EX_LAYERED` nothing passes a click to
another process's window. The game keeps keyboard focus because the output
window never activates, and titles reading raw input or clipping the cursor are
unaffected — but a game that needs ordinary mouse messages delivered to its own
window will not receive them while fullscreen output is on.

The FPS overlay is disabled automatically in fullscreen mode. It is its own
topmost layered window, and anything composited above the output silently
demotes it back to composed — which would cost the mode its entire purpose
without reporting anything.

**Eligibility is not confirmation.** OSSS reports whether the shape and present
mode permit promotion; DWM decides, and tells nobody. Confirm with PresentMon,
which reports a per-frame present mode of `Hardware: Independent Flip` when it
worked and `Composed: Flip` when it did not. Third-party overlays (Steam,
Discord, RTSS) and notifications can demote it at any moment. This has not yet
been confirmed on a physical variable-refresh display.

`--upscale` spatially upscales the finished frame when the output is larger
than the captured source — the fullscreen case where the target window is
smaller than the monitor. `--sharpness` sets the sharpening pass from 0 through
1, defaulting to 0.35.

Ordering is deliberate: it runs on the **fused** frame, never on the source.
Optical flow keeps estimating on native captured pixels, which is both cheaper
and more accurate — an upscaler invents plausible detail, and feeding invented
detail to a block matcher gives it confident matches for structure that was
never there.

Two passes: an edge-directed 12-tap upsample steered by a 3x3 luma structure
tensor, then a contrast-limited sharpener that cannot overshoot past values the
neighbourhood already spans. Measured by `osss_upscaler_tests --report` as a
downscale/upscale round trip scored against the original, with bilinear as the
baseline: **+0.51 dB mean over bilinear**, +1.48 dB on the best instant, and
-0.60 dB on the worst. A 1:1 pass with sharpening off is 47.5 dB, so it is
near-lossless when it has nothing to do.

The negative worst case is honest rather than a loosened gate. Whole-frame PSNR
includes the thin-detail lane, and a 2x downsample destroys that lane outright —
below Nyquist there is no direction left to steer along, so no edge-directed
kernel can beat a blur there. What the gate catches is the direction estimate
breaking everywhere, which is what a NaN-producing eigenvector looked like when
this bench caught one during development: -9.58 dB mean.

`--debug-view` replaces the output with a picture of the interpolator's own
internals. It exists because of how every motion defect in this project was
actually found: all seven were diagnosed by temporarily editing the fusion
shader to return an intermediate as colour, rebuilding, looking, and reverting.

| Value | Shows |
| --- | --- |
| `flow` | Direction as hue, magnitude as brightness. Grey is no motion; large flat areas of one hue are a camera pan; speckle means the estimator disagrees with itself. |
| `confidence` | White where the warps were trusted, black where the crossfade fallback took the pixel. A mostly black frame means the interpolator has silently become a crossfade. |
| `fallback` | Which safeguard claimed each pixel: red below the confidence floor, green UI-masked, blue static-pixel protection. |

```powershell
.\out\release\osss.exe --title "Game title" --debug-view confidence
```

These replace the frame rather than overlaying it, because a legible picture of
a flow field cannot share pixels with the scene it describes. They read only
intermediates the normal path already computes, so switching one on costs
nothing extra. The startup banner says plainly when a view is active, since a
diagnostic frame is easy to mistake for a rendering bug.

`--profile <name.exe>` applies the arguments stored for that executable before
the ones on the command line, so anything given explicitly still wins.
`--save-profile <name.exe>` writes the rest of the command line to that section
and exits without capturing.

```powershell
.\out\release\osss.exe --save-profile witcher3.exe --target-fps 240 --max-multiplier 4 --flow-scale quality
.\out\release\osss.exe --title Witcher --profile witcher3.exe
```

A profile is stored as **the command line you would have typed**, not as a
settings record. That is the whole design: every flag is validated by the same
branch that validates it on the real command line, a flag added to OSSS works in
profiles the day it lands with no serialisation code to update, and the file
stays readable enough to edit by hand. Profiles live in
`%LOCALAPPDATA%\\OSSS\\profiles.txt`:

```ini
# Lines starting with # are comments.
[witcher3.exe]
--target-fps 240 --max-multiplier 4
--flow-scale quality

[vlc.exe]
--target-fps 120 --ui-mask "0,0.9,1,1"
```

A malformed file is reported with its line number rather than partly applied —
half a profile is indistinguishable from the settings simply not working. Saving
writes through a temporary file and a rename, so an interrupted save cannot
leave a truncated file that takes every other profile down with it.

`--profile auto` keys off the target's own executable instead of naming one, so
each game picks up its own settings without being told which. Having no profile
for a program is then the ordinary case rather than an error — the defaults
apply. The launcher's **Apply saved profile for the target program** checkbox is
this same flag.

Auto-matching resolves the target window twice at startup: once to learn the
executable name, then again after re-parsing with the profile applied. That is
deliberate. The alternative is a settings structure merged by hand, which is the
duplication this design exists to avoid, and the cost is one window-list lookup
that happens once.



filter and detector passes.


`--max-multiplier` accepts 2 through 20 and also answers to `--multiplier`
and `-m`. It is a **ceiling**, not a pacing target: the output clock decides
when a frame is due, and the multiplier only bounds how many generated
frames a single source pair may supply.

High multipliers are close to free per output frame, which is why the range
goes this far. Optical flow is estimated once per source pair and every
generated position reuses it, so the Nth frame of a pair costs one fusion
pass and nothing else — per-output-frame cost falls as the multiplier rises.
Measured on an RTX 5090, fusion is 0.09-0.29 ms per output frame from 960x540
through 1080p and 0.41-0.43 ms at 2160p, against the 1.67 ms output period of
600 FPS, which is 20x from a 30 FPS source.

What does not scale is the motion model. Alphas stay inside [0, 1] — nothing
is extrapolated — but at 20x from a 30 FPS source a linear model is being
asked to hold across 33 ms of real time, and every artifact stays on screen
proportionally longer. The top of the range is for low-rate content: video,
emulated or engine-capped titles, power-limited handhelds. It is not a
better setting for a game already running at 60.

`--warm-shader-cache` compiles the motion shaders into the per-user bytecode
cache and exits, creating no window:

```powershell
.\out\release\osss.exe --warm-shader-cache
```

The shaders are inline HLSL compiled at runtime through `d3dcompiler`. The first
compile costs several seconds — almost all of it in one entry point — and every
later start reads the cache instead. The launcher runs this in the background
when it opens. Deleting `%LOCALAPPDATA%\OSSS\shadercache` is always safe; the
next start simply recompiles. The cache key covers the shader source, entry
point, profile, and compile flags, so editing a shader invalidates it
automatically.

One further flag is not intended for direct use. `--stop-event <name>` makes
`osss.exe` wait on a named Windows event and shut down cleanly when it is
signalled; `osss_gui.exe` passes it so the launcher's **Stop** button can end a
session. It is part of the launcher contract, so keep it working when changing
argument parsing.

### UI/HUD masks

`--ui-mask` (or the launcher's HUD mask field) takes one or more rectangles
written `left,top,right,bottom`, separated by `;`. Values are fractions of the
captured frame in `[0, 1]`; append `px` (or use any value above 1) to give
source-frame pixels instead. Fractions survive window resizes and HUD-scale
changes; pixel regions are clamped to the frame.

Inside a masked rectangle OSSS shows the newest real source frame at source
cadence rather than a warped or cross-faded image, so counters tick instead of
blending. Masked pixels are also removed from the optical-flow matching
evidence, which stops a static HUD from voting for zero motion on behalf of the
gameplay around it, and they do not count toward scene-cut detection, so a menu
opening under a mask does not force a full-frame fallback. Masks apply to the
motion interpolator only; the temporal-blend A/B baseline stays unmasked so the
comparison remains a plain cross-fade.

Rectangles are fixed for the session.

#### Automatic detection

`--ui-mask-auto on`, or the launcher checkbox, additionally discovers static
overlays without being told where they are:

```powershell
.\out\release\osss.exe --title "Game title" --ui-mask-auto on
```

Each source pair, every cell of a quarter-resolution grid asks two questions:
did my own pixels hold still, and did the scene around me move? Cells that
answer yes to both accumulate a score, mask once it passes the arm threshold
after roughly ten source pairs, and are released within about three pairs once
they stop qualifying. The neighbourhood is sampled at 6% and 18% of the frame
rather than a few pixels, so the interior of a large element such as a minimap
can still see gameplay moving outside it, and the armed region is dilated by one
cell into neighbouring cells that were themselves still, so an overlay's outline
is covered without the mask bleeding onto the moving content beside it.

The detector reads the flow of the pair it runs on, and its result is published
for the *next* pair. That one-pair lag is deliberate: a counter must still be
masked on the frame it ticks, and only stops being masked if it keeps changing.

It is off by default, combines with any explicit rectangles, and is best treated
as experimental. Known behaviours:

- A genuinely static part of the *scene* next to moving content can arm. Masking
  a static region shows the newest real frame, which for static content is the
  same image, so this is a no-op rather than an artifact.
- Content that sits still for about ten source pairs and then starts moving is
  masked for up to about three more pairs before release — roughly 50 ms at a
  60 FPS source.
- Detection is per cell at quarter resolution, so overlays thinner than a few
  pixels are not reliably found. Use an explicit rectangle for those.
- Explicit rectangles remain the predictable option; automatic detection is
  additive, never subtractive.

The output window does not activate and is click-through, so keyboard and
pointer input remain directed at the target.

### Overlay visibility

The generated-frame surface is shown only while the target is the window you are
actually looking at. It is hidden whenever the target is minimized, or when the
foreground moves to a window belonging to another process. Generation keeps
running and capture stays warm, so alt-tabbing away and back resumes without a
restart, and the first pair after the target returns is re-acquired rather than
interpolated across the gap. Note that `MISSED` still climbs while hidden: the
output clock counts slots it had to skip regardless of whether anything was due
to be shown, and re-anchoring it on resume would discard the genuine count.

This gating is not cosmetic. The surface is topmost, opaque and click-through:
left up over a backgrounded target it paints a frozen frame across the whole
display while passing every click to whatever is underneath, which reads as a
hung machine.

If a Direct3D exclusive-fullscreen application holds the display, OSSS prints a
warning at startup. Windows Graphics Capture cannot read that path — switch the
target to borderless or windowed.

The overlay also hides when the source goes silent for more than a second, and
comes back when frames resume. Windows Graphics Capture only delivers on content
change, so a static source is silent too — hiding then is visually a no-op,
because what shows through is the same unchanged window. Without this, a capture
that dies leaves a frozen frame painted over the target at the full target rate.

Minimizing and restoring the target permanently breaks Windows Graphics Capture:
the frame pool was sized against a surface that no longer exists, and the session
never recovers on its own. OSSS rebuilds the capture session on the restore edge.
A game that minimizes on focus loss hits this on every alt-tab.

### Stopping a session

Any of these ends a session:

- the stop hotkey, printed in the startup banner and by `--self-test`;
- the launcher's **Stop** button;
- closing the target window.

The hotkey is `Ctrl+Alt+F12` where that chord is free. It is not always free —
the Intel Graphics Command Center hotkey service claims exactly that combination
by default — so OSSS falls back in order to `Ctrl+Shift+F12`,
`Ctrl+Alt+Shift+F12`, then `Ctrl+Alt+End`, and reports which one it got. If every
candidate is already owned, the banner says so instead of promising a shortcut
that does not exist, and the launcher's Stop button is the way out.

### Turning generation off and on

`Ctrl+Alt+F11` toggles frame generation without ending the session. Capture, the
output window, the overlay, and the output clock all stay up; the display simply
sees native frames and nothing else until you press it again.

It walks its own fallback list — `Ctrl+Shift+F11`, `Ctrl+Alt+Shift+F11`, then
`Ctrl+Alt+Home` — separately from the stop chord, so losing one never costs the
other, and the one it got is printed in the startup banner beside the stop key.
If every candidate is taken it says so and the session runs exactly as it did
before this existed.

This is for comparing generated against native **on the same scene**. Stopping
and relaunching loses the scene, which is the one thing an A/B cannot afford, and
it is also how you tell a generation artifact from something the game was doing
anyway.

Off is implemented as a maximum multiplier of one rather than as a separate path:
the slot gate declines the output slots the source cannot fill from a real frame,
so nothing is synthesised and nothing is repeated. While it is off the HUD header
reads `GEN OFF`, the telemetry line reports `generation=off`, submitted frames
fall to the source rate, and the generated share goes to zero. Those numbers are
still real measurements — of a native session.

## Target and maximum-multiplier behavior

The target rate owns an absolute deadline sequence and never chases the source
cadence estimate. For 60→144, the selector walks timestamped source pairs and
uses continuously varying fractional alphas while telemetry reports a required
multiplier near `2.4x`; there is no alternating integer `2x`/`3x` mode.

The multiplier is only a safety/quality ceiling. With a 240 FPS target and a 2x
maximum, 120 unique FPS can reach 240 FPS, while 200 unique FPS needs only about
1.2x. A 30 FPS source with a 6x ceiling is admitted to at most about 180 of the
240 target slots each second under `spread`; the production `even` policy uses
the next whole-slot cadence (120 submitted slots for that 180/240 bound) to
avoid alternating frame durations. Held slots do not create backlog or shift
the 240 Hz clock.

Queue target 1 waits for a known future endpoint. This deliberately adds roughly
one source-frame period plus the adaptive jitter budget, capture delay, GPU work,
and presentation latency. Cold start, source stall, recovery, multiplier holds,
and buffer underrun select or hold a real frame; no alpha leaves `[0, 1]` and no
extrapolation is attempted.

## Known limitations

- Motion is inferred only from captured pixels; OSSS cannot access game depth,
  engine motion vectors, or engine-supplied UI masks. HUD masks are user-drawn
  static rectangles, optionally extended by the experimental static-overlay
  detector; pop-up UI that neither covers is still interpolated.
- Flow runs at quarter resolution through 1440p and eighth resolution above
  it. Large/ambiguous motion, thin geometry, and newly revealed regions can
  still smear, split, or fall back to blending.
- SDR and one GPU are the current design target. The shader path is not tied to
  a GPU vendor, but this checkout has only been run on an NVIDIA RTX 5090; AMD
  and Intel hardware validation remains open.
- HDR, VRR-aware pacing, exclusive fullscreen, protected content, and
  anti-cheat compatibility are not claimed. Exclusive fullscreen is detected and
  warned about at startup, not supported.
- The overlay follows the foreground, not the target's own display mode. A game
  that keeps the foreground while rendering somewhere OSSS cannot capture will
  still show a stale surface; only exclusive fullscreen is detected explicitly.
- Windows Graphics Capture cadence can differ from application presents. The
  current duplicate classifier compares exact 16×16 quantized GPU signatures.
  It avoids a synchronous full-frame readback, but noise can create false
  uniques and very small changes between sampled points can be missed.
- Queue target 1 trades added media delay for interpolation-only bracketing.
  Lower-latency extrapolation and selectable queue targets are intentionally out
  of scope.
- DXGI frame statistics are optional and compositor-dependent. `SUBMITTED`,
  `DISPLAY`, and physical monitor output remain distinct claims.
- Resizing is recovered by recreating the capture pool; rapid resize is not yet
  polished.

See [docs/ROADMAP.md](docs/ROADMAP.md) for the remaining quality, latency, and
compatibility gates.

## Authorship

Most of OSSS was written by [Claude](https://claude.com/claude-code), Anthropic's
AI coding assistant, working from direction and review by
[@Lenovive](https://github.com/Lenovive). That includes the optical-flow
estimator and its HLSL, the output clock and pacing loop, the Win32 launcher,
the test and benchmark harnesses, and this documentation. Design decisions,
priorities, and every merge were human calls; the prose and the code largely
are not.

This is stated plainly because you should know what you are reading before you
trust it, and because the alternative — letting you assume otherwise — is
worse. Two things follow from it, and both cut in your favour:

- **Verify rather than trust.** Nothing here asks you to take a claim on
  authority. `ctest --preset release` is 16 tests, `osss.exe --self-test`
  compiles every shader, and the quality numbers come from
  `osss_interpolation_quality_tests --report`, which scores the interpolator
  against an analytic ground truth and against the plain crossfade it has to
  beat. Run them. Where a measurement came from one machine, this repository
  says so.
- **Review it like any other code.** AI-written code fails in its own ways: it
  is fluent, which makes a wrong assumption read as confidently as a right one,
  and it is prone to plausible-looking comments that no longer match the code
  beneath them. If you find one, that is a real bug — open an issue.

Contributions from humans and from AI assistants are equally welcome, under the
same standard: the checks in [CONTRIBUTING.md](CONTRIBUTING.md) must pass, and
you should be able to explain what your change does and what you measured.

## Documentation

| Document | Covers |
| --- | --- |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Data flow, device and thread ownership, invariants |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Milestone status and open validation gates |
| [docs/TEST_ANIMATIONS.md](docs/TEST_ANIMATIONS.md) | The deterministic test-source harness |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Building, the checks a change must pass, code conventions |
| [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) | Ground rules for project spaces |
| [SECURITY.md](SECURITY.md) | Reporting a vulnerability, and what is in scope |
| [NOTICE.md](NOTICE.md) | What OSSS links, and what adopting a dependency requires |

## Contributing

Contributions are welcome — bug reports, measurements, and code alike. See
[CONTRIBUTING.md](CONTRIBUTING.md) for the build, the checks a change has to
pass, and the conventions the codebase follows, and
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) for the ground rules.

Because this project is about frame timing and image quality, a claim about
either is only as good as its measurement. `osss_interpolation_quality_tests`
and `osss_test_animation.exe` exist so a change can be argued with numbers;
please bring them.

## License

[MIT](LICENSE) — Copyright (c) 2026 Joe Olson and contributors. Use it, fork
it, ship it, sell it; just keep the copyright notice.

OSSS has no third-party source or binary dependencies; it links only Windows
system libraries supplied by the OS and the Windows SDK. [NOTICE.md](NOTICE.md)
lists exactly which ones and why.
