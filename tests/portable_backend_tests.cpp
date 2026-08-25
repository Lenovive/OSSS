#include "platform/software_interpolator.h"
#include "adaptive_scheduler.h"

#include "test_harness.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using osss::test::Require;
using osss::test::RequireNear;

osss::PixelFrame Pattern(const int box_x) {
    osss::PixelFrame frame;
    frame.Reset(96, 64);
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const bool box = static_cast<int>(x) >= box_x && static_cast<int>(x) < box_x + 16 &&
                y >= 20 && y < 44;
            frame.pixels[static_cast<std::size_t>(y) * frame.width + x] =
                box ? 0xFF40D0FFU : 0xFF101820U;
        }
    }
    return frame;
}

} // namespace

int main() {
    try {
    const osss::PixelFrame previous = Pattern(20);
    const osss::PixelFrame current = Pattern(28);
    osss::SoftwareInterpolator interpolator;
    Require(interpolator.Prepare(previous, current), "software prepare succeeds");
    RequireNear(interpolator.MotionX(), 8.0, 2.0, "global translation recovered");
    RequireNear(interpolator.MotionY(), 0.0, 2.0, "vertical translation remains stable");

    const osss::PixelFrame midpoint = interpolator.Render(0.5F);
    const osss::PixelFrame expected = Pattern(24);
    std::size_t equal = 0;
    for (std::size_t index = 0; index < midpoint.pixels.size(); ++index) {
        equal += midpoint.pixels[index] == expected.pixels[index] ? 1U : 0U;
    }
    Require(equal > midpoint.pixels.size() * 9U / 10U, "midpoint reconstruction is stable");

    osss::PixelFrame white;
    white.Reset(previous.width, previous.height);
    std::fill(white.pixels.begin(), white.pixels.end(), 0xFFFFFFFFU);
    osss::SoftwareInterpolator cut;
    Require(cut.Prepare(previous, white), "scene cut prepare succeeds");
    Require(cut.Render(0.0F).pixels == white.pixels, "scene cut ignores alpha at pair start");
    Require(cut.Render(0.75F).pixels == white.pixels, "scene cut selects newest frame");

    std::vector<std::uint8_t> mask(previous.pixels.size(), 0);
    for (std::uint32_t y = 20; y < 44; ++y) {
        for (std::uint32_t x = 20; x < 36; ++x) {
            mask[static_cast<std::size_t>(y) * previous.width + x] = 255;
        }
    }
    osss::SoftwareInterpolator masked;
    masked.SetMode(osss::SoftwareInterpolator::Mode::blend);
    masked.SetMask(mask);
    Require(masked.Prepare(previous, current), "masked prepare succeeds");
    const osss::PixelFrame masked_midpoint = masked.Render(0.5F);
    Require(
        masked_midpoint.pixels[20U * previous.width + 24U] == current.pixels[20U * previous.width + 24U],
        "masked region uses newest frame");

    osss::SourceTimeline timeline;
    const osss::SourceTimeline::Clock::time_point epoch{};
    Require(
        timeline.Ingest(1, epoch, epoch, previous.width, previous.height, false).has_value(),
        "portable timeline accepts the first frame");
    Require(
        timeline.Ingest(2, epoch + std::chrono::milliseconds(16),
            epoch + std::chrono::milliseconds(16), current.width, current.height, false).has_value(),
        "portable timeline accepts the second frame");
    osss::FrameSelector selector(2, std::chrono::milliseconds(0),
        osss::FrameSelector::CeilingPacing::even);
    const osss::FrameSelection selection = selector.Select(
        osss::OutputClock::Deadline{epoch + std::chrono::milliseconds(30), 1, 0},
        osss::FrameRate{120, 1}, timeline);
    Require(selection.submit, "portable scheduler submits a bracketed slot");
    Require(selection.mode == osss::FrameSelectionMode::interpolate,
        "portable scheduler selects interpolation above unity");
    Require(selection.alpha >= 0.0F && selection.alpha <= 1.0F,
        "portable scheduler keeps alpha in range");
    return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
