#pragma once

#include "gpu_timing.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace osss {

// Spatial upscaling of the *finished* frame.
//
// Ordering matters and is not arbitrary: this runs on the fused output, never on
// the source. Optical flow keeps estimating on native captured pixels at the
// lowest resolution available, which is both cheaper and more accurate than
// estimating on interpolated ones -- an upscaler invents plausible detail, and
// feeding invented detail to a block matcher gives it confident matches for
// structure that was never in the source.
//
// Two passes, because they do different jobs and the second needs the first to
// be finished across a neighbourhood:
//
//   1. Edge-directed upsample. A 3x3 luma structure tensor gives the dominant
//      local gradient direction and how anisotropic the neighbourhood is. The
//      12-tap kernel is then measured in a space stretched along the edge, so
//      taps lying along an edge keep their weight and taps crossing it lose it.
//      That is what keeps a diagonal from stair-stepping without smearing the
//      texture on either side of it.
//   2. Contrast-limited sharpen. A 5-tap cross sharpener whose strength is
//      clamped by the local min/max, so it cannot overshoot past values that
//      already existed in the neighbourhood. Unlimited sharpening after an
//      upscale is what produces the halo that makes upscaled output obvious.
//
// This is FSR1-class in structure and is a reimplementation from published
// technique, not a port: no third-party code is vendored here, which keeps the
// project's dependency and licence rules intact.
enum class UpscaleMode {
    // Never upscale; present the fused frame at its own size.
    off,
    // Upscale only when the output is genuinely larger than the source, which is
    // the fullscreen case where the target window is smaller than the monitor.
    automatic,
    // Always run the passes, even at 1:1. Only useful for measuring their cost
    // and for seeing the sharpener in isolation.
    always,
};

[[nodiscard]] constexpr const wchar_t* UpscaleModeArgument(const UpscaleMode mode) noexcept {
    switch (mode) {
    case UpscaleMode::off:
        return L"off";
    case UpscaleMode::always:
        return L"always";
    case UpscaleMode::automatic:
        break;
    }
    return L"auto";
}

[[nodiscard]] constexpr const char* UpscaleModeName(const UpscaleMode mode) noexcept {
    switch (mode) {
    case UpscaleMode::off:
        return "off";
    case UpscaleMode::always:
        return "always";
    case UpscaleMode::automatic:
        break;
    }
    return "auto";
}

[[nodiscard]] inline std::optional<UpscaleMode> ParseUpscaleMode(
    const std::wstring_view value) noexcept {
    if (value == L"off" || value == L"none") {
        return UpscaleMode::off;
    }
    if (value == L"auto" || value == L"automatic" || value == L"on") {
        return UpscaleMode::automatic;
    }
    if (value == L"always" || value == L"force") {
        return UpscaleMode::always;
    }
    return std::nullopt;
}

// Sharpening strength, 0 through 1. The default is deliberately mild: an
// upscaler that looks impressive on a still frame at high sharpness is usually
// shimmering in motion, and this pipeline's output is judged in motion.
inline constexpr float kMinimumSharpness = 0.0F;
inline constexpr float kMaximumSharpness = 1.0F;
inline constexpr float kDefaultSharpness = 0.35F;

[[nodiscard]] constexpr bool IsValidSharpness(const float sharpness) noexcept {
    return sharpness >= kMinimumSharpness && sharpness <= kMaximumSharpness;
}

class Upscaler {
public:
    Upscaler(ID3D11Device* device, ID3D11DeviceContext* context);

    Upscaler(const Upscaler&) = delete;
    Upscaler& operator=(const Upscaler&) = delete;

    // Runs both passes from `source` into `destination`. `destination` must be a
    // render target of `output_width` x `output_height`. Returns false with
    // LastError set rather than throwing, so the presentation path can fall back
    // to a plain blit without a try/catch on every frame.
    [[nodiscard]] bool Draw(
        ID3D11ShaderResourceView* source,
        UINT source_width,
        UINT source_height,
        ID3D11RenderTargetView* destination,
        UINT output_width,
        UINT output_height) noexcept;

    void SetSharpness(float sharpness) noexcept;
    [[nodiscard]] float Sharpness() const noexcept;

    void PollGpuTimings() noexcept;
    [[nodiscard]] GpuTimingStatistics GpuTiming() const noexcept;
    [[nodiscard]] bool ShadersCameFromCache() const noexcept;
    [[nodiscard]] const std::string& LastError() const noexcept;

private:
    struct alignas(16) UpscaleConstants {
        float source_size[2]{};
        float inverse_source_size[2]{};
        float output_size[2]{};
        float inverse_output_size[2]{};
        float sharpness = kDefaultSharpness;
        float padding[3]{};
    };

    static_assert(sizeof(UpscaleConstants) % 16 == 0);

    void CreateIntermediate(UINT width, UINT height);
    void StoreError(const std::string& message) noexcept;

    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11VertexShader> vertex_shader_;
    winrt::com_ptr<ID3D11PixelShader> upsample_shader_;
    winrt::com_ptr<ID3D11PixelShader> sharpen_shader_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11Buffer> constants_;

    // Output-resolution scratch holding the upsample result for the sharpener.
    // The sharpener reads a neighbourhood, so it cannot be folded into the first
    // pass without recomputing the upsample once per tap.
    winrt::com_ptr<ID3D11Texture2D> intermediate_;
    winrt::com_ptr<ID3D11ShaderResourceView> intermediate_view_;
    winrt::com_ptr<ID3D11RenderTargetView> intermediate_target_;
    UINT intermediate_width_ = 0;
    UINT intermediate_height_ = 0;

    GpuTimestampCollector timing_;
    float sharpness_ = kDefaultSharpness;
    bool shaders_came_from_cache_ = false;
    std::string last_error_;
};

} // namespace osss
