#pragma once

#include <optional>
#include <string_view>

namespace osss {

// Runtime visualisation of the interpolator's own internals.
//
// This exists because of how every motion defect in this project was actually
// found. All seven were diagnosed by temporarily editing the fusion shader to
// return one of its intermediates as colour, rebuilding, looking, and reverting
// -- the tie-break that made `EstimateCoarse` pick a 45-pixel vector, the flow
// field quantised to the search step, the coarse level aliasing on its own grid,
// the fusion sampling flow at the wrong position, the static-pixel test that
// erased a moving marker, the bilinear warp softening it, and the fixed-point
// iteration that oscillated at motion boundaries. Seven times through the same
// edit-rebuild-revert loop is enough evidence that it should be a flag.
//
// These are diagnostics, not output modes. They deliberately replace the frame
// rather than overlaying it, because a legible visualisation of a flow field is
// not something that can share pixels with the scene it describes.
//
//   off         normal output.
//   flow        forward flow as direction and magnitude. Hue is the direction a
//               pixel came from, brightness is how far. Grey means no motion;
//               large flat areas of one hue mean a camera pan; speckle means the
//               estimator is not agreeing with itself, which is what a broken
//               tie-break looks like from across the room.
//   confidence  the fused weight that decides between the warps and the
//               crossfade fallback. White is a confident reconstruction, black
//               is a pixel that fell back. A frame that is mostly black is the
//               signature of the `confidence_floor` failure this repo has hit
//               before -- output that looks like a crossfade because it *is* a
//               crossfade.
//   fallback    which safeguard claimed each pixel, as separate channels: red
//               where confidence fell below the floor, green where the UI mask
//               or the automatic detector took over, blue where static-pixel
//               protection held the crossfade. Answers "why is this region not
//               being interpolated" directly.
//
// Dependency-free for the same reason present_mode.h, flow_scale.h, and
// output_mode.h are: `osss_gui.exe` names one on a command line.
enum class DebugView {
    off,
    flow,
    confidence,
    fallback,
};

[[nodiscard]] constexpr const wchar_t* DebugViewArgument(const DebugView view) noexcept {
    switch (view) {
    case DebugView::flow:
        return L"flow";
    case DebugView::confidence:
        return L"confidence";
    case DebugView::fallback:
        return L"fallback";
    case DebugView::off:
        break;
    }
    return L"off";
}

[[nodiscard]] constexpr const char* DebugViewName(const DebugView view) noexcept {
    switch (view) {
    case DebugView::flow:
        return "flow";
    case DebugView::confidence:
        return "confidence";
    case DebugView::fallback:
        return "fallback";
    case DebugView::off:
        break;
    }
    return "off";
}

// The value the fusion shader branches on. Kept next to the enum so the two
// cannot drift; the shader compares against these as floats.
[[nodiscard]] constexpr float DebugViewShaderValue(const DebugView view) noexcept {
    switch (view) {
    case DebugView::flow:
        return 1.0F;
    case DebugView::confidence:
        return 2.0F;
    case DebugView::fallback:
        return 3.0F;
    case DebugView::off:
        break;
    }
    return 0.0F;
}

[[nodiscard]] inline std::optional<DebugView> ParseDebugView(
    const std::wstring_view value) noexcept {
    if (value == L"off" || value == L"none") {
        return DebugView::off;
    }
    if (value == L"flow" || value == L"motion") {
        return DebugView::flow;
    }
    if (value == L"confidence") {
        return DebugView::confidence;
    }
    if (value == L"fallback" || value == L"rejected") {
        return DebugView::fallback;
    }
    return std::nullopt;
}

} // namespace osss
