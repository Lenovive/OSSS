#include "window_catalog.h"

#include "platform/unicode.h"

#include <dwmapi.h>

#include <algorithm>
#include <string>
#include <vector>

namespace osss {
namespace {

bool IsCloaked(const HWND window) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(
               window,
               DWMWA_CLOAKED,
               &cloaked,
               sizeof(cloaked))) &&
           cloaked != 0;
}

std::wstring ProcessImageName(const DWORD process_id) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) {
        return {};
    }

    std::wstring path(MAX_PATH, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const BOOL queried = QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process);
    if (!queried || length == 0) {
        return {};
    }

    path.resize(length);
    const std::size_t separator = path.find_last_of(L"\\/");
    if (separator != std::wstring::npos) {
        path.erase(0, separator + 1);
    }
    return path;
}

std::string Lowercase(std::string value) {
    for (auto& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

bool EqualDeviceNames(const wchar_t* left, const wchar_t* right) {
    return left && right && _wcsicmp(left, right) == 0;
}

std::optional<FrameRate> ValidDisplayRate(const DISPLAYCONFIG_RATIONAL rate) {
    const FrameRate candidate{rate.Numerator, rate.Denominator};
    return candidate.IsValid() ? std::optional<FrameRate>(candidate) : std::nullopt;
}

std::optional<DisplayRefreshInfo> QueryRationalDisplayRate(
    const MONITORINFOEXW& monitor_information) {
    UINT32 query_flags =
        QDC_ONLY_ACTIVE_PATHS |
        QDC_VIRTUAL_MODE_AWARE |
        QDC_VIRTUAL_REFRESH_RATE_AWARE;

    for (int flag_attempt = 0; flag_attempt < 2; ++flag_attempt) {
        for (int buffer_attempt = 0; buffer_attempt < 3; ++buffer_attempt) {
            UINT32 path_count = 0;
            UINT32 mode_count = 0;
            LONG result = GetDisplayConfigBufferSizes(query_flags, &path_count, &mode_count);
            if (result == ERROR_INVALID_PARAMETER && flag_attempt == 0) {
                break;
            }
            if (result != ERROR_SUCCESS) {
                return std::nullopt;
            }

            std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
            result = QueryDisplayConfig(
                query_flags,
                &path_count,
                paths.data(),
                &mode_count,
                modes.data(),
                nullptr);
            if (result == ERROR_INSUFFICIENT_BUFFER) {
                continue;
            }
            if (result == ERROR_INVALID_PARAMETER && flag_attempt == 0) {
                break;
            }
            if (result != ERROR_SUCCESS) {
                return std::nullopt;
            }

            paths.resize(path_count);
            modes.resize(mode_count);
            for (const auto& path : paths) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name{};
                source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                source_name.header.size = sizeof(source_name);
                source_name.header.adapterId = path.sourceInfo.adapterId;
                source_name.header.id = path.sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&source_name.header) != ERROR_SUCCESS ||
                    !EqualDeviceNames(
                        source_name.viewGdiDeviceName,
                        monitor_information.szDevice)) {
                    continue;
                }

                DisplayRefreshInfo information{};
                information.gdi_device_name = ToUtf8(monitor_information.szDevice);
                information.dynamic_refresh_enabled =
                    (path.flags & DISPLAYCONFIG_PATH_BOOST_REFRESH_RATE) != 0;
                information.rational_path_available = true;

                const auto active_rate = ValidDisplayRate(path.targetInfo.refreshRate);
                UINT32 target_mode_index = path.targetInfo.modeInfoIdx;
                if ((path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) != 0) {
                    target_mode_index = path.targetInfo.targetModeInfoIdx;
                }
                if (target_mode_index != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
                    target_mode_index < modes.size() &&
                    modes[target_mode_index].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
                    information.physical_rate = ValidDisplayRate(
                        modes[target_mode_index]
                            .targetMode.targetVideoSignalInfo.vSyncFreq);
                }

                if (active_rate) {
                    information.active_rate = *active_rate;
                    return information;
                }
                if (information.physical_rate) {
                    information.active_rate = *information.physical_rate;
                    return information;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        query_flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    }
    return std::nullopt;
}

BOOL CALLBACK CollectWindow(const HWND window, const LPARAM parameter) {
    auto* entries = reinterpret_cast<std::vector<WindowEntry>*>(parameter);
    if (!IsWindowVisible(window) || IsIconic(window) || IsCloaked(window)) {
        return TRUE;
    }

    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if ((style & WS_DISABLED) != 0) {
        return TRUE;
    }

    const std::string title = WindowTitle(WindowHandle::FromNative(window));
    if (title.empty()) {
        return TRUE;
    }

    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    // Never offer our own windows as a capture target. The output overlay is a
    // visible, titled, top-level window like any other, so without this a
    // running session can list -- and be asked to capture -- itself.
    if (process_id == GetCurrentProcessId()) {
        return TRUE;
    }
    entries->push_back(WindowEntry{
        WindowHandle::FromNative(window),
        process_id,
        title,
        ToUtf8(ProcessImageName(process_id)),
    });
    return TRUE;
}

} // namespace

std::vector<WindowEntry> ListCapturableWindows() {
    std::vector<WindowEntry> entries;
    EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&entries));
    // Sort by executable first: a window titled after the file it has open ("movie.mkv -
    // VLC media player") is otherwise filed under the file name, where nobody looks for it.
    std::sort(entries.begin(), entries.end(), [](const WindowEntry& left, const WindowEntry& right) {
        const std::string left_process = Lowercase(left.process_name);
        const std::string right_process = Lowercase(right.process_name);
        if (left_process != right_process) {
            // Unidentified processes sort last rather than ahead of everything.
            if (left_process.empty() != right_process.empty()) {
                return right_process.empty();
            }
            return left_process < right_process;
        }
        return Lowercase(left.title) < Lowercase(right.title);
    });
    return entries;
}

std::vector<WindowEntry> FindWindowsMatching(const std::string& fragment) {
    return SelectWindowsMatching(ListCapturableWindows(), fragment);
}

std::optional<IntRect> ExtendedWindowBounds(const WindowHandle window) {
    const HWND handle = reinterpret_cast<HWND>(window.Native());
    if (!IsWindow(handle) || IsIconic(handle)) {
        return std::nullopt;
    }

    RECT bounds{};
    if (SUCCEEDED(DwmGetWindowAttribute(
            handle,
            DWMWA_EXTENDED_FRAME_BOUNDS,
            &bounds,
            sizeof(bounds)))) {
        return IntRect{bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top};
    }

    if (GetWindowRect(handle, &bounds)) {
        return IntRect{bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top};
    }
    return std::nullopt;
}

std::optional<DisplayRefreshInfo> WindowDisplayRefreshInfo(const WindowHandle window) {
    const HWND handle = reinterpret_cast<HWND>(window.Native());
    if (!IsWindow(handle)) {
        return std::nullopt;
    }

    const HMONITOR monitor = MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST);
    if (!monitor) {
        return std::nullopt;
    }

    MONITORINFOEXW monitor_information{};
    monitor_information.cbSize = sizeof(monitor_information);
    if (!GetMonitorInfoW(monitor, &monitor_information)) {
        return std::nullopt;
    }

    if (const auto rational = QueryRationalDisplayRate(monitor_information)) {
        return rational;
    }

    DEVMODEW display_mode{};
    display_mode.dmSize = sizeof(display_mode);
    if (!EnumDisplaySettingsW(
            monitor_information.szDevice,
            ENUM_CURRENT_SETTINGS,
            &display_mode) ||
        (display_mode.dmFields & DM_DISPLAYFREQUENCY) == 0 ||
        display_mode.dmDisplayFrequency <= 1) {
        return std::nullopt;
    }

    DisplayRefreshInfo fallback{};
    fallback.active_rate = FrameRate{
        static_cast<std::uint64_t>(display_mode.dmDisplayFrequency),
        1,
    };
    fallback.gdi_device_name = ToUtf8(monitor_information.szDevice);
    fallback.rational_path_available = false;
    return fallback;
}

std::optional<double> WindowDisplayRefreshRate(const WindowHandle window) {
    if (const auto information = WindowDisplayRefreshInfo(window)) {
        return information->active_rate.AsDouble();
    }
    return std::nullopt;
}

std::string WindowTitle(const WindowHandle window) {
    const HWND handle = reinterpret_cast<HWND>(window.Native());
    const int length = GetWindowTextLengthW(handle);
    if (length <= 0) {
        return {};
    }

    std::wstring title(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(handle, title.data(), static_cast<int>(title.size()));
    if (copied <= 0) {
        return {};
    }
    title.resize(static_cast<std::size_t>(copied));
    return ToUtf8(title);
}

} // namespace osss
