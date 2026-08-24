#pragma once

#include "adaptive_scheduler.h"
#include "platform/int_rect.h"
#include "platform/window_handle.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace osss {

struct WindowEntry {
    WindowHandle handle{};
    std::uint32_t process_id = 0;
    // UTF-8. Titles come from the platform's wide APIs and are converted at
    // the boundary, because core code (matching, printing, tests) works in
    // UTF-8 on every platform.
    std::string title;
    // Executable file name ("vlc.exe"). Empty when the process cannot be opened.
    std::string process_name;
};

struct DisplayRefreshInfo {
    FrameRate active_rate{};
    std::optional<FrameRate> physical_rate;
    // UTF-8 (e.g. "\\.\DISPLAY1").
    std::string gdi_device_name;
    bool dynamic_refresh_enabled = false;
    bool rational_path_available = false;
};

[[nodiscard]] std::vector<WindowEntry> ListCapturableWindows();
// Matches the fragment against the window title and the executable file name.
[[nodiscard]] std::vector<WindowEntry> FindWindowsMatching(const std::string& fragment);

// The ranking behind FindWindowsMatching, split out so it can be tested without
// a desktop.
//
// A fragment is matched against both the executable name and the window title,
// because a window titled after the file it has open ("movie.mkv - VLC media
// player") is not reliably named after its app. That has one sharp edge: a
// terminal's title is the command line running in it, so launching OSSS from a
// shell puts the *shell* in the candidate set under the very name the user
// typed. `--title osss_test_animation` matched both the animation and the
// terminal that started it, and aborted as ambiguous.
//
// The rule that fixes it without losing the file-name case: an executable-name
// match is more specific than a title match, so when any candidate matches by
// executable, only those are returned. A fragment that names an app resolves to
// the app; a fragment that names a document still falls through to titles.
[[nodiscard]] std::vector<WindowEntry> SelectWindowsMatching(
    const std::vector<WindowEntry>& candidates,
    const std::string& fragment);
[[nodiscard]] std::optional<IntRect> ExtendedWindowBounds(WindowHandle window);
[[nodiscard]] std::optional<DisplayRefreshInfo> WindowDisplayRefreshInfo(WindowHandle window);
[[nodiscard]] std::optional<double> WindowDisplayRefreshRate(WindowHandle window);
[[nodiscard]] std::string WindowTitle(WindowHandle window);

} // namespace osss
