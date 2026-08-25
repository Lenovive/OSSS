#include "window_catalog.h"

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace osss {
namespace {

std::string StringValue(NSDictionary* dictionary, NSString* key) {
    NSString* value = dictionary[key];
    if (![value isKindOfClass:[NSString class]]) {
        return {};
    }
    const char* utf8 = value.UTF8String;
    return utf8 ? std::string(utf8) : std::string{};
}

std::uint32_t NumberValue(NSDictionary* dictionary, NSString* key) {
    NSNumber* value = dictionary[key];
    return [value isKindOfClass:[NSNumber class]] ? value.unsignedIntValue : 0;
}

std::optional<IntRect> RectValue(NSDictionary* dictionary) {
    NSDictionary* value = dictionary[(NSString*)kCGWindowBounds];
    if (![value isKindOfClass:[NSDictionary class]]) {
        return std::nullopt;
    }
    CGRect rect{};
    if (!CGRectMakeWithDictionaryRepresentation(
            (__bridge CFDictionaryRef)value,
            &rect)) {
        return std::nullopt;
    }
    if (rect.size.width <= 0.0 || rect.size.height <= 0.0) {
        return std::nullopt;
    }
    return IntRect{
        static_cast<int>(rect.origin.x),
        static_cast<int>(rect.origin.y),
        static_cast<int>(rect.size.width),
        static_cast<int>(rect.size.height)};
}

NSArray<NSDictionary*>* WindowInfo() {
    return CFBridgingRelease(CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID));
}

CGDirectDisplayID DisplayForBounds(const IntRect bounds) {
    CGDirectDisplayID displays[16]{};
    uint32_t display_count = 0;
    if (CGGetActiveDisplayList(16, displays, &display_count) == kCGErrorSuccess) {
        const CGPoint point = CGPointMake(
            static_cast<CGFloat>(bounds.x), static_cast<CGFloat>(bounds.y));
        for (uint32_t index = 0; index < display_count; ++index) {
            if (CGRectContainsPoint(CGDisplayBounds(displays[index]), point)) {
                return displays[index];
            }
        }
    }
    return CGMainDisplayID();
}

} // namespace

std::vector<WindowEntry> ListCapturableWindows() {
    std::vector<WindowEntry> result;
    for (NSDictionary* dictionary in WindowInfo()) {
        if (NumberValue(dictionary, (NSString*)kCGWindowLayer) != 0) {
            continue;
        }
        const auto bounds = RectValue(dictionary);
        const auto window_id = NumberValue(dictionary, (NSString*)kCGWindowNumber);
        if (!bounds || window_id == 0) {
            continue;
        }
        const auto pid = NumberValue(dictionary, (NSString*)kCGWindowOwnerPID);
        std::string process = StringValue(dictionary, (NSString*)kCGWindowOwnerName);
        if (NSRunningApplication* application =
                [NSRunningApplication runningApplicationWithProcessIdentifier:pid]) {
            if (application.executableURL.lastPathComponent.UTF8String) {
                process = application.executableURL.lastPathComponent.UTF8String;
            }
        }
        result.push_back(WindowEntry{
            WindowHandle::FromNative(static_cast<std::uint32_t>(window_id)),
            pid,
            StringValue(dictionary, (NSString*)kCGWindowName),
            std::move(process)});
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
    const std::uint32_t wanted = static_cast<std::uint32_t>(window.Native());
    for (NSDictionary* dictionary in WindowInfo()) {
        if (NumberValue(dictionary, (NSString*)kCGWindowNumber) == wanted) {
            return RectValue(dictionary);
        }
    }
    return std::nullopt;
}

std::optional<DisplayRefreshInfo> WindowDisplayRefreshInfo(const WindowHandle window) {
    const auto bounds = ExtendedWindowBounds(window);
    if (!bounds) {
        return std::nullopt;
    }
    DisplayRefreshInfo result;
    const CGDirectDisplayID display = DisplayForBounds(*bounds);
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    const double refresh = mode ? CGDisplayModeGetRefreshRate(mode) : 0.0;
    if (mode) {
        CGDisplayModeRelease(mode);
    }
    result.active_rate = FrameRate::FromFps(refresh > 1.0 ? refresh : 60.0);
    result.rational_path_available = false;
    result.gdi_device_name = "CoreGraphics";
    return result;
}

std::optional<double> WindowDisplayRefreshRate(const WindowHandle window) {
    const auto info = WindowDisplayRefreshInfo(window);
    return info ? std::optional<double>(info->active_rate.AsDouble()) : std::nullopt;
}

std::string WindowTitle(const WindowHandle window) {
    const std::uint32_t wanted = static_cast<std::uint32_t>(window.Native());
    for (NSDictionary* dictionary in WindowInfo()) {
        if (NumberValue(dictionary, (NSString*)kCGWindowNumber) == wanted) {
            return StringValue(dictionary, (NSString*)kCGWindowName);
        }
    }
    return {};
}

} // namespace osss

#endif
