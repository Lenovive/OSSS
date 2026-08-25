#include "platform/desktop_backend.h"

namespace osss {

struct DesktopCapture::Impl {};
struct DesktopPresenter::Impl {};

DesktopCapture::DesktopCapture() : impl_(std::make_unique<Impl>()) {}
DesktopCapture::~DesktopCapture() = default;

bool DesktopCapture::Start(WindowHandle, std::string& error) {
    error = "no desktop capture backend is available on this platform";
    return false;
}
bool DesktopCapture::Read(PixelFrame&, std::string& error) {
    error = "no desktop capture backend is available on this platform";
    return false;
}
void DesktopCapture::Stop() noexcept {}
WindowHandle DesktopCapture::Target() const noexcept { return {}; }
IntRect DesktopCapture::Bounds() const noexcept { return {}; }

DesktopPresenter::DesktopPresenter() : impl_(std::make_unique<Impl>()) {}
DesktopPresenter::~DesktopPresenter() = default;
bool DesktopPresenter::Start(IntRect, bool, std::string& error) {
    error = "no desktop presentation backend is available on this platform";
    return false;
}
bool DesktopPresenter::Present(std::span<const std::uint32_t>, std::uint32_t, std::uint32_t, std::string& error) {
    error = "no desktop presentation backend is available on this platform";
    return false;
}
bool DesktopPresenter::UpdateBounds(IntRect, std::string& error) {
    error = "no desktop presentation backend is available on this platform";
    return false;
}
void DesktopPresenter::Pump() noexcept {}
void DesktopPresenter::Stop() noexcept {}

} // namespace osss
