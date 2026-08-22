#pragma once

#include "flow_scale.h"
#include "gpu_timing.h"
#include "output_mode.h"
#include "pacing_mode.h"
#include "debug_view.h"
#include "upscaler.h"
#include "present_mode.h"
#include "ui_mask.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_3.h>
#include <winrt/base.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace osss {

class MotionInterpolator;

// What `automatic` actually resolved to, plus why. Reported in the banner and
// the HUD: a user who asked for tearing and silently got vsync would otherwise
// have no way to tell why their pacing still beats.
struct PresentModeStatus {
    PresentMode requested = PresentMode::automatic;
    PresentMode effective = PresentMode::vsync;
    bool tearing_supported = false;
    // True once the swap chain is one DXGI can promote out of DWM composition.
    // A layered window can never be, which is why the overlay is no longer one.
    bool independent_flip_eligible = false;
};

class Renderer {
public:
    struct PresentationMetrics {
        std::uint64_t submitted = 0;
        std::uint64_t confirmed = 0;
        std::uint64_t statistics_disjoint = 0;
        UINT present_refresh_count = 0;
        UINT sync_refresh_count = 0;
        LARGE_INTEGER sync_qpc_time{};
        bool statistics_available = false;
    };

    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void InitializeDevice();
    // Must be called before CreateOutputWindow; the swap-chain flags depend on it.
    // Must be set before CreateOutputWindow: it decides the window styles,
    // and those cannot be changed afterwards without recreating the surface.
    // Spatial upscaling of the fused frame. Only does anything when the
    // output is larger than the captured source, which is the fullscreen
    // case where the target window is smaller than the monitor.
    void SetUpscaleMode(UpscaleMode mode) noexcept;
    [[nodiscard]] UpscaleMode UpscaleModeSetting() const noexcept;
    void SetUpscaleSharpness(float sharpness) noexcept;
    [[nodiscard]] GpuTimingStatistics UpscaleGpuTiming() const noexcept;
    void SetOutputMode(OutputMode mode) noexcept;
    [[nodiscard]] OutputMode OutputModeSetting() const noexcept;
    void SetPresentMode(PresentMode mode) noexcept;
    [[nodiscard]] PresentModeStatus PresentStatus() const noexcept;
    // Must be set before CreateOutputWindow: it decides the swap chain's
    // maximum frame latency and buffer count, which are fixed at creation. The
    // loop that feeds this renderer reads the same setting back to decide
    // whether to render a slot ahead of its deadline; see pacing_mode.h.
    void SetPacingMode(PacingMode mode) noexcept;
    [[nodiscard]] PacingMode PacingModeSetting() const noexcept;
    // How many presents DXGI lets this swap chain hold before the frame-latency
    // waitable object stops signalling. One for paced and unpaced, two for
    // queued. Zero before the swap chain exists.
    [[nodiscard]] unsigned MaximumFrameLatency() const noexcept;
    void CreateOutputWindow(const RECT& bounds, int max_multiplier, double target_fps);
    void PushCapturedFrame(ID3D11Texture2D* source);
    void StoreCapturedFrame(ID3D11Texture2D* source, std::uint64_t unique_sequence);
    [[nodiscard]] bool SelectFramePair(
        std::uint64_t previous_unique_sequence,
        std::uint64_t current_unique_sequence);
    [[nodiscard]] bool SelectRealFrame(std::uint64_t unique_sequence);
    [[nodiscard]] bool WaitForPresentationSlot(DWORD timeout_milliseconds = 0);
    // The frame-latency waitable object is a semaphore: whoever waits on it
    // consumes the signal. A loop that folds it into its own multi-object wait
    // (to wake on a free back buffer *or* a capture *or* a stop) has therefore
    // already taken the slot when that wait returns, and must say so here
    // instead of calling WaitForPresentationSlot again and finding it empty.
    void MarkPresentationSlotAcquired() noexcept;
    // Whether a back buffer is currently held for rendering, i.e. whether the
    // last successful slot wait has not yet been spent by a Present.
    [[nodiscard]] bool PresentationSlotAcquired() const noexcept;
    void Render(float alpha);
    void Present();
    void Show();
    // Pull the output window off screen without tearing down the swap chain or
    // the frame history. `Show` brings back the same pipeline, so a target that
    // leaves and re-enters the foreground resumes without a reinitialization.
    void Hide();
    [[nodiscard]] bool OutputVisible() const noexcept;
    void FollowTarget(HWND target);
    void SetMotionEnabled(bool enabled) noexcept;
    // Static UI/HUD regions excluded from motion interpolation. Applies to the
    // motion path only; the temporal-blend A/B baseline stays unmasked.
    // Returns false (with InterpolatorError set) if the mask could not be
    // uploaded; the mask is otherwise retained across source resizes.
    bool SetUiMask(std::vector<UiMaskRect> rects) noexcept;
    [[nodiscard]] std::size_t UiMaskRegionCount() const noexcept;
    // Automatic static-overlay detection, combined with any user rectangles.
    void SetAutoUiMaskEnabled(bool enabled) noexcept;
    [[nodiscard]] bool AutoUiMaskEnabled() const noexcept;
    // Resolution motion estimation runs at. Applies to the motion path
    // only; the temporal-blend A/B baseline estimates no motion at all.
    void SetFlowScale(FlowScale scale) noexcept;
    // Cheaper motion estimation at the same flow resolution.
    bool SetPerformanceMode(bool enabled) noexcept;
    [[nodiscard]] bool PerformanceMode() const noexcept;
    // Seeds each pair's motion search with the previous pair's flow when the
    // pairs are consecutive, which the renderer knows from the unique
    // sequences it selects pairs by. Invalidates nothing; on by default.
    void SetTemporalPriorEnabled(bool enabled) noexcept;
    [[nodiscard]] bool TemporalPriorEnabled() const noexcept;
    // Measured mean interval between unique source frames, in seconds. Scales
    // how far the coarse motion search looks, so that the search covers a
    // constant velocity rather than a constant displacement. Unlike every other
    // setter here it invalidates nothing and rebuilds nothing, which is what
    // makes it safe to drive from the presentation loop each iteration.
    void SetSourcePeriod(double seconds) noexcept;
    // Largest displacement the coarse search can currently find, in source
    // pixels per pair. Zero when there is no motion interpolator.
    [[nodiscard]] unsigned CoarseSearchPixels() const noexcept;
    // Diagnostic visualisation of the interpolator internals.
    void SetDebugView(DebugView view) noexcept;
    [[nodiscard]] DebugView DebugViewSetting() const noexcept;
    [[nodiscard]] FlowScale FlowScaleSetting() const noexcept;

    [[nodiscard]] bool PumpMessages();
    [[nodiscard]] bool HasCapturedFrame() const noexcept;
    [[nodiscard]] ID3D11Device* Device() const noexcept;
    [[nodiscard]] std::wstring AdapterName() const;
    [[nodiscard]] std::string InterpolatorDescription() const;
    [[nodiscard]] std::string InterpolatorError() const;
    [[nodiscard]] HWND OutputWindow() const noexcept;
    // Human-readable name of the stop hotkey that actually registered, or an
    // empty string if every candidate was already owned by another process.
    // Callers must not promise a shortcut this returns empty for.
    [[nodiscard]] const std::wstring& StopHotkeyDescription() const noexcept;
    // The chord that toggles frame generation, or empty if none was free. A
    // separate registration from the stop chord and independently optional:
    // losing one must not cost the other.
    [[nodiscard]] const std::wstring& ToggleHotkeyDescription() const noexcept;
    // How many times that chord has been pressed. A counter rather than a flag
    // for two reasons: the renderer does not own whether generation is on --
    // FrameSelector does -- and a press arriving between two polls is remembered
    // rather than lost. Incremented from the window procedure, which
    // PumpMessages runs on the same thread as the presentation loop.
    [[nodiscard]] std::uint64_t GenerationToggleCount() const noexcept;
    [[nodiscard]] HANDLE FrameLatencyWaitableObject() const noexcept;
    [[nodiscard]] const PresentationMetrics& PresentationStatistics() const noexcept;
    // A steady_clock estimate of a recent vblank on the output's display, used to
    // phase-align the output clock. Returns nullopt until DXGI has reported a
    // presentation statistic carrying a sync time.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
        LastVblank() const noexcept;
    [[nodiscard]] std::uint64_t MotionPreparationCount() const noexcept;
    void PollGpuTimings() noexcept;
    [[nodiscard]] GpuTimingStatistics FlowGpuTiming() const noexcept;
    [[nodiscard]] GpuTimingStatistics FusionGpuTiming() const noexcept;

private:
    static constexpr wchar_t kWindowClassName[] = L"OSSS.FrameOutput";
    static constexpr int kQuitHotkeyId = 0x4A4C;
    static constexpr int kToggleHotkeyId = 0x4A4D;

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    void RegisterStopHotkey();
    void RegisterToggleHotkey();
    void DetectTearingSupport() noexcept;
    [[nodiscard]] bool EnsureFusionTarget(UINT width, UINT height) noexcept;
    void CreateSwapChain(UINT width, UINT height);
    void CreateRenderTarget();
    void CreatePipeline();
    void CreateHistoryTextures(const D3D11_TEXTURE2D_DESC& source_description);
    void RefreshPresentationStatistics() noexcept;
    void Resize(UINT width, UINT height);
    void ReleaseWindow() noexcept;

    struct HistoryFrame {
        std::uint64_t unique_sequence = 0;
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11ShaderResourceView> view;
    };

    [[nodiscard]] const HistoryFrame* FindHistoryFrame(std::uint64_t unique_sequence) const noexcept;

    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<IDXGIAdapter1> adapter_;
    winrt::com_ptr<IDXGISwapChain1> swap_chain_;
    winrt::com_ptr<IDXGISwapChain2> swap_chain_2_;
    winrt::com_ptr<ID3D11RenderTargetView> render_target_;
    winrt::com_ptr<ID3D11VertexShader> vertex_shader_;
    winrt::com_ptr<ID3D11PixelShader> blend_pixel_shader_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11Buffer> interpolation_constants_;
    winrt::com_ptr<ID3D11Texture2D> previous_frame_;
    winrt::com_ptr<ID3D11Texture2D> current_frame_;
    winrt::com_ptr<ID3D11ShaderResourceView> previous_view_;
    winrt::com_ptr<ID3D11ShaderResourceView> current_view_;
    std::deque<HistoryFrame> frame_history_;
    std::vector<HistoryFrame> recycled_history_frames_;
    std::unique_ptr<MotionInterpolator> motion_interpolator_;
    std::unique_ptr<GpuTimestampCollector> fusion_timing_;

    HWND output_window_ = nullptr;
    RECT last_bounds_{};
    UINT output_width_ = 0;
    UINT output_height_ = 0;
    UINT source_width_ = 0;
    UINT source_height_ = 0;
    DXGI_FORMAT source_format_ = DXGI_FORMAT_UNKNOWN;
    HANDLE frame_latency_waitable_object_ = nullptr;
    PresentationMetrics presentation_metrics_{};
    // The exact flags the live swap chain was created with. ResizeBuffers must
    // be handed the same set or DXGI fails the call with E_INVALIDARG, so this
    // is remembered rather than re-derived: ALLOW_TEARING is conditional, and
    // a re-derivation that drifted from the creation path would only surface on
    // the first window resize.
    UINT swap_chain_flags_ = 0;
    PresentMode requested_present_mode_ = PresentMode::automatic;
    PresentMode effective_present_mode_ = PresentMode::vsync;
    bool tearing_supported_ = false;
    bool tearing_support_detected_ = false;
    std::optional<std::chrono::steady_clock::time_point> last_vblank_;
    std::uint64_t last_confirmed_present_id_ = 0;
    std::uint64_t active_previous_sequence_ = 0;
    std::uint64_t active_current_sequence_ = 0;
    std::uint64_t legacy_unique_sequence_ = 0;
    std::uint64_t motion_preparation_count_ = 0;
    std::wstring stop_hotkey_description_;
    bool stop_hotkey_registered_ = false;
    std::wstring toggle_hotkey_description_;
    bool toggle_hotkey_registered_ = false;
    std::uint64_t generation_toggle_count_ = 0;
    bool window_class_registered_ = false;
    bool output_visible_ = false;
    bool presentation_slot_acquired_ = false;
    bool has_confirmed_present_id_ = false;
    bool has_current_frame_ = false;
    bool has_frame_pair_ = false;
    bool motion_ready_ = false;
    bool motion_enabled_ = true;
    std::string motion_initialization_error_;
    std::vector<UiMaskRect> ui_mask_rects_;
    bool auto_ui_mask_enabled_ = false;
    FlowScale flow_scale_ = FlowScale::automatic;
    bool performance_mode_ = false;
    bool temporal_prior_enabled_ = true;
    double source_period_seconds_ = 1.0 / kReferenceCoarseReachSourceFps;
    DebugView debug_view_ = DebugView::off;
    OutputMode output_mode_ = OutputMode::overlay;
    PacingMode pacing_mode_ = PacingMode::paced;
    unsigned maximum_frame_latency_ = 0;
    UpscaleMode upscale_mode_ = UpscaleMode::automatic;
    float upscale_sharpness_ = kDefaultSharpness;
    std::unique_ptr<Upscaler> upscaler_;
    // Source-resolution target the fusion draws into when upscaling is active,
    // so the upscaler receives native pixels rather than ones the rasteriser
    // already stretched.
    winrt::com_ptr<ID3D11Texture2D> fusion_texture_;
    winrt::com_ptr<ID3D11RenderTargetView> fusion_target_;
    winrt::com_ptr<ID3D11ShaderResourceView> fusion_view_;
    UINT fusion_width_ = 0;
    UINT fusion_height_ = 0;
};

} // namespace osss
