#pragma once

#include <cstdint>

namespace osss {

// A rectangle in logical pixels: top-left corner plus size. The neutral
// replacement for Win32's RECT (which stores top-left and bottom-right).
// Core code takes and returns this; each platform converts to its native
// rectangle type only at the boundary.
struct IntRect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;

    [[nodiscard]] constexpr int32_t Right() const noexcept {
        return x + width;
    }

    [[nodiscard]] constexpr int32_t Bottom() const noexcept {
        return y + height;
    }

    [[nodiscard]] constexpr bool IsEmpty() const noexcept {
        return width <= 0 || height <= 0;
    }

    friend constexpr bool operator==(const IntRect& a, const IntRect& b) noexcept {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    }

    friend constexpr bool operator!=(const IntRect& a, const IntRect& b) noexcept {
        return !(a == b);
    }
};

} // namespace osss
