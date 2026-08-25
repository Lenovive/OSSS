#include "platform/desktop_backend.h"
#include "window_catalog.h"

#if defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

@interface OsssFrameView : NSView
@property(nonatomic, assign) std::vector<std::uint32_t>* pixels;
@property(nonatomic, assign) std::uint32_t pixelWidth;
@property(nonatomic, assign) std::uint32_t pixelHeight;
@end

@implementation OsssFrameView

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    if (!self.pixels || self.pixels->empty() || self.pixelWidth == 0 || self.pixelHeight == 0) {
        [[NSColor blackColor] setFill];
        NSRectFill(self.bounds);
        return;
    }
    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider = CGDataProviderCreateWithData(
        nullptr,
        self.pixels->data(),
        self.pixels->size() * sizeof(std::uint32_t),
        nullptr);
    const CGBitmapInfo presentation_bitmap_info = static_cast<CGBitmapInfo>(
        static_cast<std::uint32_t>(kCGBitmapByteOrder32Host) |
        static_cast<std::uint32_t>(kCGImageAlphaPremultipliedFirst));
    CGImageRef image = CGImageCreate(
        self.pixelWidth,
        self.pixelHeight,
        8,
        32,
        static_cast<std::size_t>(self.pixelWidth) * sizeof(std::uint32_t),
        color_space,
        presentation_bitmap_info,
        provider,
        nullptr,
        false,
        kCGRenderingIntentDefault);
    if (image) {
        CGContextRef context = [[NSGraphicsContext currentContext] CGContext];
        CGContextSetInterpolationQuality(context, kCGInterpolationNone);
        CGContextSaveGState(context);
        CGContextTranslateCTM(context, 0.0, self.bounds.size.height);
        CGContextScaleCTM(context, 1.0, -1.0);
        CGContextDrawImage(context, NSRectToCGRect(self.bounds), image);
        CGContextRestoreGState(context);
        CGImageRelease(image);
    }
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(color_space);
}

@end

namespace osss {
namespace {

PixelFrame CopyWindowImage(const CGWindowID window, std::string& error) {
    PixelFrame frame;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGImageRef image = CGWindowListCreateImage(
        CGRectNull,
        kCGWindowListOptionIncludingWindow,
        window,
        kCGWindowImageBoundsIgnoreFraming | kCGWindowImageNominalResolution);
#pragma clang diagnostic pop
    if (!image) {
        error = "CoreGraphics could not capture the target window (screen recording permission may be missing)";
        return frame;
    }
    const std::size_t width = CGImageGetWidth(image);
    const std::size_t height = CGImageGetHeight(image);
    frame.Reset(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    frame.media_time = PixelFrame::Clock::now();
    std::vector<std::uint8_t> rgba(width * height * 4U, 0);
    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
    const CGBitmapInfo capture_bitmap_info = static_cast<CGBitmapInfo>(
        static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast) |
        static_cast<std::uint32_t>(kCGBitmapByteOrder32Big));
    CGContextRef context = CGBitmapContextCreate(
        rgba.data(),
        width,
        height,
        8,
        width * 4U,
        color_space,
        capture_bitmap_info);
    if (!context) {
        CGColorSpaceRelease(color_space);
        CGImageRelease(image);
        frame = {};
        error = "CoreGraphics could not allocate a capture surface";
        return frame;
    }
    // Keep PixelFrame rows top-to-bottom. Core Graphics contexts are
    // bottom-to-top by default, so mirror the draw into the owned bitmap.
    CGContextTranslateCTM(context, 0.0, static_cast<CGFloat>(height));
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    CGColorSpaceRelease(color_space);
    CGImageRelease(image);
    for (std::size_t index = 0; index < width * height; ++index) {
        const std::uint8_t red = rgba[index * 4U + 0U];
        const std::uint8_t green = rgba[index * 4U + 1U];
        const std::uint8_t blue = rgba[index * 4U + 2U];
        const std::uint8_t alpha = rgba[index * 4U + 3U];
        frame.pixels[index] = (static_cast<std::uint32_t>(alpha) << 24U) |
            (static_cast<std::uint32_t>(red) << 16U) |
            (static_cast<std::uint32_t>(green) << 8U) | static_cast<std::uint32_t>(blue);
    }
    return frame;
}

} // namespace

struct DesktopCapture::Impl {
    CGWindowID target = kCGNullWindowID;
    IntRect bounds{};
};

DesktopCapture::DesktopCapture() : impl_(std::make_unique<Impl>()) {}

DesktopCapture::~DesktopCapture() {
    Stop();
}

bool DesktopCapture::Start(const WindowHandle target, std::string& error) {
    Stop();
    impl_ = std::make_unique<Impl>();
    impl_->target = static_cast<CGWindowID>(target.Native());
    const auto bounds = ExtendedWindowBounds(target);
    if (impl_->target == kCGNullWindowID || !bounds) {
        error = "CoreGraphics target window is no longer presentable";
        return false;
    }
    impl_->bounds = *bounds;
    return true;
}

bool DesktopCapture::Read(PixelFrame& frame, std::string& error) {
    if (!impl_ || impl_->target == kCGNullWindowID) {
        error = "CoreGraphics capture is not started";
        return false;
    }
    frame = CopyWindowImage(impl_->target, error);
    const auto bounds = ExtendedWindowBounds(WindowHandle::FromNative(impl_->target));
    if (bounds) {
        impl_->bounds = *bounds;
    }
    return frame.IsValid();
}

void DesktopCapture::Stop() noexcept {
    if (impl_) {
        impl_->target = kCGNullWindowID;
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
    NSWindow* window = nil;
    OsssFrameView* view = nil;
    IntRect bounds{};
    bool fullscreen = false;
    std::vector<std::uint32_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

NSScreen* ScreenForBounds(const IntRect bounds) {
    NSScreen* screen = [NSScreen mainScreen];
    const CGPoint point = CGPointMake(
        static_cast<CGFloat>(bounds.x), static_cast<CGFloat>(bounds.y));
    for (NSScreen* candidate in [NSScreen screens]) {
        NSDictionary* device_description = [candidate deviceDescription];
        NSNumber* number = device_description[@"NSScreenNumber"];
        if (!number) {
            continue;
        }
        if (CGRectContainsPoint(
                CGDisplayBounds(static_cast<CGDirectDisplayID>(number.unsignedIntValue)),
                point)) {
            screen = candidate;
            break;
        }
    }
    return screen;
}

NSRect CocoaFrameForBounds(const IntRect bounds) {
    NSScreen* screen = ScreenForBounds(bounds);
    CGRect display_bounds = CGRectNull;
    for (NSScreen* candidate in [NSScreen screens]) {
        NSDictionary* device_description = [candidate deviceDescription];
        NSNumber* number = device_description[@"NSScreenNumber"];
        if (!number) {
            continue;
        }
        const CGRect candidate_bounds = CGDisplayBounds(
            static_cast<CGDirectDisplayID>(number.unsignedIntValue));
        if (CGRectContainsPoint(
                candidate_bounds,
                CGPointMake(static_cast<CGFloat>(bounds.x), static_cast<CGFloat>(bounds.y)))) {
            screen = candidate;
            display_bounds = candidate_bounds;
            break;
        }
    }
    const NSRect screen_frame = screen ? screen.frame : NSMakeRect(0, 0, 0, 0);
    if (CGRectIsNull(display_bounds)) {
        display_bounds = CGRectMake(0, 0, screen_frame.size.width, screen_frame.size.height);
    }
    // CoreGraphics reports window origins from the top-left of the desktop;
    // Cocoa's global window coordinates use the bottom-left. Account for the
    // selected display's virtual-desktop origin so secondary monitors follow
    // the target instead of being projected onto the main screen.
    const CGFloat local_x = static_cast<CGFloat>(bounds.x) - display_bounds.origin.x;
    const CGFloat local_y = static_cast<CGFloat>(bounds.y) - display_bounds.origin.y;
    return NSMakeRect(
        screen_frame.origin.x + local_x,
        screen_frame.origin.y + screen_frame.size.height - local_y -
            static_cast<CGFloat>(bounds.height),
        static_cast<CGFloat>(std::max(1, bounds.width)),
        static_cast<CGFloat>(std::max(1, bounds.height)));
}

DesktopPresenter::DesktopPresenter() : impl_(std::make_unique<Impl>()) {}

DesktopPresenter::~DesktopPresenter() {
    Stop();
}

bool DesktopPresenter::Start(const IntRect bounds, const bool fullscreen, std::string& error) {
    Stop();
    impl_ = std::make_unique<Impl>();
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    NSScreen* screen = ScreenForBounds(bounds);
    NSRect frame = CocoaFrameForBounds(bounds);
    if (fullscreen && screen) {
        frame = screen.frame;
    }
    // Keep the logical CoreGraphics bounds for equality checks and future
    // target-follow updates. `frame` is the converted Cocoa rectangle.
    impl_->bounds = fullscreen
        ? IntRect{
            static_cast<int>(frame.origin.x),
            static_cast<int>(frame.origin.y),
            static_cast<int>(frame.size.width),
            static_cast<int>(frame.size.height)}
        : bounds;
    impl_->fullscreen = fullscreen;
    impl_->window = [[NSWindow alloc]
        initWithContentRect:frame
        styleMask:NSWindowStyleMaskBorderless
        backing:NSBackingStoreBuffered
        defer:NO];
    if (!impl_->window) {
        error = "Cocoa output window could not be created";
        return false;
    }
    impl_->window.level = NSFloatingWindowLevel;
    impl_->window.opaque = YES;
    impl_->window.backgroundColor = NSColor.blackColor;
    impl_->window.ignoresMouseEvents = YES;
    impl_->window.hasShadow = NO;
    impl_->view = [[OsssFrameView alloc]
        initWithFrame:NSMakeRect(0, 0, frame.size.width, frame.size.height)];
    impl_->view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    impl_->pixels.clear();
    impl_->view.pixels = &impl_->pixels;
    impl_->window.contentView = impl_->view;
    [impl_->window orderFrontRegardless];
    return true;
}

bool DesktopPresenter::Present(
    const std::span<const std::uint32_t> pixels,
    const std::uint32_t width,
    const std::uint32_t height,
    std::string& error) {
    if (!impl_ || !impl_->window || !impl_->view || width == 0 || height == 0 ||
        pixels.size() != static_cast<std::size_t>(width) * height) {
        error = "Cocoa presenter received an invalid frame";
        return false;
    }
    impl_->pixels.assign(pixels.begin(), pixels.end());
    impl_->width = width;
    impl_->height = height;
    impl_->view.pixelWidth = width;
    impl_->view.pixelHeight = height;
    [impl_->view setNeedsDisplay:YES];
    [impl_->window displayIfNeeded];
    return true;
}

bool DesktopPresenter::UpdateBounds(const IntRect bounds, std::string& error) {
    if (!impl_ || !impl_->window) {
        error = "Cocoa presenter is not started";
        return false;
    }
    if (impl_->fullscreen || bounds.IsEmpty()) {
        return true;
    }
    if (bounds == impl_->bounds) {
        return true;
    }
    impl_->bounds = bounds;
    [impl_->window setFrame:CocoaFrameForBounds(bounds) display:YES];
    return true;
}

void DesktopPresenter::Pump() noexcept {
    if (!NSApp) {
        return;
    }
    for (;;) {
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
            untilDate:[NSDate dateWithTimeIntervalSinceNow:0.0]
            inMode:NSDefaultRunLoopMode
            dequeue:YES];
        if (!event) {
            break;
        }
        [NSApp sendEvent:event];
    }
}

void DesktopPresenter::Stop() noexcept {
    if (impl_ && impl_->window) {
        [impl_->window orderOut:nil];
        impl_->window = nil;
        impl_->view = nil;
    }
}

} // namespace osss

#endif
