#include "window_catalog.h"

#if defined(__linux__)

#include "platform/unicode.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#if defined(OSSS_HAVE_XRANDR)
#  include <X11/extensions/Xrandr.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace osss {
namespace {

class DisplayGuard {
public:
    DisplayGuard() : display_(XOpenDisplay(nullptr)) {}
    ~DisplayGuard() {
        if (display_) {
            XCloseDisplay(display_);
        }
    }
    DisplayGuard(const DisplayGuard&) = delete;
    DisplayGuard& operator=(const DisplayGuard&) = delete;
    [[nodiscard]] Display* Get() const noexcept { return display_; }

private:
    Display* display_ = nullptr;
};

std::string PropertyText(Display* display, const ::Window window, const char* name) {
    const Atom property = XInternAtom(display, name, True);
    if (property == None) {
        return {};
    }
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* raw = nullptr;
    const int result = XGetWindowProperty(
        display,
        window,
        property,
        0,
        4096,
        False,
        AnyPropertyType,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &raw);
    if (result != Success || !raw || actual_format != 8) {
        if (raw) {
            XFree(raw);
        }
        return {};
    }
    std::string value(reinterpret_cast<char*>(raw), item_count);
    XFree(raw);
    return value;
}

std::uint32_t PropertyCardinal(Display* display, const ::Window window, const char* name) {
    const Atom property = XInternAtom(display, name, True);
    if (property == None) {
        return 0;
    }
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* raw = nullptr;
    const int result = XGetWindowProperty(
        display,
        window,
        property,
        0,
        1,
        False,
        XA_CARDINAL,
        &actual_type,
        &actual_format,
        &item_count,
        &bytes_after,
        &raw);
    if (result != Success || !raw || actual_format != 32 || item_count == 0) {
        if (raw) {
            XFree(raw);
        }
        return 0;
    }
    const auto value = static_cast<std::uint32_t*>(static_cast<void*>(raw))[0];
    XFree(raw);
    return value;
}

std::string ProcessName(const std::uint32_t pid) {
    if (pid == 0) {
        return {};
    }
    std::error_code code;
    const auto executable = std::filesystem::read_symlink(
        std::filesystem::path("/proc") / std::to_string(pid) / "exe", code);
    if (!code && !executable.empty()) {
        return executable.filename().string();
    }
    std::ifstream comm(std::filesystem::path("/proc") / std::to_string(pid) / "comm");
    std::string name;
    std::getline(comm, name);
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
        name.pop_back();
    }
    return name;
}

std::vector<::Window> ClientWindows(Display* display) {
    const ::Window root = DefaultRootWindow(display);
    const Atom client_list = XInternAtom(display, "_NET_CLIENT_LIST", True);
    std::vector<::Window> windows;
    auto query_tree_fallback = [&]() {
        ::Window root_return = 0;
        ::Window parent_return = 0;
        ::Window* children = nullptr;
        unsigned int child_count = 0;
        if (XQueryTree(
                display,
                root,
                &root_return,
                &parent_return,
                &children,
                &child_count)) {
            windows.reserve(child_count);
            for (unsigned int index = 0; index < child_count; ++index) {
                windows.push_back(children[index]);
            }
        }
        if (children) {
            XFree(children);
        }
    };
    if (client_list == None) {
        query_tree_fallback();
        return windows;
    }
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char* raw = nullptr;
    if (XGetWindowProperty(
            display,
            root,
            client_list,
            0,
            16384,
            False,
            XA_WINDOW,
            &actual_type,
            &actual_format,
            &item_count,
            &bytes_after,
            &raw) != Success || !raw || actual_format != 32) {
        if (raw) {
            XFree(raw);
        }
        query_tree_fallback();
        return windows;
    }
    windows.reserve(item_count);
    const auto* values = static_cast<::Window*>(static_cast<void*>(raw));
    for (unsigned long index = 0; index < item_count; ++index) {
        windows.push_back(values[index]);
    }
    XFree(raw);
    return windows;
}

std::optional<IntRect> Bounds(Display* display, const ::Window window) {
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(display, window, &attributes) ||
        attributes.map_state == IsUnmapped || attributes.c_class == InputOnly) {
        return std::nullopt;
    }
    int root_x = 0;
    int root_y = 0;
    ::Window child = 0;
    if (!XTranslateCoordinates(
            display,
            window,
            DefaultRootWindow(display),
            0,
            0,
            &root_x,
            &root_y,
            &child)) {
        return std::nullopt;
    }
    if (attributes.width <= 0 || attributes.height <= 0) {
        return std::nullopt;
    }
    return IntRect{root_x, root_y, attributes.width, attributes.height};
}

#if defined(OSSS_HAVE_XRANDR)
double ActiveRefreshRate(Display* display, const IntRect target_bounds) {
    const ::Window root = DefaultRootWindow(display);
    XRRScreenResources* resources = XRRGetScreenResourcesCurrent(display, root);
    if (!resources) {
        return 0.0;
    }
    double result = 0.0;
    double target_result = 0.0;
    for (int index = 0; index < resources->ncrtc; ++index) {
        XRRCrtcInfo* crtc = XRRGetCrtcInfo(display, resources, resources->crtcs[index]);
        if (!crtc || crtc->mode == None || crtc->noutput == 0) {
            if (crtc) {
                XRRFreeCrtcInfo(crtc);
            }
            continue;
        }
        for (int mode_index = 0; mode_index < resources->nmode; ++mode_index) {
            const XRRModeInfo& mode = resources->modes[mode_index];
            if (mode.id != crtc->mode || mode.hTotal == 0 || mode.vTotal == 0) {
                continue;
            }
            const double refresh = static_cast<double>(mode.dotClock) /
                (static_cast<double>(mode.hTotal) * static_cast<double>(mode.vTotal));
            result = std::max(result, refresh);
            const int crtc_right = crtc->x + static_cast<int>(crtc->width);
            const int crtc_bottom = crtc->y + static_cast<int>(crtc->height);
            if (target_bounds.x >= crtc->x && target_bounds.x < crtc_right &&
                target_bounds.y >= crtc->y && target_bounds.y < crtc_bottom) {
                target_result = std::max(target_result, refresh);
            }
            break;
        }
        XRRFreeCrtcInfo(crtc);
    }
    XRRFreeScreenResources(resources);
    return target_result > 1.0 ? target_result : result;
}
#endif

} // namespace

std::vector<WindowEntry> ListCapturableWindows() {
    DisplayGuard guard;
    Display* display = guard.Get();
    if (!display) {
        return {};
    }
    std::vector<WindowEntry> result;
    for (const ::Window window : ClientWindows(display)) {
        const auto bounds = Bounds(display, window);
        if (!bounds) {
            continue;
        }
        std::string title = PropertyText(display, window, "_NET_WM_NAME");
        if (title.empty()) {
            char* raw_title = nullptr;
            if (XFetchName(display, window, &raw_title) && raw_title) {
                title = raw_title;
                XFree(raw_title);
            }
        }
        const auto pid = PropertyCardinal(display, window, "_NET_WM_PID");
        result.push_back(WindowEntry{
            WindowHandle::FromNative(window),
            pid,
            std::move(title),
            ProcessName(pid)});
    }
    std::sort(result.begin(), result.end(), [](const WindowEntry& left, const WindowEntry& right) {
        if (left.process_name != right.process_name) {
            return left.process_name < right.process_name;
        }
        return left.title < right.title;
    });
    return result;
}

std::vector<WindowEntry> FindWindowsMatching(const std::string& fragment) {
    return SelectWindowsMatching(ListCapturableWindows(), fragment);
}

std::optional<IntRect> ExtendedWindowBounds(const WindowHandle window) {
    DisplayGuard guard;
    return guard.Get() ? Bounds(guard.Get(), static_cast<::Window>(window.Native())) : std::nullopt;
}

std::optional<DisplayRefreshInfo> WindowDisplayRefreshInfo(const WindowHandle window) {
    DisplayGuard guard;
    std::optional<IntRect> bounds;
    if (guard.Get()) {
        bounds = Bounds(guard.Get(), static_cast<::Window>(window.Native()));
    }
    if (!bounds) {
        return std::nullopt;
    }
    DisplayRefreshInfo result;
#if defined(OSSS_HAVE_XRANDR)
    const double refresh = ActiveRefreshRate(guard.Get(), *bounds);
#else
    const double refresh = 0.0;
#endif
    result.active_rate = FrameRate::FromFps(refresh > 1.0 ? refresh : 60.0);
    result.rational_path_available = false;
    result.gdi_device_name = "X11";
    return result;
}

std::optional<double> WindowDisplayRefreshRate(const WindowHandle window) {
    const auto info = WindowDisplayRefreshInfo(window);
    return info ? std::optional<double>(info->active_rate.AsDouble()) : std::nullopt;
}

std::string WindowTitle(const WindowHandle window) {
    DisplayGuard guard;
    if (!guard.Get()) {
        return {};
    }
    std::string title = PropertyText(
        guard.Get(), static_cast<::Window>(window.Native()), "_NET_WM_NAME");
    if (!title.empty()) {
        return title;
    }
    char* raw_title = nullptr;
    if (XFetchName(guard.Get(), static_cast<::Window>(window.Native()), &raw_title) && raw_title) {
        title = raw_title;
        XFree(raw_title);
    }
    return title;
}

} // namespace osss

#endif
