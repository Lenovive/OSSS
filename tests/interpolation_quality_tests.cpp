// Reference-image quality bench for MotionInterpolator.
//
// The test-animation pattern is analytic in continuous time, so the exact
// intermediate frame is known. This bench renders the two bracketing source
// frames, asks MotionInterpolator to reconstruct a set of alphas, and scores
// the result against the analytic truth for that instant. It also scores the
// plain temporal crossfade of the same pair, which is what the interpolator has
// to beat: reporting PSNR alone hides whether motion compensation did anything.
//
// Scoring is per lane, because the three lanes fail in different ways --
// translation, occlusion, and thin high-frequency detail -- and a single
// whole-frame number lets a regression in one hide behind a gain in another.
//
// Run with --report for the full table and --dump <dir> to write PPMs.
//
// --dump-sequence <dir> is the one to reach for when the complaint is
// flickering rather than a bad score. It writes the consecutive runs -- the
// temporal sequence and the reach ramp, the two places in this file where
// frames are ordered in time -- as PNGs with a viewer to step and loop them,
// including a per-frame flicker map. Every other output here is either one
// frame in isolation or a scalar averaged over a whole run, and neither can say
// which frame changed or where.

#include "flow_scale.h"
#include "frame_sequence.h"
#include "motion_interpolator.h"
#include "png_writer.h"
#include "test_harness.h"
#include "test_pattern.h"

#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using osss::test::Require;

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

// The three failure surfaces of the pattern, in source pixels. Bounds match the
// lane rectangles in src/test_pattern.cpp; the insets keep the lane border out
// of the score so a one-pixel outline cannot swamp a 128-row lane.
using Lane = osss::TestPatternLane;

// Shared with the burst scorer in src/test_animation_main.cpp; see
// TestPatternLane in test_pattern.h for why they must not drift.
constexpr auto& kLanes = osss::kTestPatternLanes;

struct Score {
    double psnr_db = 0.0;
    double mean_absolute_error = 0.0;
    double bad_pixel_percent = 0.0;
    int maximum_channel_error = 0;
};

// A single scored position: one alpha of one source pair.
struct Sample {
    int base_fps = 0;
    double animation_seconds = 0.0;
    float alpha = 0.0F;
    const char* lane = "";
    Score interpolated;
    Score crossfade;
};

int ChannelOf(const std::uint32_t pixel, const unsigned shift) {
    return static_cast<int>((pixel >> shift) & 0xFFU);
}

Score ScoreRegion(
    std::span<const std::uint32_t> expected,
    std::span<const std::uint32_t> observed,
    const std::uint32_t width,
    const Lane& lane) {
    double squared_total = 0.0;
    double absolute_total = 0.0;
    std::uint64_t bad = 0;
    std::uint64_t counted = 0;
    int worst = 0;
    for (std::uint32_t y = lane.top; y < lane.bottom; ++y) {
        for (std::uint32_t x = 18; x < width - 18; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            bool over_threshold = false;
            for (const unsigned shift : {0U, 8U, 16U}) {
                const int difference =
                    ChannelOf(observed[index], shift) - ChannelOf(expected[index], shift);
                const int magnitude = std::abs(difference);
                squared_total += static_cast<double>(difference) * difference;
                absolute_total += magnitude;
                worst = std::max(worst, magnitude);
                over_threshold = over_threshold || magnitude > 8;
            }
            bad += over_threshold ? 1U : 0U;
            ++counted;
        }
    }

    const double channels = static_cast<double>(counted) * 3.0;
    Score score;
    score.mean_absolute_error = absolute_total / std::max(channels, 1.0);
    const double mean_square = squared_total / std::max(channels, 1.0);
    score.psnr_db = mean_square <= 0.0
        ? 99.0
        : 10.0 * std::log10(255.0 * 255.0 / mean_square);
    score.bad_pixel_percent =
        100.0 * static_cast<double>(bad) / static_cast<double>(std::max<std::uint64_t>(counted, 1));
    score.maximum_channel_error = worst;
    return score;
}

std::vector<std::uint32_t> CrossfadeFrames(
    std::span<const std::uint32_t> previous,
    std::span<const std::uint32_t> current,
    const float alpha) {
    std::vector<std::uint32_t> blended(previous.size(), 0xFF000000U);
    for (std::size_t index = 0; index < previous.size(); ++index) {
        std::uint32_t pixel = 0xFF000000U;
        for (const unsigned shift : {0U, 8U, 16U}) {
            const double from = ChannelOf(previous[index], shift);
            const double to = ChannelOf(current[index], shift);
            const int value = static_cast<int>(std::lround(from + (to - from) * alpha));
            pixel |= static_cast<std::uint32_t>(std::clamp(value, 0, 255)) << shift;
        }
        blended[index] = pixel;
    }
    return blended;
}

// Deterministic "dead leaves" scene for the reach section: overlapping discs
// with power-law radii and full-range grey, painted in order. It is the
// standard model of natural-image statistics -- a 1/f spectrum made of sharp
// occluding edges at every scale -- and it is what a textured game frame
// gives the estimator that the test pattern does not: high-contrast structure
// the coarse level can still see at its mip. Two smoother drafts were tried
// first and rejected: multi-octave value noise, weighted toward the large
// octaves or flat, was low enough in contrast at the coarse mip that the
// zero-anchored tie-break outweighed the error surface and the search settled
// on zero across textured regions the window covered easily. That is a real
// property of the estimator on low-contrast texture, but it is not what this
// section measures. Grey: colour adds nothing the flow can use.
std::vector<std::uint32_t> RenderDeadLeavesScene(const std::uint32_t width, const std::uint32_t height) {
    // A fixed-seed generator so the scene is identical on every run and every
    // machine; std::mt19937 is specified bit-for-bit, and the distributions
    // below are hand-rolled for the same reason.
    std::mt19937 generator(0x4A4C5353U);
    const auto uniform = [&generator]() {
        return static_cast<double>(generator() >> 8) / static_cast<double>(1U << 24);
    };
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(width) * height,
        0xFF000000U | (0x60U << 16) | (0x60U << 8) | 0x60U);
    constexpr double kMinimumRadius = 3.0;
    constexpr double kMaximumRadius = 72.0;
    // Enough discs to cover the frame several times over at this size, so no
    // background shows through and the smallest discs sit on top.
    const std::size_t disc_count = static_cast<std::size_t>(width) * height / 90;
    for (std::size_t disc = 0; disc < disc_count; ++disc) {
        // Radii from 1/r^3 in area terms -- the dead-leaves law -- via inverse
        // transform sampling of 1/r^2 on the radius.
        const double u = uniform();
        const double radius = 1.0 /
            (1.0 / kMinimumRadius - u * (1.0 / kMinimumRadius - 1.0 / kMaximumRadius));
        const double centre_x = uniform() * width;
        const double centre_y = uniform() * height;
        // Greys span 0.15-0.85, not the full range. Two independent full-range
        // greys differ by a third on average, and once a pan decorrelates the
        // frames that is past the fusion's scene-cut threshold (0.28), which
        // then holds the newest frame and measures nothing about the flow.
        // Game frames sit well under it; a fast pan must read as a pan here
        // too. This range gives an expected difference of 0.23.
        const std::uint32_t grey = static_cast<std::uint32_t>((0.15 + 0.70 * uniform()) * 255.99);
        const std::uint32_t colour = 0xFF000000U | (grey << 16) | (grey << 8) | grey;
        const int left = std::max(0, static_cast<int>(std::floor(centre_x - radius)));
        const int right = std::min(static_cast<int>(width) - 1, static_cast<int>(std::ceil(centre_x + radius)));
        const int top = std::max(0, static_cast<int>(std::floor(centre_y - radius)));
        const int bottom = std::min(static_cast<int>(height) - 1, static_cast<int>(std::ceil(centre_y + radius)));
        for (int y = top; y <= bottom; ++y) {
            const double dy = (y + 0.5) - centre_y;
            for (int x = left; x <= right; ++x) {
                const double dx = (x + 0.5) - centre_x;
                if (dx * dx + dy * dy <= radius * radius) {
                    pixels[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] = colour;
                }
            }
        }
    }
    return pixels;
}

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
        std::string message = "Quality-bench vertex shader compilation failed.";
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

// Renders one interpolated frame and reads it back. Owns every GPU object the
// bench needs so the matrix loop stays readable.
class Bench {
public:
    Bench(
        const std::uint32_t width,
        const std::uint32_t height,
        const osss::FlowScale flow_scale,
        const bool performance_mode,
        const bool temporal_prior)
        : width_(width), height_(height) {
        constexpr D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
        D3D_FEATURE_LEVEL selected_level{};
        // Hardware first: it is the path production uses, and WARP takes tens of
        // seconds per pair at 960x540. WARP remains the fallback so the test
        // still runs on a GPU-less machine.
        HRESULT result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            &feature_level,
            1,
            D3D11_SDK_VERSION,
            device_.put(),
            &selected_level,
            context_.put());
        if (SUCCEEDED(result)) {
            driver_ = "hardware";
        } else {
            winrt::check_hresult(D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                &feature_level,
                1,
                D3D11_SDK_VERSION,
                device_.put(),
                &selected_level,
                context_.put()));
            driver_ = "warp";
        }

        interpolator_ = std::make_unique<osss::MotionInterpolator>(device_.get(), context_.get());
        interpolator_->SetFlowScale(flow_scale);
        interpolator_->SetPerformanceMode(performance_mode);
        interpolator_->SetTemporalPriorEnabled(temporal_prior);
        vertex_shader_ = CreateVertexShader(device_.get());

        D3D11_TEXTURE2D_DESC output_description{};
        output_description.Width = width_;
        output_description.Height = height_;
        output_description.MipLevels = 1;
        output_description.ArraySize = 1;
        output_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        output_description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
        output_description.Usage = D3D11_USAGE_DEFAULT;
        output_description.BindFlags = D3D11_BIND_RENDER_TARGET;
        winrt::check_hresult(device_->CreateTexture2D(
            &output_description,
            nullptr,
            output_texture_.put()));
        winrt::check_hresult(device_->CreateRenderTargetView(
            output_texture_.get(),
            nullptr,
            render_target_.put()));

        D3D11_TEXTURE2D_DESC staging_description = output_description;
        staging_description.Usage = D3D11_USAGE_STAGING;
        staging_description.BindFlags = 0;
        staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::check_hresult(device_->CreateTexture2D(
            &staging_description,
            nullptr,
            staging_texture_.put()));

        D3D11_QUERY_DESC fence_description{};
        fence_description.Query = D3D11_QUERY_EVENT;
        winrt::check_hresult(device_->CreateQuery(&fence_description, fence_.put()));
    }

    [[nodiscard]] const std::string& Driver() const noexcept {
        return driver_;
    }

    // `continues_previous_pair` is what the renderer would say: `previous` is
    // the frame the last pair called `current`. The matrix passes false, so
    // its numbers describe a single pair estimated from scratch; the
    // sequences pass true, and they are where the temporal prior is measured.
    void PreparePair(
        std::span<const std::uint32_t> previous,
        std::span<const std::uint32_t> current,
        const bool continues_previous_pair = false) {
        previous_texture_ = CreateSourceTexture(previous);
        current_texture_ = CreateSourceTexture(current);
        previous_view_ = CreateView(previous_texture_.get());
        current_view_ = CreateView(current_texture_.get());

        // Wall-clock around submission plus the flush below. Motion estimation
        // runs once per source pair, so this is the cost that has to fit inside
        // one source frame period, not inside an output frame period. It is a
        // rough figure on a busy desktop -- use it to catch a change that costs
        // an order of magnitude, not to certify a budget.
        const auto started = std::chrono::steady_clock::now();
        Require(
            interpolator_->PreparePair(
                previous_view_.get(),
                current_view_.get(),
                width_,
                height_,
                continues_previous_pair),
            "Motion preparation failed: " + interpolator_->LastError());
        context_->Flush();
        WaitForGpu();
        prepare_milliseconds_ +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        ++prepared_pairs_;
    }

    // Takes effect at the next PreparePair; see MotionInterpolator.
    void SetTemporalPrior(const bool enabled) {
        interpolator_->SetTemporalPriorEnabled(enabled);
    }

    // Renders one of the interpolator's diagnostic views of the prepared pair
    // instead of the fused frame; see src/debug_view.h. Restores the normal
    // output afterwards.
    //
    // Deliberately not counted in the fusion timing. A dump renders three extra
    // views per frame, and letting those into the average would make the
    // `fuse=` figure in the header depend on whether a dump flag was passed --
    // a timing that changes with an unrelated flag is worse than no timing.
    [[nodiscard]] std::vector<std::uint32_t> InterpolateDebugView(
        const osss::DebugView view,
        const float alpha) {
        interpolator_->SetDebugView(view);
        timing_enabled_ = false;
        auto pixels = Interpolate(alpha);
        timing_enabled_ = true;
        interpolator_->SetDebugView(osss::DebugView::off);
        return pixels;
    }

    [[nodiscard]] double AveragePrepareMilliseconds() const noexcept {
        return prepare_milliseconds_ / static_cast<double>(std::max<std::size_t>(prepared_pairs_, 1));
    }

    [[nodiscard]] double AverageFuseMilliseconds() const noexcept {
        return fuse_milliseconds_ / static_cast<double>(std::max<std::size_t>(fused_frames_, 1));
    }

    [[nodiscard]] std::vector<std::uint32_t> Interpolate(const float alpha) {
        ID3D11RenderTargetView* target = render_target_.get();
        context_->OMSetRenderTargets(1, &target, nullptr);
        const D3D11_VIEWPORT viewport{
            0.0F,
            0.0F,
            static_cast<float>(width_),
            static_cast<float>(height_),
            0.0F,
            1.0F};
        context_->RSSetViewports(1, &viewport);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex_shader_.get(), nullptr, 0);
        // Timed separately from PreparePair and before the readback: fusion
        // runs once per *output* frame, so its budget is an output frame period
        // while flow's is a source frame period. A change that moves work from
        // one to the other is not free even when the total looks unchanged.
        const auto started = std::chrono::steady_clock::now();
        Require(
            interpolator_->BindForDraw(previous_view_.get(), current_view_.get(), alpha),
            "Motion draw binding failed: " + interpolator_->LastError());
        context_->Draw(3, 0);
        interpolator_->UnbindAfterDraw();
        WaitForGpu();
        if (timing_enabled_) {
            fuse_milliseconds_ += std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - started)
                                      .count();
            ++fused_frames_;
        }

        context_->CopyResource(staging_texture_.get(), output_texture_.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context_->Map(staging_texture_.get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width_) * height_, 0xFF000000U);
        for (std::uint32_t y = 0; y < height_; ++y) {
            const auto* row =
                static_cast<const std::uint8_t*>(mapped.pData) +
                static_cast<std::size_t>(y) * mapped.RowPitch;
            for (std::uint32_t x = 0; x < width_; ++x) {
                const std::uint8_t* texel = row + static_cast<std::size_t>(x) * 4;
                pixels[static_cast<std::size_t>(y) * width_ + x] =
                    0xFF000000U |
                    (static_cast<std::uint32_t>(texel[2]) << 16U) |
                    (static_cast<std::uint32_t>(texel[1]) << 8U) |
                    static_cast<std::uint32_t>(texel[0]);
            }
        }
        context_->Unmap(staging_texture_.get(), 0);
        return pixels;
    }

private:
    // An event query is the only reliable fence here. Mapping a staging texture
    // does not work: with no copy pending into it the map returns immediately
    // and the timing then measures command submission rather than execution.
    void WaitForGpu() {
        context_->End(fence_.get());
        BOOL finished = FALSE;
        while (context_->GetData(fence_.get(), &finished, sizeof(finished), 0) != S_OK) {
        }
    }

    [[nodiscard]] winrt::com_ptr<ID3D11Texture2D> CreateSourceTexture(
        std::span<const std::uint32_t> pixels) const {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width_;
        description.Height = height_;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initial_data{};
        initial_data.pSysMem = pixels.data();
        initial_data.SysMemPitch = width_ * sizeof(std::uint32_t);

        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(device_->CreateTexture2D(&description, &initial_data, texture.put()));
        return texture;
    }

    [[nodiscard]] winrt::com_ptr<ID3D11ShaderResourceView> CreateView(
        ID3D11Texture2D* const texture) const {
        winrt::com_ptr<ID3D11ShaderResourceView> view;
        winrt::check_hresult(device_->CreateShaderResourceView(texture, nullptr, view.put()));
        return view;
    }

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::string driver_;
    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    std::unique_ptr<osss::MotionInterpolator> interpolator_;
    winrt::com_ptr<ID3D11VertexShader> vertex_shader_;
    winrt::com_ptr<ID3D11Texture2D> output_texture_;
    winrt::com_ptr<ID3D11RenderTargetView> render_target_;
    winrt::com_ptr<ID3D11Texture2D> staging_texture_;
    winrt::com_ptr<ID3D11Texture2D> previous_texture_;
    winrt::com_ptr<ID3D11Texture2D> current_texture_;
    winrt::com_ptr<ID3D11ShaderResourceView> previous_view_;
    winrt::com_ptr<ID3D11ShaderResourceView> current_view_;
    winrt::com_ptr<ID3D11Query> fence_;
    double prepare_milliseconds_ = 0.0;
    std::size_t prepared_pairs_ = 0;
    double fuse_milliseconds_ = 0.0;
    std::size_t fused_frames_ = 0;
    bool timing_enabled_ = true;
};

// Mean absolute frame-to-frame change of the per-pixel error signal, in 8-bit
// luma, over a sequence of generated frames.
//
// Every score above is of one frame in isolation, which cannot see the artifact
// people actually complain about in frame generation: output that is
// individually plausible but unstable between frames. A steady half-percent
// softness is nearly invisible; the same average error that appears and
// disappears every frame reads as shimmer or a pulse. Differencing the *error*
// rather than the frames removes the scene's own motion, which is supposed to
// change, and leaves only how much the interpolator changed its mind.
double TemporalRoughness(
    const std::vector<std::vector<double>>& error_sequence,
    std::size_t pixel_count) {
    if (error_sequence.size() < 2 || pixel_count == 0) {
        return 0.0;
    }
    double total = 0.0;
    for (std::size_t frame = 1; frame < error_sequence.size(); ++frame) {
        const std::vector<double>& before = error_sequence[frame - 1];
        const std::vector<double>& after = error_sequence[frame];
        double frame_total = 0.0;
        for (std::size_t index = 0; index < pixel_count; ++index) {
            frame_total += std::abs(after[index] - before[index]);
        }
        total += frame_total / static_cast<double>(pixel_count);
    }
    return total / static_cast<double>(error_sequence.size() - 1);
}

double LumaOf(const std::uint32_t pixel) {
    return 0.2126 * ChannelOf(pixel, 16) + 0.7152 * ChannelOf(pixel, 8) +
        0.0722 * ChannelOf(pixel, 0);
}

// One frame's worth of what TemporalRoughness averages: how much the error
// signal moved between this frame and the one before it.
//
// The run-wide mean is what the gates read, but it cannot answer the question a
// flicker complaint actually asks. A single frame that jumps hard and settles
// is visible and barely moves a 24-frame mean; a region a few hundred pixels
// across is invisible in a lane mean and is the most obvious thing on screen.
// `worst_index` is the offset into the lane's error vector, which the caller
// turns back into a source coordinate.
struct FrameStep {
    double mean = 0.0;
    double worst = 0.0;
    std::size_t worst_index = 0;
};

FrameStep StepBetween(
    const std::span<const double> before,
    const std::span<const double> after) {
    FrameStep step;
    const std::size_t count = std::min(before.size(), after.size());
    if (count == 0) {
        return step;
    }
    double total = 0.0;
    for (std::size_t index = 0; index < count; ++index) {
        const double change = std::abs(after[index] - before[index]);
        total += change;
        if (change > step.worst) {
            step.worst = change;
            step.worst_index = index;
        }
    }
    step.mean = total / static_cast<double>(count);
    return step;
}

// One row of the per-frame temporal table, and the record the sequence viewer
// shows beside each frame. Both read the same numbers so a value in the viewer
// and a value in --report cannot disagree.
struct TemporalFrameStep {
    int frame = 0;
    int pair = 0;
    double seconds = 0.0;
    float alpha = 0.0F;
    const char* lane = "";
    double error_step = 0.0;
    double crossfade_error_step = 0.0;
    double worst_error_step = 0.0;
    std::uint32_t worst_x = 0;
    std::uint32_t worst_y = 0;
};

// The insets match ScoreRegion: the lane border is excluded so a one-pixel
// outline cannot dominate. Kept as one constant because the error vectors are
// indexed by it in three places and an off-by-one would silently mislocate
// every reported coordinate.
constexpr std::uint32_t kLaneInset = 18;

struct LaneSummary {
    double worst_psnr_db = 1e9;
    double mean_psnr_db = 0.0;
    double worst_crossfade_psnr_db = 1e9;
    double mean_crossfade_psnr_db = 0.0;
    double mean_bad_pixel_percent = 0.0;
    double worst_bad_pixel_percent = 0.0;
    double mean_crossfade_bad_pixel_percent = 0.0;
    int maximum_channel_error = 0;
    std::size_t count = 0;
};

} // namespace

int main(const int argument_count, char** const arguments) {
    try {
        bool report = false;
        osss::FlowScale flow_scale = osss::FlowScale::automatic;
        bool performance_mode = false;
        bool temporal_prior = true;
        std::filesystem::path dump_directory;
        // Separate from --dump on purpose. --dump writes one image per scored
        // position across unrelated start times; --dump-sequence writes only
        // the runs that are ordered in time, which are the only ones where
        // stepping from one frame to the next means anything.
        std::filesystem::path sequence_directory;
        std::uint32_t embed_divisor = 0;
        for (int index = 1; index < argument_count; ++index) {
            const std::string_view argument = arguments[index];
            if (argument == "--report") {
                report = true;
            } else if (argument == "--performance-mode") {
                performance_mode = true;
            } else if (argument == "--temporal-prior" && index + 1 < argument_count) {
                // The A/B for the reach section: off reproduces the estimator
                // before the prior existed, so its gate is only applied when on.
                const std::string_view value = arguments[++index];
                if (value == "on") {
                    temporal_prior = true;
                } else if (value == "off") {
                    temporal_prior = false;
                } else {
                    throw std::runtime_error("--temporal-prior expects on or off.");
                }
            } else if (argument == "--flow-scale" && index + 1 < argument_count) {
                const std::string value(arguments[++index]);
                const std::wstring wide(value.begin(), value.end());
                const auto parsed = osss::ParseFlowScale(wide);
                if (!parsed) {
                    throw std::runtime_error(
                        "--flow-scale expects auto, quality, performance, or ultra-performance.");
                }
                flow_scale = *parsed;
            } else if (argument == "--dump" && index + 1 < argument_count) {
                dump_directory = arguments[++index];
                std::filesystem::create_directories(dump_directory);
            } else if (argument == "--dump-sequence" && index + 1 < argument_count) {
                sequence_directory = arguments[++index];
            } else if (argument == "--dump-embed" && index + 1 < argument_count) {
                embed_divisor = static_cast<std::uint32_t>(
                    std::stoul(std::string(arguments[++index])));
                if (embed_divisor == 0 || embed_divisor > 8) {
                    throw std::runtime_error("--dump-embed expects a divisor from 1 through 8.");
                }
            }
        }

        osss::TestPatternSpec specification;
        specification.api = osss::TestGraphicsApi::direct3d11;
        for (int index = 1; index < argument_count; ++index) {
            // --size lets the same matrix be re-measured at production
            // resolutions. The pattern draws its lanes at fixed pixel rows, so
            // a larger frame widens them and adds margin without moving any of
            // the scored bands.
            if (std::string_view(arguments[index]) == "--size" && index + 1 < argument_count) {
                const std::string_view value = arguments[++index];
                const std::size_t separator = value.find('x');
                if (separator == std::string_view::npos) {
                    throw std::runtime_error("--size expects WIDTHxHEIGHT, for example 1920x1080.");
                }
                specification.width = static_cast<std::uint32_t>(
                    std::stoul(std::string(value.substr(0, separator))));
                specification.height = static_cast<std::uint32_t>(
                    std::stoul(std::string(value.substr(separator + 1))));
            }
        }

        std::unique_ptr<osss::FrameSequenceWriter> sequence_writer;
        if (!sequence_directory.empty()) {
            sequence_writer = std::make_unique<osss::FrameSequenceWriter>(
                sequence_directory,
                embed_divisor);
        }

        // --dump writes a PPM and a PNG of the same pixels. The PPM stays
        // because the burst scorer and --compare in the test-animation harness
        // read that format; the PNG is added because nothing on a desktop
        // opens a PPM, which is why these dumps have historically been
        // generated and then not looked at.
        const auto dump_image = [&](
            const std::string& stem,
            const std::span<const std::uint32_t> pixels) {
            if (dump_directory.empty()) {
                return;
            }
            std::string error;
            osss::WriteTestPatternPpm(
                dump_directory / (stem + ".ppm"),
                pixels,
                specification.width,
                specification.height,
                error);
            osss::WritePng(
                dump_directory / (stem + ".png"),
                pixels,
                specification.width,
                specification.height,
                error);
        };

        Bench bench(
            specification.width,
            specification.height,
            flow_scale,
            performance_mode,
            temporal_prior);

        // Source rates chosen for motion magnitude: 60 FPS is a 1.5-3.8 pixel
        // step where sub-pixel placement dominates, 30 and 20 push the fastest
        // lane to 7.6 and 11.4 pixels where the search range and flow-edge
        // handling dominate. Every start time keeps the pair inside one half of
        // the cycle so the deliberate scene cut at 3.0 s is not under test here.
        const int rates[] = {60, 30, 20};
        const double start_times[] = {0.35, 1.10, 1.95, 2.55, 3.40, 4.25, 5.10, 5.70};
        const float alphas[] = {1.0F / 6.0F, 0.5F, 5.0F / 6.0F};

        std::vector<Sample> samples;
        for (const int base_fps : rates) {
            specification.base_fps = base_fps;
            const double step = 1.0 / static_cast<double>(base_fps);
            for (const double start : start_times) {
                const auto previous = osss::RenderTestPattern(specification, start, 100);
                const auto current = osss::RenderTestPattern(specification, start + step, 101);
                bench.PreparePair(previous, current);

                for (const float alpha : alphas) {
                    const double truth_seconds = start + step * static_cast<double>(alpha);
                    const auto expected =
                        osss::RenderTestPattern(specification, truth_seconds, 100);
                    const auto observed = bench.Interpolate(alpha);
                    const auto blended = CrossfadeFrames(previous, current, alpha);

                    if (!dump_directory.empty()) {
                        const std::string stem =
                            std::to_string(base_fps) + "fps-" +
                            std::to_string(static_cast<int>(start * 100.0)) + "cs-a" +
                            std::to_string(static_cast<int>(alpha * 100.0F));
                        dump_image(stem + "-observed", observed);
                        dump_image(stem + "-expected", expected);
                    }

                    for (const Lane& lane : kLanes) {
                        Sample sample;
                        sample.base_fps = base_fps;
                        sample.animation_seconds = start;
                        sample.alpha = alpha;
                        sample.lane = lane.name;
                        sample.interpolated =
                            ScoreRegion(expected, observed, specification.width, lane);
                        sample.crossfade =
                            ScoreRegion(expected, blended, specification.width, lane);
                        samples.push_back(sample);
                    }
                }
            }
        }

        // ---- temporal consistency -------------------------------------------
        // A continuous run of generated frames across *consecutive* source
        // pairs, at uniform output spacing. Crossing pair boundaries is the
        // point: the flow field is recomputed there, and a discontinuity in the
        // reconstruction at every source frame is a pulse at source cadence.
        struct TemporalResult {
            const char* lane;
            double interpolated;
            double crossfade;
            // Mean luma PSNR over the same frames. Not gated; printed so that a
            // change to how consecutive pairs are estimated -- the temporal
            // prior is the first -- shows its effect at 60 FPS, where the
            // matrix above, which estimates every pair from scratch, cannot.
            double interpolated_psnr_db;
            double crossfade_psnr_db;
        };
        std::vector<TemporalResult> temporal;
        std::vector<TemporalFrameStep> temporal_frames;
        {
            specification.base_fps = 60;
            const double step = 1.0 / 60.0;
            const double sequence_start = 1.10;
            constexpr int kPairs = 6;
            // Four evenly spaced positions per pair, none of them an endpoint,
            // so consecutive frames are always the same distance apart in time
            // and a jump at the boundary is the interpolator's, not the clock's.
            const float sequence_alphas[] = {0.125F, 0.375F, 0.625F, 0.875F};

            std::map<std::string, std::vector<std::vector<double>>> lane_errors;
            std::map<std::string, std::vector<std::vector<double>>> lane_crossfade_errors;
            // Rolls forward one frame so the flicker map can difference the
            // error signal without holding the whole run in memory.
            std::vector<double> previous_error_signal;
            int sequence_index = 0;

            for (int pair = 0; pair < kPairs; ++pair) {
                const double pair_start = sequence_start + step * pair;
                const auto previous = osss::RenderTestPattern(specification, pair_start, 100);
                const auto current =
                    osss::RenderTestPattern(specification, pair_start + step, 101);
                // Consecutive by construction: each pair's `previous` is the
                // last pair's `current`, as the renderer would report.
                bench.PreparePair(previous, current, pair > 0);

                for (const float alpha : sequence_alphas) {
                    const auto expected = osss::RenderTestPattern(
                        specification,
                        pair_start + step * static_cast<double>(alpha),
                        100);
                    const auto observed = bench.Interpolate(alpha);
                    const auto blended = CrossfadeFrames(previous, current, alpha);

                    for (const Lane& lane : kLanes) {
                        std::vector<double> errors;
                        std::vector<double> crossfade_errors;
                        errors.reserve(
                            static_cast<std::size_t>(lane.bottom - lane.top) * specification.width);
                        for (std::uint32_t y = lane.top; y < lane.bottom; ++y) {
                            for (std::uint32_t x = kLaneInset;
                                 x < specification.width - kLaneInset;
                                 ++x) {
                                const std::size_t index =
                                    static_cast<std::size_t>(y) * specification.width + x;
                                const double truth = LumaOf(expected[index]);
                                errors.push_back(LumaOf(observed[index]) - truth);
                                crossfade_errors.push_back(LumaOf(blended[index]) - truth);
                            }
                        }
                        lane_errors[lane.name].push_back(std::move(errors));
                        lane_crossfade_errors[lane.name].push_back(std::move(crossfade_errors));
                    }

                    // Per-frame steps, computed once and read by both the
                    // --report table and the sequence viewer. The first frame
                    // of the run has no predecessor and reports zeros.
                    const double frame_seconds =
                        pair_start + step * static_cast<double>(alpha);
                    const std::uint32_t lane_width = specification.width - 2 * kLaneInset;
                    std::vector<osss::SequenceMetric> metrics;
                    for (const Lane& lane : kLanes) {
                        const auto& series = lane_errors[lane.name];
                        const auto& crossfade_series = lane_crossfade_errors[lane.name];
                        TemporalFrameStep row;
                        row.frame = sequence_index;
                        row.pair = pair;
                        row.seconds = frame_seconds;
                        row.alpha = alpha;
                        row.lane = lane.name;
                        if (series.size() >= 2) {
                            const FrameStep measured =
                                StepBetween(series[series.size() - 2], series.back());
                            row.error_step = measured.mean;
                            row.worst_error_step = measured.worst;
                            row.worst_x = kLaneInset +
                                static_cast<std::uint32_t>(measured.worst_index % lane_width);
                            row.worst_y = lane.top +
                                static_cast<std::uint32_t>(measured.worst_index / lane_width);
                            row.crossfade_error_step =
                                StepBetween(
                                    crossfade_series[crossfade_series.size() - 2],
                                    crossfade_series.back())
                                    .mean;
                        }
                        temporal_frames.push_back(row);

                        osss::SequenceMetric metric;
                        metric.name = lane.name;
                        metric.error_step = row.error_step;
                        metric.crossfade_error_step = row.crossfade_error_step;
                        metric.worst_error_step = row.worst_error_step;
                        metric.worst_x = row.worst_x;
                        metric.worst_y = row.worst_y;
                        metrics.push_back(std::move(metric));
                    }

                    if (sequence_writer) {
                        const auto error_signal = osss::LumaError(observed, expected);
                        osss::SequenceFrame record;
                        record.index = sequence_index;
                        record.pair = pair;
                        record.seconds = frame_seconds;
                        record.alpha = alpha;
                        record.metrics = metrics;

                        const auto error_view = osss::RenderErrorView(error_signal);
                        const auto step_view =
                            osss::RenderErrorStepView(error_signal, previous_error_signal);
                        const auto flow =
                            bench.InterpolateDebugView(osss::DebugView::flow, alpha);
                        const auto confidence =
                            bench.InterpolateDebugView(osss::DebugView::confidence, alpha);
                        const auto fallback =
                            bench.InterpolateDebugView(osss::DebugView::fallback, alpha);
                        std::string dump_error;
                        Require(
                            sequence_writer->AddFrame(
                                "temporal",
                                record,
                                {{"observed", observed},
                                 {"expected", expected},
                                 {"crossfade", blended},
                                 {"error", error_view},
                                 {"error-step", step_view},
                                 {"flow", flow},
                                 {"confidence", confidence},
                                 {"fallback", fallback}},
                                specification.width,
                                specification.height,
                                dump_error),
                            "Writing the temporal sequence failed: " + dump_error);
                        previous_error_signal = error_signal;
                    }
                    ++sequence_index;
                }
            }

            const auto mean_psnr = [](const std::vector<std::vector<double>>& frames) {
                double squared = 0.0;
                std::size_t count = 0;
                for (const auto& frame : frames) {
                    for (const double error : frame) {
                        squared += error * error;
                    }
                    count += frame.size();
                }
                const double mean_square = squared / static_cast<double>(std::max<std::size_t>(count, 1));
                return mean_square <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mean_square);
            };
            for (const Lane& lane : kLanes) {
                const auto& sequence = lane_errors[lane.name];
                const auto& crossfade_sequence = lane_crossfade_errors[lane.name];
                const std::size_t pixels = sequence.empty() ? 0 : sequence.front().size();
                temporal.push_back(TemporalResult{
                    lane.name,
                    TemporalRoughness(sequence, pixels),
                    TemporalRoughness(crossfade_sequence, pixels),
                    mean_psnr(sequence),
                    mean_psnr(crossfade_sequence)});
            }
        }

        // ---- reach: a pan accelerating out past the search window -----------
        // The coarse window is a displacement ceiling per pair (64 source
        // pixels here: radius 8, coarse step 8, plus a few more from the
        // halving pass and the fine window; half that in performance mode),
        // and the matrix above stays inside it. This sequence does not. A
        // frame-sized crop slides across a wider static scene -- a camera pan,
        // which is what produces whole-frame motion in games -- by 8, 16, 24,
        // ... 80 pixels per pair, one coarse cell more each time. Estimated
        // from scratch, motion past the ceiling is not found and the pair
        // falls back to a crossfade; seeded from the previous pair's field it
        // can be followed out one cell of acceleration at a time, which is
        // exactly what the temporal prior claims, so the ramp is run with the
        // prior on and off and the two are compared. Every pan is even, so the
        // alpha-0.5 truth is an integer crop and exact.
        //
        // The scene is dead leaves rather than the test pattern. The seed only
        // helps where the previous pair's field is confident *at the same
        // position*, which for a pan is everywhere the content has aperiodic
        // structure at the coarse level's scale -- and the pattern has none:
        // its panels are flat, its markers are thinner than a coarse cell,
        // and its checkerboard, grating and grid alias onto themselves at
        // these displacements. Overlapping discs at every scale are what a
        // textured game frame looks like to the estimator, and their
        // crossfade collapses so completely that a found match and a missed
        // one are many decibels apart.
        //
        // The ramp runs on two interpolators. The bench's own, in whatever
        // mode it was started, is reported and gated only for safety: the
        // prior must never score below the estimate from scratch. The
        // benefit is gated on a second interpolator in performance mode,
        // because that is where the claim is measurable today: the window is
        // narrower, so the ramp leaves it sooner, and there is no second
        // fine-level search from zero. In quality mode that search compares
        // the seeded match against a near-zero one on a cost that penalises
        // displacement at four times the coarse level's rate -- 0.13 at 44
        // pixels, more than half this scene's typical edge contrast -- and
        // it overrides most of what the seed found. Measured with that
        // penalty rescaled to the coarse cell, every gate in this file still
        // held and the seeded pair at 40 pixels gained 1.6 dB over the
        // unseeded one; that is a separate change, recorded here so the
        // quality-mode columns below are read for what they are.
        struct ReachResult {
            int pan_pixels;
            Score seeded;
            Score unseeded;
            Score crossfade;
        };
        constexpr int kRampPairs = 10;
        constexpr int kFirstPan = 8;
        constexpr int kPanIncrement = 8;
        std::vector<ReachResult> reach;
        std::vector<ReachResult> reach_performance;
        {
            int total_pan = 0;
            for (int pair = 0; pair < kRampPairs; ++pair) {
                total_pan += kFirstPan + kPanIncrement * pair;
            }
            const std::uint32_t scene_width = specification.width + static_cast<std::uint32_t>(total_pan);
            const auto scene = RenderDeadLeavesScene(scene_width, specification.height);
            const auto crop = [&](const int left) {
                std::vector<std::uint32_t> pixels(
                    static_cast<std::size_t>(specification.width) * specification.height);
                for (std::uint32_t y = 0; y < specification.height; ++y) {
                    std::copy_n(
                        scene.begin() + static_cast<std::ptrdiff_t>(y) * scene_width + left,
                        specification.width,
                        pixels.begin() + static_cast<std::ptrdiff_t>(y) * specification.width);
                }
                return pixels;
            };
            const Lane whole_frame{"frame", 0, specification.height};

            // One pass of the ramp; returns each pair's score and the
            // crossfade of the same pair. `dump_stem` writes the diagnostic
            // views for every pair when --dump is given: the flow and
            // confidence views are the ones that say why a reach pair scored
            // as it did, since the fused frame alone cannot tell a missed
            // match from a mis-fused one.
            const auto run_ramp = [&](Bench& ramp_bench, const bool prior_on, const char* dump_stem) {
                ramp_bench.SetTemporalPrior(prior_on);
                std::vector<std::pair<Score, Score>> scores;
                std::vector<double> previous_error_signal;
                int left = 0;
                for (int pair = 0; pair < kRampPairs; ++pair) {
                    const int pan = kFirstPan + kPanIncrement * pair;
                    const auto previous = crop(left);
                    const auto current = crop(left + pan);
                    ramp_bench.PreparePair(previous, current, pair > 0);

                    const float alpha = 0.5F;
                    const auto expected = crop(left + pan / 2);
                    const auto observed = ramp_bench.Interpolate(alpha);
                    const auto blended = CrossfadeFrames(previous, current, alpha);
                    if (!dump_directory.empty() && dump_stem) {
                        const std::string stem =
                            std::string(dump_stem) + "-" + std::to_string(pan) + "px";
                        dump_image(stem + "-observed", observed);
                        dump_image(stem + "-expected", expected);
                        dump_image(
                            stem + "-flow",
                            ramp_bench.InterpolateDebugView(osss::DebugView::flow, alpha));
                        dump_image(
                            stem + "-confidence",
                            ramp_bench.InterpolateDebugView(osss::DebugView::confidence, alpha));
                    }
                    if (sequence_writer && dump_stem) {
                        const auto error_signal = osss::LumaError(observed, expected);
                        const auto crossfade_error_signal = osss::LumaError(blended, expected);
                        osss::SequenceFrame record;
                        record.index = pair;
                        record.pair = pair;
                        // The ramp is a run of consecutive source pairs, so
                        // frame k sits one source period after frame k-1 at a
                        // 60 FPS source. Alpha is 0.5 throughout.
                        record.seconds = (static_cast<double>(pair) + 0.5) / 60.0;
                        record.alpha = alpha;
                        record.label = std::to_string(pan) + " px pan";

                        osss::SequenceMetric metric;
                        metric.name = "frame";
                        if (!previous_error_signal.empty()) {
                            const FrameStep measured =
                                StepBetween(previous_error_signal, error_signal);
                            metric.error_step = measured.mean;
                            metric.worst_error_step = measured.worst;
                            metric.worst_x = static_cast<std::uint32_t>(
                                measured.worst_index % specification.width);
                            metric.worst_y = static_cast<std::uint32_t>(
                                measured.worst_index / specification.width);
                        }
                        // Whole-frame mean absolute error of the crossfade, for
                        // scale rather than as a step: the pan distance changes
                        // every pair here, so a step column would mix the
                        // interpolator changing its mind with the scene moving
                        // further. Read the reach sequence for where the flow
                        // gives up, not for a flicker rate.
                        double crossfade_total = 0.0;
                        for (const double value : crossfade_error_signal) {
                            crossfade_total += std::abs(value);
                        }
                        metric.crossfade_error_step = crossfade_total /
                            static_cast<double>(std::max<std::size_t>(
                                crossfade_error_signal.size(), 1));
                        record.metrics.push_back(std::move(metric));

                        const auto error_view = osss::RenderErrorView(error_signal);
                        const auto step_view =
                            osss::RenderErrorStepView(error_signal, previous_error_signal);
                        const auto flow =
                            ramp_bench.InterpolateDebugView(osss::DebugView::flow, alpha);
                        const auto confidence =
                            ramp_bench.InterpolateDebugView(osss::DebugView::confidence, alpha);
                        const auto fallback =
                            ramp_bench.InterpolateDebugView(osss::DebugView::fallback, alpha);
                        std::string dump_error;
                        Require(
                            sequence_writer->AddFrame(
                                dump_stem,
                                record,
                                {{"observed", observed},
                                 {"expected", expected},
                                 {"crossfade", blended},
                                 {"error", error_view},
                                 {"error-step", step_view},
                                 {"flow", flow},
                                 {"confidence", confidence},
                                 {"fallback", fallback}},
                                specification.width,
                                specification.height,
                                dump_error),
                            "Writing the reach sequence failed: " + dump_error);
                        previous_error_signal = error_signal;
                    }
                    scores.emplace_back(
                        ScoreRegion(expected, observed, specification.width, whole_frame),
                        ScoreRegion(expected, blended, specification.width, whole_frame));
                    left += pan;
                }
                return scores;
            };
            const auto combine = [&](
                const std::vector<std::pair<Score, Score>>& seeded,
                const std::vector<std::pair<Score, Score>>& unseeded) {
                std::vector<ReachResult> results;
                for (int pair = 0; pair < kRampPairs; ++pair) {
                    results.push_back(ReachResult{
                        kFirstPan + kPanIncrement * pair,
                        seeded[pair].first,
                        unseeded[pair].first,
                        seeded[pair].second});
                }
                return results;
            };

            const auto seeded = run_ramp(bench, true, "reach-seeded");
            const auto unseeded = run_ramp(bench, false, "reach-unseeded");
            bench.SetTemporalPrior(temporal_prior);
            reach = combine(seeded, unseeded);

            Bench performance_bench(
                specification.width,
                specification.height,
                flow_scale,
                /*performance_mode=*/true,
                /*temporal_prior=*/true);
            reach_performance = combine(
                run_ramp(performance_bench, true, nullptr),
                run_ramp(performance_bench, false, nullptr));
        }

        std::map<std::string, LaneSummary> by_lane;
        for (const Sample& sample : samples) {
            LaneSummary& summary = by_lane[sample.lane];
            summary.worst_psnr_db = std::min(summary.worst_psnr_db, sample.interpolated.psnr_db);
            summary.mean_psnr_db += sample.interpolated.psnr_db;
            summary.worst_crossfade_psnr_db =
                std::min(summary.worst_crossfade_psnr_db, sample.crossfade.psnr_db);
            summary.mean_crossfade_psnr_db += sample.crossfade.psnr_db;
            summary.mean_bad_pixel_percent += sample.interpolated.bad_pixel_percent;
            summary.worst_bad_pixel_percent = std::max(
                summary.worst_bad_pixel_percent,
                sample.interpolated.bad_pixel_percent);
            summary.mean_crossfade_bad_pixel_percent += sample.crossfade.bad_pixel_percent;
            summary.maximum_channel_error = std::max(
                summary.maximum_channel_error,
                sample.interpolated.maximum_channel_error);
            ++summary.count;
        }

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "driver=" << bench.Driver() << " size=" << specification.width << 'x'
                  << specification.height << " samples=" << samples.size()
                  << " flow-prepare=" << bench.AveragePrepareMilliseconds() << "ms/pair"
                  << " fuse=" << bench.AverageFuseMilliseconds() << "ms/frame\n";

        if (report) {
            std::cout << "\nfps  start  alpha  lane        psnr  xfade   gain   bad%   max\n";
            for (const Sample& sample : samples) {
                std::cout << std::setw(3) << sample.base_fps << "  " << std::setw(5)
                          << sample.animation_seconds << "  " << std::setw(5) << sample.alpha
                          << "  " << std::left << std::setw(10) << sample.lane << std::right
                          << "  " << std::setw(5) << sample.interpolated.psnr_db << "  "
                          << std::setw(5) << sample.crossfade.psnr_db << "  " << std::setw(5)
                          << (sample.interpolated.psnr_db - sample.crossfade.psnr_db) << "  "
                          << std::setw(5) << sample.interpolated.bad_pixel_percent << "  "
                          << std::setw(3) << sample.interpolated.maximum_channel_error << '\n';
            }
        }

        std::cout << "\nlane        mean   worst  xfade-mean  xfade-worst  mean-bad%  worst-bad%  xfade-bad%  max\n";
        for (auto& [name, summary] : by_lane) {
            const double count = static_cast<double>(std::max<std::size_t>(summary.count, 1));
            summary.mean_psnr_db /= count;
            summary.mean_crossfade_psnr_db /= count;
            summary.mean_bad_pixel_percent /= count;
            summary.mean_crossfade_bad_pixel_percent /= count;
            std::cout << std::left << std::setw(10) << name << std::right << "  " << std::setw(5)
                      << summary.mean_psnr_db << "  " << std::setw(5) << summary.worst_psnr_db
                      << "  " << std::setw(10) << summary.mean_crossfade_psnr_db << "  "
                      << std::setw(11) << summary.worst_crossfade_psnr_db << "  " << std::setw(9)
                      << summary.mean_bad_pixel_percent << "  " << std::setw(10)
                      << summary.worst_bad_pixel_percent << "  " << std::setw(10)
                      << summary.mean_crossfade_bad_pixel_percent << "  " << std::setw(3)
                      << summary.maximum_channel_error << '\n';
        }

        if (report) {
            // The run-wide means below cannot say which frame moved or where.
            // A pulse at pair cadence shows here as one row per pair standing
            // out; a localised artifact shows as a large worst-at against an
            // unremarkable mean, with the coordinate to look at.
            std::cout << "\ntemporal, per frame (step is against the previous frame; frame 0 has none)\n"
                      << "frame  pair   t-ms  alpha  lane        error-step  xfade-step   worst  at\n";
            for (const TemporalFrameStep& row : temporal_frames) {
                std::cout << std::setw(5) << row.frame << "  " << std::setw(4) << row.pair << "  "
                          << std::setw(5) << row.seconds * 1000.0 << "  " << std::setw(5)
                          << row.alpha << "  " << std::left << std::setw(10) << row.lane
                          << std::right << "  " << std::setw(10) << row.error_step << "  "
                          << std::setw(10) << row.crossfade_error_step << "  " << std::setw(6)
                          << row.worst_error_step << "  " << row.worst_x << ',' << row.worst_y
                          << '\n';
            }
        }

        // The single worst frame-to-frame step of the run, per lane. Printed
        // unconditionally because it is the one number that localises an
        // instability, and a run-wide mean can hide an order of magnitude in it.
        std::cout << "\ntemporal worst single step\n"
                  << "lane        worst-mean-step  at-frame   worst-pixel  at\n";
        for (const Lane& lane : kLanes) {
            const TemporalFrameStep* peak = nullptr;
            for (const TemporalFrameStep& row : temporal_frames) {
                if (std::string_view(row.lane) != std::string_view(lane.name)) {
                    continue;
                }
                if (peak == nullptr || row.error_step > peak->error_step) {
                    peak = &row;
                }
            }
            if (peak == nullptr) {
                continue;
            }
            std::cout << std::left << std::setw(10) << lane.name << std::right << "  "
                      << std::setw(15) << peak->error_step << "  " << std::setw(8) << peak->frame
                      << "  " << std::setw(12) << peak->worst_error_step << "  " << peak->worst_x
                      << ',' << peak->worst_y << '\n';
        }

        std::cout << "\ntemporal (60 FPS source, 4x output, 6 consecutive pairs, temporal prior "
                  << (temporal_prior ? "on" : "off") << ")\n"
                  << "lane        error-step  xfade-step  ratio   psnr  xfade   gain\n";
        for (const TemporalResult& result : temporal) {
            std::cout << std::left << std::setw(10) << result.lane << std::right << "  "
                      << std::setw(10) << result.interpolated << "  " << std::setw(10)
                      << result.crossfade << "  " << std::setw(5)
                      << (result.crossfade > 0.0 ? result.interpolated / result.crossfade : 0.0)
                      << "  " << std::setw(5) << result.interpolated_psnr_db << "  "
                      << std::setw(5) << result.crossfade_psnr_db << "  " << std::setw(5)
                      << (result.interpolated_psnr_db - result.crossfade_psnr_db) << '\n';
        }

        const auto print_reach = [](const char* title, const std::vector<ReachResult>& results) {
            std::cout << "\nreach (accelerating pan over dead leaves, " << kRampPairs
                      << " consecutive pairs, alpha 0.5, " << title << ")\n"
                      << "pan-px  seeded  unseeded  xfade   prior-gain  seeded-bad%  unseeded-bad%\n";
            for (const ReachResult& result : results) {
                std::cout << std::setw(6) << result.pan_pixels << "  " << std::setw(6)
                          << result.seeded.psnr_db << "  " << std::setw(8)
                          << result.unseeded.psnr_db << "  " << std::setw(5)
                          << result.crossfade.psnr_db << "  " << std::setw(10)
                          << (result.seeded.psnr_db - result.unseeded.psnr_db) << "  "
                          << std::setw(11) << result.seeded.bad_pixel_percent << "  "
                          << std::setw(13) << result.unseeded.bad_pixel_percent << '\n';
            }
        };
        print_reach(performance_mode ? "performance mode" : "quality mode", reach);
        print_reach("performance mode, dedicated interpolator", reach_performance);

        // Written before the gates, not after: a failing run is the one worth
        // looking at, and a Require below would throw past this.
        if (sequence_writer) {
            std::string dump_error;
            Require(
                sequence_writer->WriteViewer(
                    "OSSS interpolation, " + std::to_string(specification.width) + "x" +
                        std::to_string(specification.height) + ", flow-scale " +
                        osss::FlowScaleName(flow_scale) + ", " +
                        (performance_mode ? "performance" : "quality") + " mode, temporal prior " +
                        (temporal_prior ? "on" : "off"),
                    dump_error),
                "Writing the sequence viewer failed: " + dump_error);
            std::cout << "\nsequence dump: " << sequence_writer->FrameCount() << " frames, "
                      << sequence_writer->ImageCount() << " images, "
                      << sequence_writer->BytesWritten() / 1024 << " KiB\n"
                      << "open " << (sequence_writer->Directory() / "viewer.html").string() << '\n';
            if (embed_divisor > 0) {
                std::cout << "single file: "
                          << (sequence_writer->Directory() / "viewer-embedded.html").string()
                          << '\n';
            }
        }

        // The temporal prior is an extra candidate in the coarse search and
        // nothing else, so at worst it is ignored: at no pan may the seeded
        // ramp score below the unseeded one by more than float noise. Both
        // interpolators are held to that.
        constexpr double kMaximumPriorRegressionDb = 0.15;
        for (const auto* ramp : {&reach, &reach_performance}) {
            for (const ReachResult& result : *ramp) {
                Require(
                    result.seeded.psnr_db >= result.unseeded.psnr_db - kMaximumPriorRegressionDb,
                    "Temporal prior regressed the " + std::to_string(result.pan_pixels) +
                        " px pan to " + std::to_string(result.seeded.psnr_db) + " dB against " +
                        std::to_string(result.unseeded.psnr_db) + " dB unseeded.");
            }
        }

        // The benefit, gated where it is measurable (see the note above the
        // ramp). The prior can only pay where the coarse search cannot already
        // reach, so these gates sit at the first pans past the performance-mode
        // window -- and they move when that window moves.
        //
        // They were last moved when reach stopped being counted in flow cells
        // and became a target in source pixels (ResolveCoarseSearchRadius in
        // src/flow_scale.h). That doubled the performance-mode window on the
        // divisor of 4 this bench runs at, from 16 source pixels to 32, so the
        // 24 and 32 px pairs these gates used to probe are now inside the
        // search and the prior correctly contributes nothing there: it gained
        // 0.15 dB at 24 px and 0.08 at 32, against floors of 2.0 and 1.5. The
        // cliff simply moved to 40 px, and so did the gates.
        //
        // Measured when moved, at 960x540 on an RTX 5090: seeded 18.0 dB
        // against 15.2 unseeded at 40 px (29 % bad against 52 %), 15.5 against
        // 14.4 at 48, 14.3 against 13.9 at 56. Floors are set well under that;
        // the pair after the first is included so a seed that works exactly
        // once cannot pass.
        struct ReachGate {
            int pan_pixels;
            double minimum_prior_gain_db;
        };
        const ReachGate reach_gates[] = {{40, 2.0}, {48, 0.8}};
        for (const ReachGate& gate : reach_gates) {
            const auto found = std::find_if(
                reach_performance.begin(),
                reach_performance.end(),
                [&](const ReachResult& result) { return result.pan_pixels == gate.pan_pixels; });
            Require(found != reach_performance.end(), "Reach ramp is missing a gated pan.");
            const double gain = found->seeded.psnr_db - found->unseeded.psnr_db;
            Require(
                gain >= gate.minimum_prior_gain_db,
                "Temporal prior gained only " + std::to_string(gain) + " dB at the " +
                    std::to_string(gate.pan_pixels) + " px pan in performance mode; it should gain at least " +
                    std::to_string(gate.minimum_prior_gain_db) + " dB there.");
        }

        // Frame-to-frame instability, in 8-bit luma. Not a pass/fail quality
        // bar -- a crossfade is smooth and still wrong -- but two ceilings that
        // catch a change making the output visibly restless, which no
        // single-frame PSNR in this file can see.
        //
        // The absolute ceiling is a backstop. The load-bearing one is the ratio
        // to the crossfade of the same frames: measured 0.47 linear, 0.53
        // occlusion, 0.41 detail when written (0.14/0.30, 0.76/1.43, 2.28/5.52
        // luma levels), and the ratio travels between GPUs and resolutions far
        // better than the absolute figures do. An interpolator that shimmers
        // more than a plain blend has lost the argument for its own existence.
        constexpr double kMaximumRoughnessLumaLevels = 6.0;
        constexpr double kMaximumRoughnessRatioToCrossfade = 0.75;
        for (const TemporalResult& result : temporal) {
            Require(
                result.interpolated <= kMaximumRoughnessLumaLevels,
                std::string("Lane ") + result.lane + " output changes by " +
                    std::to_string(result.interpolated) +
                    " luma levels per frame; it should stay under " +
                    std::to_string(kMaximumRoughnessLumaLevels) + ".");
            Require(
                result.interpolated <= result.crossfade * kMaximumRoughnessRatioToCrossfade,
                std::string("Lane ") + result.lane + " output changes by " +
                    std::to_string(result.interpolated) + " luma levels per frame against " +
                    std::to_string(result.crossfade) + " for the crossfade; the ratio " +
                    std::to_string(result.crossfade > 0.0 ? result.interpolated / result.crossfade : 0.0) +
                    " should stay under " + std::to_string(kMaximumRoughnessRatioToCrossfade) + ".");
        }

        // Regression floors measured on this bench, set a couple of dB under the
        // values observed when they were written. They are not a universal
        // quality claim, and they are not portable: another GPU or another
        // resolution will land somewhere else. Re-measure before moving one.
        //
        // The gain column matters more than the absolute PSNR. An interpolator
        // that merely matches the plain crossfade has done no motion
        // compensation at all, and that failure is invisible in PSNR alone --
        // it is exactly how the tie-break bug in EstimateCoarse survived: every
        // lane scored a respectable 29-33 dB while the whole motion path was
        // inert.
        //
        // The detail lane is gated near parity, deliberately. Its grating has a
        // six-pixel period, so at 30 and 20 FPS the source moves more than half
        // a period per frame and the displacement is not recoverable by any
        // local method -- see the note in RefineFlow. Those rates drag the lane
        // mean down however good the rest is; at 60 FPS, where the motion is
        // sampled finely enough to be recoverable, it runs well ahead of the
        // crossfade.
        //
        // The bad-pixel columns gate a different failure from PSNR: PSNR is a
        // mean, and a small region that is badly wrong -- a halo, a torn edge,
        // a wobbling occlusion boundary -- can cost it little while being the
        // most visible thing in the frame. Measured when written: linear
        // 0.61 % mean / 1.09 % worst, occlusion 1.85 % / 5.33 %. The detail
        // lane's figures (53.8 % / 86.8 %) are the aliasing limit described
        // above, not a defect, so its ceilings are set only to catch a change
        // that makes it worse still.
        struct Gate {
            const char* lane;
            double minimum_mean_psnr_db;
            double minimum_worst_psnr_db;
            double minimum_mean_gain_db;
            double maximum_mean_bad_pixel_percent;
            double maximum_worst_bad_pixel_percent;
        };
        const Gate gates[] = {
            {"linear", 37.5, 28.0, 4.5, 1.5, 2.5},
            {"occlusion", 33.5, 19.5, 4.5, 3.5, 8.0},
            {"detail", 20.0, 8.0, -0.5, 65.0, 92.0},
        };

        for (const Gate& gate : gates) {
            const LaneSummary& summary = by_lane.at(gate.lane);
            Require(
                summary.mean_bad_pixel_percent <= gate.maximum_mean_bad_pixel_percent,
                std::string("Lane ") + gate.lane + " mean bad-pixel share rose to " +
                    std::to_string(summary.mean_bad_pixel_percent) + " %, above the " +
                    std::to_string(gate.maximum_mean_bad_pixel_percent) + " % ceiling.");
            Require(
                summary.worst_bad_pixel_percent <= gate.maximum_worst_bad_pixel_percent,
                std::string("Lane ") + gate.lane + " worst-case bad-pixel share rose to " +
                    std::to_string(summary.worst_bad_pixel_percent) + " %, above the " +
                    std::to_string(gate.maximum_worst_bad_pixel_percent) + " % ceiling.");
            Require(
                summary.mean_psnr_db >= gate.minimum_mean_psnr_db,
                std::string("Lane ") + gate.lane + " mean PSNR regressed to " +
                    std::to_string(summary.mean_psnr_db) + " dB, below the " +
                    std::to_string(gate.minimum_mean_psnr_db) + " dB floor.");
            Require(
                summary.worst_psnr_db >= gate.minimum_worst_psnr_db,
                std::string("Lane ") + gate.lane + " worst-case PSNR regressed to " +
                    std::to_string(summary.worst_psnr_db) + " dB, below the " +
                    std::to_string(gate.minimum_worst_psnr_db) + " dB floor.");
            Require(
                summary.mean_psnr_db - summary.mean_crossfade_psnr_db >= gate.minimum_mean_gain_db,
                std::string("Lane ") + gate.lane + " gained only " +
                    std::to_string(summary.mean_psnr_db - summary.mean_crossfade_psnr_db) +
                    " dB over a plain crossfade.");
        }

        std::cout << "interpolation quality: ok\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cout << "FAILED: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
