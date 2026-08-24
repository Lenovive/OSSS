#pragma once

#include <cstdint>

namespace osss {

// 8 bits per channel. The neutral replacement for Win32's COLORREF: the
// launcher palette is stored in this type so the theme module compiles on
// every platform, and each platform converts to its native colour type at
// the paint boundary.
struct RgbColor {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    friend constexpr bool operator==(const RgbColor& a, const RgbColor& b) noexcept {
        return a.red == b.red && a.green == b.green && a.blue == b.blue;
    }

    friend constexpr bool operator!=(const RgbColor& a, const RgbColor& b) noexcept {
        return !(a == b);
    }
};

} // namespace osss
