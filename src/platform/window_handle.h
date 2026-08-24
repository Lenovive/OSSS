#pragma once

#include <cstdint>
#include <type_traits>

namespace osss {

// Platform-neutral stand-in for a native window handle (HWND on Windows, an
// NSView pointer on macOS, a Window id on X11). The window catalog, the
// stats overlay, and the renderer all talk in handles, so they can all be
// written against this one type and converted to the native type at the
// platform boundary -- FromNative on the way in, Native on the way out.
//
// The value is an intptr_t, not a pointer: X11 window ids are integer
// handles, and a pointer member would need platform-specific null handling
// for no benefit on the pointer platforms.
class WindowHandle {
public:
    constexpr WindowHandle() noexcept = default;

    // Accepts any pointer or integral native type. The pointer arm exists for
    // HWND and Cocoa; the integral arm for X11. A pointer must be a valid
    // value to reinterpret: callers pass the raw handle, never a derived one.
    template <typename NativeT>
    [[nodiscard]] static constexpr WindowHandle FromNative(const NativeT native) noexcept {
        if constexpr (std::is_pointer_v<NativeT>) {
            return WindowHandle{reinterpret_cast<std::intptr_t>(native)};
        } else {
            return WindowHandle{static_cast<std::intptr_t>(native)};
        }
    }

    [[nodiscard]] constexpr std::intptr_t Native() const noexcept {
        return value_;
    }

    [[nodiscard]] constexpr bool IsNull() const noexcept {
        return value_ == 0;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value_ != 0;
    }

    friend constexpr bool operator==(const WindowHandle& a, const WindowHandle& b) noexcept {
        return a.value_ == b.value_;
    }

    friend constexpr bool operator!=(const WindowHandle& a, const WindowHandle& b) noexcept {
        return !(a == b);
    }

private:
    constexpr explicit WindowHandle(const std::intptr_t value) noexcept : value_(value) {}

    std::intptr_t value_ = 0;
};

} // namespace osss
