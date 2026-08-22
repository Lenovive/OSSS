#pragma once

#include <cmath>

namespace osss {

// Accepted maximum-interpolation ceiling, inclusive.
inline constexpr int kMinimumMultiplier = 2;
// The ceiling is a policy limit, not an architectural one. Flow is estimated
// once per source pair in MotionInterpolator::PreparePair and every generated
// position reuses it, so the Nth frame of a pair costs one fusion pass and
// nothing else -- per-output-frame cost *falls* as the multiplier rises.
//
// What actually bounds it is two deadlines: the flow pass must fit inside one
// source period, and each fusion must fit inside one output period. High
// multipliers are only reachable from low source rates, which is exactly where
// the flow pass has the most room, so the binding constraint is fusion against
// the output period. Measured on an RTX 5090: fusion is 0.09-0.29 ms per output
// frame at 960x540 through 1080p and 0.41-0.43 ms at 2160p, against a 1.67 ms
// output period at 600 FPS -- which is 20x from a 30 FPS source.
//
// What does not scale is the motion model. Alphas stay in [0, 1], but at 20x
// from a 30 FPS source a linear model is being asked to hold across 33 ms of
// real time, and every artifact stays on screen proportionally longer. The
// useful domain for the top of this range is low-rate content: video, emulated
// or engine-capped titles, power-limited handhelds.
inline constexpr int kMaximumMultiplier = 20;

// Accepted manual output-target range in frames per second, inclusive.
inline constexpr double kMinimumTargetFps = 24.0;
inline constexpr double kMaximumTargetFps = 1000.0;

[[nodiscard]] constexpr bool IsValidMultiplier(const int multiplier) noexcept {
    return multiplier >= kMinimumMultiplier && multiplier <= kMaximumMultiplier;
}

[[nodiscard]] inline bool IsValidTargetFps(const double target_fps) noexcept {
    return std::isfinite(target_fps) &&
        target_fps >= kMinimumTargetFps &&
        target_fps <= kMaximumTargetFps;
}

} // namespace osss
