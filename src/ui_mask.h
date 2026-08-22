#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace osss {

// One rectangular UI/HUD region that must be excluded from motion
// interpolation. Coordinates are stored as parsed: either fractions of the
// source frame in [0, 1] or source-frame pixels, selected by `pixels`.
struct UiMaskRect {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    bool pixels = false;

    [[nodiscard]] bool operator==(const UiMaskRect&) const noexcept = default;
};

// Integer pixel bounds resolved against a concrete source size. `right` and
// `bottom` are exclusive; empty rectangles have right <= left or bottom <= top.
struct UiMaskPixelRect {
    std::uint32_t left = 0;
    std::uint32_t top = 0;
    std::uint32_t right = 0;
    std::uint32_t bottom = 0;

    [[nodiscard]] bool Empty() const noexcept {
        return right <= left || bottom <= top;
    }
};

struct UiMaskParseResult {
    std::vector<UiMaskRect> rects;
    // Empty on success; otherwise a human-readable reason with the offending
    // region text.
    std::string error;

    [[nodiscard]] bool Ok() const noexcept {
        return error.empty();
    }
};

// Parses the textual mask syntax shared by `--ui-mask` and the settings GUI:
//
//   left,top,right,bottom[px][; left,top,right,bottom[px] ...]
//
// Regions are separated by ';' or newlines. Values are fractions of the source
// frame in [0, 1] unless the region ends in "px" or any value exceeds 1, in
// which case all four values are source-frame pixels. Empty input yields no
// regions and no error.
[[nodiscard]] UiMaskParseResult ParseUiMaskRects(std::wstring_view text);
[[nodiscard]] UiMaskParseResult ParseUiMaskRects(std::string_view text);

// Resolves a region against a source size, clamping to the frame.
[[nodiscard]] UiMaskPixelRect ResolveUiMaskRect(
    const UiMaskRect& rect,
    std::uint32_t source_width,
    std::uint32_t source_height) noexcept;

// Rasterizes the regions into a tightly packed 8-bit coverage image
// (0 = interpolate, 255 = keep the newest real frame). Returns
// source_width * source_height bytes, row-major.
[[nodiscard]] std::vector<std::uint8_t> RasterizeUiMask(
    const std::vector<UiMaskRect>& rects,
    std::uint32_t source_width,
    std::uint32_t source_height);

// Round-trips regions back to the textual syntax (fractions keep up to four
// decimals; pixel regions carry the "px" suffix).
[[nodiscard]] std::wstring FormatUiMaskRects(const std::vector<UiMaskRect>& rects);

} // namespace osss
