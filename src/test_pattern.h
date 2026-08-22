#pragma once

#include "test_animation_catalog.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace osss {

struct TestPatternSpec {
    static constexpr std::uint32_t kDefaultWidth = 960;
    static constexpr std::uint32_t kDefaultHeight = 540;
    static constexpr std::uint32_t kMachineHeaderHeight = 64;
    static constexpr std::uint32_t kScoredTop = 112;
    static constexpr double kCycleSeconds = 6.0;

    std::uint32_t width = kDefaultWidth;
    std::uint32_t height = kDefaultHeight;
    TestGraphicsApi api = TestGraphicsApi::direct3d11;
    int base_fps = 60;
    // Draws static-position HUD panels over the moving lanes. Their content is
    // a function of the source frame index alone, so it steps at source cadence
    // and never takes an intermediate value: any temporal blending of the HUD
    // produces a colour that matches no valid state. Off by default so the
    // historical pattern and its scores are unchanged.
    bool hud_overlay = false;
};

// The three failure surfaces of the pattern, in source pixels. They fail in
// different ways -- translation, occlusion, and thin high-frequency detail --
// and a single whole-frame number lets a regression in one hide behind a gain
// in another, which is exactly what happened to the interpolator before the
// quality bench existed.
//
// Defined here rather than in either bench so the two cannot drift: the
// reference-image bench and the burst scorer must describe the same bands or
// their numbers are not comparable, and comparing them is the point.
struct TestPatternLane {
    const char* name;
    std::uint32_t top;
    std::uint32_t bottom;
};

inline constexpr TestPatternLane kTestPatternLanes[] = {
    {"linear", 82, 202},
    {"occlusion", 226, 346},
    {"detail", 370, 518},
};

struct TestPatternRect {
    std::uint32_t left = 0;
    std::uint32_t top = 0;
    std::uint32_t right = 0;
    std::uint32_t bottom = 0;

    [[nodiscard]] bool operator==(const TestPatternRect&) const noexcept = default;
};

// Result of checking a captured HUD region against the discrete states it is
// allowed to hold.
struct HudOverlayCheck {
    // True when the region exactly matches one source frame's HUD content.
    bool matches_discrete_state = false;
    std::uint64_t matched_source_frame_index = 0;
    // Largest channel error against the closest valid state. A cross-faded HUD
    // sits far from every state and reports a large value here.
    std::uint8_t maximum_channel_error = 255;
    std::uint64_t compared_pixels = 0;
};

struct TestPatternMetrics {
    double mean_absolute_error = 0.0;
    double root_mean_square_error = 0.0;
    double psnr_db = std::numeric_limits<double>::infinity();
    double pixels_over_threshold_percent = 0.0;
    std::uint8_t maximum_channel_error = 0;
    std::uint64_t compared_pixels = 0;
};

[[nodiscard]] std::vector<std::uint32_t> RenderTestPattern(
    const TestPatternSpec& specification,
    double animation_seconds,
    std::uint64_t source_frame_index);

// HUD panel bounds for this specification, in source pixels. Empty unless
// `hud_overlay` is set. Both panels sit below `kScoredTop` and over moving
// content, so they are scored unless explicitly excluded.
[[nodiscard]] std::vector<TestPatternRect> TestPatternHudRects(
    const TestPatternSpec& specification);

// The same regions written in OSSS `--ui-mask` pixel syntax.
[[nodiscard]] std::string TestPatternHudMaskArgument(const TestPatternSpec& specification);

// Checks the HUD regions of a captured frame against every source-frame state
// in [first_source_frame_index, last_source_frame_index]. A correctly masked
// HUD matches one state exactly; an interpolated one matches none.
[[nodiscard]] HudOverlayCheck CheckTestPatternHud(
    std::span<const std::uint32_t> observed,
    const TestPatternSpec& specification,
    std::uint64_t first_source_frame_index,
    std::uint64_t last_source_frame_index,
    std::uint8_t exact_match_tolerance = 8);

// Signed distance, in milliseconds, from `animation_seconds` to the nearest
// instant at which the source presents a real frame (k / base_fps). The source
// renders each frame for exactly that instant, so a captured real frame -- or a
// real frame OSSS held or re-presented -- best-matches at a phase of ~0, while
// a generated frame matches strictly between two source instants. Negative
// means the instant precedes the nearest source frame.
[[nodiscard]] double SourceFramePhaseMilliseconds(double animation_seconds, int base_fps);

// True when a matched instant is close enough to a source frame instant to be a
// real frame rather than a generated one. `tolerance_milliseconds` should
// exceed the temporal-search resolution and stay well under the smallest
// generated-frame spacing (a 120 FPS source at 6x is 1.39 ms).
[[nodiscard]] bool IsSourceFrameInstant(
    double animation_seconds,
    int base_fps,
    double tolerance_milliseconds = 0.5);

[[nodiscard]] TestPatternMetrics CompareTestPatternFrames(
    std::span<const std::uint32_t> expected,
    std::span<const std::uint32_t> observed,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t scored_top = TestPatternSpec::kScoredTop,
    std::uint8_t bad_pixel_threshold = 8,
    std::uint32_t sampling_stride = 1,
    std::span<const TestPatternRect> excluded_regions = {});

bool WriteTestPatternPpm(
    const std::filesystem::path& path,
    std::span<const std::uint32_t> pixels,
    std::uint32_t width,
    std::uint32_t height,
    std::string& error);

bool ReadTestPatternPpm(
    const std::filesystem::path& path,
    std::vector<std::uint32_t>& pixels,
    std::uint32_t& width,
    std::uint32_t& height,
    std::string& error);

} // namespace osss
