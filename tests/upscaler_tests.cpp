// Reference-image quality bench for Upscaler.
//
// The measurement is a round trip, not a rescale of the pattern at two sizes.
// The test pattern draws its lanes at fixed pixel rows, so rendering it at
// 1920x1080 and at 960x540 does not produce the same image at two scales -- it
// produces the same lanes with different margins. Comparing those would measure
// the pattern, not the upscaler.
//
// So: render the pattern, box-downsample it by two, upscale it back, and score
// against the original. That asks the only question worth asking of an
// upscaler -- how much of what was removed does it put back -- and it has an
// exact answer to score against.
//
// The baseline it has to beat is bilinear. Reporting a PSNR alone would hide
// whether the edge-directed kernel does anything: bilinear on a 2x upscale is
// already respectable, and an upscaler that ties with it is an expensive blur.

#include "test_harness.h"
#include "test_pattern.h"
#include "upscaler.h"

#include <windows.h>

#include <d3d11.h>
#include <winrt/base.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using osss::test::Require;

int ChannelOf(const std::uint32_t pixel, const unsigned shift) {
    return static_cast<int>((pixel >> shift) & 0xFFU);
}

std::uint32_t PackPixel(const int blue, const int green, const int red) {
    return 0xFF000000U |
        (static_cast<std::uint32_t>(std::clamp(red, 0, 255)) << 16) |
        (static_cast<std::uint32_t>(std::clamp(green, 0, 255)) << 8) |
        static_cast<std::uint32_t>(std::clamp(blue, 0, 255));
}

// Box downsample by exactly two. Deterministic, and the inverse the upscaler is
// being asked to approximate.
std::vector<std::uint32_t> HalveByBox(
    std::span<const std::uint32_t> source,
    const std::uint32_t width,
    const std::uint32_t height) {
    const std::uint32_t half_width = width / 2;
    const std::uint32_t half_height = height / 2;
    std::vector<std::uint32_t> result(
        static_cast<std::size_t>(half_width) * half_height,
        0xFF000000U);
    for (std::uint32_t y = 0; y < half_height; ++y) {
        for (std::uint32_t x = 0; x < half_width; ++x) {
            int totals[3]{0, 0, 0};
            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const std::size_t index =
                        static_cast<std::size_t>(y * 2 + dy) * width + (x * 2 + dx);
                    for (int channel = 0; channel < 3; ++channel) {
                        totals[channel] += ChannelOf(source[index], static_cast<unsigned>(channel * 8));
                    }
                }
            }
            result[static_cast<std::size_t>(y) * half_width + x] =
                PackPixel(totals[0] / 4, totals[1] / 4, totals[2] / 4);
        }
    }
    return result;
}

// The baseline. Matches the GPU's texel-centre convention so the comparison is
// about the kernel and not about a half-pixel offset.
std::vector<std::uint32_t> BilinearUpscale(
    std::span<const std::uint32_t> source,
    const std::uint32_t source_width,
    const std::uint32_t source_height,
    const std::uint32_t output_width,
    const std::uint32_t output_height) {
    std::vector<std::uint32_t> result(
        static_cast<std::size_t>(output_width) * output_height,
        0xFF000000U);
    const double scale_x = static_cast<double>(source_width) / output_width;
    const double scale_y = static_cast<double>(source_height) / output_height;
    for (std::uint32_t y = 0; y < output_height; ++y) {
        const double source_y = (y + 0.5) * scale_y - 0.5;
        const double base_y = std::floor(source_y);
        const double fraction_y = source_y - base_y;
        for (std::uint32_t x = 0; x < output_width; ++x) {
            const double source_x = (x + 0.5) * scale_x - 0.5;
            const double base_x = std::floor(source_x);
            const double fraction_x = source_x - base_x;
            int channels[3]{0, 0, 0};
            for (int channel = 0; channel < 3; ++channel) {
                double accumulated = 0.0;
                for (int dy = 0; dy <= 1; ++dy) {
                    for (int dx = 0; dx <= 1; ++dx) {
                        const auto clamped_x = static_cast<std::uint32_t>(std::clamp(
                            static_cast<int>(base_x) + dx, 0, static_cast<int>(source_width) - 1));
                        const auto clamped_y = static_cast<std::uint32_t>(std::clamp(
                            static_cast<int>(base_y) + dy, 0, static_cast<int>(source_height) - 1));
                        const double weight =
                            (dx == 0 ? 1.0 - fraction_x : fraction_x) *
                            (dy == 0 ? 1.0 - fraction_y : fraction_y);
                        accumulated += weight * ChannelOf(
                            source[static_cast<std::size_t>(clamped_y) * source_width + clamped_x],
                            static_cast<unsigned>(channel * 8));
                    }
                }
                channels[channel] = static_cast<int>(std::lround(accumulated));
            }
            result[static_cast<std::size_t>(y) * output_width + x] =
                PackPixel(channels[0], channels[1], channels[2]);
        }
    }
    return result;
}

struct Harness {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    std::string driver;

    Harness() {
        constexpr D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
        D3D_FEATURE_LEVEL selected{};
        HRESULT result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            &feature_level,
            1,
            D3D11_SDK_VERSION,
            device.put(),
            &selected,
            context.put());
        if (SUCCEEDED(result)) {
            driver = "hardware";
        } else {
            winrt::check_hresult(D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                &feature_level,
                1,
                D3D11_SDK_VERSION,
                device.put(),
                &selected,
                context.put()));
            driver = "warp";
        }
    }

    [[nodiscard]] std::vector<std::uint32_t> Upscale(
        std::span<const std::uint32_t> source,
        const std::uint32_t source_width,
        const std::uint32_t source_height,
        const std::uint32_t output_width,
        const std::uint32_t output_height,
        const float sharpness) const {
        D3D11_TEXTURE2D_DESC source_description{};
        source_description.Width = source_width;
        source_description.Height = source_height;
        source_description.MipLevels = 1;
        source_description.ArraySize = 1;
        source_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        source_description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
        source_description.Usage = D3D11_USAGE_IMMUTABLE;
        source_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{};
        initial.pSysMem = source.data();
        initial.SysMemPitch = source_width * 4;
        winrt::com_ptr<ID3D11Texture2D> source_texture;
        winrt::check_hresult(device->CreateTexture2D(
            &source_description,
            &initial,
            source_texture.put()));
        winrt::com_ptr<ID3D11ShaderResourceView> source_view;
        winrt::check_hresult(device->CreateShaderResourceView(
            source_texture.get(),
            nullptr,
            source_view.put()));

        D3D11_TEXTURE2D_DESC output_description{};
        output_description.Width = output_width;
        output_description.Height = output_height;
        output_description.MipLevels = 1;
        output_description.ArraySize = 1;
        output_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        output_description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
        output_description.Usage = D3D11_USAGE_DEFAULT;
        output_description.BindFlags = D3D11_BIND_RENDER_TARGET;
        winrt::com_ptr<ID3D11Texture2D> output_texture;
        winrt::check_hresult(device->CreateTexture2D(
            &output_description,
            nullptr,
            output_texture.put()));
        winrt::com_ptr<ID3D11RenderTargetView> output_target;
        winrt::check_hresult(device->CreateRenderTargetView(
            output_texture.get(),
            nullptr,
            output_target.put()));

        osss::Upscaler upscaler(device.get(), context.get());
        upscaler.SetSharpness(sharpness);
        Require(
            upscaler.Draw(
                source_view.get(),
                source_width,
                source_height,
                output_target.get(),
                output_width,
                output_height),
            "Upscaler::Draw failed: " + upscaler.LastError());

        D3D11_TEXTURE2D_DESC staging_description = output_description;
        staging_description.Usage = D3D11_USAGE_STAGING;
        staging_description.BindFlags = 0;
        staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(device->CreateTexture2D(
            &staging_description,
            nullptr,
            staging.put()));
        context->CopyResource(staging.get(), output_texture.get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<std::uint32_t> result(
            static_cast<std::size_t>(output_width) * output_height,
            0xFF000000U);
        for (std::uint32_t y = 0; y < output_height; ++y) {
            const auto* row = reinterpret_cast<const std::uint32_t*>(
                static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch);
            std::copy(row, row + output_width, result.begin() + static_cast<std::size_t>(y) * output_width);
        }
        context->Unmap(staging.get(), 0);
        return result;
    }
};

} // namespace

int main(const int argument_count, char** const arguments) {
    try {
        bool report = false;
        float sharpness = osss::kDefaultSharpness;
        for (int index = 1; index < argument_count; ++index) {
            if (std::string_view(arguments[index]) == "--report") {
                report = true;
            } else if (std::string_view(arguments[index]) == "--sharpness" &&
                       index + 1 < argument_count) {
                sharpness = std::stof(arguments[++index]);
                if (!osss::IsValidSharpness(sharpness)) {
                    throw std::runtime_error("--sharpness expects 0.0 through 1.0.");
                }
            }
        }

        osss::TestPatternSpec specification;
        specification.api = osss::TestGraphicsApi::direct3d11;
        const Harness harness;

        struct Result {
            double animation_seconds;
            osss::TestPatternMetrics upscaled;
            osss::TestPatternMetrics bilinear;
        };
        std::vector<Result> results;

        // Several instants so the score is not one lucky frame: the pattern's
        // lanes are at different phases at each.
        for (const double seconds : {0.4, 1.7, 3.1, 4.6}) {
            const auto truth = osss::RenderTestPattern(
                specification,
                seconds,
                static_cast<std::uint64_t>(seconds * specification.base_fps));
            const auto halved = HalveByBox(truth, specification.width, specification.height);
            const std::uint32_t half_width = specification.width / 2;
            const std::uint32_t half_height = specification.height / 2;

            const auto upscaled = harness.Upscale(
                halved,
                half_width,
                half_height,
                specification.width,
                specification.height,
                sharpness);
            const auto bilinear = BilinearUpscale(
                halved,
                half_width,
                half_height,
                specification.width,
                specification.height);

            results.push_back(Result{
                seconds,
                osss::CompareTestPatternFrames(
                    truth,
                    upscaled,
                    specification.width,
                    specification.height),
                osss::CompareTestPatternFrames(
                    truth,
                    bilinear,
                    specification.width,
                    specification.height)});
        }

        double upscaled_total = 0.0;
        double bilinear_total = 0.0;
        double worst_gain = 1e9;
        for (const Result& result : results) {
            upscaled_total += result.upscaled.psnr_db;
            bilinear_total += result.bilinear.psnr_db;
            worst_gain = std::min(worst_gain, result.upscaled.psnr_db - result.bilinear.psnr_db);
        }
        const double count = static_cast<double>(results.size());
        const double upscaled_mean = upscaled_total / count;
        const double bilinear_mean = bilinear_total / count;

        std::cout << std::fixed << std::setprecision(2)
                  << "driver=" << harness.driver
                  << " size=" << specification.width << "x" << specification.height
                  << " samples=" << results.size() << '\n';
        if (report) {
            std::cout << "\nseconds  upscaled  bilinear    gain   bad%   xbad%\n";
            for (const Result& result : results) {
                std::cout << std::setw(7) << result.animation_seconds
                          << std::setw(10) << result.upscaled.psnr_db
                          << std::setw(10) << result.bilinear.psnr_db
                          << std::setw(8) << (result.upscaled.psnr_db - result.bilinear.psnr_db)
                          << std::setw(7) << result.upscaled.pixels_over_threshold_percent
                          << std::setw(8) << result.bilinear.pixels_over_threshold_percent
                          << '\n';
            }
        }
        std::cout << "\nupscaled mean " << upscaled_mean << " dB, bilinear mean "
                  << bilinear_mean << " dB, mean gain " << (upscaled_mean - bilinear_mean)
                  << " dB, worst gain " << worst_gain << " dB\n";

        // Regression floors, not physical constants. They were set a margin past
        // what was measured when this was written; re-measure with --report
        // before moving one, and move it for a reason you can state.
        //
        // The relative gate is the load-bearing one. An absolute PSNR cannot
        // tell an edge-directed kernel from an expensive blur, because on a 2x
        // upscale bilinear already scores respectably -- only the margin over
        // bilinear says the direction estimate is doing work.
        // Measured on an RTX 5090 at 960x540: mean gain +0.54 dB, worst instant
        // -0.57 dB. The floors sit a margin past both.
        //
        // The worst-case floor is negative on purpose, and that is the honest
        // shape of this result rather than a loosened gate. Whole-frame PSNR
        // here includes the thin-detail lane, and a 2x box downsample destroys
        // that lane outright -- below Nyquist there is no direction left to
        // steer along, so an edge-directed kernel cannot beat a blur and a
        // confident one would do worse. What the gate has to catch is the
        // kernel becoming worse than bilinear *everywhere*, which is what a
        // broken direction estimate looks like: the NaN-producing eigenvector
        // this bench caught during development scored -9.58 dB mean.
        constexpr double kMinimumMeanGainDb = 0.35;
        constexpr double kMinimumWorstGainDb = -0.75;
        Require(
            upscaled_mean - bilinear_mean >= kMinimumMeanGainDb,
            "mean gain over bilinear fell below the floor: " +
                std::to_string(upscaled_mean - bilinear_mean) + " dB");
        Require(
            worst_gain >= kMinimumWorstGainDb,
            "worst-case gain over bilinear fell below the floor: " +
                std::to_string(worst_gain) + " dB");

        // A 1:1 pass must be close to a copy. If the kernel or the texel-centre
        // convention drifts, this catches it long before a scaled case would.
        const auto truth = osss::RenderTestPattern(specification, 2.0, 120);
        const auto identity = harness.Upscale(
            truth,
            specification.width,
            specification.height,
            specification.width,
            specification.height,
            0.0F);
        const auto identity_metrics = osss::CompareTestPatternFrames(
            truth,
            identity,
            specification.width,
            specification.height);
        std::cout << "1:1 passthrough " << identity_metrics.psnr_db << " dB\n";
        Require(
            identity_metrics.psnr_db >= 38.0,
            "a 1:1 pass with sharpening off must be near-lossless, got " +
                std::to_string(identity_metrics.psnr_db) + " dB");

        std::cout << "OSSS upscaler quality tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
    }
    return EXIT_FAILURE;
}
