#pragma once

#include "test_animation_catalog.h"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace osss {

class TestAnimationBackend {
public:
    virtual ~TestAnimationBackend() = default;

    TestAnimationBackend(const TestAnimationBackend&) = delete;
    TestAnimationBackend& operator=(const TestAnimationBackend&) = delete;

    virtual void Present(std::span<const std::uint32_t> pixels) = 0;
    [[nodiscard]] virtual std::wstring_view RuntimeName() const noexcept = 0;

protected:
    TestAnimationBackend() = default;
};

[[nodiscard]] std::unique_ptr<TestAnimationBackend> CreateTestAnimationBackend(
    TestGraphicsApi api,
    HWND window,
    std::uint32_t width,
    std::uint32_t height);

} // namespace osss
