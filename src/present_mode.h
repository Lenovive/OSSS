#pragma once

#include <optional>
#include <string_view>

namespace osss {

// How a finished frame reaches the display. Frame pacing is mostly a property of
// this choice, so it is named, selected, and reported rather than implied by a
// hard-coded Present() argument.
//
//   vsync    Present(1). Every present waits for a vblank. Tear-free, and the
//            only correct choice on a fixed-refresh panel -- but it quantizes
//            output to the refresh period, so a target rate that is not a
//            divisor of the refresh rate beats against it. A 120 FPS target on a
//            144 Hz panel alternates 6.9 ms and 13.9 ms frames indefinitely.
//   tearing  Present(0, DXGI_PRESENT_ALLOW_TEARING). The rational output clock
//            alone decides when a frame appears, so an arbitrary target rate is
//            paced exactly. This is also what G-Sync and FreeSync require: with
//            variable refresh on, the display follows the presents and nothing
//            tears. With it off, it tears.
//   automatic  tearing wherever DXGI reports support, else vsync.
//
// This header is deliberately dependency-free. `osss.exe` resolves the mode
// against a real adapter, but `osss_gui.exe` only has to name one on a command
// line and must not acquire a Direct3D dependency to do it -- the same reason
// frame_rate_limits.h exists.
enum class PresentMode {
    automatic,
    vsync,
    tearing,
};

// The spelling `osss.exe --present-mode` accepts for a mode. Kept next to the
// parser so the two cannot drift apart.
[[nodiscard]] constexpr const wchar_t* PresentModeArgument(const PresentMode mode) noexcept {
    switch (mode) {
    case PresentMode::tearing:
        return L"tearing";
    case PresentMode::vsync:
        return L"vsync";
    case PresentMode::automatic:
        break;
    }
    return L"auto";
}

// The same names in narrow characters for portable diagnostics and banners.
[[nodiscard]] constexpr const char* PresentModeName(const PresentMode mode) noexcept {
    switch (mode) {
    case PresentMode::tearing:
        return "tearing";
    case PresentMode::vsync:
        return "vsync";
    case PresentMode::automatic:
        break;
    }
    return "auto";
}

// Accepts the canonical spellings plus the aliases users reach for first: `vrr`
// and `gsync` for tearing, and `on`/`off` read as vsync on and vsync off.
// Returns nullopt for anything else; the caller decides how to complain.
[[nodiscard]] inline std::optional<PresentMode> ParsePresentMode(
    const std::wstring_view value) noexcept {
    if (value == L"auto" || value == L"automatic") {
        return PresentMode::automatic;
    }
    if (value == L"vsync" || value == L"on") {
        return PresentMode::vsync;
    }
    if (value == L"tearing" || value == L"vrr" || value == L"gsync" || value == L"off") {
        return PresentMode::tearing;
    }
    return std::nullopt;
}

} // namespace osss
