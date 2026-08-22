# Implementation inventory

Captured: 2026-08-15

Symbols and files this package's evidence depends on. Names stay valid; line
numbers drift.

## Burst harness (`src/test_animation_main.cpp`)

| Symbol | Role |
| --- | --- |
| `kDefaultBurstIntervalSourcePeriods` (0.618…) | Default `--burst-interval-ms` as a fraction of the source period; sweeps generated-frame phases. |
| `kRealFramePhaseToleranceMilliseconds` (0.5) | A matched instant this close to `k / base_fps` is a real frame. |
| `CaptureClientPixels` | GDI `BitBlt` + `CAPTUREBLT` of the client area from the screen DC. |
| `FindBestMatch` | Temporal search: −`--search-ms` … +25 ms at 2 ms, refined to 0.25 ms; MAE at stride 4 then 2, final metrics at stride 1. |
| `ScoreAndWriteBurst` | Concurrent per-frame scoring; classification, summary, verdict, JSON, stdout line, title. |
| `FrameOutputIsVisibleOver` | `FindWindowW(L"OSSS.FrameOutput")` + rect overlap → `osss_frame_output_visible`. |
| `RunInteractive` | Burst scheduling from `--burst-at-ms`; capture interleaved with source presents. |

## Pattern maths (`src/test_pattern.{h,cpp}`)

| Symbol | Role |
| --- | --- |
| `SourceFramePhaseMilliseconds` | Signed distance from an instant to the nearest source-frame instant. |
| `IsSourceFrameInstant` | Threshold on the above. |
| `CompareTestPatternFrames` | MAE / RMSE / PSNR / bad-pixel % / max channel error, below `kScoredTop`, HUD rects excluded. |
| `RenderTestPattern` | Analytic ground truth at any instant. |

## Offline bench (`tests/interpolation_quality_tests.cpp`)

| Symbol | Role |
| --- | --- |
| `Gate` / `gates[]` | Per-lane PSNR floors, gain floor, and (new) mean and worst bad-pixel ceilings. |
| `kMaximumRoughnessLumaLevels`, `kMaximumRoughnessRatioToCrossfade` | Temporal ceilings: absolute 6.0 luma levels and (new) ≤ 0.75 of the crossfade's error-step. |
| `TemporalRoughness` | Mean frame-to-frame change of the per-pixel error signal. |
| `LaneSummary` | Now carries `mean_bad_pixel_percent` and `mean_crossfade_bad_pixel_percent`. |

## Production path exercised by the OSSS legs (`src/main.cpp` `RunFrameGeneration`)

`ResolveTarget` (`--title` fragment) → `ResolveTargetRate` (`--target-fps 240`,
manual) → `Renderer::InitializeDevice` / `SetMotionEnabled` (runtime HLSL
compile; the 10–15 s startup) → `CaptureSession::Start` → `CreateOutputWindow`
→ the loop with `OutputClock` / `SourceTimeline` / `FrameSelector`. Stats
overlay off (`--stats-overlay off`); no UI mask; auto mask off.
