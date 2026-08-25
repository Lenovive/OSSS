#pragma once

#include "pixel_frame.h"

#include "../platform/int_rect.h"
#include "../platform/window_handle.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace osss {

// Platform boundary for the live desktop path. The scheduler and software
// renderer never see X11, Cocoa, CoreGraphics, or a toolkit handle; all of that
// remains in the small per-platform implementation behind this interface.
class DesktopCapture {
public:
    DesktopCapture();
    ~DesktopCapture();

    DesktopCapture(const DesktopCapture&) = delete;
    DesktopCapture& operator=(const DesktopCapture&) = delete;

    [[nodiscard]] bool Start(WindowHandle target, std::string& error);
    [[nodiscard]] bool Read(PixelFrame& frame, std::string& error);
    void Stop() noexcept;

    [[nodiscard]] WindowHandle Target() const noexcept;
    [[nodiscard]] IntRect Bounds() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class DesktopPresenter {
public:
    DesktopPresenter();
    ~DesktopPresenter();

    DesktopPresenter(const DesktopPresenter&) = delete;
    DesktopPresenter& operator=(const DesktopPresenter&) = delete;

    [[nodiscard]] bool Start(IntRect bounds, bool fullscreen, std::string& error);
    [[nodiscard]] bool Present(
        std::span<const std::uint32_t> pixels,
        std::uint32_t width,
        std::uint32_t height,
        std::string& error);
    // Follow the target when it moves or changes size. Fullscreen presenters
    // ignore this request; windowed presenters keep their original output
    // shape and only update the native position/extent.
    [[nodiscard]] bool UpdateBounds(IntRect bounds, std::string& error);
    void Pump() noexcept;
    void Stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace osss
