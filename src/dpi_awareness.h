#pragma once

#include <windows.h>

namespace osss {

[[nodiscard]] inline bool IsPerMonitorV2DpiAware() noexcept {
    return AreDpiAwarenessContextsEqual(
               GetThreadDpiAwarenessContext(),
               DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
}

[[nodiscard]] inline bool EnablePerMonitorV2DpiAwareness() noexcept {
    if (IsPerMonitorV2DpiAware()) {
        return true;
    }
    return SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
}

} // namespace osss
