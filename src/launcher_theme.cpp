#include "launcher_theme.h"

#include <algorithm>

namespace osss {
namespace {

constexpr COLORREF Rgb(const int red, const int green, const int blue) noexcept {
    return RGB(red, green, blue);
}

} // namespace

LauncherPalette LauncherPaletteFor(const bool dark) noexcept {
    if (dark) {
        return LauncherPalette{
            Rgb(0x20, 0x20, 0x20), // background
            Rgb(0x2B, 0x2B, 0x2B), // surface
            Rgb(0xFF, 0xFF, 0xFF), // foreground
            Rgb(0xCF, 0xCF, 0xCF), // foreground_muted
            Rgb(0x9A, 0x9A, 0x9A), // foreground_faint
            Rgb(0x3A, 0x3A, 0x3A), // line
            Rgb(0x2D, 0x2D, 0x2D), // field
            Rgb(0x4A, 0x4A, 0x4A), // field_line
            Rgb(0x19, 0x19, 0x19), // band
            Rgb(0x4C, 0xC2, 0xFF), // accent
            Rgb(0x00, 0x1A, 0x26), // accent_foreground
            Rgb(0x2C, 0x2C, 0x2C), // tooltip_background
            Rgb(0xFF, 0xFF, 0xFF), // tooltip_foreground
            Rgb(0x45, 0x45, 0x45), // tooltip_line
            Rgb(0x4F, 0xBF, 0x7A), // ok
            Rgb(0xC2, 0x9B, 0x1D), // warning
            Rgb(0xB3, 0x3A, 0x3A), // failure
        };
    }
    return LauncherPalette{
        Rgb(0xF3, 0xF3, 0xF3), // background
        Rgb(0xFF, 0xFF, 0xFF), // surface
        Rgb(0x1A, 0x1A, 0x1A), // foreground
        Rgb(0x44, 0x44, 0x44), // foreground_muted
        Rgb(0x75, 0x75, 0x75), // foreground_faint
        Rgb(0xDE, 0xDE, 0xDE), // line
        Rgb(0xFF, 0xFF, 0xFF), // field
        Rgb(0xC4, 0xC4, 0xC4), // field_line
        Rgb(0xEA, 0xEA, 0xEA), // band
        Rgb(0x00, 0x67, 0xC0), // accent
        Rgb(0xFF, 0xFF, 0xFF), // accent_foreground
        Rgb(0xFF, 0xFF, 0xFF), // tooltip_background
        Rgb(0x1A, 0x1A, 0x1A), // tooltip_foreground
        Rgb(0xC7, 0xC7, 0xC7), // tooltip_line
        Rgb(0x2E, 0x9E, 0x4F), // ok
        Rgb(0xC2, 0x9B, 0x1D), // warning
        Rgb(0xB3, 0x3A, 0x3A), // failure
    };
}

bool SystemPrefersDarkApps() noexcept {
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    return value == 0;
}

COLORREF ShadeColor(const COLORREF color, const int percent) noexcept {
    const auto mix = [percent](const int channel) {
        const int target = percent >= 0 ? 255 : 0;
        const int weight = std::clamp(percent < 0 ? -percent : percent, 0, 100);
        return std::clamp(channel + (target - channel) * weight / 100, 0, 255);
    };
    return RGB(mix(GetRValue(color)), mix(GetGValue(color)), mix(GetBValue(color)));
}

RECT LauncherLayout::Row(const int height) noexcept {
    const RECT rect{
        kContentLeft,
        cursor_,
        kContentRight,
        cursor_ + height,
    };
    cursor_ += height;
    return rect;
}

RECT LauncherLayout::Column(const int index, const int of, const int height) const noexcept {
    const int columns = std::max(of, 1);
    const int clamped = std::clamp(index, 0, columns - 1);
    const int usable = kContentWidth - (columns - 1) * kGutter;
    const int base = usable / columns;
    // Columns that carry an extra pixel of the division remainder sit on the
    // right, so the row's right edge is always kContentRight regardless of how
    // many ways the content width was split.
    const int remainder = usable - base * columns;
    const int wider_from = columns - remainder;

    int left = kContentLeft;
    for (int column = 0; column < clamped; ++column) {
        left += base + (column >= wider_from ? 1 : 0) + kGutter;
    }
    const int width = base + (clamped >= wider_from ? 1 : 0);
    return RECT{left, cursor_, left + width, cursor_ + height};
}

} // namespace osss
