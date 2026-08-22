#pragma once

#include "adaptive_scheduler.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace osss {

// Share of submitted presents that exist only because of frame generation, in
// [0, 1]. Its complement is the native share.
//
// Two independent bounds; the answer is the smaller of them, because each one
// alone is wrong in a way the other catches.
//
//   submitted - unique   Presents the source did not supply -- the uplift a user
//                        means by "frame gen". A capture stall inflates it: hold,
//                        cold start, underrun, and stall all submit a *real*
//                        frame, so submitted stays up while unique collapses and
//                        the ratio claims generation that never happened.
//   interpolated         Presents the interpolator actually produced. Alone this
//                        reads ~100% whenever generation is active at all: an
//                        output slot almost never lands within the 1e-6 alpha
//                        snap window of a source timestamp, so nearly every slot
//                        blends -- including one at alpha 0.999 that is visually
//                        the real frame. Measured at 96-99% on a clean 60->144.
//
// min() takes the stall guard from the first and the "every slot blends" guard
// from the second. Do not simplify this to either term.
//
// Counts may also tear -- read either side of a present -- so the result is
// clamped rather than trusted.
[[nodiscard]] constexpr double GeneratedFrameShare(
    const std::uint64_t interpolated_presents,
    const std::uint64_t unique_source_frames,
    const std::uint64_t submitted_presents) noexcept {
    if (submitted_presents == 0 || unique_source_frames >= submitted_presents) {
        return 0.0;
    }
    const std::uint64_t uplift = submitted_presents - unique_source_frames;
    const std::uint64_t generated =
        interpolated_presents < uplift ? interpolated_presents : uplift;
    if (generated >= submitted_presents) {
        return 1.0;
    }
    return static_cast<double>(generated) / static_cast<double>(submitted_presents);
}

struct RuntimeStats {
    double raw_capture_fps = 0.0;
    double unique_source_fps = 0.0;
    double target_fps = 0.0;
    double submitted_fps = 0.0;
    std::optional<double> confirmed_fps;
    double required_multiplier = 0.0;
    double allowed_multiplier = 0.0;
    double realized_multiplier = 0.0;
    std::size_t queue_occupancy = 0;
    double queue_delay_milliseconds = 0.0;
    double capture_to_present_milliseconds = 0.0;
    double media_to_ingest_p50_milliseconds = 0.0;
    double media_to_ingest_p95_milliseconds = 0.0;
    double flow_gpu_p50_milliseconds = 0.0;
    double flow_gpu_p95_milliseconds = 0.0;
    double fusion_gpu_p50_milliseconds = 0.0;
    double fusion_gpu_p95_milliseconds = 0.0;
    // Present-interval distribution. Distinct from every rate above: those say
    // how many frames were submitted, these say whether they were evenly
    // spaced. A submitted rate exactly on target is compatible with badly
    // uneven spacing, and that combination is what judder looks like in
    // telemetry, so it needs its own numbers.
    double pacing_p50_milliseconds = 0.0;
    double pacing_p95_milliseconds = 0.0;
    double pacing_maximum_milliseconds = 0.0;
    double pacing_mean_absolute_error_milliseconds = 0.0;
    // Fraction of intervals within PacingMonitor::kOnTimeTolerance of the
    // target period, in [0, 1]. The single number to read first.
    double pacing_on_time_fraction = 0.0;
    std::size_t pacing_sample_count = 0;
    // Whether an output clock owns the timeline (paced and queued pacing). When
    // false the loop is free-running: there is no target period for an interval
    // to be on time against and no deadline to miss, so the HUD must not print
    // an on-time share or a missed count as if they measured something.
    bool pacing_clock_owned = true;
    // Interpolated share of this interval's submitted presents, in [0, 1]. The
    // console reports the session-cumulative equivalent; the HUD is instantaneous
    // so it tracks the rest of the panel.
    double generated_share = 0.0;
    // Whether the generation toggle is currently on. Distinct from
    // `motion_enabled`, which says *how* frames would be generated if they were:
    // this says whether any are. With it false the HUD's own rates are still
    // real measurements of a native-only session, so they stay meaningful and
    // only the header changes.
    bool generation_enabled = true;
    std::uint64_t missed_deadlines = 0;
    std::uint64_t duplicate_frames = 0;
    std::uint64_t dropped_frames = 0;
    FrameSelectionCounters selection_interval{};
    FrameSelectionCounters selection_cumulative{};
};

class StatsOverlay {
public:
    StatsOverlay() = default;
    ~StatsOverlay();

    StatsOverlay(const StatsOverlay&) = delete;
    StatsOverlay& operator=(const StatsOverlay&) = delete;

    [[nodiscard]] bool Create(
        const RECT& target_bounds,
        int max_multiplier,
        double target_fps,
        bool motion_enabled);
    void Show();
    // Hide the HUD without destroying it; `Show` restores it in place.
    void Hide();
    void Update(double source_fps, double output_fps);
    void Update(const RuntimeStats& statistics);
    void FollowTarget(HWND target);

    [[nodiscard]] HWND Window() const noexcept;

private:
    static constexpr wchar_t kWindowClassName[] = L"OSSS.StatsOverlay";
    static constexpr UINT kDefaultDpi = 96;
    static constexpr UINT kMinimumLayoutDpi = 72;
    static constexpr int kOffsetDip = 14;
    static constexpr int kWidthDip = 500;
    static constexpr int kHeightDip = 216;

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    [[nodiscard]] int ScaleDip(int value) const noexcept;
    [[nodiscard]] bool UpdateScale(UINT monitor_dpi, const RECT& target_bounds);
    void ApplyRoundedRegion(bool redraw);
    void HandleDpiChanged(UINT dpi, const RECT& suggested_bounds);
    void Paint();
    void Position(const RECT& target_bounds, bool show);
    void Release() noexcept;

    HWND window_ = nullptr;
    HFONT label_font_ = nullptr;
    HFONT value_font_ = nullptr;
    int max_multiplier_ = 2;
    double target_fps_ = 60.0;
    bool motion_enabled_ = true;
    bool has_sample_ = false;
    bool visible_ = false;
    bool window_class_registered_ = false;
    UINT layout_dpi_ = kDefaultDpi;
    RuntimeStats statistics_{};
    RECT target_bounds_{};
};

} // namespace osss
