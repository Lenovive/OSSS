#pragma once

#include <optional>
#include <string_view>

namespace osss {

// The shape of the output window, which decides whether the display can follow
// OSSS's presents or only DWM's.
//
// A variable-refresh display only follows an application when that
// application's swap chain drives scanout directly. On Windows there are two
// ways in: legacy exclusive fullscreen, or **independent flip**, where DWM
// notices that one opaque window covers an entire output and hands it the
// display plane instead of compositing it. Independent flip has a hard
// checklist and it fails silently, so the mode is named and its eligibility is
// reported rather than assumed.
//
//   overlay     A click-through surface sized to the target window, layered so
//               clicks reach the game underneath. `WS_EX_LAYERED` is what makes
//               that work -- measured, not assumed, in
//               tests/input_passthrough_smoke.cpp -- and a layered window is
//               composed through a redirection surface whatever the swap chain
//               asks for. So this mode can never reach independent flip, and
//               `--present-mode tearing` here removes OSSS's own vblank
//               quantization rather than handing timing to the display.
//   fullscreen  An opaque, non-layered window covering the whole monitor, which
//               DWM can promote to independent flip. That is what makes G-Sync
//               and FreeSync follow the output clock. The cost is real and is
//               the reason this is not the default: without `WS_EX_LAYERED`
//               nothing passes a click to another process's window. The game
//               keeps keyboard focus because the window never activates, and
//               titles reading raw input or clipping the cursor are unaffected
//               -- but a game that needs ordinary mouse messages delivered to
//               its own window will not receive them while this is on.
//
// This header is deliberately dependency-free for the same reason
// present_mode.h, flow_scale.h, and frame_rate_limits.h are: `osss_gui.exe` has
// to name a mode on a command line without acquiring a Direct3D dependency.
enum class OutputMode {
    overlay,
    fullscreen,
};

// The spelling `osss.exe --output-mode` accepts. Kept next to the parser so the
// two cannot drift apart.
[[nodiscard]] constexpr const wchar_t* OutputModeArgument(const OutputMode mode) noexcept {
    switch (mode) {
    case OutputMode::fullscreen:
        return L"fullscreen";
    case OutputMode::overlay:
        break;
    }
    return L"overlay";
}

// The same names in narrow characters, for diagnostics and banners.
[[nodiscard]] constexpr const char* OutputModeName(const OutputMode mode) noexcept {
    switch (mode) {
    case OutputMode::fullscreen:
        return "fullscreen";
    case OutputMode::overlay:
        break;
    }
    return "overlay";
}

// Accepts the canonical spellings plus the aliases users reach for first.
// Returns nullopt for anything else; the caller decides how to complain.
[[nodiscard]] inline std::optional<OutputMode> ParseOutputMode(
    const std::wstring_view value) noexcept {
    if (value == L"overlay" || value == L"window" || value == L"windowed") {
        return OutputMode::overlay;
    }
    if (value == L"fullscreen" || value == L"exclusive" || value == L"borderless") {
        return OutputMode::fullscreen;
    }
    return std::nullopt;
}

// Whether this shape can be promoted out of DWM composition at all. Being
// eligible is necessary and not sufficient: the swap chain must also be
// flip-model with tearing allowed, the window must exactly cover the output,
// and nothing may be composited above it. DWM decides, and reports nothing --
// PresentMon's per-frame present mode is the only way to confirm it happened.
[[nodiscard]] constexpr bool OutputModeCanReachIndependentFlip(const OutputMode mode) noexcept {
    return mode == OutputMode::fullscreen;
}

} // namespace osss
