#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace osss {

enum class TestGraphicsApi {
    direct3d9,
    direct3d10,
    direct3d11,
    direct3d12,
};

inline constexpr std::array<TestGraphicsApi, 4> kTestGraphicsApis{
    TestGraphicsApi::direct3d9,
    TestGraphicsApi::direct3d10,
    TestGraphicsApi::direct3d11,
    TestGraphicsApi::direct3d12,
};

inline constexpr std::array<int, 12> kTestAnimationBaseRates{
    10,
    20,
    30,
    40,
    50,
    60,
    70,
    80,
    90,
    100,
    110,
    120,
};

[[nodiscard]] constexpr std::wstring_view TestGraphicsApiDisplayName(
    const TestGraphicsApi api) noexcept {
    switch (api) {
    case TestGraphicsApi::direct3d9:
        return L"Direct3D 9";
    case TestGraphicsApi::direct3d10:
        return L"Direct3D 10";
    case TestGraphicsApi::direct3d11:
        return L"Direct3D 11";
    case TestGraphicsApi::direct3d12:
        return L"Direct3D 12";
    }
    return L"Unknown Direct3D API";
}

[[nodiscard]] constexpr std::wstring_view TestGraphicsApiArgument(
    const TestGraphicsApi api) noexcept {
    switch (api) {
    case TestGraphicsApi::direct3d9:
        return L"d3d9";
    case TestGraphicsApi::direct3d10:
        return L"d3d10";
    case TestGraphicsApi::direct3d11:
        return L"d3d11";
    case TestGraphicsApi::direct3d12:
        return L"d3d12";
    }
    return L"unknown";
}

[[nodiscard]] constexpr std::optional<TestGraphicsApi> ParseTestGraphicsApi(
    const std::wstring_view value) noexcept {
    if (value == L"d3d9" || value == L"dx9") {
        return TestGraphicsApi::direct3d9;
    }
    if (value == L"d3d10" || value == L"dx10") {
        return TestGraphicsApi::direct3d10;
    }
    if (value == L"d3d11" || value == L"dx11") {
        return TestGraphicsApi::direct3d11;
    }
    if (value == L"d3d12" || value == L"dx12") {
        return TestGraphicsApi::direct3d12;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr bool IsTestAnimationBaseRate(const int fps) noexcept {
    for (const int supported : kTestAnimationBaseRates) {
        if (fps == supported) {
            return true;
        }
    }
    return false;
}

} // namespace osss
