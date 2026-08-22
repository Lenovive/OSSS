# Current implementation inventory

Verified against the live files during package creation. Line numbers are a navigation aid and must be rechecked if the worktree changes.

## Already implemented and worth preserving

- `src/main.cpp:25-184` parses automatic/manual output targets and `2x`–`6x` maximum multipliers.
- `src/main.cpp:231-241` resolves automatic target FPS from the selected window's display.
- `src/gui_main.cpp:19-568` exposes target FPS and maximum interpolation controls. The automatic display target is the first option.
- `src/capture_session.cpp:45-63` best-effort requests `IGraphicsCaptureSession5::MinUpdateInterval` from the output target and records whether the request was accepted.
- `src/frame_pacer.cpp:51-58` calculates a fractional effective multiplier using `min(target, estimated source × ceiling)`.
- `src/motion_interpolator.*` accepts arbitrary interpolation alpha, computes bidirectional flow once in `PreparePair`, reuses it for draws, protects static pixels, and selects the current frame on scene cuts.
- `src/renderer.cpp:222-251` keeps two GPU history textures and prepares motion once when a new pair arrives.
- `src/stats_overlay.*` and `src/main.cpp:418-432` report raw capture/present-call FPS plus target/max/mode.
- `tests/frame_pacer_tests.cpp`, `tests/motion_interpolator_tests.cpp`, `tests/input_passthrough_smoke.cpp`, and `src/capture_smoke.cpp` provide count, motion, scene-cut, GUI, capture/presentation, and input-routing baselines.

## Architectural gaps blocking true target-driven adaptive scheduling

1. `FramePacer` still owns source estimation, output intervals, pair progress, and alpha in one object. `OnSourceFrame` resets `pair_started_`, alpha, and `presentations_for_pair_`; `TakePresentationAlphaAt` rejects work after the integer per-pair quota. The source pair still owns output opportunities.
2. `PresentationInterval()` uses `EffectiveOutputFpsLimit()`, which depends on the changing source EMA. The output clock can therefore breathe or re-space when the source estimate changes instead of remaining target-owned.
3. Capture stores only an overwritten latest texture and callback `steady_clock::now()`. WGC `SystemRelativeTime` is not retained, callback delay is not measurable, and intermediary history is unavailable.
4. Raw compositor callbacks are treated as unique source frames. There is no duplicate signature/classifier, so capture requests near the target can make source FPS appear equal to target FPS even when game content changes much more slowly.
5. `main.cpp` performs one latest-frame consume, one pacer query, and `Sleep(1)` polling. It cannot select a bracketing pair for an independent media time.
6. The renderer owns exactly one current pair. It cannot retain/select among timestamped pairs for a buffered playback head.
7. The swap chain has two flip-discard buffers but no frame-latency waitable flag/handle, per-chain latency setting, or confirmed-present/missed-refresh instrumentation. `Present(1,0)` remains the only synchronization.
8. Display refresh uses integer `EnumDisplaySettingsW::dmDisplayFrequency`, losing rational rates and providing no DRR/VRR context.
9. Existing tests assert counts over coarse simulated time. They do not assert deadline sequence, drift, phase, selected frames, alphas, queue age, duplicates, stalls, or missed-deadline recovery.
10. The UI currently defaults the maximum multiplier to `2x`, which cannot reach the representative 60→144 target.

## Suggested component boundary

These names are proposed, not mandatory:

- `FrameRate`: rational numerator/denominator and drift-free deadline arithmetic.
- `OutputClock`: fixed phase, next deadline, stale-deadline skipping, missed count.
- `CapturedFrame`: GPU texture/reference, raw sequence, unique sequence, compositor time, callback arrival, dimensions, duplicate/scene metadata.
- `SourceTimeline`: bounded history, duplicate classification, robust unique-period estimate, capture-delay estimate.
- `FrameSelector`: queue-1 playback time, bracketing pair, alpha, mode (`interpolate`, `real`, `hold`, `scene_cut`, `ceiling_skip`, `stalled`).
- `PresentationTelemetry`: scheduled, submitted, DXGI-confirmed, missed, latency, queue, raw/unique FPS.

Conceptual loop:

```text
drain/ingest available captures into SourceTimeline
wait until swap chain can accept work and OutputClock reaches a target deadline
skip/count stale target deadlines; do not enqueue catch-up frames
sample_time = deadline - queue_1_delay
selection = FrameSelector.Select(SourceTimeline, sample_time, ceiling/state)
prepare flow only if the selected unique pair changed
render selection.alpha or explicit real/hold fallback
submit Present and record submitted/confirmed timing
```

## Documentation discrepancy

`README.md` and `docs/ROADMAP.md` describe the current count-based target/ceiling feature as target-driven pacing. Preserve their useful UI/capture details, but update the completion language after deadline ownership, timestamped unique history, waitable presentation, and diagnostics actually pass.

## Primary API and behavior references

- User-supplied target behavior: https://losslessscaling.com/adaptive-frame-generation/
- WGC compositor timestamp: https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframe.systemrelativetime
- Frame-latency waitable handle: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject
- Rational display target refresh: https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_path_target_info
- Submitted versus displayed presentation statistics: https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics

Use these as API/behavior references only. Do not claim implementation parity with Lossless Scaling's proprietary model or scheduler.
