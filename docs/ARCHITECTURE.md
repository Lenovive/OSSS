# OSSS architecture

How a captured pixel becomes a presented frame in the Open Source Super Scaler,
and who owns what along the way.
This document describes structure and ownership; [README.md](../README.md) is the
source of truth for user-visible behavior and limitations, and
[ROADMAP.md](ROADMAP.md) tracks what is still unverified.

Symbols are cited by name, not line number, because line numbers drift.

## Where to start reading

| If you are asked about... | Read, in this order |
| --- | --- |
| Pacing, multipliers, alphas, fallbacks | `src/adaptive_scheduler.h`, then `RunFrameGeneration` in `src/main.cpp` |
| Present modes, vsync/tearing, VRR | `src/present_mode.h`, then `Renderer::Present` and `Renderer::CreateSwapChain` |
| Pacing modes: unpaced / paced / queued, render-ahead, frame latency | `src/pacing_mode.h`, then the three loop branches in `RunFrameGeneration` and `FrameSelector::SetLookahead` |
| Why startup is fast now | `src/shader_cache.h` |
| Capture, timestamps, duplicate frames | `src/capture_session.h`, then `CaptureSession::OnFrameArrived` |
| Output window, swap chain, frame history | `src/renderer.h` |
| Interpolation quality | `src/motion_interpolator.h`, then `kMotionComputeShaderSource` |
| HUD masking | `src/ui_mask.h`, then `Renderer::SetUiMask` |
| Anything at all, first time | This file, then `RunFrameGeneration` |

`RunFrameGeneration` in `src/main.cpp` is the one function that touches every
subsystem. If you read only one thing, read it.

## Binaries

| Binary | Source | Role |
| --- | --- | --- |
| `osss.exe` | `src/main.cpp` on Windows; `src/portable_main.cpp` elsewhere | The pipeline. The Windows build owns the D3D11/WGC loop; the portable build owns the scheduler loop around `DesktopCapture`, `SoftwareInterpolator`, and `DesktopPresenter`. |
| `osss_gui.exe` | `src/gui_main.cpp`, `src/launcher_theme.*` on Windows; `src/portable_gui_main.cpp` elsewhere | Settings launcher. Windows uses the owner-painted native settings window; portable builds use a dependency-free terminal launcher that enumerates windows and starts the same pipeline with the selected native window handle. |
| `osss_test_animation.exe` | `src/test_animation_*`, `src/test_pattern.*` | Deterministic D3D9Ex/10/11/12 source windows for testing. Not part of the pipeline. |

`osss_gui.exe` never interpolates anything. It builds an argument list and
launches `osss.exe`; the Windows launcher also signals a named event to request
shutdown, while portable sessions are stopped with Ctrl+C. The portable
launcher deliberately has no toolkit dependency so the same binary works from
an X11 terminal or a macOS terminal.

## One device, two threads

The following device/thread invariant is Windows-specific. Portable builds do
not create a shared GPU device: `portable_main.cpp` runs capture, CPU motion,
presentation, and scheduler bookkeeping on one loop, while each native API is
confined to `DesktopCapture` or `DesktopPresenter`.

There is exactly one `ID3D11Device` in the process.

- `Renderer::InitializeDevice` creates it with `D3D11_CREATE_DEVICE_BGRA_SUPPORT`
  and immediately calls `ID3D10Multithread::SetMultithreadProtected(TRUE)`.
- `CaptureSession` does **not** create a device. It is constructed from
  `renderer.Device()` and takes the same immediate context via
  `GetImmediateContext`.

So capture and presentation share one device *and one immediate context* across
two threads. Multithread protection is the only thing making that legal. If you
ever remove that call, or introduce a second device, re-derive this whole section
before you do.

The two threads:

| Thread | Entry | Does |
| --- | --- | --- |
| WGC callback | `CaptureSession::OnFrameArrived`, registered via `FramePool::CreateFreeThreaded` | Copies the arriving surface, stamps media time, queues signature work. Never renders. |
| Main | `RunFrameGeneration` | Message pump, timeline ingest, scheduling, render, present, telemetry. |

Everything shared between them lives behind `CaptureSession::mutex_`. The
scheduler, the renderer, and all of `src/adaptive_scheduler.*` are main-thread
only and are not internally synchronized. `RunFrameGeneration` gives the main
thread MMCSS `Games` priority where available, with a
`THREAD_PRIORITY_HIGHEST` fallback.

## The journey of one frame

```text
  target window
       |
       | Windows Graphics Capture
       v
  OnFrameArrived                          [WGC callback thread]
       |  copy into a recycled texture
       |  media_time = QPC-mapped SystemRelativeTime  (the media clock)
       |  arrival    = steady_clock::now()            (telemetry only)
       |  queue a 16x16 signature compute + ID3D11Query
       v
  classified_frames_  (duplicate flag resolved asynchronously)
       |
=======|=================================================================
       |  DrainClassifiedFramesAfter                   [main thread]
       v
  SourceTimeline::Ingest
       |  ingest timestamp -> media-to-ingest p50/p95 telemetry
       |  duplicate    -> returns nullopt, frame is dropped here
       |  size changed -> discontinuity, history cleared, selector reset
       |  otherwise    -> assigns a unique sequence number
       v
  Renderer::StoreCapturedFrame  -> pooled bounded GPU history (up to 8 frames)
       |
       v
  OutputClock::TakeLatestDue(now)
       |  absolute rational deadlines from a fixed epoch
       |  stale slots are skipped and counted, never queued
       v
  FrameSelector::Select  -> FrameSelection { mode, alpha, two endpoints }
       |
       +-- interpolate -> Renderer::SelectFramePair -> MotionInterpolator::PreparePair
       +-- real_frame / cold_start / hold / ceiling_hold / underrun / stalled
       |               -> Renderer::SelectRealFrame
       v
  Renderer::WaitForPresentationSlot   (frame-latency waitable object)
       v
  Renderer::Render(alpha)
       |  fusion draws to fusion_target_ at SOURCE size when upscaling,
       |  straight to the back buffer when not
       |
       +-- upscaling? -> Upscaler::Draw  -> back buffer at OUTPUT size
       |                 edge-directed upsample, then contrast-limited sharpen
       v
  Renderer::Present()
       |
       +-- vsync   Present(1, 0)
       +-- tearing Present(0, ALLOW_TEARING)
```

Two orderings in that diagram are load-bearing rather than incidental.

The upscaler runs on the **fused output**, at the end, and never on the source.
Optical flow keeps estimating on native captured pixels at the lowest resolution
available, which is both cheaper and more accurate than estimating on
interpolated ones: an upscaler invents plausible detail, and feeding invented
detail to a block matcher gives it confident matches for structure that was
never in the source. For the same reason fusion draws into a separate
source-sized `fusion_target_` when upscaling rather than the back buffer --
letting the rasteriser stretch the frame first and then upscaling that would be
a different, worse operation. See `src/upscaler.h`.

Note also the ordering of the last two steps: OSSS waits for the presentation slot
*before* rendering, not after. That is what holds maximum frame latency at one
without building a queue. Preserve it.

That diagram is the `paced` pacing mode, the default. `--pacing` selects one of
three loop shapes around the same components (`src/pacing_mode.h` names them
and the two mechanism facts each implies -- maximum frame latency and whether
the loop renders ahead -- so the swap chain and the loop cannot disagree):

| Mode | Loop shape | Max frame latency | Selector |
| --- | --- | --- | --- |
| `unpaced` | No `TakeLatestDue`. Whenever the waitable object signals: `SelectNow(now)`, render, present. The waitable object is folded into the loop's multi-object wait, and since it is a semaphore the wait *takes* the slot, which is what `Renderer::MarkPresentationSlotAcquired` records. | 1 | `SelectNow`: bypasses the ceiling gate (there are no slots to ration); a real frame already on screen is not re-presented. |
| `paced` | The diagram above. | 1 | `Select` at the deadline. |
| `queued` | After slot *k* is presented, `WaitForPresentationSlot`, `Select(k+1)`, `Render` immediately; at *k+1*'s deadline only `Present`. Each slot is selected exactly once, at prepare time; if nothing was prepared (cold start, no free buffer) the slot is served the paced way. | 2, three back buffers | `Select` one slot early, with `SetLookahead(one target slot)` added to the queue delay so the early selection is still bracketed. |

The `queued` cost is exactly one target slot of media delay and nothing else:
`tests/adaptive_scheduler_tests.cpp` `TestLookaheadSelectsOneSlotEarly` shows
that the pair and alpha prepared for slot *k+1* are the ones paced would have
used for slot *k*. Frames are still handed over at their deadlines, never
early -- an early `Present(0, ALLOW_TEARING)` would simply appear early, and
that is the whole reason render-ahead and present-on-deadline are separate steps.

Two more things happen on a timer in the same loop: the output window and HUD
re-follow the target every 250 ms, and telemetry is sampled every 1 s. Without
an output clock (`unpaced`) the loop still wakes on a 100 ms housekeeping tick
so those, and the source-silence hide, keep running when neither a capture nor
a free back buffer arrives.

Every iteration also re-evaluates whether the target is presentable: alive, not
minimized, owning the foreground (directly or through a window of the same
process), and having produced a unique frame within the last second. When it is
not, `RunFrameGeneration` hides both surfaces and consumes due slots without
submitting. Capture keeps running, so the transition back only costs a
`FrameSelector::Reset`.

Two counts do *not* stop while hidden, deliberately. `OutputClock` still tallies
slots it skipped — it owns that number, and re-anchoring the clock on resume
would clear the genuine misses along with the artificial ones. And capture keeps
classifying frames, which is what makes resuming cheap.

The restore edge is special-cased: minimizing and restoring the target
permanently breaks WGC delivery, so `RunFrameGeneration` tears the session down
and rebuilds it. `CaptureSession::Start` restarts raw sequence numbering at one,
so the drain cursor rewinds with it; `SourceTimeline`'s unique counter is
deliberately *not* rewound, which is what stops the renderer's frame history from
matching a stale entry against a reused sequence number.

## Who owns which decision

| Owner | Owns | Explicitly does not own |
| --- | --- | --- |
| `CaptureSession` | Source media time, duplicate classification, capture-drop accounting | Any notion of the output rate |
| `SourceTimeline` | Unique-frame history, cadence estimate, capture delay, media-to-ingest latency, stall detection | Deadlines |
| `OutputClock` | Absolute rational deadlines from a fixed epoch, missed-slot count, vblank phase alignment | Which frame fills a slot, and its own rate once set |
| `FrameSelector` | Mode, alpha, endpoint pair, adaptive queue delay, render-ahead lookahead, multiplier ceiling gate | When a deadline occurs |
| `TargetSlotGate` | Spread or explicit even distribution of ceiling-limited slots | Anything time-based |
| `PacingMonitor` | Present-interval distribution and the on-time fraction | Any decision -- it is measurement only, and nothing reads it back into the loop |
| `PacingMode` (`src/pacing_mode.h`) | Which loop shape runs, the swap chain's maximum frame latency, whether a slot is rendered ahead, and how much lookahead the selector needs | Anything at runtime -- it is a compile-time table of facts the loop and renderer both read |
| `Renderer` | The device, swap chain, pooled GPU frame history, output window, fusion timing, present mode resolution, maximum frame latency as the pacing mode dictates | Pacing -- it never decides when to present, only carries out the loop's decision |
| `MotionInterpolator` | Flow estimation and fusion, UI masking, the luma pyramid, flow timing | Frame selection |

The load-bearing consequence: **the target clock never re-phases from the
source.** Source cadence feeds buffering, telemetry, and ceiling admission only.
A change that lets the measured source rate move a deadline is a bug, not a
tuning choice.

The clock does re-phase from the **display**, and only from the display, through
`OutputClock::PhaseAlignToVblank`. That is a different thing and it is allowed:
the display is where the frames actually appear. Only the phase moves, never the
rate, and only under sync interval one, where a deadline's offset from the
vblank raster decides whether a present waits a whole refresh period to be seen.

## Presentation modes

Which frames get generated is `FrameSelector`'s decision. *When a generated frame
appears* is the present mode's, and the two are independent:

| Mode | Call | Consequence |
| --- | --- | --- |
| `vsync` | `Present(1, 0)` | The swap chain blocks until a vblank, so every deadline is quantized to the refresh period. Any target rate that is not a divisor of it beats against it. |
| `tearing` | `Present(0, DXGI_PRESENT_ALLOW_TEARING)` | The present returns immediately, so the output clock sets handover time and arbitrary target rates keep their cadence. DWM still composes the overlay, so this is not literal tearing. |

`PresentMode::automatic` resolves to tearing wherever
`IDXGIFactory5::CheckFeatureSupport` reports `DXGI_FEATURE_PRESENT_ALLOW_TEARING`.
Resolution happens in `CreateSwapChain`, because the swap-chain flags depend on
it, which is why `SetPresentMode` must be called before `CreateOutputWindow`.

Two consequences worth knowing before editing the renderer:

- The swap chain's creation flags are stored in `swap_chain_flags_` and reused by
  `ResizeBuffers`. DXGI rejects a flag set that differs from the creation one, so
  re-deriving them there would fail on the first window resize and nowhere else.
- The output window **must** be `WS_EX_LAYERED`, and the reason is input, not
  appearance. Its layered alpha is 255 -- fully opaque -- so the style looks
  free to remove, and removing it is tempting because a layered window is always
  composed by DWM and can never reach independent flip. It was removed, measured,
  and put back: `WS_EX_TRANSPARENT` plus `WM_NCHITTEST` returning `HTTRANSPARENT`
  does **not** pass a click to a window owned by another process, and
  `tests/input_passthrough_smoke.cpp` reports the overlay intercepting the click.
  Only the layered-plus-transparent pair is click-through.

  The consequence is architectural and worth stating plainly: **this surface is
  permanently DWM-composed.** Independent flip, multiplane overlay, and true
  tear-free variable refresh are unreachable for it, and no present flag changes
  that. `PresentModeStatus::independent_flip_eligible` reports `false` for
  exactly this reason rather than leaving it as folklore.

  The HUD in `src/stats_overlay.cpp` is layered too, and there it is
  uncontroversial: a GDI text window with no swap chain and nothing to pace.

## Invariants worth knowing before you edit

- Alphas stay in `[0, 1]`. There is no extrapolation anywhere, by design. Cold
  start, scene cut, stall, underrun, and ceiling holds all select a *real* frame
  rather than synthesising one.
- **Generation off is a multiplier of one, not a second code path.**
  `FrameSelector::SetGenerationEnabled(false)` drives the slot gate at the source
  rate instead of the target and returns the newest real frame; the slots the
  source cannot fill become ordinary ceiling holds. Nothing new decides which
  slots reach the display, so the toggle cannot drift away from the behaviour
  `--max-multiplier` already has. It survives `Reset` deliberately -- a capture
  discontinuity is not a reason to overturn a user's decision.
  `Renderer` only *counts* presses of the toggle chord; the presentation loop
  reads that count and applies the change between output slots, so the state
  lives with the selector that acts on it rather than with the window that
  received the keystroke.
- Optical flow runs **once per source pair**, in `MotionInterpolator::PreparePair`.
  Every generated position for that pair reuses the same fields. 6x must never
  mean five flow estimates.
- Each pyramid level matches on a **band-limited** image, never on
  full-resolution pixels point-sampled at that level's step. `PreparePair`
  builds a mip-chained luma copy of both frames for exactly this reason, and the
  coarse level reads the mip whose texel spans one search step. Sampling finer
  detail than the step can resolve makes the coarse level confidently wrong on
  any repeating texture, and the fine level's window is too narrow to escape it.
- Candidate selection uses `MotionCost`, never the raw match error. Uniform
  regions tie across the whole search window, and a bare `error < best`
  comparison hands those ties to whichever candidate the loop visited first --
  the far corner. Forward and backward estimation then disagree by twice that,
  the consistency check rejects the pair, and the interpolator degrades into a
  crossfade over most of the frame without failing any test.
- Confidence is judged against the patch's own contrast, not an absolute error
  threshold, and the fine level reports the confidence of the search it actually
  kept.
- The coarse search covers a **velocity in source pixels**, not a displacement
  and not a cell count. Its radius is resolved per pair from the measured source
  period *and the flow divisor* (`ResolveCoarseSearchRadius`,
  `src/flow_scale.h`), fed down the chain
  `SourceTimeline::EstimatedSourcePeriod` -> `Renderer::SetSourcePeriod` ->
  `MotionInterpolator::SetSourcePeriod` -> the `coarse_radius` cbuffer field.
  Both corrections exist for the same reason: reach is the contract, and two
  unrelated knobs used to move it. A fixed radius bounds how far a pixel may
  move between two frames, but what a scene actually fixes is how fast it
  moves; the two differ by the source period, so a fixed radius silently halves
  the camera speed the estimator can follow every time the source rate halves.
  A radius counted in *cells* had the same defect against `--flow-scale`:
  choosing a finer grid halved reach, so the setting a user picks for accuracy
  took away range. Dividing the pixel target by the cell size decouples them,
  and every flow scale now reaches equally far. Motion past the reach is
  unrecoverable rather than approximate -- the fine level cannot escape it -- so
  this bound decides where fusion degrades to a crossfade.
- The **previous pair's field is a candidate for the next, never an anchor.**
  When two pairs share a frame, `PreparePair(..., continues_previous_pair)`
  offers the coarse search nine extra taps around the most confident vector of
  the last pair's backward field (which lives on the shared frame's grid) --
  negated for the forward pass, as is for the backward. Selection stays
  regularised toward zero, so the seed wins only where the pixels support it
  and a wrong vector cannot perpetuate itself; the field is read with `Load`
  and never filtered, because it is discontinuous by construction. Only the
  renderer knows whether two pairs are consecutive -- it selects pairs by
  unique sequence -- so `SelectFramePair` decides continuity before it
  overwrites the active sequences, and the argument defaults to false for
  every caller that cannot vouch for it. Anything that rebuilds or resets the
  interpolator clears the prior. What this buys is following whole-frame motion
  that accelerates out past the window; what it cannot do is seed a moving
  object past the window, because the backward pass is seeded from the same
  position in the shared frame, which for an object is offset by its own
  displacement.
- The radius is a **cbuffer value, not a shader define.** It changes with the
  source rate, and specialising it would recompile the motion shaders mid-run
  every time that rate drifted across a boundary. Both search loops are already
  `[loop]`, so a dynamic bound costs nothing. `SetSourcePeriod` correspondingly
  invalidates nothing: unlike `SetFlowScale` it does not resize a surface or
  drop the prepared pair, which is what makes it safe to drive every iteration
  of the presentation loop.
- Anything in the fusion that decides "nothing moved here" from the two frames
  alone is wrong, and `static_protection` is the cautionary case: comparing the
  frames at a fixed position is a no-motion test, and an object narrower than
  its own displacement leaves the pixels it is about to cross unchanged in both.
  Such a test must be gated on the motion field as well.
- Nothing in the fusion iterates to a fixed point. Solving each pixel's source
  position that way is not a contraction at a motion boundary, and it fails by
  oscillating -- which alternates with the iteration count and so reads as
  working at even counts. If flow at time t is wanted, resample the field by
  splatting in its own pass.
- The flow outlier filter feeds fusion, not the UI-mask detector. Fusion turns a
  single wrong vector into a single visibly wrong pixel; the detector only asks
  whether a neighbourhood moved, samples it sparsely, and needs the outliers the
  filter removes.
- The warp's reconstruction filter is part of the quality result, not an
  implementation detail. A warp lands on a fractional position, both sides do
  it, and the fusion averages them, so a two-tap bilinear fetch softens every
  moving edge twice over. `SampleCatmullRom` is what keeps thin moving features
  sharp; no improvement to the flow substitutes for it.
- Duplicate frames are rejected before they can advance unique cadence or pair
  identity. `SourceTimeline::Ingest` returning `nullopt` is that choke point.
- Telemetry classes stay distinct in code and prose: raw capture, unique source,
  target, submitted, DXGI-confirmed, generated. A `Present` count is never
  physical display output.
- Frame *rate* and frame *pacing* are separate measurements and neither implies
  the other. `PacingMonitor` exists because every counter in this codebase was a
  rate, and a rate exactly on target is compatible with arbitrarily uneven
  spacing. Anything claiming pacing improved must cite the interval
  distribution, not a frames-per-second figure.
- Generated share (`osss::GeneratedFrameShare`) is the smaller of two bounds:
  `submitted - unique`, which a stall inflates because every non-`interpolate`
  mode still submits a real frame; and the interpolated-present count, which on
  its own reads 96-99% because the 1e-6 alpha snap window in `FrameSelector`
  practically never catches a slot. Neither bound is the metric on its own.
- The output window is click-through and non-activating. Input belongs to the
  target; `tests/input_passthrough_smoke.cpp` is the fixture that proves it.
- Click-through is necessary but not sufficient. The surface is also topmost and
  opaque, so it must not outlive the target's turn in the foreground: left up
  over a minimized or backgrounded target it covers the display with a frozen
  frame and hands clicks to whatever is behind it, which is not the target.
  `RunFrameGeneration` owns that gate; `Renderer` owns only `Show`/`Hide`.
- `Renderer` never assumes it holds a stop hotkey. `RegisterHotKey` fails when
  another process owns the chord — the Intel Graphics Command Center hotkey
  service takes `Ctrl+Alt+F12` by default — so registration walks a candidate
  list and `StopHotkeyDescription()` returns empty rather than lying. Anything
  that tells the user how to stop must read that string.

## Shaders

All HLSL lives inline as raw string literals in the `.cpp` files. There are six
in `src/`, plus one in a test. The two motion-compute literals are one HLSL
translation unit split in half: MSVC caps a single string literal at 16 KB, and
they are concatenated before compilation, so a function declared in the head may
be used in the tail. Keep the split between top-level definitions, and move it
if either half approaches the cap.

| Literal | File |
| --- | --- |
| `kSignatureShaderSource` | `src/capture_session.cpp` |

| `kMotionComputeShaderSourceHead` | `src/motion_interpolator.cpp` |
| `kMotionComputeShaderSourceTail` | `src/motion_interpolator.cpp` |
| `kMotionPixelShaderSource` | `src/motion_interpolator.cpp` |
| `kVertexShaderSource` | `src/renderer.cpp` |
| `kPixelShaderSource` | `src/renderer.cpp` |
| `kVertexShaderSource` | `tests/motion_interpolator_tests.cpp` |

To find them all, grep for `ShaderSource[] = R"`. Grepping for `cbuffer` finds
the three `src/` files but misses the test's vertex shader, which has none.

They compile at runtime through `d3dcompiler`, so an HLSL syntax error surfaces
as a runtime `InterpolatorError` rather than a build failure. Run
`osss.exe --self-test` after editing one; a green `ctest` will not catch it.

## What is deliberately absent

Knowing what is *not* here saves a search:

- No engine integration. No depth buffer, no motion vectors, no UI atlas. Motion
  is inferred from captured pixels only.
- No third-party dependencies, no ML model, no vendored code.
- No media catch-up queue deeper than one. Queue target 1 remains a fixed
  endpoint policy; its adaptive jitter floor is a latency control, not a second
  media timeline.
- No HDR and no exclusive-fullscreen path. Variable refresh is reachable in one
  shape only, and which one is a `--output-mode` choice. The overlay has to be
  click-through, which forces `WS_EX_LAYERED`, which forces DWM composition,
  which rules out independent flip -- so in overlay mode the display cannot
  follow OSSS's presents and `--present-mode tearing` removes OSSS's own vblank
  quantization and nothing more. `--output-mode fullscreen` drops the layered
  style for an opaque monitor-sized window that DWM can promote, which is what
  G-Sync and FreeSync need; it gives up click-through to get there, and
  disables the HUD because a second topmost window would demote it again.
  Eligibility is reported, never assumed: DWM decides and reports nothing, so
  PresentMon is the only confirmation. See `src/output_mode.h`.
- No per-application profiles yet. See [ROADMAP.md](ROADMAP.md), Milestone 3.

## Module map

Every file in `src/` and `tests/`, and what it is responsible for. This is the
fastest way to find the code that owns a behaviour you want to change.

| Path | What lives there |
| --- | --- |
| `src/adaptive_scheduler.*` | `FrameRate`, `OutputClock`, `SourceTimeline`, `TargetSlotGate`, `FrameSelector`, `PacingMonitor` — the target-owned timeline. Start here for pacing questions. `PacingMonitor` is measurement only: it reports the present-interval distribution and nothing reads it back into the loop. |
| `src/flow_scale.h` | The flow-grid divisor **and** `ResolveCoarseSearchRadius`, which resolves the coarse motion search from a reach target stated in *source pixels*. It divides that target by both the measured source period and the flow divisor, so reach covers a constant velocity rather than a constant displacement, and stays the same distance on every `--flow-scale` rather than halving when the grid gets finer. Header-only and dependency-free. |
| `src/frame_rate_limits.h` | Accepted multiplier (2–20) and target-FPS (24–1000) validators. Header-only. |
| `src/capture_session.*` | Windows WGC capture, compositor timestamps, async GPU duplicate signature. |
| `src/platform/pixel_frame.h` | Owned 0xAARRGGBB frame storage shared by portable capture, interpolation, presentation, and diagnostic code. |
| `src/platform/desktop_backend.*` | Portable capture/presentation boundary. `desktop_backend_x11.cpp` uses X11/Xext; `desktop_backend_macos.mm` uses CoreGraphics/Cocoa; the stub reports an explicit unavailable-backend error. |
| `src/platform/software_interpolator.*` | Dependency-free portable interpolation: bounded global translation, bilinear warps, scene-cut newest-frame fallback, explicit blend mode, and source-sized HUD-mask coverage. It is a functional CPU backend, not a claim of D3D11 quality or speed parity. |
| `src/renderer.*` | Output window, waitable flip-model swap chain, GPU frame history, blend shader, hands pairs to `MotionInterpolator`. |
| `src/gpu_timing.*` | Non-blocking D3D11 timestamp-query rings and rolling p50/p95 GPU timing statistics. |
| `src/present_mode.h` | `PresentMode` (vsync / tearing / automatic) plus its CLI spelling and parser. Header-only and dependency-free so `osss_gui.exe` can name a mode without acquiring a Direct3D dependency. |
| `src/pacing_mode.h` | `PacingMode` (unpaced / paced / queued): *when* a frame is rendered and handed over relative to the output clock, plus the two mechanism facts each mode implies -- maximum frame latency and whether the loop renders a slot ahead -- so the renderer and the loop cannot drift. Header-only and dependency-free for the same reason as `present_mode.h`. |
| `src/shader_cache.*` | Content-addressed on-disk HLSL bytecode cache and concurrent batch compilation. Startup was ~12 s of `D3DCompile` before this existed; it is ~0.3 s warm. |
| `src/motion_interpolator.*` | Bidirectional optical flow + fusion (inline HLSL). Flow runs once per pair in `PreparePair`, over a mip-chained luma pyramid built in the same call. |
| `src/png_writer.*` | Dependency-free 8-bit truecolour PNG encoding (fixed-Huffman deflate with a hash-chain match finder), base64, and an integer box downscale. It exists because every image this repo produced was a binary PPM, which nothing on a desktop opens -- so the dumps were generated and then not looked at. Header-only in spirit: no zlib, no image library. |
| `src/frame_sequence.*` | Writes a temporally ordered run of frames as PNGs plus `viewer.html`, a self-contained stepper/looper/blinker over them. Also owns `RenderErrorView` and `RenderErrorStepView` -- the latter is the flicker map, the only image here that shows a temporal artifact directly rather than by inference. |
| `src/ui_mask.*` | UI/HUD mask rectangle parsing, resolution against a source size, and rasterization. Backs `--ui-mask` and the launcher's HUD-mask field. |
| `src/upscaler.*` | Spatial upscaling of the **finished** frame, never the source: an edge-directed upsample (3x3 luma structure tensor picks the dominant gradient, a 12-tap kernel is measured in a space stretched along the edge) followed by a contrast-limited 5-tap sharpen that cannot overshoot the local min/max. `UpscaleMode` is `off` / `automatic` / `always`. FSR1-class in structure, reimplemented from published technique -- no vendored code. |
| `src/output_mode.h` | `OutputMode` (`overlay` / `fullscreen`): the *shape* of the output window, which decides whether a variable-refresh display can follow OSSS's presents or only DWM's. `overlay` is layered and click-through, so it can never reach independent flip; `fullscreen` gives up click-through to get there. Eligibility is reported, never assumed. Header-only and dependency-free. |
| `src/debug_view.h` | `DebugView` (`off` / `flow` / `confidence` / `fallback`): runtime visualisation of the interpolator's internals, replacing the frame rather than overlaying it. Exists because all seven motion defects in this project were found by hand-editing the fusion shader to return an intermediate as colour. `fallback` answers "why is this region not being interpolated" directly. Header-only. |
| `src/app_profile.*` | Per-application settings in `%LOCALAPPDATA%\OSSS\profiles.txt`, stored as **argument lists keyed by executable name** rather than a struct mirroring `Options`. That is the design: one parsing path, so a profile cannot express what the CLI rejects; nothing to keep in sync when a flag is added; and a file users can edit by hand. |
| `src/stats_overlay.*` | Topmost click-through HUD, `RuntimeStats`, and the header-only `GeneratedFrameShare` telemetry helper. |
| `src/window_catalog.*` | Target-window enumeration and display refresh at the common boundary; Win32, X11, and CoreGraphics implementations live beside it. |
| `src/dpi_awareness.h` | Per-monitor DPI opt-in, included by each executable. Header-only. |
| `src/main.cpp` / `src/portable_main.cpp` | `osss.exe`: platform-specific CLI parsing and production loop. Windows owns the D3D11 self/capture tests and `RunFrameGeneration`; portable builds own the `DesktopCapture`/`SoftwareInterpolator`/`DesktopPresenter` loop and software self-test. |
| `src/launcher_theme.*` | The launcher's colour tokens per theme, the system light/dark query, and `LauncherLayout` -- the y cursor every launcher control asks for its rect instead of naming coordinates. Pure and GPU-less, so the overlap rule is unit-tested. |
| `src/gui_main.cpp` | `osss_gui.exe`: Win32 settings launcher. Two-tier layout with an Advanced disclosure, a tooltip per option, and a status panel; every interactive control is owner-painted so the window follows the system light/dark theme. Builds the `osss.exe` command line and owns the `--stop-event` handshake. |
| `src/capture_smoke.*` | Real-desktop WGC integration checks used by the self-tests. |
| `src/test_animation_main.cpp`, `src/test_animation_backends.*`, `src/test_animation_catalog.h`, `src/test_pattern.*` | `osss_test_animation.exe`: deterministic D3D9Ex/10/11/12 source windows and reference scoring. See [TEST_ANIMATIONS.md](TEST_ANIMATIONS.md). |
| `src/osss.manifest` | The Win32 application manifest linked into every executable: per-monitor DPI v2, and Common Controls 6 for the launcher's tooltips and subclassing. |
| `tests/test_harness.h` | Shared `Require` / `RequireNear`. Every test uses these. |
| `tests/launcher_layout_tests.cpp` | `LauncherLayout` and the theme tokens: that a row's columns can never be made to overlap, and that both palettes are complete. The GPU-less half of what `osss_gui --self-test` checks on real controls. |
| `tests/window_catalog_tests.cpp` | `SelectWindowsMatching`: that an executable-name match outranks a title match, so a shell whose title is the command line cannot make an unambiguous `--title` ambiguous. Enumeration needs a desktop; the ranking does not. |
| `tests/png_writer_tests.cpp` | Round-trips `EncodePng` through a minimal inflater and PNG parser carried in the test. There is no library behind the encoder, so a decode is the only evidence that what it writes is a real PNG. |
| `tests/interpolation_quality_tests.cpp` | Reference-image quality bench: scores `MotionInterpolator` against the analytic test pattern, per lane, alongside the plain crossfade it has to beat. |
| `tests/` | One `*_tests.cpp` per module. Dependency-free; failures throw, `main` catches. |
| `handoffs/TEMPLATE/` | Skeleton + field docs for a new handoff package. Copy it; do not edit in place. |
| `handoffs/<date>-<topic>/` | Dated agent handoff packages with evidence. Historical: symbol names stay valid, **line numbers drift**. Read for intent and invariants, not as current-state truth. |

`src/` is intentionally flat, but it holds four separable groups: the **pipeline**
(`adaptive_scheduler`, `capture_session`, `motion_interpolator`, `renderer`,
`stats_overlay`, `ui_mask`, `window_catalog`, `main`), the **launcher**
(`gui_main`), the **test-animation harness** (`test_animation_*`, `test_pattern`),
and **smoke/self-test support** (`capture_smoke`). Put a new file in the group it
belongs to and keep the name prefix consistent; a directory split can happen
later without renaming anything.

Shaders are raw-string literals inside the `.cpp` files — grep for
`ShaderSource[] = R"` to find all of them. They compile at runtime, so an HLSL
error is a runtime `InterpolatorError`, not a build failure; `ctest` will not
catch it and `osss.exe --self-test` will.
