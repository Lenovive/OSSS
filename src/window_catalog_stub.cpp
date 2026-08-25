#include "window_catalog.h"

namespace osss {

std::vector<WindowEntry> ListCapturableWindows() { return {}; }
std::vector<WindowEntry> FindWindowsMatching(const std::string&) { return {}; }
std::optional<IntRect> ExtendedWindowBounds(WindowHandle) { return std::nullopt; }
std::optional<DisplayRefreshInfo> WindowDisplayRefreshInfo(WindowHandle) { return std::nullopt; }
std::optional<double> WindowDisplayRefreshRate(WindowHandle) { return std::nullopt; }
std::string WindowTitle(WindowHandle) { return {}; }

} // namespace osss
