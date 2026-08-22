# DirectX test animations

OSSS ships a deterministic source-window harness for testing dynamic frame-rate
targets. It presents the same CPU-generated pixels through four source APIs:

- Direct3D 9Ex
- Direct3D 10
- Direct3D 11
- Direct3D 12

Each API is available at 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, and
120 FPS. These are source-compatibility modes; OSSS's capture and interpolation
pipeline remains Direct3D 11.

## Launch from the Frame Generation UI

Open `osss_gui.exe`, then choose:

```text
Dev > Test animation > <Direct3D API> > <base FPS>
```

The launcher starts the test source, refreshes the target list, and selects its
window when Windows has made it available. Choose the OSSS output target and
maximum interpolation ceiling, then select **Start**.

The test window repeats a six-second cycle until it is closed:

- `S` captures the visible client area, aligns it against the deterministic
  ground truth, and writes a score report.
- `B` captures a burst of consecutive frames (default 10, 0.618 of a source
  frame apart) while the source keeps presenting, then scores each one and
  writes a sequence report. See "Burst scoring" below.
- `O` opens the current evidence folder.
- `R` resets the animation clock.
- `Space` pauses or resumes.
- `Esc` closes the source window.

Evidence is written under `%TEMP%\OSSS\test-captures\<session>`. Each scored
snapshot contains the observed frame, the best-aligned expected frame, and a
JSON report. The window caption displays the latest PSNR and temporal offset.

## Why these patterns

Frame-interpolation benchmarks such as the
[Middlebury optical-flow evaluation](https://vision.middlebury.edu/flow/eval/)
compare synthesized intermediate frames with held-back ground truth. Real
footage is useful for subjective review, but it is a poor first diagnostic when
the exact intermediate image is unknown. OSSS therefore uses an analytically
rendered, continuous-time scene where an expected image can be generated for
any sub-frame timestamp.

The cycle contains four deliberately different failure surfaces:

1. **Constant-velocity translation** — a large, high-contrast object with a
   thin attached edge exposes double images, incorrect motion magnitude, and
   non-linear alpha placement.
2. **Occlusion and disocclusion** — patterned objects pass behind a fixed
   foreground block, exposing invalid pixels and reveal smearing.
3. **Thin high-frequency detail** — a translating sinusoidal grating and
   opposing narrow markers expose phase error, lost detail, and inconsistent
   bidirectional flow.
4. **Hard scene cut** — the palette and most of the frame change halfway
   through the cycle, making cross-scene blending objectively obvious.

The top 64 pixels are a machine-readable header. It identifies the API, base
rate, source-frame sequence, cycle phase, and reset pulse. Image scoring starts
at row 112 so neither those bits nor OSSS's default upper-left diagnostics HUD
can dominate the content result.

## HUD overlay (UI-mask fixture)

`--hud-overlay` adds two static-position panels over the moving lanes:

```powershell
.\out\release\osss_test_animation.exe --api d3d11 --fps 60 --hud-overlay
```

Each panel is a row of cells driven by the source frame index alone, so it steps
at source cadence and never takes an intermediate value. That makes it a direct
test of OSSS's UI masking: a masked panel equals exactly one real source frame,
while an interpolated one cross-fades into a colour no source frame contained.
The startup output prints the matching `--ui-mask` argument, and the panels are
also what the automatic detector is expected to find on its own.

Because a masked panel deliberately shows a different source frame than the
interpolated content around it, the panels are excluded from the pixel metrics
and from the temporal search, then reported separately:

```json
"hud_overlay": {
  "excluded_from_pixel_metrics": true,
  "holds_one_real_source_frame": true,
  "matched_source_frame_index": 91,
  "maximum_channel_error_to_nearest_state": 0
}
```

`holds_one_real_source_frame` is the result that matters. The window caption
shows the same verdict as `HUD clean` or `HUD interpolated`. The overlay is off
by default, so historical references and scores are unchanged.

## Reading a score

Before comparing pixels, the scorer searches from 250 ms behind the current
source clock through 25 ms ahead, then refines the best match to 0.25 ms. This
keeps image error separate from presentation delay:

- `temporal_offset_ms` is the best expected-frame time minus snapshot time. A
  negative value means the visible frame trails the source clock.
- `mae` and `rmse` are per-channel 8-bit pixel errors; lower is better.
- `psnr_db` is derived from RMSE; higher is better. An exact match is emitted
  as `null` in JSON because mathematical PSNR is infinite.
- `pixels_over_8_percent` is the share of scored pixels where at least one
  channel differs by more than 8/255.
- `maximum_channel_error` exposes isolated severe artifacts that averages can
  hide.

Do not invent a universal pass threshold from one run. Hold API, source rate,
target rate, multiplier ceiling, resolution, renderer, display state, and
capture method fixed; establish a baseline; then compare changes against that
baseline. Pixel scores measure the best-aligned visible image. They do not
measure source/present cadence or prove that a physical monitor displayed every
submission.

For frame-time and presentation-mode evidence, use DXGI statistics or a tool
such as [Intel PresentMon](https://github.com/Intel-PresentMon). Microsoft notes
that flip-model presentation statistics distinguish submitted presents from
display timing and can become disjoint across mode changes; see the
[DXGI flip-model documentation](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model).

## Burst scoring

A single snapshot cannot show judder, duplicated frames, or ghosting that only
appears on interpolated frames. Burst mode captures `N` consecutive desktop
frames at a fixed interval, interleaved with source presentation so the source
cadence OSSS observes is unchanged, and scores them all afterwards:

```powershell
.\out\release\osss_test_animation.exe --api d3d11 --fps 60 `
  --burst 10 --burst-at-ms 1500 --exit-after-burst
```

`--burst-at-ms` schedules the burst on the animation clock; without it, press
`B`. Output is `burst-<n>.json` plus
`burst-<n>-frame-<i>-{observed,expected}` as both a `.ppm` and a `.png` of the
same pixels, and a one-line summary on standard output. The PPM is what
`--compare` reads; the PNG is the one that opens.

The interval defaults to 0.618 of a source frame period (`--burst-interval-ms`
overrides it). That is the golden-ratio conjugate, chosen because successive
captures then land at sub-frame phases that never repeat and spread as evenly
as any sequence can, so a burst visits every generated-frame slot whatever the
multiplier. A simple fraction of the period aliases against the output cadence
instead: at half a period against a 4x output, every capture lands on one of
the same two alphas and the other two are never seen. Whatever the interval,
you cannot observe faster than the desktop composes — on a 60 Hz display a burst
sees at most 60 unique frames a second however fast OSSS presents, and the rest
come back `identical_to_previous`.

Beyond the per-snapshot fields, each frame row carries:

- `identical_to_previous` — the desktop had not changed since the previous
  capture. `unique_frames / capture_span_ms` therefore bounds the *observable*
  output rate; it does not prove the monitor showed every present.
- `capture_cost_ms` — how long the BitBlt took; if this approaches the
  interval, the burst is capture-bound and the sampling is not uniform. One
  throwaway capture is taken before the burst so frame 0 does not absorb GDI
  setup cost and report a false latency spike.
- `source_phase_ms` and `classification` — where the best-matched instant falls
  against the source frame grid, and the verdict for that row: `real`,
  `generated`, or `repeat` (identical to the previous capture). The source
  renders each frame for exactly `k / base_fps`, so a real frame — captured
  from the source, or held or re-presented by OSSS — matches within
  `real_frame_phase_tolerance_ms` (0.5 ms) of a grid line, while a generated
  frame matches strictly between two. This is the burst's answer to a question
  per-frame PSNR cannot ask: a real frame is a perfect match at *some* instant,
  so a session that never generated anything scores as well as one that did.

Captures are cheap, but scoring is not: each frame runs an independent temporal
search over the analytic pattern. The searches run concurrently, and
`summary.scoring_ms` reports how long the pass took — roughly 1.2 s for ten
frames on a 960x540 pattern. The source stops presenting for that time, so the
animation clock jumps forward once per burst. Use `--exit-after-burst` for
scripted single-burst runs, and expect a visible pause when pressing `B`
repeatedly.

The `summary` block reports PSNR and temporal-offset ranges, the ground-truth
time step between unique frames (`expected_step_ms_*`), and `backward_steps`,
the count of unique frames that showed an *earlier* moment than the frame
before them. Steady offsets and uniform steps mean smooth pacing; a large step
range means judder; any backward step means a frame was shown out of order or a
stale frame was re-presented.

`verdict` is `occluded` before it is anything else. The scorer reads the
*screen*, which is what lets it measure any generator without its cooperation,
and the cost is that it will just as happily score a window parked on top of the
target. That failure does not look like a failure: an unrelated image still
matches the temporal search at some instant, so its frames classify as
*generated* and the run resembles a good one. A measured example -- a burst
taken while OSSS had failed to start -- reported 19 of 24 frames generated, 9
backward steps, and `generated-frames-observed`, entirely from capturing a
console window at 11 dB. Every capture is now hit-tested on a 5x5 grid; a
foreign window at any sample point sets `occluded` on that frame, and one
occluded frame makes the whole burst's verdict `occluded`, with `occluded_by`
naming the executable and a warning printed under the summary line. A frame
generator's overlay does not trip it: OSSS's output window is click-through and
`WindowFromPoint` skips those by design, so the overlay being measured is never
mistaken for a window in the way. Verified both directions -- a decoy parked
over the target scores 9.5 dB and is refused, while a clean OSSS session over an
unobstructed target reports `generated-frames-observed` with no occlusion. The
single-frame `S` snapshot refuses to score for the same reason rather than
writing a plausible report.

It also counts the classifications above over the unique frames —
`generated_frames_observed` and `real_frames_observed` — and states a
`verdict`: `generated-frames-observed`, `source-frames-only`, or `inconclusive`
(a single unique frame, meaning the desktop never changed during the burst).
Read the verdict before the PSNR. `source-frames-only` with a OSSS output window
over the test means OSSS was holding or repeating real frames, or the capture
was seeing the source underneath rather than the overlay; either way the pixel
scores are not measuring interpolation.

A recorded baseline for one fixed configuration, native and through OSSS, is in
[handoffs/2026-08-15-burst-baseline](../handoffs/2026-08-15-burst-baseline/HANDOFF.md).
Compare a new run against it under the same configuration rather than reading
its numbers as absolute.

## Reference-image quality bench

The harness above scores what a real display path actually produced, which means
it also measures capture, pacing, and presentation. `osss_interpolation_quality_tests`
answers the narrower question -- *how good is the interpolator itself* -- with no
window, no capture, and no clock:

```powershell
.\out\release\osss_interpolation_quality_tests.exe --report
```

It renders the two bracketing source frames for a source rate and instant, asks
`MotionInterpolator` to reconstruct alphas 1/6, 1/2 and 5/6, and scores each
against `RenderTestPattern` evaluated at the exact intermediate time. Because the
pattern is analytic in continuous time, the ground truth is known rather than
approximated. The matrix is three source rates (60, 30, 20 FPS) across eight
start times, each inside one half of the cycle so the deliberate scene cut is not
under test.

Three things about the output matter more than the PSNR column:

- **Scores are per lane.** Translation, occlusion, and thin high-frequency
  detail fail differently, and one whole-frame number lets a regression in one
  hide behind a gain in another.
- **Every lane is scored twice**, once for the interpolator and once for the
  plain temporal crossfade of the same pair. The `gain` column is the difference.
  An interpolator that has silently stopped doing motion compensation still
  posts a respectable absolute PSNR; only the gain column shows it. That is not
  hypothetical -- it is how a tie-break bug in the coarse estimator went
  unnoticed while every other test passed.
- **`flow-prepare` and `fuse` are different budgets.** Flow runs once per source
  pair, so it has a source frame period to fit in (16.7 ms at 60 FPS). Fusion
  runs once per generated frame and has an output frame period (4.2 ms at a
  240 FPS target). Work moved between the two is not free even when the total
  looks unchanged.

Useful flags: `--report` for the full per-sample table, `--dump <dir>` for
observed/expected images (a PPM and a PNG of the same pixels; the PPM is what
`--compare` above reads, the PNG is what opens), `--dump-sequence <dir>` for the
consecutive runs and a viewer over them (below), and `--size WxH` to re-measure
at a production resolution.

### Looking at consecutive frames

Everything above is either one frame in isolation or a scalar averaged over a
whole run. Neither can answer what a flicker complaint asks -- *which* frame and
*where* -- so `--dump-sequence <dir>` writes the two runs in this bench that are
ordered in time:

```powershell
.\out\release\osss_interpolation_quality_tests.exe --dump-sequence out\seq
```

That is 44 frames in three sequences -- the 24-frame temporal run and the reach
ramp with the temporal prior on and off -- at eight views each: `observed`,
`expected`, `crossfade`, `error`, `error-step`, and the three `--debug-view`
renders (`flow`, `confidence`, `fallback`). About 52 MB of PNGs and roughly a
minute on top of the normal run; the reach ramp is 47 MB of that, because a
dead-leaves scene does not compress and the 24-frame temporal run does.

Two of those views do not exist anywhere else:

- **`error`** is the signed luma error as a diverging ramp: red where the frame
  is brighter than the truth, blue where it is darker. Ghosting shows as a
  red/blue pair straddling a moving edge, which an absolute-error map cannot
  tell from ordinary softness.
- **`error-step`** is the flicker map: how much the error changed since the
  previous frame of the run. A frame that is softly wrong in a *steady* way is
  nearly invisible in motion and prints black here. The same average error
  appearing and disappearing every frame is what reads as shimmer, and prints
  bright. It is the only image in this repo that shows a temporal artifact
  directly rather than by inference.

`viewer.html` lands in the same directory: step, loop, and play the run at a
chosen rate, A/B-blink any two views against each other, a contact sheet, and a
per-frame error-step readout with the coordinate of the worst pixel. Open it
from disk; the images sit beside it. `--dump-embed <divisor>` additionally
writes `viewer-embedded.html` with every image inlined as a data URI and
box-downscaled by that divisor, for a single file that can be moved or shared
(divisor 2 over the full dump is about 32 MB, so pick 4 if that matters).

`--report` prints the same per-frame numbers as a table, and the worst single
step per lane is printed unconditionally. That table is what first showed the
detail lane roughly doubling its error-step on the first output frame of every
source pair -- a pulse at source cadence that the run-wide mean of 2.28 luma
levels had been averaging flat.

The thresholds it enforces are regression floors measured on one machine, not
portable quality claims. Establish your own baseline before reading a change into
them. Per lane they are: a mean and a worst-case PSNR floor, a minimum mean gain
over the crossfade, and a mean and worst-case ceiling on the share of bad pixels
(any channel more than 8/255 off). The bad-pixel ceilings gate a failure PSNR is
blind to: PSNR is a mean, so a small region that is badly wrong — a halo, a torn
edge, a wobbling occlusion boundary — costs it little while being the most
visible thing in the frame. The detail lane's bad-pixel ceilings are set only to
catch it getting worse; its figures are the aliasing limit described below, not a
defect.

### Temporal consistency

A second section scores a continuous run of generated frames across *consecutive*
source pairs at uniform output spacing, rather than isolated pairs. Crossing pair
boundaries is the point: the flow field is recomputed at every source frame, and
a discontinuity there is a pulse at source cadence.

`error-step` is the mean frame-to-frame change of the per-pixel error signal, in
8-bit luma. Differencing the *error* rather than the frames removes the scene's
own motion, which is supposed to change, and leaves only how much the
interpolator changed its mind. It is reported next to the same figure for the
crossfade.

It is a ceiling, not a quality bar -- a crossfade is perfectly smooth and still
wrong -- but it catches a whole class of defect that no single-frame PSNR can
see: output that is individually plausible and collectively restless. A steady
half-percent softness is nearly invisible; the same average error appearing and
disappearing every frame reads as shimmer.

Two ceilings are enforced. The absolute one (6.0 luma levels per frame) is a
backstop. The load-bearing one is the `ratio` column: the interpolator's
error-step may be at most 0.75 of the crossfade's on the same frames. It was
0.45 / 0.53 / 0.41 across the lanes when written, and the ratio travels between
GPUs and resolutions far better than the absolute figures do. An interpolator
that shimmers more than a plain blend has lost the argument for its own
existence, and until this gate existed a change that doubled the flicker on the
linear lane would have passed with room to spare.

### What it cannot measure

The detail lane's grating has a six-pixel period. At 30 FPS the source moves
3.03 pixels per frame -- more than half a period -- and at 20 FPS, 4.55. Past
half a period a repeating texture is genuinely ambiguous: the same pixels are
explained equally well by a smaller displacement the other way, and no local
method distinguishes them. OSSS resolves the tie toward the smaller displacement,
which is right at 60 FPS and wrong below it, so the lane scores near the
crossfade overall while running ahead of it at 60 FPS. Treat that lane as a
sensitivity probe, not a target to optimise.

## Command-line references

Launch a source directly:

```powershell
.\out\release\osss_test_animation.exe --api d3d12 --fps 70
```

Export the analytically correct frame at 1.25 seconds:

```powershell
.\out\release\osss_test_animation.exe --api d3d12 --fps 70 `
  --time-ms 1250 --export-reference expected.ppm
```

Compare a P6 PPM screenshot around that time, searching up to 250 ms backward:

```powershell
.\out\release\osss_test_animation.exe --api d3d12 --fps 70 `
  --time-ms 1250 --search-ms 250 --compare observed.ppm
```

The comparison command writes one JSON object to standard output, which makes
it convenient to collect a fixed API/rate/target matrix in a script.

Two further flags apply to any mode:

- `--duration-seconds <seconds>` closes the window automatically once that much
  animation has elapsed. The default of `0` runs until the window is closed.
  Use it for unattended capture runs.
- `--self-test` runs the built-in reference checks and exits without creating a
  window. It is not a CTest; `osss_test_pattern_tests` covers the same pattern
  maths under `ctest`.

## Capture caveats

The interactive `S` path reads the composited desktop pixels covering the test
window. Keep the test unobscured. Some independent-flip, hardware-overlay,
remote-desktop, HDR, or protected-content paths may not be visible to a GDI
desktop capture. `osss_frame_output_visible` reports whether a OSSS output
window overlapped the test at capture time; it does not prove that the returned
pixels came from that surface. Use an external Windows Graphics Capture or
camera workflow when validating those display paths.


## Scoring another frame generator against the same truth

The burst scorer does not know or care which program drew the pixels it reads.
`CaptureClientPixels` takes its bitmap from the **screen** DC, so it captures
whatever is composited at the capture rectangle: the source window when nothing
covers it, OSSS's overlay when OSSS is running, and a third-party frame
generator's output when that is what is on top. Everything downstream — the
temporal best-fit search, the real-versus-generated classification, the metrics,
the JSON — is identical either way.

That makes a genuine head-to-head possible, because the reference is analytic
rather than a recording of one of the contestants. Neither tool gets to define
"correct".

### Procedure

1. Start the source and let it settle:

```powershell
.\out\release\osss_test_animation.exe --api d3d11 --fps 60
```

2. Point the frame generator at that window and start it.

3. Run a second copy of the harness in burst mode over the same source, with
   `--capture-origin` set to where the generator actually draws. Read that
   position off the generator's output window; it is only the source window's
   client origin when the generator draws over the source at 1:1, which a
   fullscreen scaler does not.

```powershell
.\out\release\osss_test_animation.exe --api d3d11 --fps 60 --burst 60 --burst-at-ms 4000 --exit-after-burst --capture-origin 0,0
```

4. Repeat with OSSS in place of the other generator, at the same source rate,
   the same target rate, and the same multiplier. Compare the two JSON summaries.

### What to compare, and what not to

`generated_frames_observed` against `real_frames_observed` is the first thing to
check and the easiest to get wrong. A tool that holds or re-presents a real frame
produces a capture that best-matches *on* the source frame grid, and the scorer
classifies it as real. A tool claiming a 4x multiplier that shows three real
frames for every generated one is not doing what its setting says, and this is
where that shows up — before any quality number is worth reading.

Only then compare the quality metrics, and only across runs with the same
`generated_frames_observed` share. Mean PSNR over a burst where one tool
generated half as many frames is not a comparison.

### Known limits

- **The capture is a screen grab.** Anything drawn over the region during the
  burst — a notification, a cursor, another overlay — lands in the score. Run it
  on an idle desktop.
- **Scaling must be off.** The scorer compares against a pattern rendered at the
  source resolution, so an upscaled output does not align. Frame generation only.
- **Per-lane numbers are in the JSON, under `summary.lanes`.** Each of
  `linear`, `occlusion`, and `detail` carries `generated_psnr_db`,
  `generated_bad_percent`, `real_psnr_db`, and the frame counts behind them. The
  generated/real split is the one to read: real frames passing through at native
  fidelity says the capture and present path are clean, and only the generated
  ones measure the interpolator. The bands come from `kTestPatternLanes` in
  `src/test_pattern.h`, which the reference-image bench also uses, so the two
  cannot drift apart and their numbers stay comparable.
