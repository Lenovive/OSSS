#include "platform/desktop_backend.h"

#if defined(__linux__)

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>

namespace osss {
namespace {

unsigned MaskShift(const unsigned long mask) noexcept {
    if (mask == 0) {
        return 0;
    }
    unsigned shift = 0;
    while ((mask >> shift & 1UL) == 0U) {
        ++shift;
    }
    return shift;
}

std::uint8_t MaskChannel(const unsigned long pixel, const unsigned long mask) noexcept {
    if (mask == 0) {
        return 0;
    }
    const unsigned shift = MaskShift(mask);
    const unsigned long value = (pixel & mask) >> shift;
    const unsigned long maximum = mask >> shift;
    return static_cast<std::uint8_t>(maximum == 0 ? 0 : (value * 255UL + maximum / 2UL) / maximum);
}

unsigned long PackChannel(
    const unsigned channel,
    const unsigned long mask) noexcept {
    if (mask == 0) {
        return 0;
    }
    const unsigned shift = MaskShift(mask);
    const unsigned long maximum = mask >> shift;
    return ((static_cast<unsigned long>(channel) * maximum + 127UL) / 255UL) << shift;
}

std::optional<IntRect> CurrentBounds(Display* display, const ::Window window) {
    if (!display || window == 0) {
        return std::nullopt;
    }
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(display, window, &attributes) ||
        attributes.map_state == IsUnmapped || attributes.c_class == InputOnly ||
        attributes.width <= 0 || attributes.height <= 0) {
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
    return IntRect{root_x, root_y, attributes.width, attributes.height};
}

} // namespace

struct DesktopCapture::Impl {
    Display* display = nullptr;
    ::Window target = 0;
    IntRect bounds{};
};

DesktopCapture::DesktopCapture() : impl_(std::make_unique<Impl>()) {}

DesktopCapture::~DesktopCapture() {
    Stop();
}

bool DesktopCapture::Start(const WindowHandle target, std::string& error) {
    Stop();
    impl_ = std::make_unique<Impl>();
    impl_->display = XOpenDisplay(nullptr);
    if (!impl_->display) {
        error = std::getenv("WAYLAND_DISPLAY")
            ? "Wayland capture is not available in the X11 backend; use an X11 session"
            : "X11 display could not be opened (is DISPLAY set?)";
        return false;
    }
    impl_->target = static_cast<::Window>(target.Native());
    const auto bounds = impl_->target == 0
        ? std::optional<IntRect>{}
        : CurrentBounds(impl_->display, impl_->target);
    if (!bounds) {
        error = "X11 target window is no longer presentable";
        Stop();
        return false;
    }
    impl_->bounds = *bounds;
    return true;
}

bool DesktopCapture::Read(PixelFrame& frame, std::string& error) {
    if (!impl_ || !impl_->display || impl_->target == 0) {
        error = "X11 capture is not started";
        return false;
    }
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(impl_->display, impl_->target, &attributes) ||
        attributes.map_state == IsUnmapped || attributes.c_class == InputOnly ||
        attributes.width <= 0 || attributes.height <= 0) {
        error = "X11 target window could not be queried";
        return false;
    }
    XImage* image = XGetImage(
        impl_->display,
        impl_->target,
        0,
        0,
        static_cast<unsigned>(attributes.width),
        static_cast<unsigned>(attributes.height),
        AllPlanes,
        ZPixmap);
    if (!image) {
        error = "X11 could not capture the target window";
        return false;
    }
    frame.Reset(static_cast<std::uint32_t>(image->width), static_cast<std::uint32_t>(image->height));
    frame.media_time = PixelFrame::Clock::now();
    for (int y = 0; y < image->height; ++y) {
        for (int x = 0; x < image->width; ++x) {
            const unsigned long pixel = XGetPixel(image, x, y);
            const auto red = MaskChannel(pixel, image->red_mask);
            const auto green = MaskChannel(pixel, image->green_mask);
            const auto blue = MaskChannel(pixel, image->blue_mask);
            frame.pixels[static_cast<std::size_t>(y) * frame.width + static_cast<std::size_t>(x)] =
                0xFF000000U | (static_cast<std::uint32_t>(red) << 16U) |
                (static_cast<std::uint32_t>(green) << 8U) | static_cast<std::uint32_t>(blue);
        }
    }
    XDestroyImage(image);
    const auto bounds = CurrentBounds(impl_->display, impl_->target);
    if (bounds) {
        impl_->bounds = *bounds;
    }
    return true;
}

void DesktopCapture::Stop() noexcept {
    if (impl_ && impl_->display) {
        XCloseDisplay(impl_->display);
        impl_->display = nullptr;
    }
    if (impl_) {
        impl_->target = 0;
        impl_->bounds = {};
    }
}

WindowHandle DesktopCapture::Target() const noexcept {
    return impl_ ? WindowHandle::FromNative(impl_->target) : WindowHandle{};
}

IntRect DesktopCapture::Bounds() const noexcept {
    return impl_ ? impl_->bounds : IntRect{};
}

struct DesktopPresenter::Impl {
    Display* display = nullptr;
    ::Window window = 0;
    Visual* visual = nullptr;
    unsigned depth = 0;
    IntRect bounds{};
    bool fullscreen = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

DesktopPresenter::DesktopPresenter() : impl_(std::make_unique<Impl>()) {}

DesktopPresenter::~DesktopPresenter() {
    Stop();
}

bool DesktopPresenter::Start(const IntRect bounds, const bool fullscreen, std::string& error) {
    Stop();
    impl_ = std::make_unique<Impl>();
    impl_->display = XOpenDisplay(nullptr);
    if (!impl_->display) {
        error = std::getenv("WAYLAND_DISPLAY")
            ? "Wayland presentation is not available in the X11 backend; use an X11 session"
            : "X11 display could not be opened for presentation";
        return false;
    }
    const int screen = DefaultScreen(impl_->display);
    impl_->visual = DefaultVisual(impl_->display, screen);
    impl_->depth = static_cast<unsigned>(DefaultDepth(impl_->display, screen));
    impl_->bounds = bounds;
    impl_->fullscreen = fullscreen;
    if (fullscreen) {
        impl_->bounds = IntRect{0, 0, DisplayWidth(impl_->display, screen), DisplayHeight(impl_->display, screen)};
    }
    impl_->window = XCreateSimpleWindow(
        impl_->display,
        DefaultRootWindow(impl_->display),
        impl_->bounds.x,
        impl_->bounds.y,
        static_cast<unsigned>(std::max(1, impl_->bounds.width)),
        static_cast<unsigned>(std::max(1, impl_->bounds.height)),
        0,
        BlackPixel(impl_->display, screen),
        BlackPixel(impl_->display, screen));
    if (!impl_->window) {
        error = "X11 output window could not be created";
        Stop();
        return false;
    }
    XSetWindowAttributes window_attributes{};
    window_attributes.override_redirect = True;
    XChangeWindowAttributes(
        impl_->display,
        impl_->window,
        CWOverrideRedirect,
        &window_attributes);
    int shape_event_base = 0;
    int shape_error_base = 0;
    if (!XShapeQueryExtension(impl_->display, &shape_event_base, &shape_error_base)) {
        error = "X11 Shape extension is unavailable; click-through presentation cannot be provided";
        Stop();
        return false;
    }
    static_cast<void>(shape_event_base);
    static_cast<void>(shape_error_base);
    XSelectInput(impl_->display, impl_->window, ExposureMask | StructureNotifyMask);
    // Empty input shape keeps the overlay from stealing clicks on X11. Shape
    // is an optional X extension, but is present on the desktop targets that
    // provide the classic override-redirect overlay path.
    XShapeCombineRectangles(
        impl_->display,
        impl_->window,
        ShapeInput,
        0,
        0,
        nullptr,
        0,
        ShapeSet,
        YXBanded);
    const Atom above = XInternAtom(impl_->display, "_NET_WM_STATE_ABOVE", False);
    const Atom state = XInternAtom(impl_->display, "_NET_WM_STATE", False);
    if (above != None && state != None) {
        XChangeProperty(impl_->display, impl_->window, state, XA_ATOM, 32, PropModeReplace,
            reinterpret_cast<const unsigned char*>(&above), 1);
    }
    XMapRaised(impl_->display, impl_->window);
    XFlush(impl_->display);
    return true;
}

bool DesktopPresenter::Present(
    const std::span<const std::uint32_t> pixels,
    const std::uint32_t width,
    const std::uint32_t height,
    std::string& error) {
    if (!impl_ || !impl_->display || !impl_->window || width == 0 || height == 0 ||
        pixels.size() != static_cast<std::size_t>(width) * height) {
        error = "X11 presenter received an invalid frame";
        return false;
    }
    const std::uint32_t output_width = impl_->fullscreen
        ? static_cast<std::uint32_t>(std::max(1, impl_->bounds.width))
        : width;
    const std::uint32_t output_height = impl_->fullscreen
        ? static_cast<std::uint32_t>(std::max(1, impl_->bounds.height))
        : height;
    XImage* image = XCreateImage(
        impl_->display,
        impl_->visual,
        impl_->depth,
        ZPixmap,
        0,
        nullptr,
        output_width,
        output_height,
        32,
        0);
    if (!image) {
        error = "X11 could not allocate an output image";
        return false;
    }
    const std::size_t bytes = static_cast<std::size_t>(image->bytes_per_line) * output_height;
    image->data = static_cast<char*>(std::calloc(bytes, 1));
    if (!image->data) {
        image->f.destroy_image(image);
        error = "X11 could not allocate output pixels";
        return false;
    }
    for (std::uint32_t y = 0; y < output_height; ++y) {
        for (std::uint32_t x = 0; x < output_width; ++x) {
            const std::uint32_t source_x = std::min(width - 1U,
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * width) / output_width));
            const std::uint32_t source_y = std::min(height - 1U,
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * height) / output_height));
            const std::uint32_t pixel = pixels[static_cast<std::size_t>(source_y) * width + source_x];
            const unsigned long native =
                PackChannel((pixel >> 16U) & 0xFFU, image->red_mask) |
                PackChannel((pixel >> 8U) & 0xFFU, image->green_mask) |
                PackChannel(pixel & 0xFFU, image->blue_mask);
            XPutPixel(image, static_cast<int>(x), static_cast<int>(y), native);
        }
    }
    if (!impl_->fullscreen && (width != impl_->width || height != impl_->height)) {
        XResizeWindow(impl_->display, impl_->window, width, height);
        impl_->width = width;
        impl_->height = height;
    }
    XPutImage(impl_->display, impl_->window, DefaultGC(impl_->display, DefaultScreen(impl_->display)),
        image, 0, 0, 0, 0, output_width, output_height);
    XFlush(impl_->display);
    XDestroyImage(image);
    return true;
}

bool DesktopPresenter::UpdateBounds(const IntRect bounds, std::string& error) {
    if (!impl_ || !impl_->display || !impl_->window) {
        error = "X11 presenter is not started";
        return false;
    }
    if (impl_->fullscreen || bounds.IsEmpty()) {
        return true;
    }
    if (bounds == impl_->bounds) {
        return true;
    }
    impl_->bounds = bounds;
    XMoveResizeWindow(
        impl_->display,
        impl_->window,
        bounds.x,
        bounds.y,
        static_cast<unsigned>(std::max(1, bounds.width)),
        static_cast<unsigned>(std::max(1, bounds.height)));
    XFlush(impl_->display);
    return true;
}

void DesktopPresenter::Pump() noexcept {
    if (!impl_ || !impl_->display) {
        return;
    }
    while (XPending(impl_->display) != 0) {
        XEvent event{};
        XNextEvent(impl_->display, &event);
    }
}

void DesktopPresenter::Stop() noexcept {
    if (impl_ && impl_->display) {
        if (impl_->window) {
            XDestroyWindow(impl_->display, impl_->window);
            impl_->window = 0;
        }
        XCloseDisplay(impl_->display);
        impl_->display = nullptr;
    }
}

} // namespace osss

#endif
