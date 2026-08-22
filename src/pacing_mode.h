#pragma once

#include <optional>
#include <string_view>

namespace osss {

// When, relative to the output clock, a finished frame is rendered and handed
// over. `--present-mode` decides *how* a present reaches the display; this
// decides *when* the loop presents at all, and how much work it is allowed to
// have in flight ahead of that moment.
//
//   unpaced  No deadline grid. Whenever the swap chain has a free back buffer
//            the loop selects for "now", renders, and presents. Output cadence
//            is whatever the swap chain permits: refresh-locked under vsync,
//            composition-limited for the DWM-composed overlay under tearing,
//            genuinely free-running only for fullscreen independent flip.
//            Nothing waits for a slot, so nothing is ever late, and nothing is
//            evenly spaced either: the first slot of every source pair carries
//            the optical-flow pass and lands later than the ones after it. It
//            exists as a measurement floor and for parity with tools that
//            offer it, not as something to run for its own sake.
//   paced    A rational output clock owns the timeline. Each slot's frame is
//            selected, rendered, and presented at its deadline, with maximum
//            frame latency one so a queued frame can never add hidden delay. A
//            slot whose back buffer is not free is dropped and counted, never
//            queued for catch-up. The default, and the behaviour every
//            measurement in this repo was taken with.
//   queued   The same clock, but the loop renders one slot ahead: as soon as
//            slot k is handed over, slot k+1 is selected and rendered, and at
//            its deadline only Present() runs. Maximum frame latency two and a
//            third back buffer make that legal. The flow pass leaves the
//            critical path entirely -- a slow pair costs nothing unless it
//            exceeds a whole slot -- at a price of exactly one *target* slot of
//            extra media delay, because slot k+1's source frames must already
//            be in hand when slot k is presented. Frames are still handed over
//            at their deadlines, never early, which is what keeps this correct
//            under tearing where an early present would simply appear early.
//
// This header is deliberately dependency-free for the same reason
// present_mode.h and output_mode.h are: `osss_gui.exe` has to name a mode on a
// command line without acquiring a Direct3D dependency.
enum class PacingMode {
    unpaced,
    paced,
    queued,
};

// The spelling `osss.exe --pacing` accepts for a mode. Kept next to the parser
// so the two cannot drift apart.
[[nodiscard]] constexpr const wchar_t* PacingModeArgument(const PacingMode mode) noexcept {
    switch (mode) {
    case PacingMode::unpaced:
        return L"unpaced";
    case PacingMode::queued:
        return L"queued";
    case PacingMode::paced:
        break;
    }
    return L"paced";
}

// The same names in narrow characters, for diagnostics and banners.
[[nodiscard]] constexpr const char* PacingModeName(const PacingMode mode) noexcept {
    switch (mode) {
    case PacingMode::unpaced:
        return "unpaced";
    case PacingMode::queued:
        return "queued";
    case PacingMode::paced:
        break;
    }
    return "paced";
}

// Accepts the canonical spellings plus the aliases users reach for first:
// `off`/`free` for unpaced, `on` for paced, `queue`/`render-ahead` for queued.
// Returns nullopt for anything else; the caller decides how to complain.
[[nodiscard]] inline std::optional<PacingMode> ParsePacingMode(
    const std::wstring_view value) noexcept {
    if (value == L"unpaced" || value == L"off" || value == L"free") {
        return PacingMode::unpaced;
    }
    if (value == L"paced" || value == L"on") {
        return PacingMode::paced;
    }
    if (value == L"queued" || value == L"queue" || value == L"render-ahead") {
        return PacingMode::queued;
    }
    return std::nullopt;
}

// The two mechanism facts each mode implies. Named here rather than re-derived
// in the renderer and the presentation loop separately, so the swap chain and
// the loop that feeds it cannot disagree about how many frames may be in
// flight.
//
// DXGI's frame-latency waitable object signals while fewer than this many
// presents are outstanding. One means "render only when the previous frame has
// been consumed", which is what keeps a paced or unpaced present from queueing
// hidden latency; two is what lets the queued mode render slot k+1 while slot k
// is still waiting to be shown.
[[nodiscard]] constexpr unsigned PacingModeMaximumFrameLatency(const PacingMode mode) noexcept {
    return mode == PacingMode::queued ? 2U : 1U;
}

// Whether the loop is expected to render a slot before its deadline arrives.
[[nodiscard]] constexpr bool PacingModeRendersAhead(const PacingMode mode) noexcept {
    return mode == PacingMode::queued;
}

// Whether a rational output clock owns the timeline at all. When false there
// are no deadlines to miss, and the pacing telemetry measures the interval
// between presents rather than deadline-to-handover.
[[nodiscard]] constexpr bool PacingModeUsesOutputClock(const PacingMode mode) noexcept {
    return mode != PacingMode::unpaced;
}

// How many target slots of source lookahead the selector needs, on top of the
// media queue, for the mode to work without underrunning: the queued mode
// selects slot k+1 while slot k is being presented, so k+1's bracketing source
// frames must be one slot earlier than the media queue alone provides.
[[nodiscard]] constexpr unsigned PacingModeLookaheadSlots(const PacingMode mode) noexcept {
    return mode == PacingMode::queued ? 1U : 0U;
}

} // namespace osss
