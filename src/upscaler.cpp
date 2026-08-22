#include "upscaler.h"

#include "shader_cache.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace osss {
namespace {

constexpr DXGI_FORMAT kIntermediateFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

constexpr char kUpscaleShaderSource[] = R"(
cbuffer UpscaleConstants : register(b0) {
    float2 source_size;
    float2 inverse_source_size;
    float2 output_size;
    float2 inverse_output_size;
    float sharpness;
    float3 padding;
};

Texture2D<float4> source_texture : register(t0);
SamplerState linear_clamp : register(s0);

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput VertexMain(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    output.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(
        output.uv.x * 2.0f - 1.0f,
        1.0f - output.uv.y * 2.0f,
        0.0f,
        1.0f);
    return output;
}

float Luma(float3 colour) {
    return dot(colour, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 FetchSource(float2 source_position) {
    // Half-texel offset puts the sample at the texel centre, so a 1:1 scale is
    // an exact copy rather than a quarter-pixel blur.
    return source_texture.SampleLevel(
        linear_clamp,
        (source_position + 0.5f) * inverse_source_size,
        0).rgb;
}

// Edge-directed upsample.
//
// The 3x3 luma structure tensor gives the dominant gradient direction and how
// strongly the neighbourhood favours it. Tap distance is then measured in a
// space stretched along the edge and squeezed across it, so taps lying *along*
// an edge keep their weight while taps crossing it lose theirs. That is what
// stops a near-diagonal from stair-stepping without smearing the texture to
// either side of it -- an isotropic kernel has to choose between the two.
float4 UpsampleMain(VertexOutput input) : SV_Target {
    const float2 output_position = input.uv * output_size - 0.5f;
    const float2 source_position = (output_position + 0.5f) * (source_size / output_size) - 0.5f;
    const float2 base = floor(source_position);
    const float2 fraction = source_position - base;

    // Structure tensor over the 3x3 luma neighbourhood around the base texel.
    float gradient_xx = 0.0f;
    float gradient_yy = 0.0f;
    float gradient_xy = 0.0f;
    [unroll]
    for (int offset_y = 0; offset_y <= 2; ++offset_y) {
        [unroll]
        for (int offset_x = 0; offset_x <= 2; ++offset_x) {
            const float2 centre = base + float2(offset_x - 1, offset_y - 1);
            const float left = Luma(FetchSource(centre + float2(-1.0f, 0.0f)));
            const float right = Luma(FetchSource(centre + float2(1.0f, 0.0f)));
            const float up = Luma(FetchSource(centre + float2(0.0f, -1.0f)));
            const float down = Luma(FetchSource(centre + float2(0.0f, 1.0f)));
            const float dx = (right - left) * 0.5f;
            const float dy = (down - up) * 0.5f;
            gradient_xx += dx * dx;
            gradient_yy += dy * dy;
            gradient_xy += dx * dy;
        }
    }

    // Principal direction of the tensor. `trace` and `determinant` give the two
    // eigenvalues without a matrix decomposition; their ratio is how directional
    // the neighbourhood is, and it is what decides whether to stretch at all.
    const float trace = gradient_xx + gradient_yy;
    const float difference = gradient_xx - gradient_yy;
    const float root = sqrt(max(difference * difference + 4.0f * gradient_xy * gradient_xy, 0.0f));
    const float major = 0.5f * (trace + root);
    const float minor = 0.5f * (trace - root);

    // Both eigenvector forms of a symmetric 2x2, because each one degenerates
    // where the other does not. (difference + root, 2*gxy) collapses to exactly
    // (0, 0) whenever gxy is 0 and gyy exceeds gxx -- a plain vertical edge,
    // which is not a corner case -- and normalize() of that is NaN, which then
    // propagates through every tap weight and turns the pixel into garbage.
    // Taking whichever form has more magnitude avoids both collapses.
    const float2 candidate_a = float2(difference + root, 2.0f * gradient_xy);
    const float2 candidate_b = float2(2.0f * gradient_xy, root - difference);
    const float2 chosen =
        (dot(candidate_a, candidate_a) >= dot(candidate_b, candidate_b)) ? candidate_a : candidate_b;
    const float2 direction =
        (dot(chosen, chosen) > 1e-12f) ? normalize(chosen) : float2(1.0f, 0.0f);
    // 0 where the neighbourhood is flat or isotropic, approaching 1 on a clean
    // edge. Anything below the floor is treated as isotropic, because a tensor
    // built from near-zero gradients has a direction made of noise.
    const float anisotropy = (major > 1e-5f) ? saturate(1.0f - (minor / major)) : 0.0f;
    // Squared, and gated on absolute gradient energy as well as on the ratio.
    //
    // Measured, not guessed. A linear gate stretched the kernel on anything with
    // a direction at all, including detail that the source no longer resolves --
    // below Nyquist the tensor still reports a confident direction, but it is a
    // direction built from aliasing, and steering along it is worse than not
    // steering. Squaring pushes the kernel back to isotropic everywhere except
    // on neighbourhoods that are strongly and unambiguously directional, which
    // is where the steering is actually earning something.
    const float strength = anisotropy * anisotropy * saturate(major * 16.0f);
    // Across-edge taps are compressed by up to 2.5x, which is enough to hold a
    // diagonal together and mild enough not to erase texture that happens to sit
    // near one.
    const float across_scale = 1.0f + 1.5f * strength;

    float3 accumulated = 0.0f;
    float weight_total = 0.0f;
    [unroll]
    for (int tap_y = -1; tap_y <= 2; ++tap_y) {
        [unroll]
        for (int tap_x = -1; tap_x <= 2; ++tap_x) {
            const float2 tap = float2(tap_x, tap_y);
            const float2 delta = tap - fraction;
            // Split the offset into along-edge and across-edge components and
            // measure distance in the stretched space.
            const float along = dot(delta, float2(direction.y, -direction.x));
            const float across = dot(delta, direction) * across_scale;
            const float distance_squared = along * along + across * across;
            // Compact positive windowed kernel, zero past 2.0 so the tap set
            // stays 4x4.
            //
            // A radial Catmull-Rom was built here and measured, on the reasoning
            // that reconstruction wants a negative lobe. It was worse: mean gain
            // over bilinear fell from +0.51 dB to -0.12 dB and every instant
            // regressed. Separable Catmull-Rom earns its lobe because the two 1D
            // passes each sum to one; scattering the same lobe radially over a
            // 4x4 grid does not, and the residual is a ring the normalisation
            // cannot remove. Do not re-add one without a measurement.
            const float window = saturate(1.0f - distance_squared * 0.25f);
            const float weight = window * window * exp2(-1.6f * distance_squared);
            accumulated += FetchSource(base + tap) * weight;
            weight_total += weight;
        }
    }

    const float3 colour = accumulated / max(weight_total, 1e-5f);
    return float4(saturate(colour), 1.0f);
}

// Contrast-limited sharpen.
//
// A 5-tap cross sharpener whose result is clamped into the range the immediate
// neighbourhood already spans. Unlimited sharpening after an upscale overshoots
// on both sides of every edge, and that halo is what makes upscaled output
// recognisable at a glance. Clamping to the local min/max cannot remove
// sharpening that the neighbourhood supports, and cannot invent contrast it
// does not.
float4 SharpenMain(VertexOutput input) : SV_Target {
    const float3 centre = source_texture.SampleLevel(linear_clamp, input.uv, 0).rgb;
    if (sharpness <= 0.0f) {
        return float4(centre, 1.0f);
    }

    const float2 step_x = float2(inverse_output_size.x, 0.0f);
    const float2 step_y = float2(0.0f, inverse_output_size.y);
    const float3 left = source_texture.SampleLevel(linear_clamp, input.uv - step_x, 0).rgb;
    const float3 right = source_texture.SampleLevel(linear_clamp, input.uv + step_x, 0).rgb;
    const float3 up = source_texture.SampleLevel(linear_clamp, input.uv - step_y, 0).rgb;
    const float3 down = source_texture.SampleLevel(linear_clamp, input.uv + step_y, 0).rgb;

    const float3 neighbour_minimum = min(min(left, right), min(up, down));
    const float3 neighbour_maximum = max(max(left, right), max(up, down));

    // Scale the amount by how much headroom the neighbourhood has. A region
    // already at full contrast gets less, which is where ringing would show.
    const float3 headroom = min(neighbour_minimum, 1.0f - neighbour_maximum);
    const float3 span = max(neighbour_maximum - neighbour_minimum, 1e-4f);
    const float amount = sharpness * 0.4f;
    const float3 gain = amount * saturate(headroom / span);

    const float3 blurred = (left + right + up + down) * 0.25f;
    const float3 sharpened = centre + (centre - blurred) * (1.0f + gain * 4.0f);

    // Never leave the range the neighbourhood plus the centre already spans.
    const float3 lower = min(neighbour_minimum, centre);
    const float3 upper = max(neighbour_maximum, centre);
    return float4(clamp(sharpened, lower, upper), 1.0f);
}
)";

} // namespace

Upscaler::Upscaler(ID3D11Device* const device, ID3D11DeviceContext* const context)
    : timing_(device, context) {
    if (!device || !context) {
        throw std::invalid_argument("Upscaler requires a Direct3D 11 device and context.");
    }
    device_.copy_from(device);
    context_.copy_from(context);

    const std::string_view source(kUpscaleShaderSource);
    const std::vector<ShaderCompileRequest> requests{
        {source, "VertexMain", "vs_5_0"},
        {source, "UpsampleMain", "ps_5_0"},
        {source, "SharpenMain", "ps_5_0"},
    };
    const auto compiled = CompileShadersCached(
        requests,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        "Upscale shader compilation failed for ");
    shaders_came_from_cache_ = std::all_of(
        compiled.begin(),
        compiled.end(),
        [](const ShaderCompileResult& result) {
            return result.outcome == ShaderCacheOutcome::hit;
        });

    winrt::check_hresult(device_->CreateVertexShader(
        compiled[0].bytecode->GetBufferPointer(),
        compiled[0].bytecode->GetBufferSize(),
        nullptr,
        vertex_shader_.put()));
    winrt::check_hresult(device_->CreatePixelShader(
        compiled[1].bytecode->GetBufferPointer(),
        compiled[1].bytecode->GetBufferSize(),
        nullptr,
        upsample_shader_.put()));
    winrt::check_hresult(device_->CreatePixelShader(
        compiled[2].bytecode->GetBufferPointer(),
        compiled[2].bytecode->GetBufferSize(),
        nullptr,
        sharpen_shader_.put()));

    D3D11_SAMPLER_DESC sampler_description{};
    sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_description.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
    winrt::check_hresult(device_->CreateSamplerState(&sampler_description, sampler_.put()));

    D3D11_BUFFER_DESC constants_description{};
    constants_description.ByteWidth = sizeof(UpscaleConstants);
    constants_description.Usage = D3D11_USAGE_DYNAMIC;
    constants_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    winrt::check_hresult(device_->CreateBuffer(
        &constants_description,
        nullptr,
        constants_.put()));
}

void Upscaler::CreateIntermediate(const UINT width, const UINT height) {
    if (intermediate_ && intermediate_width_ == width && intermediate_height_ == height) {
        return;
    }
    intermediate_target_ = nullptr;
    intermediate_view_ = nullptr;
    intermediate_ = nullptr;

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = kIntermediateFormat;
    description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    winrt::check_hresult(device_->CreateTexture2D(&description, nullptr, intermediate_.put()));
    winrt::check_hresult(device_->CreateShaderResourceView(
        intermediate_.get(),
        nullptr,
        intermediate_view_.put()));
    winrt::check_hresult(device_->CreateRenderTargetView(
        intermediate_.get(),
        nullptr,
        intermediate_target_.put()));
    intermediate_width_ = width;
    intermediate_height_ = height;
}

bool Upscaler::Draw(
    ID3D11ShaderResourceView* const source,
    const UINT source_width,
    const UINT source_height,
    ID3D11RenderTargetView* const destination,
    const UINT output_width,
    const UINT output_height) noexcept {
    try {
        if (!source || !destination || source_width == 0 || source_height == 0 ||
            output_width == 0 || output_height == 0) {
            throw std::invalid_argument("Upscaler::Draw received an empty surface.");
        }
        CreateIntermediate(output_width, output_height);

        UpscaleConstants constants{};
        constants.source_size[0] = static_cast<float>(source_width);
        constants.source_size[1] = static_cast<float>(source_height);
        constants.inverse_source_size[0] = 1.0F / static_cast<float>(source_width);
        constants.inverse_source_size[1] = 1.0F / static_cast<float>(source_height);
        constants.output_size[0] = static_cast<float>(output_width);
        constants.output_size[1] = static_cast<float>(output_height);
        constants.inverse_output_size[0] = 1.0F / static_cast<float>(output_width);
        constants.inverse_output_size[1] = 1.0F / static_cast<float>(output_height);
        constants.sharpness = sharpness_;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context_->Map(
            constants_.get(),
            0,
            D3D11_MAP_WRITE_DISCARD,
            0,
            &mapped));
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(constants_.get(), 0);

        const D3D11_VIEWPORT viewport{
            0.0F,
            0.0F,
            static_cast<float>(output_width),
            static_cast<float>(output_height),
            0.0F,
            1.0F};

        timing_.Begin();

        ID3D11Buffer* constant_buffers[]{constants_.get()};
        ID3D11SamplerState* samplers[]{sampler_.get()};
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_shader_.get(), nullptr, 0);
        context_->RSSetViewports(1, &viewport);
        context_->PSSetConstantBuffers(0, 1, constant_buffers);
        context_->PSSetSamplers(0, 1, samplers);

        // Pass 1: source -> intermediate, at output resolution.
        ID3D11RenderTargetView* upsample_target = intermediate_target_.get();
        context_->OMSetRenderTargets(1, &upsample_target, nullptr);
        ID3D11ShaderResourceView* upsample_input[]{source};
        context_->PSSetShader(upsample_shader_.get(), nullptr, 0);
        context_->PSSetShaderResources(0, 1, upsample_input);
        context_->Draw(3, 0);

        // Unbind before the intermediate becomes an input, or the runtime drops
        // the binding and the second pass reads nothing.
        ID3D11ShaderResourceView* const cleared[]{nullptr};
        context_->PSSetShaderResources(0, 1, cleared);

        // Pass 2: intermediate -> destination, same resolution.
        ID3D11RenderTargetView* sharpen_target = destination;
        context_->OMSetRenderTargets(1, &sharpen_target, nullptr);
        ID3D11ShaderResourceView* sharpen_input[]{intermediate_view_.get()};
        context_->PSSetShader(sharpen_shader_.get(), nullptr, 0);
        context_->PSSetShaderResources(0, 1, sharpen_input);
        context_->Draw(3, 0);
        context_->PSSetShaderResources(0, 1, cleared);

        timing_.End();
        return true;
    } catch (const winrt::hresult_error& error) {
        timing_.Cancel();
        StoreError(winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        timing_.Cancel();
        StoreError(error.what());
    }
    return false;
}

void Upscaler::SetSharpness(const float sharpness) noexcept {
    sharpness_ = std::clamp(sharpness, kMinimumSharpness, kMaximumSharpness);
}

float Upscaler::Sharpness() const noexcept {
    return sharpness_;
}

void Upscaler::PollGpuTimings() noexcept {
    timing_.Poll();
}

GpuTimingStatistics Upscaler::GpuTiming() const noexcept {
    return timing_.Statistics();
}

bool Upscaler::ShadersCameFromCache() const noexcept {
    return shaders_came_from_cache_;
}

const std::string& Upscaler::LastError() const noexcept {
    return last_error_;
}

void Upscaler::StoreError(const std::string& message) noexcept {
    try {
        last_error_ = message;
    } catch (...) {
        // A diagnostic string is not worth propagating an allocation failure
        // into the presentation path.
    }
}

} // namespace osss
