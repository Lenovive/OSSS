#pragma once

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace osss {

// The portable runtime's owned pixel surface. Pixels use the same 0xAARRGGBB
// convention as the diagnostic and test-pattern code, so a captured frame can
// move between a platform boundary, the software interpolator, and a presenter
// without a format-dependent copy in the common path.
struct PixelFrame {
    using Clock = std::chrono::steady_clock;

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Clock::time_point media_time{};
    std::vector<std::uint32_t> pixels;

    [[nodiscard]] bool IsValid() const noexcept {
        return width != 0 && height != 0 &&
            pixels.size() == static_cast<std::size_t>(width) * height;
    }

    void Reset(std::uint32_t new_width, std::uint32_t new_height) {
        width = new_width;
        height = new_height;
        pixels.assign(static_cast<std::size_t>(width) * height, 0xFF000000U);
    }

    [[nodiscard]] std::span<const std::uint32_t> View() const noexcept {
        return pixels;
    }
};

} // namespace osss
