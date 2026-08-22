# OSSS frame-generation roadmap

## Milestone 0: pipeline baseline — complete

The current executable establishes the pieces that every later interpolator
needs:

1. Select a visible window by title or handle.
2. Capture its compositor surface into Direct3D 11 textures.
3. Maintain a bounded timestamped history of unique source frames in GPU memory.
4. Run a fixed rational target clock, defaulting to the active display path,
   while enforcing a selectable 2x through 20x maximum.
5. Select a known timestamp bracket and fractional alpha for each admitted
   target slot.
6. Render into a click-through, latency-one, waitable flip-model output window.
7. Skip stale positions rather than queueing them and increasing latency.

The baseline shader crossfades the two source frames. That validates resource
lifetime, target/ceiling semantics, resize recovery, and presentation. It does
not claim motion reconstruction quality.

## Milestone 1: motion-aware local prototype — initial backend complete

The first real interpolator remains dependency-free and executes as a Direct3D
11 compute/pixel pipeline:

1. Sample source-frame luma at coarse and fine search resolutions.
2. Estimate coarse-to-fine forward and backward motion fields in HLSL.
3. Reject inconsistent vectors with a forward/backward confidence test.
4. Warp both source frames toward an arbitrary interpolation time `t`.
5. Fuse the warped frames using confidence and simple occlusion masks.
6. Detect scene cuts and fall back to the newest real frame.

Flow should be computed once per source pair. The same fields can then generate
`t = 1/6, 2/6, ... 5/6`; 6x must not mean running motion estimation five times.

Completed acceptance evidence:

- A deterministic 16-pixel translation reconstructs the midpoint more strongly
  than either blend trail.
- A deterministic black-to-white scene cut returns the newest frame rather than
  blending unrelated images.
- Rate-limit tests cover every accepted multiplier from 2x through 20x and the
  24-1000 FPS target range.
- Historical target-count tests (carried into `osss_adaptive_scheduler_tests`
  from the retired per-pair pacer) cover 60->120, 60->240, 120->240, 200->240,
  native cadence above target, and a multiplier-limited 30->180 path.
- Additive scheduler tests cover 60->144, 50->144, 80->144, four source/output
  phase offsets, a deterministic fluctuating 55-70 FPS source, callback jitter,
  duplicates, cold start, underrun, resize discontinuity, bounded ceiling slots,
  lateness, and stall recovery.
- Motion estimation runs once in `PreparePair`; all generated positions reuse
  the resulting forward and backward fields.

Closed since: reference-image quality measurement, and the correctness work it
immediately exposed.

`osss_interpolation_quality_tests` scores the interpolator against the analytic
test pattern at alphas 1/6, 1/2 and 5/6, for three source rates and eight
instants, per lane, alongside the plain crossfade it has to beat. Standing that
up showed the motion path was **inert**: output matched a crossfade to within
0.2 dB on every lane at every rate. Four defects, in the order they were found:

1. `EstimateCoarse` selected on raw match error. Uniform regions tie across the
   whole search window, so ties went to the first candidate scanned -- the far
   corner, a 45-pixel vector. Forward and backward landed on opposite corners,
   the consistency check rejected the pair, and the fused weight fell below
   `confidence_floor` for ~99% of pixels. Fixed with a regularized selection
   cost that prefers the smallest displacement explaining the pixels.
2. The flow field was quantised to the fine search step -- two pixels, larger
   than the 1.5-3.8 pixels per frame the pattern actually moves at 60 FPS. Fixed
   with successive halving to a quarter pixel and a parabolic vertex fit.
3. The coarse level point-sampled full-resolution pixels on its own eight-pixel
   grid, so any detail finer than the step aliased and stranded the fine
   window a full period away. Fixed with a mip-chained luma pyramid; each level
   now matches on a band-limited image.
4. The fusion sampled flow at the output position rather than the source
   position, misplacing every warp by roughly `t` times the local flow gradient.
   Fixed with two fixed-point iterations.

A second pass, driven by a per-column error profile showing that *all* remaining
error in the two structural lanes sat at moving-object edges, found two more:

5. Static-pixel protection compared the two frames at a fixed position, which is
   a no-motion test. An object narrower than its own displacement leaves the
   pixels it is about to cross unchanged in both source frames, so the test
   concluded nothing had moved and crossfaded the object away at exactly the
   position it should have been drawn. Both warps had the pattern's three-pixel
   marker correct to the byte; the protection discarded them, turning a
   maximum channel error of 8 into 78. Now gated on confidence-weighted flow
   magnitude, so a pixel counts as static only when the frames agree *and* the
   motion field says nothing passed through it.
6. Both warps reconstructed with plain bilinear sampling, a two-tap box filter
   at the fractional position a warp lands on. The same marker came back five
   pixels wide at about 60% contrast, and the fusion applied that softening once
   per side before averaging. Replaced with a five-fetch cardinal spline
   (`kWarpCardinal`, softened from Catmull-Rom to trade a little sharpness for
   proportionally less edge ringing).

A third pass added temporal measurement -- scoring a continuous run of frames
across consecutive source pairs rather than isolated pairs -- and found one more:

7. The fusion solved for each pixel's source position by fixed-point iteration.
   That map is a contraction only where `t * |grad flow|` is under 1, and where
   it is not -- motion boundaries, which is what the correction was for -- it
   oscillated instead of converging. The failure alternated, which is why it
   survived: two steps landed back near where none started and looked correct,
   while one and three steps each cost about 2.5 dB. Damping it into a genuine
   contraction converged on an answer *worse* than no correction at all, because
   at an occlusion boundary the true fixed point lies in the region one frame
   never saw. Removed; the plain backward warp is cheaper, stable, and measured
   equal or better on every lane. A flow field resampled to time t by forward
   splatting is the real answer and is a separate pass, not a smarter sample.

Also added: a vector-median outlier filter on the finished fine flow field,
replacing only cells that at most one of their eight neighbours agrees with, so
genuine motion boundaries survive. Worth +0.3 dB on occlusion. Note it feeds
fusion but deliberately *not* the automatic UI-mask detector: the detector takes
a maximum over sparse ring probes and the filter removes exactly the isolated
strong vectors those probes land on, which starved it below its arming
threshold.

Three ways of detecting the periodic-aliasing case and falling back to a
crossfade were also built and measured -- vetoing when the two searches
disagree, when they disagree at equal magnitude, and the same test applied to a
separately refined runner-up. All three lost more across the ordinary lanes than
the aliased case returned, the last at seven times the GPU cost. The negative
result is recorded in `RefineFlow` so it is not rediscovered.

Measured on an RTX 5090 at 960x540, mean PSNR against analytic ground truth,
with the same pair's crossfade for reference:

| Lane | Before | After | Crossfade | Gain | Bad pixels before/after |
| --- | --- | --- | --- | --- | --- |
| Linear translation | 33.45 dB | 39.72 dB | 33.54 dB | +6.18 dB | 2.46% -> 1.09% |
| Occlusion | 29.44 dB | 35.69 dB | 29.56 dB | +6.13 dB | 7.90% -> 5.33% |
| Thin detail | 21.08 dB | 21.29 dB | 21.24 dB | +0.05 dB | unchanged |

The same matrix at 1920x1080: 41.98 dB linear (+5.98 over crossfade), 37.22 dB
occlusion (+5.71), 22.09 dB detail.

Frame-to-frame instability, mean change of the per-pixel error in 8-bit luma
over a continuous run at 4x output, against the same figure for the crossfade:
0.14 vs 0.30 linear, 0.76 vs 1.43 occlusion, 2.28 vs 5.52 detail. The
interpolator is more temporally settled than the crossfade on every lane.

The detail lane is at an aliasing limit below 60 FPS and is discussed in
[docs/TEST_ANIMATIONS.md](TEST_ANIMATIONS.md#what-it-cannot-measure); at 60 FPS
it reconstructs the grating to within 0.006 pixels of the correct phase, against
3.055 pixels -- half a period -- before. Those low rates hold the lane mean down
however good the rest is.

Cost on the same GPU, measured with an event-query fence: flow preparation
0.44 ms per source pair at 960x540 and 0.49 ms at 1080p; fusion 0.15 ms and
0.22 ms per *output* frame. The two budgets are different -- flow runs once per
source pair (16.7 ms at a 60 FPS source), fusion once per generated frame
(4.2 ms at a 240 FPS target) -- so a change that moves work between them is not
free even when the total is unchanged. Single-GPU figures on a desktop that was
not otherwise idle.

Fixing the flow also exposed that the automatic UI-mask detector's
"neighbourhood is moving" gate had never functioned: it was reading a field that
held the same large bogus vector everywhere, so the gate was true at all times.
Against a correct and therefore sparse flow field it needed a decaying motion
memory to accumulate at all.

A finer flow grid was also built and measured: two source pixels per cell
instead of four. On the hard cases it is markedly better -- worst-case occlusion
PSNR up 4.4 dB, bad pixels down a quarter, thin-detail flicker down 17% -- but
isolating the variables showed the gain came from the smaller matching patch
that fell out of halving the grid, not from the finer grid itself. Decoupling
them (finer grid, patch held at two pixels) recovered the large-object quality
and gave back most of the hard-case gain. A one-pixel patch radius is also the
configuration most exposed to sensor and compression noise, which a noise-free
analytic pattern cannot test, so it was not adopted. The knobs are in
`CreateResources` and the bench measures it; revisit against captured game
frames.

A first end-to-end reference for the live path exists as well: the burst
harness through `osss.exe` at 60→240, 4x, D3D11, on the same GPU, recorded in
[handoffs/2026-08-15-burst-baseline](../handoffs/2026-08-15-burst-baseline/HANDOFF.md).
Generated frames scored 36.1 dB mean with 1.2 % bad pixels for the motion
interpolator against 32.7 dB and 13.3 % for the plain blend, real frames passed
through at native fidelity (≥ 53 dB), and the burst's real/generated verdict
agreed with `osss.exe`'s own generated-share telemetry. Its native leg still
needs re-recording on an idle desktop.

### Output shape, ceiling, and upscaling, 2026-08-16

Three parity gaps against external frame generators closed together.

**`--output-mode overlay|fullscreen`.** Variable refresh is now reachable, in
one shape only. The overlay must stay layered to pass clicks to another
process's window, and a layered window is composed through a redirection
surface whatever the swap chain asks for -- so overlay mode can never be
promoted to independent flip, and `--present-mode tearing` there removes only
OSSS's own vblank quantization. Fullscreen mode drops the layered style for an
opaque monitor-sized window DWM can promote, which is what G-Sync and FreeSync
need in order to follow the output clock. It gives up click-through to get
there, and disables the HUD, because a second topmost window silently demotes
the output back to composed. `PresentModeStatus::independent_flip_eligible` is
now computed rather than hard-coded false, and `osss.exe --self-test` asserts
the window shape per mode -- fullscreen must not carry `WS_EX_LAYERED`.

**Multiplier ceiling 6 -> 20.** A policy limit, not an architectural one: flow
is estimated once per source pair and every generated position reuses it, so
the Nth frame of a pair costs one fusion pass and per-output-frame cost falls
as the multiplier rises. Fusion is 0.09-0.29 ms per output frame at 540p-1080p
and 0.41-0.43 ms at 2160p, against the 1.67 ms output period of 600 FPS, which
is 20x from a 30 FPS source. What does not scale is the motion model: alphas
stay in [0, 1], but at 20x from 30 FPS a linear model holds across 33 ms and
every artifact stays on screen proportionally longer. The default stays 6x.

**`--upscale` and `--sharpness`.** An edge-directed 12-tap upsample steered by
a 3x3 luma structure tensor, then a contrast-limited sharpener. It runs on the
fused frame, never the source, so flow keeps estimating on native captured
pixels -- an upscaler invents plausible detail, and a block matcher given
invented detail returns confident matches for structure that was never there.
Measured by `osss_upscaler_tests` as a downscale/upscale round trip against the
original, with bilinear as the baseline: +0.51 dB mean, +1.48 dB best instant,
-0.60 dB worst, and 47.5 dB on a 1:1 pass with sharpening off.

Two negative results are recorded in `src/upscaler.cpp` so they are not
rediscovered. A degenerate eigenvector -- `(difference + root, 2*gxy)` collapses
to exactly `(0, 0)` when `gxy` is zero and `gyy` exceeds `gxx`, which is a plain
vertical edge -- produced NaN through every tap weight and scored **-9.58 dB**
against bilinear until the dual-form fix. And a radial Catmull-Rom, added on the
reasoning that reconstruction wants a negative lobe, measured *worse* (-0.12 dB)
than the positive windowed kernel: separable Catmull-Rom earns its lobe because
each 1D pass sums to one, and scattering the same lobe radially over a 4x4 grid
does not.

Also added: `osss_test_animation.exe --capture-origin X,Y`, which lets the burst
scorer capture from an arbitrary screen position. `CaptureClientPixels` reads
the *screen* DC, so it already captured whatever was composited over the source
window; the only thing pinning it to OSSS was the assumption that the output is
drawn at the source window's client origin. A third-party frame generator
scoring against the same analytic truth is now a runbook rather than a project.
See docs/TEST_ANIMATIONS.md.

Per-lane burst metrics are now in as well. `summary.lanes` in the burst JSON
carries generated and real PSNR plus a bad-pixel share for each of the three
lanes, split by frame class, and both benches read the lane bounds from
`kTestPatternLanes` in `src/test_pattern.h` so they cannot drift. That is what
makes a head-to-head against another frame generator diagnostic rather than
merely quantitative: a whole-frame mean says two tools differ, and the lanes say
how.

Still open from this work: a VRR range query, an in-swap-chain HUD so fullscreen
can show stats, PresentMon confirmation of `Hardware: Independent Flip` on a
physical variable-refresh display, and the Lossless Scaling head-to-head run
itself, which needs an idle interactive desktop.

### Coarse search scaled by source period, 2026-08-16

**Open gate: not yet measured against a real game.** The bench runs at a fixed
60 FPS source, which is exactly the rate this change is defined to leave alone,
so `ctest` and the quality bench confirm no regression and can say nothing about
the intended gain. Closing this needs a capture at 30 and at 60 of content that
pans fast enough to outrun the old reach.

The coarse search radius was a constant: 4 cells, so 4 * 8 = 32 source pixels of
reach at 1080p. That is a bound on *displacement* between two frames, but the
quantity a scene actually holds fixed is *velocity*, and the two differ by the
source period. At 60 FPS and Minecraft's default FOV, 32 pixels is about 1.7
degrees per frame -- roughly 100 degrees per second of mouse-look, which
ordinary play exceeds constantly. Past the reach the fine level cannot recover
(it refines only a few pixels around the coarse result), confidence collapses,
and fusion crossfades. At 4x that reads as smearing on every turn.

The radius now resolves per pair from `SourceTimeline::EstimatedSourcePeriod`,
with 60 FPS on a divisor of 8 pinned to the historical 4. So 30 FPS searches 8
and 120 searches 2, and the reach is one velocity across the range instead of
varying by a factor of four across it.

**Follow-up, 2026-08-16: reach is now also independent of `--flow-scale`.** The
radius above was counted in flow *cells*, which meant a second, unintended knob
moved the same contract: a finer grid halved reach, so `--flow-scale quality`
reached 32 source pixels where `auto` above 1440p reached 64. The user-visible
effect was that the setting you pick for accuracy quietly cost you the range
that decides whether a fast pan survives. `ResolveCoarseSearchRadius` now takes
the divisor and works back from a target in source pixels (64 at 60 FPS, ceiling
128), so every flow scale reaches equally far and a divisor of 8 is unchanged.

Measured at 2752x2064 with `--report`, this recovers the range `quality` was
giving up while keeping all of its accuracy: the detail lane holds at 22.53 dB
(against 18.03 for a divisor of 8), and the reach ramp no longer falls *below*
the crossfade past 56 px — 14.57 dB against 13.92 at 56 and 14.07 against 13.69
at 64, where before the fix it scored 13.85 and 13.55. Every gate passes at that
resolution, which the default divisor of 8 still fails.

The cost was measured this time, in Factorio at 2752x2064 and a 60 FPS source:
flow GPU time 0.2 ms to 0.4 ms, board power 85.4 W to 88.7 W — **+3.3 W** for a
4x finer grid at identical reach. The 30 FPS case is still untimed, and it is
the expensive one: holding 128 px of reach on a divisor of 4 needs radius 16,
which is 1089 comparisons per cell against 289. Time it before raising the
ceiling.

The cost question was already answered by the section above: two levers that
each cut coarse search work substantially moved the wall clock by under 0.05 ms,
because the flow pass is dominated by the pyramid build, the six dispatches, and
the full-resolution filter and detector passes. Going the other way should be
similarly cheap, but **that is an inference from the earlier measurement, not a
measurement of this change** -- radius 8 is 289 comparisons per cell against 81,
and the 30 FPS case has not been timed. Time it before assuming the ceiling can
go higher.

Verified: `ctest` 12/12, `osss.exe --self-test`, and the quality bench unchanged
at 60 FPS (linear mean 39.72 dB, 6.18 dB over the crossfade; temporal roughness
ratios 0.45 / 0.53 / 0.41). Unchanged is the whole claim -- the default path is
the same search it always was.

### Flow scale and performance mode, 2026-08-16

Both were built as user settings and measured on the bench. The headline is a
negative result about where the flow pass actually spends its time.

`--flow-scale auto|quality|performance|ultra-performance` selects the divisor
that sets the motion-field resolution; `auto` reproduces the historical rule
exactly, so every number above still describes the default.
`--performance-mode on` halves the coarse search radius and drops the second
local search that resolves periodic detail. (It pinned the radius at 2 against 4
when this was measured; it now halves whatever the source period resolves to --
see the next section. At the 60 FPS the bench runs at, those are the same
numbers, so the measurements below still describe it.)

Measured on an RTX 5090, warm shader cache:

| Change | Flow prepare | Cost |
| --- | --- | --- |
| flow scale 4 -> 8 @ 960x540 | 0.51 -> 0.44 ms | -2.0 dB linear, -3.7 dB detail, detail roughness 0.41x -> 10.8x |
| flow scale 4 -> 8 @ 3840x2160 | 0.92 -> 0.88 ms | -1.8 dB linear, -4.4 dB detail, detail roughness 0.33x -> 11.65x |
| performance mode @ 960x540 | 0.39-0.44 -> 0.35-0.36 ms | -7.3 dB detail, linear roughness 0.45x -> 1.05x |
| performance mode @ 1920x1080 | 0.44 -> 0.47 ms | inside run-to-run noise |

Two independent levers that each cut search work substantially, and neither
moved the wall clock by more than about 0.05 ms. **The search is not where the
flow pass spends its time.** What is left is the pyramid build, six separate
dispatches per source pair, and the full-resolution filter and detector passes.
Anyone trying to make the interpolator cheaper should start there and not
repeat these two experiments.

A second finding, and an open gate: **the `automatic` rule is the wrong choice
above 1440p.** Its divisor of 8 fails this repository's own quality gates at
3840x2160 while `quality` passes them for 4% more GPU time. The default is
unchanged pending a second GPU vendor, because every number here is from one
adapter; `kAutomaticFlowScalePixelThreshold` in `src/flow_scale.h` is the
one-line change if a second adapter agrees. Note the original 4K stutter report
that prompted the architecture review ran on the coarse grid throughout.

### Temporal prior, 2026-08-16

`--temporal-prior on|off` (default on; launcher checkbox) seeds each pair's
coarse motion search with the previous pair's field when the renderer says the
pairs are consecutive -- nine extra candidates around the most confident vector
of the fine cells under each coarse cell, read with `Load` and never filtered,
because the field is discontinuous by design. It is a candidate and not an
anchor: selection stays regularised toward zero, so it cannot lock a wrong
vector in. Cost is about a tenth of the coarse pass (0.37 to 0.41 ms per pair
at 960x540, warm). Continuity is decided in `Renderer::SelectFramePair` from
the unique sequences and passed as a `PreparePair` argument that defaults to
false, so every caller that cannot vouch for it estimates from scratch.

The bench gained a *reach* section to measure it: a frame-sized crop sliding
across a wider dead-leaves scene by 8, 16, ... 80 pixels per pair, run seeded
and unseeded on the bench's own interpolator and on a dedicated performance-mode
one. Measured on the RTX 5090:

| Pan (px) | Performance mode, seeded / unseeded | Quality mode, seeded / unseeded |
| --- | --- | --- |
| 16 (last inside the 16 px window) | 31.6 / 31.4 dB | 23.2 / 23.1 dB |
| 24 | **21.7 / 17.2 dB** (14 % / 35 % bad) | 19.9 / 19.9 dB |
| 32 | **18.8 / 15.5 dB** | 17.8 / 17.8 dB |
| 40 | 16.4 / 14.7 dB | 15.5 / 15.0 dB |
| 48 | 14.9 / 14.2 dB | 14.5 / 14.4 dB |

Gated: the seeded ramp may never fall below the unseeded one on either
interpolator, and it must gain at least 2.0 dB at 24 px and 1.5 dB at 32 px in
performance mode. The pattern matrix is unchanged to the second decimal, and
the consecutive-pair temporal run within 0.06 dB.

Three findings came out of building that section, all about the estimator
rather than the prior, and all recorded in the bench source:

- **The fine level's second search from zero limits large motion in quality
  mode.** It compares the seeded (or coarse) match against a near-zero one on
  `MotionCost` scaled by the 2 px refinement step, i.e. a penalty of 0.003 per
  pixel of displacement -- 0.13 at 44 px, more than half the dead-leaves
  scene's typical edge contrast -- and overrides genuine large motion. Rescaled
  to the coarse cell (`coarse_scale`) every gate in the bench still held
  (linear +0.10 dB, occlusion -0.13 dB, worst-bad 5.3 % -> 5.9 % under the 8 %
  ceiling), in-window pans on the dead-leaves scene gained 2-3.5 dB, and the
  seeded pair at 40 px gained 1.6 dB over the unseeded one. **Not applied**; it
  is a one-argument change in `RefineFlow` and an open gate below.
- **The zero-anchored tie-break outweighs the error surface on low-contrast
  texture.** Multi-octave value noise at 0.2 amplitude per octave was
  estimated as zero across regions the window covered easily; with the
  regularisation zeroed, exact-cell pans over the same scene scored 35-41 dB.
  Real frames are higher-contrast than that noise; captured game frames are
  the way to find out how much of this matters.
- **A pan that is not a whole number of coarse cells is estimated worse than
  one that is**, because the two frames' box-filtered mips are then
  phase-mismatched and the half-cell halving pass at the coarse mip is close to
  a coin flip, which can hand the fine level a seed outside its 4 px window.
  The pattern's large edges hide it; dense texture does not.

Also found and worked around in the bench: a pan whose decorrelated mean
difference exceeds the fusion's scene-cut threshold (0.28) is treated as a cut
and holds the newest frame. Full-range dead leaves crosses it at 44 px; the
scene's greys are restricted to 0.15-0.85 so a fast pan reads as a pan.

Remaining milestone gates:

- Measure the finer-grid/smaller-patch configuration against captured real game
  frames, where noise is present. It is the largest measured quality gain still
  on the table and the only thing blocking it is that this bench cannot test the
  risk it carries.
- Rescale the fine level's from-zero comparison to the coarse cell, or find a
  reason not to. Bench evidence above says it is a gain everywhere measured;
  what it needs is the same measurement on captured frames, where the risk --
  periodic detail sliding one period -- is present at real contrast.
- Add disocclusion, thin-object, UI, and noisy-texture sequences to the quality
  bench beyond the single temporal run it now does. (The dead-leaves reach
  ramp is the first textured sequence; it is a pan, not an object.)
- Measure the quality bench on a second vendor. Every number above is one GPU.
- ~~Add runtime diagnostic views for flow, confidence, and rejected pixels.~~
  **Closed 2026-08-16.** `--debug-view off|flow|confidence|fallback`, and the
  launcher's Diagnostic view selector. `flow` shows direction as hue and
  magnitude as brightness; `confidence` shows white where the warps were trusted
  and black where the crossfade fallback took the pixel, which is the direct
  picture of the failure that made the interpolator inert; `fallback` separates
  the three safeguards into channels. They replace the frame rather than
  overlaying it, and read only intermediates the normal path already computes,
  so enabling one costs nothing. See `src/debug_view.h`.
- Validate the feature-level-11 path on representative AMD and Intel GPUs. The
  pyramid adds a requirement: `R16_FLOAT` with mip autogen and typed UAV store.
  `MotionInterpolator`'s constructor checks for it and reports a clear error.

## Milestone 2: source timeline and presentation latency — foundation complete

Completed in the current implementation:

- Carry `Direct3D11CaptureFrame::SystemRelativeTime()` through an explicit QPC
  to steady-clock mapping. Callback arrival is recorded separately and cannot
  re-time source media.
- Reduce each capture to a small asynchronous GPU signature. Exact repeats are
  counted as raw duplicates and do not advance unique cadence, pair identity,
  or optical-flow setup.
- Retain up to eight timestamped unique GPU frames. Queue target 1 delays media
  by one estimated source period plus a default 8 ms jitter floor; underruns
  grow an adaptive budget in 2 ms steps up to 32 ms, while clean source frames
  decay it gradually. The `--buffer 0..32` setting overrides the floor, and the
  selector still chooses a known bracket without extrapolation.
- Drain completed signatures on the main thread, submit the classified frames,
  then issue one `Flush()` before collecting the next batch. Capture arrival is
  recorded independently from media time, with p50/p95 media-to-ingest
  telemetry and interval/cumulative selection-mode counters.
- Generate absolute rational target deadlines. Source cadence affects only
  buffering, telemetry, and multiplier-ceiling admission; it cannot resize or
  re-phase the target clock.
- Distribute multiplier-limited submissions across target slots with a bounded
  deterministic credit gate. Late slots are skipped and counted with no
  catch-up queue.
- Use the active display configuration path's rational refresh when available,
  with an integer compatibility fallback and manual 24-1000 FPS targets.
- Use `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`, maximum frame latency
  one, wait-before-render ordering, matching resize flags, and explicit handle
  teardown.
- Report raw, unique, target, submitted, optional DXGI-confirmed, required,
  allowed, realized, queue, latency, missed, duplicate, and capture-drop
  diagnostics, plus media-ingest, flow-GPU, fusion-GPU, and selection-mode
  diagnostics.
- Instrument flow and fusion with nonblocking D3D11 timestamp queries, pool
  reusable history textures, and prefer the Games MMCSS class with a restored
  high-priority fallback. Lower-resolution fusion remains measurement-gated.
  A max-frame-latency-two policy is now selectable as `--pacing queued`
  (see "Pacing modes" below); it is measured, and deliberately not the default.
- Add an explicit even ceiling cadence. Production uses `--ceiling-pacing even`
  so a bound target such as 180 from a 240 Hz ceiling admits every second slot;
  the historical spread cadence remains available for comparison and regression
  coverage.
- Preserve the historical capture smoke and add a separate 60->144 real-desktop
  timing mode with a two-second warmup and ten-second measurement.

Latest local adoption evidence (2026-08-14, RTX 5090, active path
`240000/1000`) used an isolated 60 Hz synthetic source and measured 144.00
scheduled FPS, 144.00 submitted FPS, 143.80 DXGI-confirmed FPS, 59.90 raw WGC
FPS, 56.90 unique FPS, 30 compositor duplicates, zero capture drops, zero missed
target slots, a bounded eight-frame history, 26.56 ms queue delay, and 26.59 ms
average media-to-present age (41.56 ms maximum). The preserved historical WGC
smoke and the exact click-through/pointer-restoration fixture also passed. This
is local digital/runtime evidence, not a real-game comparison or physical
display certification.

### Presentation path rebuilt, 2026-08-16

Four user-reported defects were traced and fixed together, because they shared
two causes.

The first hypothesis was that `WS_EX_LAYERED` on the output window was pure
cost. A layered window is always DWM-composed and can never be promoted to
independent flip, and the layered alpha here is 255 -- fully opaque -- so it
looked free to remove.

**That hypothesis was wrong, and the frozen input fixture is what caught it.**
With the style removed, `osss_input_passthrough_smoke.exe` reports "the
generated-frame surface intercepted the target click"; with it restored, the
same binary passes. `WS_EX_TRANSPARENT` plus `HTTRANSPARENT` does not pass a
click to another process's window -- only the layered-plus-transparent pair
does. The style is required and is now documented as such, with the measurement
recorded next to it.

The standing consequence: the overlay is permanently DWM-composed, so
independent flip and true tear-free VRR are unreachable for this program shape.
That is now stated in ARCHITECTURE.md under "What is deliberately absent"
instead of being an open aspiration.

The second cause was real: `Present` hard-coded sync interval one. That hands
presentation time to the display, so any target rate that is not a divisor of
the refresh rate rounds each deadline up to the next vblank and alternates frame
durations indefinitely. `--present-mode auto|vsync|tearing` now selects it,
`auto` resolves to `Present(0, DXGI_PRESENT_ALLOW_TEARING)` wherever DXGI
supports it, and the resolved mode is printed in the startup banner. Because the
overlay is composed, this removes OSSS's own quantization rather than producing
literal tearing — the banner and README say so rather than overclaiming. Under vsync
only, `OutputClock::PhaseAlignToVblank` aligns the deadline grid to the vblank
raster — phase only, never rate, and skipped when the grids are incommensurate.

Startup was separately dominated by runtime HLSL compilation: seven shaders, six
of them different entry points into one ~35 KB translation unit, compiled
serially at `D3DCOMPILE_OPTIMIZATION_LEVEL3`. Measured cold on an RTX 5090 the
whole `--self-test` took 12.3 s, of which `RefineFlow` alone was 11.5 s.
`src/shader_cache.*` adds a content-addressed on-disk bytecode cache and
concurrent batch compilation: the same run is **0.27 s** warm, a 46x
improvement. Cold cost is unchanged because one entry point dominates, so the
launcher now pre-warms the cache in the background when it opens, and
`--warm-shader-cache` exposes the same thing.

Attempting `[loop]` on `RefineTo` to cut that 11.5 s made it *worse* — 19.2 s
measured — and was reverted. The flow shader is unchanged.

The launcher also used to pin itself topmost after **Start**, which left the
target in the background; since the overlay deliberately hides whenever the
target lacks the foreground, nothing appeared until the user minimized the
launcher by hand. It now minimizes itself and hands the foreground to the
target.

Evidence, all on one machine (RTX 5090, primary display a 480 Hz Sunshine
virtual display): `ctest --preset release` 7/7 on a clean rebuild with no
warnings; `--self-test` and `--capture-self-test` green;
`--adaptive-capture-self-test` reporting `scheduled=144.00 submitted=144.00
confirmed=144.00 raw=60.10 unique=60.00 missed=0 latency-avg=25.10ms` -- 2.4x
from a 60 FPS source with no missed slot; and
`osss_input_passthrough_smoke.exe` passing, which is the fixture that caught the
layered-window mistake above.

Two environment notes worth recording, because both cost time to diagnose and
will recur on this machine. The adaptive check paces its GDI source with
`DwmFlush()`, so while the virtual display was unattached it reported
`raw=3.8 unique=3.8` with `scheduled=144 submitted=144 missed=0` -- OSSS's own
path perfect, the source starved. That is environment-scoped, not a regression,
and the contract failure now prints the failing clause and the measurements so
the next person can tell in one run rather than re-instrumenting. Second, this
host carries seven display adapters, most of them virtual, so `--target-fps
auto` resolves against whichever is primary at the time.

**Still not measured: a real game.** Whether this closes the reported "2x
behaves like 1.5x" and "pacing is atrocious" complaints needs a Clair Obscur
run, which is the immediate next gate. Note that a streamed virtual display adds
its own pacing stage below OSSS, so the first real-game measurement should say
which display it ran on.

### Pacing modes: unpaced, paced, queued, 2026-08-16

The presentation loop's shape is now selectable with `--pacing` (launcher:
Pacing). `src/pacing_mode.h` names the three modes and the two mechanism facts
each implies -- maximum frame latency and whether a slot is rendered ahead --
so the swap chain and the loop read one table and cannot disagree. `paced` is
byte-for-byte the previous behaviour and stays the default.

What was built: `queued` renders slot *k+1* as soon as slot *k* is handed over
and presents it at its own deadline (maximum frame latency two, three back
buffers, one target slot of selector lookahead so the early selection is still
bracketed); `unpaced` drops the deadline grid and presents whenever the
waitable object signals, with `FrameSelector::SelectNow` bypassing the ceiling
gate. Telemetry says which class it is measuring: the HUD prints `PACING
(free-run)` and `MISSED --` without a clock, and the console prints
`render-ahead=`/`served-late=` under `queued` so the render-ahead can be
checked rather than assumed. New tests: `osss_pacing_mode_tests`, and
`TestLookaheadSelectsOneSlotEarly` / `TestSelectNowBypassesCeilingAndLeavesGateAlone`
/ `TestSelectNowGenerationOff` in the scheduler suite; frozen fixtures untouched.

Measured, same machine (RTX 5090; the 4K panel reporting 239 Hz was primary),
60 FPS D3D11 test animation into a 240 FPS target, `--present-mode tearing`,
overlay output, 12 s each:

| Mode | submitted | queue-delay | capture-to-present | present-interval p50/p95 | missed |
| --- | --- | --- | --- | --- | --- |
| `paced` | 236.0 | 24.7 ms | 29.5-30.2 ms | 4.24 / 4.24 ms | ~4/s |
| `queued` | 236.0 | 28.8 ms | 34.5-35.6 ms | 4.24 / 4.24 ms | ~4/s (`render-ahead=237 served-late=0`) |
| `unpaced` | 236.0 | 24.3-25.0 ms | 28.4-29.2 ms | 4.24 / 4.24 ms | n/a |

Under `vsync` at a 120 target, `queued` submitted 120.0 with every present
rendered ahead and 2 missed total (cold start).

Three things this measurement settles. First, the ~4 missed slots per second
under `paced` are not the flow pass -- `queued` takes the flow pass off the
critical path and the count does not move -- they are DWM picking the composed
overlay up at ~236/s on a 239 Hz display; `unpaced` lands on the same 236 and
the same 4.24 ms interval, which is the compositor's cadence showing through.
Second, `queued` costs exactly the one target slot it was designed to cost
(24.7 -> 28.8 ms) and nothing else; on a source this regular it buys nothing
visible, which is why it is not the default -- it is insurance for a heavier
game's flow spikes, and `served-late=` is how to tell whether it is earning
its slot there. Third, `unpaced` saves about a millisecond of capture-to-present
and no more, because latency comes from the media queue, not the grid; it is a
measurement floor, and README says so.

Still open here: the same three-way measurement on a real game where the flow
pass actually spikes, which is the case `queued` exists for; and whether
`unpaced` under `--output-mode fullscreen` on the VRR panel free-runs above the
compositor rate as predicted (it should, and nothing here checked it).

Remaining validation and hardening gates:

- Re-run the four reported complaints against Clair Obscur specifically:
  time-to-HUD after Start, realized multiplier, frame-time distribution under
  `--present-mode tearing` with G-Sync on versus `vsync`, and felt latency.
  This is the gate that decides whether the 2026-08-16 work actually landed.
- Measure representative games at fixed 60 FPS and fluctuating 55-70 FPS under
  controlled foreground/background conditions. Keep scheduled, submitted,
  DXGI-confirmed, and physically observed output as separate evidence classes.
- Exercise the full capture/render path at 30, 60, 120, 180, and 240 Hz and
  record capture-to-present distributions rather than only one-sample HUD age.
- Add GPU-signature fixtures for low-amplitude motion, UI-only motion, noisy
  static content, and resize/scene-cut transitions.
- Measure 1080p and 1440p GPU headroom with the signature, optical-flow, warp,
  and presentation stages enabled together.
- Validate Windows 10 fallback behavior and representative AMD and Intel GPUs.

## Milestone 3: quality and compatibility

- Explicit HUD and UI-region masks beyond the current static-pixel safeguard.
  Done for user-defined static rectangles (`--ui-mask`, launcher HUD mask
  field): masked pixels show the newest real frame, are excluded from
  optical-flow evidence, and do not vote for scene cuts.
- Automatic static-overlay detection (`--ui-mask-auto on`). Done as an
  experimental, default-off addition: a quarter-resolution persistence grid arms
  cells that hold still while the scene around them moves, with frame-relative
  neighbourhood sampling, one-cell edge dilation, and a one-pair publication lag
  so a ticking counter stays masked on the frame it ticks. Validated against a
  deterministic panel fixture in `osss_motion_tests`. Still open: measuring it
  against real games, telemetry for armed coverage, and a debug visualization of
  the detected mask.
- Per-application profiles: **done for explicit selection, 2026-08-16.**
  `--profile <name.exe>` and `--save-profile <name.exe>`, backed by
  `src/app_profile.*` and `%LOCALAPPDATA%\OSSS\profiles.txt`. A profile is
  stored as an argument list rather than a settings record, so there is exactly
  one parsing path and no serialisation to keep in sync with the CLI; profile
  arguments are applied before the command line so an explicit flag still wins.
  `--profile auto` keys off the target's executable, resolving the window twice
  at startup -- once to learn the name, once after re-parsing -- which is a
  window-list lookup rather than the hand-merged settings structure the design
  exists to avoid. Applying is guarded to the capture path so `--help` and the
  self-tests never fail for want of a target. The launcher exposes it as a
  checkbox, and a Save button writes the current settings as the selected
  target's profile. The launcher stores what `ProfileArgumentsFor` emits rather
  than its own launch command line, because `--title`, `--hwnd`, and
  `--stop-event` name a session and not a setting. All fourteen capture settings
  round-trip; the flags the two paths emit were cross-checked against the CLI
  parser, which is how `--interpolator` was caught missing from profiles.
- Cursor and rapidly changing overlay isolation.
- Better disocclusion reconstruction and thin-object handling.
- Multi-monitor/DPI recovery and HDR investigation. VRR-aware scheduling is
  partly done: `--present-mode tearing` gives the output clock control of
  presentation time, which is what a variable-refresh display follows. OSSS
  still does not query the VRR range or adapt the target rate to it.
- Per-application profiles and a foreground-window activation workflow. The
  foreground half now exists as a safety gate rather than a feature: the overlay
  hides whenever the target is minimized or loses the foreground, after a
  2026-08-15 run against Clair Obscur left a topmost opaque surface frozen over
  the display with the game unreachable behind it. What remains open is the
  *workflow* — arming per application, and resuming without the deliberate
  `FrameSelector::Reset` on the first pair back.
- Real-game validation is still thin. The Clair Obscur run above is the first
  and only one on record, and it found two defects no fixture covered: the
  missing foreground gate, and `RegisterHotKey` failing unchecked so the stop
  chord could be advertised without existing. Both are fixed and covered by
  `osss.exe --self-test`. Assume the next real title finds more; the harness
  fixtures are deterministic sources, not games.

An ML interpolator should be evaluated only after this harness exists. It can
replace the motion estimator/fusion stages while retaining capture, scheduling,
diagnostics, and presentation. Any candidate model needs a commercial-license
audit and per-vendor latency measurements before adoption.
