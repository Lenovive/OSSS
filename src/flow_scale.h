#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

namespace osss {

// The resolution motion estimation runs at, as a divisor of the source size.
//
// Optical flow is overwhelmingly low-frequency: a camera pan assigns one vector
// to a million pixels, so the information content of the flow field is orders of
// magnitude below that of the image. That is what makes estimating it on a
// downscaled grid and bilinearly upsampling the result to full resolution for
// the warp a good trade rather than a corner cut.
//
// The saving is at least quadratic. Doubling the divisor quarters the cell
// count, and because the search radius is expressed in flow texels it also
// covers twice the source-space motion per texel, so a coarser grid searches
// less as well as searching fewer places.
//
// What it costs is specific and predictable: structures thinner than one flow
// cell lose their motion to the surrounding cell, motion boundaries blur so the
// warping halo around a moving object widens, and small fast objects can be
// missed outright. That is why this is a setting and not a constant -- the right
// answer depends on resolution and on whether the user would rather have the
// frames or the edges.
//
//   automatic          the historical rule, and the default: four source pixels
//                      per fine cell up to 1440p, eight above it. Chosen because
//                      one flow cell at 4K still subtends a small angle while
//                      the flow pass costs four times as much, so the trade is
//                      good there and poor at 1080p.
//   quality            four everywhere. Above 1440p this is finer than automatic
//                      and costs more.
//   performance        eight everywhere. At or below 1440p this is coarser than
//                      automatic and costs less.
//   ultra_performance  sixteen everywhere. The cheapest flow this pipeline will
//                      estimate. Reach for it only when the interpolator is
//                      taking GPU time the game needs back.
//
// Divisors must stay powers of two: the coarse level runs at twice the divisor
// and both levels index the luma pyramid by log2 of it, so a non-power-of-two
// would land the search between mip levels.
//
// This header is deliberately dependency-free for the same reason
// present_mode.h and frame_rate_limits.h are: `osss_gui.exe` has to name a
// setting on a command line without acquiring a Direct3D dependency to do it.
enum class FlowScale {
    automatic,
    quality,
    performance,
    ultra_performance,
};

// Source pixel count at or below which `automatic` uses the finer grid. 2560x1440.
inline constexpr std::uint64_t kAutomaticFlowScalePixelThreshold = 2560ULL * 1440ULL;

// The spelling `osss.exe --flow-scale` accepts. Kept next to the parser so the
// two cannot drift apart.
[[nodiscard]] constexpr const wchar_t* FlowScaleArgument(const FlowScale scale) noexcept {
    switch (scale) {
    case FlowScale::quality:
        return L"quality";
    case FlowScale::performance:
        return L"performance";
    case FlowScale::ultra_performance:
        return L"ultra-performance";
    case FlowScale::automatic:
        break;
    }
    return L"auto";
}

// The same names in narrow characters, for diagnostics and banners.
[[nodiscard]] constexpr const char* FlowScaleName(const FlowScale scale) noexcept {
    switch (scale) {
    case FlowScale::quality:
        return "quality";
    case FlowScale::performance:
        return "performance";
    case FlowScale::ultra_performance:
        return "ultra-performance";
    case FlowScale::automatic:
        break;
    }
    return "auto";
}

// Accepts the canonical spellings plus the aliases users reach for first.
// Returns nullopt for anything else; the caller decides how to complain.
[[nodiscard]] inline std::optional<FlowScale> ParseFlowScale(
    const std::wstring_view value) noexcept {
    if (value == L"auto" || value == L"automatic") {
        return FlowScale::automatic;
    }
    if (value == L"quality" || value == L"high") {
        return FlowScale::quality;
    }
    if (value == L"performance" || value == L"fast") {
        return FlowScale::performance;
    }
    if (value == L"ultra-performance" || value == L"ultra" || value == L"fastest") {
        return FlowScale::ultra_performance;
    }
    return std::nullopt;
}

// Source pixels per fine flow cell, along each axis, for this setting at this
// source size. `automatic` reproduces the historical rule exactly, so the
// default resolves to the same divisor the pipeline used before this setting
// existed and the frozen quality baselines still describe it.
[[nodiscard]] constexpr unsigned ResolveFlowScaleDivisor(
    const FlowScale scale,
    const std::uint64_t source_pixels) noexcept {
    switch (scale) {
    case FlowScale::quality:
        return 4U;
    case FlowScale::performance:
        return 8U;
    case FlowScale::ultra_performance:
        return 16U;
    case FlowScale::automatic:
        break;
    }
    return source_pixels <= kAutomaticFlowScalePixelThreshold ? 4U : 8U;
}

// Half-width of the coarse motion search, in coarse cells.
//
// The coarse level scans a (2r+1)^2 grid of candidate displacements spaced
// `coarse_scale` source pixels apart, where `coarse_scale` is twice the divisor
// above. So the largest displacement the estimator can find *at all* is
// `radius * 2 * divisor` source pixels per source pair -- the value
// `CoarseSearchPixels` below reports and the telemetry line calls `flow-search`.
// The fine level only searches a few pixels either side of whatever the coarse
// level hands it, so motion past that ceiling is not merely estimated coarsely
// -- it is unrecoverable, confidence collapses, and fusion falls back to a
// crossfade.
//
// Reach is therefore the contract, and it is stated in *source pixels*. Two
// things move it, and both are corrected for here.
//
// The first is the source rate. A fixed radius encodes a fixed *displacement*
// ceiling, but what is fixed about a scene is its *velocity*: a camera turning
// at 100 degrees per second turns at 100 degrees per second whatever the source
// is rendering at. Displacement per pair is velocity times the source period, so
// halving the source rate doubles the motion the estimator must find while
// leaving the ceiling exactly where it was. That asymmetry is why the same game
// can read clean at 120 FPS and smear at 60.
//
// The second is the flow divisor, and it used to be an unfixed bug. Because the
// radius was counted in cells, choosing a *finer* grid silently *halved* reach:
// `--flow-scale quality` at 4 reached 32 source pixels where `auto` at 8 reached
// 64. So the setting a user picks to get more accuracy quietly took away the
// range that decides whether a pan survives at all, and the coarser, less
// accurate setting looked better on fast motion for a reason that had nothing to
// do with accuracy. Measured on the quality bench at 2752x2064, that trade was
// severe in both directions: at a divisor of 8 the detail lane scored 18.03 dB
// against a crossfade's 21.25 -- worse than not interpolating -- while at 4 it
// scored 22.53; and the reach ramp put the collapse at ~64 px for a divisor of 8
// but ~40 px for 4. Dividing the target reach by the cell size decouples them, so
// a finer grid now buys accuracy and keeps the range.
//
// Cost is why there is a ceiling. The search is (2r+1)^2 patch comparisons per
// coarse cell, and a finer grid has quadratically more cells, so holding reach
// constant makes a divisor of 4 markedly dearer than 8 rather than equal to it.
// That is affordable because the two scale in opposite directions against the
// budget: reach only grows when the source rate falls, and a slow source is
// precisely the case with source period to spend. Measured in Factorio at
// 2752x2064 and 60 FPS, a divisor of 4 at the reach a divisor of 8 gets costs
// 0.4 ms of flow against 0.2, and 3.3 W of board power.
//
// Fed from `SourceTimeline::EstimatedSourcePeriod`, which is already smoothed,
// rather than from the interval of the individual pair being estimated. A single
// hitched pair would otherwise spike the radius, and so the cost of the coarse
// pass, at exactly the moment the GPU is furthest behind.

// Reach the reference source rate resolves to, in source pixels per pair. This
// is what a divisor of 8 got from the historical radius of 4, which is the
// configuration `automatic` picks above 1440p and the one the reach ramp shows
// surviving to ~64 px. Pinning the target here rather than pinning a radius is
// what makes every divisor agree.
inline constexpr int kReferenceCoarseReachPixels = 64;

// The source rate the reference reach describes, in frames per second.
inline constexpr double kReferenceCoarseReachSourceFps = 60.0;

// Cost ceiling, also in source pixels. 128 is what a divisor of 8 reached at the
// old radius ceiling of 8, so the rate at which a coarse grid stops buying reach
// is exactly where it always was.
inline constexpr int kMaximumCoarseReachPixels = 128;

// Below this the search is too narrow to seed the fine level usefully even when
// the source is fast. It is a cell count rather than a pixel count because that
// is what it is about: two cells either side is the least that can disambiguate
// a direction, whatever those cells span.
inline constexpr int kMinimumCoarseRadius = 2;

// The largest radius any divisor can ask for, which the ceiling above implies at
// the finest grid this pipeline estimates on (4). Kept as a named bound so the
// (2r+1)^2 cost of the coarse pass has a stated worst case.
inline constexpr int kMaximumCoarseRadius = kMaximumCoarseReachPixels / (2 * 4);

// `source_period_seconds` is the measured mean interval between unique source
// frames. A non-positive period means nothing has been measured yet -- cold
// start, or a stalled source -- and resolves to the reference reach, so the
// first pairs after a start search exactly as far as steady state will.
//
// `flow_scale_divisor` is the value `ResolveFlowScaleDivisor` returned for the
// current setting and source size. Passing it is what holds reach constant
// across flow scales; a zero is treated as one so a caller that has not resolved
// a divisor yet still gets a sane radius rather than a division by zero.
[[nodiscard]] constexpr int ResolveCoarseSearchRadius(
    const double source_period_seconds,
    const unsigned flow_scale_divisor,
    const bool performance_mode) noexcept {
    // Performance mode halves the target reach rather than pinning the radius,
    // so it stays a constant *fraction* of the reach the quality path gets at
    // the same source rate and divisor, instead of silently becoming the whole
    // search on a coarse grid.
    const double reference = performance_mode
        ? static_cast<double>(kReferenceCoarseReachPixels) / 2.0
        : static_cast<double>(kReferenceCoarseReachPixels);
    double reach = reference;
    if (source_period_seconds > 0.0) {
        reach = reference * source_period_seconds * kReferenceCoarseReachSourceFps;
    }
    if (reach > static_cast<double>(kMaximumCoarseReachPixels)) {
        reach = static_cast<double>(kMaximumCoarseReachPixels);
    }
    // A coarse cell spans twice the fine divisor and the search steps a whole
    // cell at a time, so this is the inverse of `CoarseSearchPixels`.
    const double cell_pixels =
        2.0 * static_cast<double>(flow_scale_divisor == 0U ? 1U : flow_scale_divisor);
    // `reach` is strictly positive here, so adding a half and truncating is a
    // round-half-up without reaching for a non-constexpr rounding function.
    const int rounded = static_cast<int>(reach / cell_pixels + 0.5);
    return std::clamp(rounded, kMinimumCoarseRadius, kMaximumCoarseRadius);
}

// The largest displacement that radius can find, in source pixels per pair.
// This is the number that answers "will this pan survive": compare it against
// the motion the content actually produces at the current source rate.
[[nodiscard]] constexpr unsigned CoarseSearchPixels(
    const int coarse_radius,
    const unsigned flow_scale_divisor) noexcept {
    return static_cast<unsigned>(coarse_radius < 0 ? 0 : coarse_radius) *
        flow_scale_divisor * 2U;
}

} // namespace osss
