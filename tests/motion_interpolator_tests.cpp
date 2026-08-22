#include "motion_interpolator.h"
#include "test_harness.h"

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr UINT kWidth = 96;
constexpr UINT kHeight = 64;
constexpr UINT kBoxSize = 16;
constexpr UINT kPreviousBoxX = 16;
constexpr UINT kCurrentBoxX = 32;
constexpr UINT kBoxY = 24;

constexpr char kVertexShaderSource[] = R"(
struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput main(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    output.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(
        output.uv.x * 2.0f - 1.0f,
        1.0f - output.uv.y * 2.0f,
        0.0f,
        1.0f);
    return output;
}
)";

using osss::test::Require;

winrt::com_ptr<ID3D11VertexShader> CreateVertexShader(ID3D11Device* const device) {
    winrt::com_ptr<ID3DBlob> bytecode;
    winrt::com_ptr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        kVertexShaderSource,
        std::strlen(kVertexShaderSource),
        nullptr,
        nullptr,
        nullptr,
        "main",
        "vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        bytecode.put(),
        errors.put());
    if (FAILED(result)) {
        std::string message = "Test vertex shader compilation failed.";
        if (errors) {
            message.append(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        throw std::runtime_error(message);
    }

    winrt::com_ptr<ID3D11VertexShader> shader;
    winrt::check_hresult(device->CreateVertexShader(
        bytecode->GetBufferPointer(),
        bytecode->GetBufferSize(),
        nullptr,
        shader.put()));
    return shader;
}

std::vector<std::uint32_t> CreateFrame(const UINT box_x) {
    std::vector<std::uint32_t> pixels(kWidth * kHeight, 0xFF101010U);
    for (UINT y = kBoxY; y < kBoxY + kBoxSize; ++y) {
        for (UINT x = box_x; x < box_x + kBoxSize; ++x) {
            pixels[y * kWidth + x] = 0xFFFFFFFFU;
        }
    }
    return pixels;
}

std::vector<std::uint32_t> CreateSolidFrame(const std::uint32_t color) {
    return std::vector<std::uint32_t>(kWidth * kHeight, color);
}

// A "HUD counter" band across the top that flips colour between frames while
// the same box as CreateFrame moves underneath it.
constexpr UINT kHudTop = 2;
constexpr UINT kHudBottom = 8;
constexpr UINT kHudLeft = 8;
constexpr UINT kHudRight = 88;

std::vector<std::uint32_t> CreateHudFrame(const UINT box_x, const std::uint32_t hud_color) {
    std::vector<std::uint32_t> pixels = CreateFrame(box_x);
    for (UINT y = kHudTop; y < kHudBottom; ++y) {
        for (UINT x = kHudLeft; x < kHudRight; ++x) {
            pixels[y * kWidth + x] = hud_color;
        }
    }
    return pixels;
}

winrt::com_ptr<ID3D11Texture2D> CreateSourceTexture(
    ID3D11Device* const device,
    const std::vector<std::uint32_t>& pixels) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = kWidth;
    description.Height = kHeight;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initial_data{};
    initial_data.pSysMem = pixels.data();
    initial_data.SysMemPitch = kWidth * sizeof(std::uint32_t);

    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(device->CreateTexture2D(&description, &initial_data, texture.put()));
    return texture;
}

winrt::com_ptr<ID3D11ShaderResourceView> CreateView(
    ID3D11Device* const device,
    ID3D11Texture2D* const texture) {
    winrt::com_ptr<ID3D11ShaderResourceView> view;
    winrt::check_hresult(device->CreateShaderResourceView(texture, nullptr, view.put()));
    return view;
}

float RegionLuminance(
    const D3D11_MAPPED_SUBRESOURCE& mapped,
    const UINT left,
    const UINT top,
    const UINT right,
    const UINT bottom) {
    double total = 0.0;
    std::uint64_t count = 0;
    for (UINT y = top; y < bottom; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        for (UINT x = left; x < right; ++x) {
            const std::uint8_t* pixel = row + x * 4;
            const float blue = static_cast<float>(pixel[0]) / 255.0F;
            const float green = static_cast<float>(pixel[1]) / 255.0F;
            const float red = static_cast<float>(pixel[2]) / 255.0F;
            total += 0.2126 * red + 0.7152 * green + 0.0722 * blue;
            ++count;
        }
    }
    return static_cast<float>(total / static_cast<double>(std::max<std::uint64_t>(1, count)));
}

// Auto-detection fixture: a left-side panel that holds still while a box moves
// past and behind it. The panel is what a real static HUD looks like to the
// detector; it "ticks" only on the frame under test.
constexpr UINT kPanelLeft = 36;
constexpr UINT kPanelTop = 20;
constexpr UINT kPanelRight = 60;
constexpr UINT kPanelBottom = 44;

// Triangle path so the box never leaves the frame and never stalls: the
// detector needs continuous scene motion to accumulate evidence against.
UINT PanelSequenceBoxX(const int step) {
    constexpr int period = 12;
    const int phase = step % period;
    const int position = phase <= period / 2 ? phase : period - phase;
    return static_cast<UINT>(2 + position * 12);
}
constexpr std::uint32_t kPanelIdle = 0xFF3060C0U;
constexpr std::uint32_t kPanelTicked = 0xFFC08030U;

std::vector<std::uint32_t> CreatePanelFrame(const UINT box_x, const std::uint32_t panel_color) {
    std::vector<std::uint32_t> pixels(kWidth * kHeight, 0xFF101010U);
    for (UINT y = kBoxY; y < kBoxY + kBoxSize; ++y) {
        for (UINT x = box_x; x < std::min(box_x + kBoxSize, kWidth); ++x) {
            pixels[y * kWidth + x] = 0xFFFFFFFFU;
        }
    }
    // Drawn last, so it occludes the moving box exactly as a HUD would.
    for (UINT y = kPanelTop; y < kPanelBottom; ++y) {
        for (UINT x = kPanelLeft; x < kPanelRight; ++x) {
            pixels[y * kWidth + x] = panel_color;
        }
    }
    return pixels;
}

std::uint32_t PixelAt(const D3D11_MAPPED_SUBRESOURCE& mapped, const UINT x, const UINT y) {
    const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
    const std::uint8_t* pixel = row + x * 4;
    return 0xFF000000U |
        (static_cast<std::uint32_t>(pixel[2]) << 16U) |
        (static_cast<std::uint32_t>(pixel[1]) << 8U) |
        static_cast<std::uint32_t>(pixel[0]);
}

// Largest per-channel distance from the panel interior to a reference colour.
int PanelChannelError(const D3D11_MAPPED_SUBRESOURCE& mapped, const std::uint32_t reference) {
    int worst = 0;
    for (UINT y = kPanelTop + 2; y < kPanelBottom - 2; ++y) {
        for (UINT x = kPanelLeft + 2; x < kPanelRight - 2; ++x) {
            const std::uint32_t observed = PixelAt(mapped, x, y);
            for (const unsigned shift : {0U, 8U, 16U}) {
                worst = std::max(
                    worst,
                    std::abs(
                        static_cast<int>((observed >> shift) & 0xFFU) -
                        static_cast<int>((reference >> shift) & 0xFFU)));
            }
        }
    }
    return worst;
}

} // namespace

int main() {
    try {
        constexpr D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL selected_level{};
        winrt::check_hresult(D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            &feature_level,
            1,
            D3D11_SDK_VERSION,
            device.put(),
            &selected_level,
            context.put()));

        const auto previous_texture = CreateSourceTexture(device.get(), CreateFrame(kPreviousBoxX));
        const auto current_texture = CreateSourceTexture(device.get(), CreateFrame(kCurrentBoxX));
        const auto previous_view = CreateView(device.get(), previous_texture.get());
        const auto current_view = CreateView(device.get(), current_texture.get());

        D3D11_TEXTURE2D_DESC output_description{};
        output_description.Width = kWidth;
        output_description.Height = kHeight;
        output_description.MipLevels = 1;
        output_description.ArraySize = 1;
        output_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        output_description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
        output_description.Usage = D3D11_USAGE_DEFAULT;
        output_description.BindFlags = D3D11_BIND_RENDER_TARGET;
        winrt::com_ptr<ID3D11Texture2D> output_texture;
        winrt::check_hresult(device->CreateTexture2D(&output_description, nullptr, output_texture.put()));

        winrt::com_ptr<ID3D11RenderTargetView> render_target;
        winrt::check_hresult(device->CreateRenderTargetView(
            output_texture.get(),
            nullptr,
            render_target.put()));

        D3D11_TEXTURE2D_DESC staging_description = output_description;
        staging_description.Usage = D3D11_USAGE_STAGING;
        staging_description.BindFlags = 0;
        staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> staging_texture;
        winrt::check_hresult(device->CreateTexture2D(
            &staging_description,
            nullptr,
            staging_texture.put()));

        osss::MotionInterpolator interpolator(device.get(), context.get());
        Require(
            interpolator.PreparePair(previous_view.get(), current_view.get(), kWidth, kHeight),
            "Motion preparation failed: " + interpolator.LastError());

        const auto vertex_shader = CreateVertexShader(device.get());
        ID3D11RenderTargetView* target = render_target.get();
        context->OMSetRenderTargets(1, &target, nullptr);
        const D3D11_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0F, 1.0F};
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex_shader.get(), nullptr, 0);
        Require(
            interpolator.BindForDraw(previous_view.get(), current_view.get(), 0.5F),
            "Motion draw binding failed: " + interpolator.LastError());
        context->Draw(3, 0);
        interpolator.UnbindAfterDraw();

        context->CopyResource(staging_texture.get(), output_texture.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context->Map(staging_texture.get(), 0, D3D11_MAP_READ, 0, &mapped));

        const float midpoint_luminance = RegionLuminance(mapped, 24, kBoxY, 40, kBoxY + kBoxSize);
        const float trailing_luminance = RegionLuminance(mapped, 16, kBoxY, 24, kBoxY + kBoxSize);
        const float leading_luminance = RegionLuminance(mapped, 40, kBoxY, 48, kBoxY + kBoxSize);
        context->Unmap(staging_texture.get(), 0);

        std::cout
            << "midpoint=" << midpoint_luminance
            << " trailing=" << trailing_luminance
            << " leading=" << leading_luminance << '\n';
        Require(midpoint_luminance > 0.52F, "Interpolated object did not reconstruct strongly at the midpoint.");
        Require(
            midpoint_luminance > trailing_luminance + 0.08F,
            "Motion reconstruction did not reduce the trailing double image.");
        Require(
            midpoint_luminance > leading_luminance + 0.08F,
            "Motion reconstruction did not reduce the leading double image.");

        const auto cut_previous_texture = CreateSourceTexture(device.get(), CreateSolidFrame(0xFF000000U));
        const auto cut_current_texture = CreateSourceTexture(device.get(), CreateSolidFrame(0xFFFFFFFFU));
        const auto cut_previous_view = CreateView(device.get(), cut_previous_texture.get());
        const auto cut_current_view = CreateView(device.get(), cut_current_texture.get());
        Require(
            interpolator.PreparePair(cut_previous_view.get(), cut_current_view.get(), kWidth, kHeight),
            "Scene-cut motion preparation failed: " + interpolator.LastError());
        Require(
            interpolator.BindForDraw(cut_previous_view.get(), cut_current_view.get(), 0.5F),
            "Scene-cut draw binding failed: " + interpolator.LastError());
        context->Draw(3, 0);
        interpolator.UnbindAfterDraw();
        context->CopyResource(staging_texture.get(), output_texture.get());
        winrt::check_hresult(context->Map(staging_texture.get(), 0, D3D11_MAP_READ, 0, &mapped));
        const float scene_cut_luminance = RegionLuminance(mapped, 32, 16, 64, 48);
        context->Unmap(staging_texture.get(), 0);
        Require(
            scene_cut_luminance > 0.98F,
            "Scene-cut detection blended unrelated source frames instead of selecting the current frame.");

        // UI mask: a HUD band that flips white -> dark must cross-fade without a
        // mask and snap to the newest real frame with one, while the moving box
        // underneath still reconstructs.
        const auto hud_previous_texture =
            CreateSourceTexture(device.get(), CreateHudFrame(kPreviousBoxX, 0xFFFFFFFFU));
        const auto hud_current_texture =
            CreateSourceTexture(device.get(), CreateHudFrame(kCurrentBoxX, 0xFF202020U));
        const auto hud_previous_view = CreateView(device.get(), hud_previous_texture.get());
        const auto hud_current_view = CreateView(device.get(), hud_current_texture.get());

        const auto draw_hud_pair = [&](const char* label) {
            Require(
                interpolator.PreparePair(hud_previous_view.get(), hud_current_view.get(), kWidth, kHeight),
                std::string(label) + " motion preparation failed: " + interpolator.LastError());
            Require(
                interpolator.BindForDraw(hud_previous_view.get(), hud_current_view.get(), 0.5F),
                std::string(label) + " draw binding failed: " + interpolator.LastError());
            context->Draw(3, 0);
            interpolator.UnbindAfterDraw();
            context->CopyResource(staging_texture.get(), output_texture.get());
        };

        draw_hud_pair("Unmasked HUD");
        winrt::check_hresult(context->Map(staging_texture.get(), 0, D3D11_MAP_READ, 0, &mapped));
        const float unmasked_hud_luminance = RegionLuminance(mapped, kHudLeft, kHudTop, kHudRight, kHudBottom);
        const float unmasked_box_luminance = RegionLuminance(mapped, 24, kBoxY, 40, kBoxY + kBoxSize);
        context->Unmap(staging_texture.get(), 0);
        Require(
            unmasked_hud_luminance > 0.30F && unmasked_hud_luminance < 0.85F,
            "Without a mask the flipping HUD band should cross-fade at the midpoint.");

        const auto mask = osss::ParseUiMaskRects(std::string_view("8,2,88,8px"));
        Require(mask.Ok() && mask.rects.size() == 1, "HUD mask text must parse: " + mask.error);
        Require(interpolator.SetUiMask(mask.rects), "SetUiMask failed: " + interpolator.LastError());
        Require(!interpolator.Ready(), "Changing the mask must invalidate prepared flow.");
        Require(interpolator.UiMask() == mask.rects, "SetUiMask must retain the regions.");

        draw_hud_pair("Masked HUD");
        winrt::check_hresult(context->Map(staging_texture.get(), 0, D3D11_MAP_READ, 0, &mapped));
        const float masked_hud_luminance = RegionLuminance(mapped, kHudLeft, kHudTop, kHudRight, kHudBottom);
        // One pixel inside the mask edge so bilinear feathering does not vote.
        const float masked_hud_interior_luminance =
            RegionLuminance(mapped, kHudLeft + 1, kHudTop + 1, kHudRight - 1, kHudBottom - 1);
        const float masked_box_luminance = RegionLuminance(mapped, 24, kBoxY, 40, kBoxY + kBoxSize);
        const float masked_box_trailing = RegionLuminance(mapped, 16, kBoxY, 24, kBoxY + kBoxSize);
        const float masked_box_leading = RegionLuminance(mapped, 40, kBoxY, 48, kBoxY + kBoxSize);
        context->Unmap(staging_texture.get(), 0);

        std::cout
            << "hud-unmasked=" << unmasked_hud_luminance
            << " hud-masked=" << masked_hud_luminance
            << " hud-masked-interior=" << masked_hud_interior_luminance
            << " box-unmasked=" << unmasked_box_luminance
            << " box-masked=" << masked_box_luminance << '\n';
        Require(
            masked_hud_interior_luminance < 0.16F,
            "Masked HUD interior must show the newest real frame instead of a blend.");
        Require(
            masked_hud_luminance < unmasked_hud_luminance - 0.25F,
            "Masking must remove the HUD cross-fade.");
        Require(
            masked_box_luminance > 0.52F &&
                masked_box_luminance > masked_box_trailing + 0.08F &&
                masked_box_luminance > masked_box_leading + 0.08F,
            "The moving object must still reconstruct with a HUD mask active elsewhere.");

        Require(interpolator.SetUiMask({}), "Clearing the mask failed: " + interpolator.LastError());
        draw_hud_pair("Cleared-mask HUD");
        winrt::check_hresult(context->Map(staging_texture.get(), 0, D3D11_MAP_READ, 0, &mapped));
        const float cleared_hud_luminance = RegionLuminance(mapped, kHudLeft, kHudTop, kHudRight, kHudBottom);
        context->Unmap(staging_texture.get(), 0);
        Require(
            std::fabs(cleared_hud_luminance - unmasked_hud_luminance) < 0.02F,
            "Clearing the mask must restore unmasked behaviour.");

        // Automatic static-overlay detection. The panel must be discovered from
        // the frames alone, with no user rectangle supplied.
        Require(interpolator.SetUiMask({}), "Clearing the mask for auto-detect failed.");

        const auto run_panel_sequence = [&](const bool automatic, const int warmup_pairs) {
            interpolator.SetAutoUiMaskEnabled(automatic);
            std::vector<winrt::com_ptr<ID3D11Texture2D>> textures;
            std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> views;
            for (int step = 0; step <= warmup_pairs + 1; ++step) {
                const bool ticked = step == warmup_pairs + 1;
                textures.push_back(CreateSourceTexture(
                    device.get(),
                    CreatePanelFrame(
                        PanelSequenceBoxX(step),
                        ticked ? kPanelTicked : kPanelIdle)));
                views.push_back(CreateView(device.get(), textures.back().get()));
            }
            // Walk the sequence so the detector accumulates persistence.
            for (std::size_t step = 0; step + 1 < views.size(); ++step) {
                Require(
                    interpolator.PreparePair(
                        views[step].get(),
                        views[step + 1].get(),
                        kWidth,
                        kHeight),
                    "Auto-detect motion preparation failed: " + interpolator.LastError());
            }
            // The final pair is the one where the panel ticks.
            Require(
                interpolator.BindForDraw(
                    views[views.size() - 2].get(),
                    views.back().get(),
                    0.5F),
                "Auto-detect draw binding failed: " + interpolator.LastError());
            context->Draw(3, 0);
            interpolator.UnbindAfterDraw();
            context->CopyResource(staging_texture.get(), output_texture.get());
        };

        run_panel_sequence(false, 12);
        winrt::check_hresult(context->Map(staging_texture.get(), 0, D3D11_MAP_READ, 0, &mapped));
        const int unmasked_to_new = PanelChannelError(mapped, kPanelTicked);
        const int unmasked_to_old = PanelChannelError(mapped, kPanelIdle);
        context->Unmap(staging_texture.get(), 0);
        Require(
            unmasked_to_new > 24 && unmasked_to_old > 24,
            "Without auto-detection the ticking panel must land between its two states.");

        run_panel_sequence(true, 12);
        {
            std::vector<osss::MotionInterpolator::UiPersistenceSample> state;
            UINT state_width = 0;
            UINT state_height = 0;
            Require(
                interpolator.ReadUiPersistence(state, state_width, state_height),
                "Reading detector state failed.");
            const UINT scale = kWidth / state_width;
            double panel_score = 0.0;
            double panel_difference = 0.0;
            double panel_motion = 0.0;
            double outside_score = 0.0;
            int panel_cells = 0;
            int outside_cells = 0;
            for (UINT y = 0; y < state_height; ++y) {
                for (UINT x = 0; x < state_width; ++x) {
                    const auto& sample = state[y * state_width + x];
                    const UINT pixel_x = x * scale + scale / 2;
                    const UINT pixel_y = y * scale + scale / 2;
                    const bool inside = pixel_x >= kPanelLeft && pixel_x < kPanelRight &&
                        pixel_y >= kPanelTop && pixel_y < kPanelBottom;
                    if (inside) {
                        panel_score += sample.score;
                        panel_difference += sample.frame_difference;
                        panel_motion += sample.neighbour_motion;
                        ++panel_cells;
                    } else {
                        outside_score += sample.score;
                        ++outside_cells;
                    }
                }
            }
            std::cout
                << "detector cells=" << state_width << "x" << state_height
                << " panel-score=" << panel_score / std::max(1, panel_cells)
                << " panel-diff=" << panel_difference / std::max(1, panel_cells)
                << " panel-motion=" << panel_motion / std::max(1, panel_cells)
                << " outside-score=" << outside_score / std::max(1, outside_cells) << '\n';
        }
        winrt::check_hresult(context->Map(staging_texture.get(), 0, D3D11_MAP_READ, 0, &mapped));
        const int auto_to_new = PanelChannelError(mapped, kPanelTicked);
        // On the tick pair the box moves from PanelSequenceBoxX(12) to (13), so
        // at alpha 0.5 it straddles the midpoint. Sample inside that midpoint
        // box, well clear of the panel.
        const UINT box_midpoint = (PanelSequenceBoxX(12) + PanelSequenceBoxX(13)) / 2;
        const float auto_box_luminance = RegionLuminance(
            mapped,
            box_midpoint + 4,
            kBoxY + 4,
            box_midpoint + kBoxSize - 4,
            kBoxY + kBoxSize - 4);
        context->Unmap(staging_texture.get(), 0);

        // Arming must be earned: one pair of evidence is not enough.
        interpolator.SetAutoUiMaskEnabled(false);
        run_panel_sequence(true, 1);
        winrt::check_hresult(context->Map(staging_texture.get(), 0, D3D11_MAP_READ, 0, &mapped));
        const int cold_to_new = PanelChannelError(mapped, kPanelTicked);
        context->Unmap(staging_texture.get(), 0);

        std::cout
            << "auto-panel-armed-err=" << auto_to_new
            << " auto-panel-cold-err=" << cold_to_new
            << " unmasked-err-to-new=" << unmasked_to_new
            << " auto-box-luminance=" << auto_box_luminance << '\n';
        Require(
            auto_to_new <= 8,
            "An auto-detected static overlay must show the newest real frame when it ticks.");
        Require(
            auto_to_new < unmasked_to_new - 24,
            "Auto-detection must measurably beat the unmasked cross-fade.");
        Require(
            cold_to_new > 24,
            "The detector must require several source pairs before it masks anything.");
        Require(
            auto_box_luminance > 0.5F,
            "Auto-detection must not suppress the moving content it was detected against.");

        interpolator.SetAutoUiMaskEnabled(false);
        Require(!interpolator.AutoUiMaskEnabled(), "Auto-detection must report its state.");

        std::cout
            << "scene-cut-current=" << scene_cut_luminance << '\n'
            << "OSSS deterministic motion interpolation test passed.\n";
        return EXIT_SUCCESS;
    } catch (const winrt::hresult_error& error) {
        std::cerr << "Windows error: " << winrt::to_string(error.message()) << '\n';
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
}
