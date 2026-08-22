#pragma once

#include "debug_view.h"
#include "flow_scale.h"
#include "gpu_timing.h"
#include "ui_mask.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace osss {

class MotionInterpolator {
public:
    MotionInterpolator(ID3D11Device* device, ID3D11DeviceContext* context);

    MotionInterpolator(const MotionInterpolator&) = delete;
    MotionInterpolator& operator=(const MotionInterpolator&) = delete;

    // Estimates flow for one source pair. `continues_previous_pair` says that
    // `previous` is the frame the last successful PreparePair called `current`
    // -- the pairs share a frame and follow each other in time -- which is
    // what makes the last pair's flow a usable prior for this one (see
    // SetTemporalPriorEnabled). Callers that cannot vouch for that pass false
    // and get an estimate from scratch; the default is the safe one.
    bool PreparePair(
        ID3D11ShaderResourceView* previous,
        ID3D11ShaderResourceView* current,
        UINT source_width,
        UINT source_height,
        bool continues_previous_pair = false) noexcept;

    bool BindForDraw(
        ID3D11ShaderResourceView* previous,
        ID3D11ShaderResourceView* current,
        float interpolation_time) noexcept;

    void UnbindAfterDraw() noexcept;
    void Reset() noexcept;
    void PollGpuTimings() noexcept;

    // Replaces the static UI/HUD exclusion regions. Masked pixels always show
    // the newest real frame and are ignored as optical-flow matching evidence.
    // Regions are resolved against the current source size and re-resolved
    // whenever the source size changes. Returns false and keeps the previous
    // mask if the GPU mask texture cannot be created.
    bool SetUiMask(std::vector<UiMaskRect> rects) noexcept;
    [[nodiscard]] const std::vector<UiMaskRect>& UiMask() const noexcept;

    // Enables automatic detection of static overlays: regions that hold still
    // while the scene around them moves accumulate persistence over several
    // source pairs and are then treated exactly like a user rectangle. The
    // detector reads the flow of the pair it runs on, so its result applies to
    // that pair's fusion and to the next pair's flow. Disabling it clears the
    // accumulated state.
    void SetAutoUiMaskEnabled(bool enabled) noexcept;
    [[nodiscard]] bool AutoUiMaskEnabled() const noexcept;

    struct UiPersistenceSample {
        float score = 0.0F;            // 0-1 accumulation the next update reads
        float masked_score = 0.0F;     // score after edge dilation; drives the mask
        float frame_difference = 0.0F; // how still the cell held this pair
        float neighbour_motion = 0.0F; // strongest confident flow around it
    };

    // Diagnostics only: copies the detector state to system memory with a
    // blocking readback. Never call this from the presentation path.
    [[nodiscard]] bool ReadUiPersistence(
        std::vector<UiPersistenceSample>& samples,
        UINT& width,
        UINT& height) const noexcept;

    // Chooses the resolution motion estimation runs at. Every flow surface
    // is sized from the resolved divisor, so changing this after the
    // resources exist rebuilds them and drops the current pair: the next
    // PreparePair re-estimates on the new grid rather than mixing two
    // resolutions. Cheap to call with an unchanged value.
    void SetFlowScale(FlowScale scale) noexcept;
    [[nodiscard]] FlowScale FlowScaleSetting() const noexcept;

    // Replaces the output with a visualisation of the interpolator's own
    // internals. Diagnostics only; see src/debug_view.h.
    void SetDebugView(DebugView view) noexcept;
    [[nodiscard]] DebugView DebugViewSetting() const noexcept;

    // Cheaper motion estimation at the same flow resolution: a narrower coarse
    // search window, and no second local search to resolve periodic detail that
    // landed in the wrong period. Recompiles the compute shaders, so call it
    // before the first PreparePair where possible. Returns false with LastError
    // set if the rebuild failed.
    bool SetPerformanceMode(bool enabled) noexcept;
    [[nodiscard]] bool PerformanceMode() const noexcept;

    // Seeds the coarse motion search with the previous pair's flow when the
    // caller says the pairs are consecutive. The window the coarse level scans
    // is a fixed displacement ceiling; a seed lets the estimate follow motion
    // that accelerated out past it, one cell per pair, and costs about a
    // ninth of the window. It is only ever an extra candidate -- selection
    // stays regularised toward zero -- so it cannot lock a wrong vector in.
    // On by default; a constant-buffer value, so toggling rebuilds nothing.
    void SetTemporalPriorEnabled(bool enabled) noexcept;
    [[nodiscard]] bool TemporalPriorEnabled() const noexcept;

    // The measured mean interval between unique source frames, in seconds.
    //
    // This is the one input that decides how far the coarse level searches, and
    // it is here rather than derived inside because the interpolator has no view
    // of the source timeline: it sees two frames, not the cadence they arrived
    // at. Feed it `SourceTimeline::EstimatedSourcePeriod`, which is smoothed --
    // see ResolveCoarseSearchRadius in src/flow_scale.h for why a single pair's
    // own interval is the wrong thing to use.
    //
    // Cheap and safe to call every iteration; it stores a double and nothing
    // else. The value is read at the next PreparePair, so a change takes effect
    // on the next pair rather than retroactively. Non-positive is treated as
    // "not measured yet" and resolves to the reference radius. Unlike
    // SetFlowScale this rebuilds nothing: the flow surfaces are sized by the
    // divisor, and the radius only bounds a loop over them.
    void SetSourcePeriod(double seconds) noexcept;
    [[nodiscard]] double SourcePeriod() const noexcept;

    // What that period currently resolves to: the coarse search half-width in
    // coarse cells, and the largest displacement it can find in source pixels
    // per pair. The second is the one worth reporting -- motion past it is
    // unrecoverable, so it is the number to compare against what the content
    // actually moves.
    [[nodiscard]] int CoarseSearchRadius() const noexcept;
    [[nodiscard]] unsigned CoarseSearchPixels() const noexcept;

    [[nodiscard]] bool Ready() const noexcept;
    // Source pixels per fine flow cell along each axis, after the setting
    // above is resolved against the current source size.
    [[nodiscard]] UINT FlowScaleDivisor() const noexcept;
    // True when every motion shader came from the on-disk bytecode cache, so a
    // slow start is attributable to a cold cache rather than to something else.
    [[nodiscard]] bool ShadersCameFromCache() const noexcept;
    [[nodiscard]] const std::string& LastError() const noexcept;
    [[nodiscard]] GpuTimingStatistics FlowGpuTiming() const noexcept;

private:
    struct FlowSurface {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11ShaderResourceView> view;
        winrt::com_ptr<ID3D11UnorderedAccessView> unordered_view;
    };

    // Mip-chained luma of one source frame. Level 0 comes from the BuildLuma
    // dispatch and the rest from GenerateMips. This is what makes the coarse
    // search band-limited: without it the coarse level point-samples
    // full-resolution pixels on its own step grid and aliases anything finer.
    struct LumaPyramid {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11ShaderResourceView> view;
        winrt::com_ptr<ID3D11UnorderedAccessView> unordered_view;
    };

    struct alignas(16) MotionConstants {
        std::uint32_t source_width = 0;
        std::uint32_t source_height = 0;
        std::uint32_t coarse_width = 0;
        std::uint32_t coarse_height = 0;
        std::uint32_t fine_width = 0;
        std::uint32_t fine_height = 0;
        float coarse_scale = 0.0F;
        float fine_scale = 0.0F;
        float inverse_source_width = 0.0F;
        float inverse_source_height = 0.0F;
        std::uint32_t ui_mask_enabled = 0;
        std::uint32_t auto_mask_enabled = 0;
        float coarse_mip = 0.0F;
        float fine_mip = 0.0F;
        std::int32_t coarse_radius = 0;
        // -1 forward, +1 backward, 0 = no prior; see the HLSL cbuffer.
        float flow_prior_sign = 0.0F;
    };

    struct alignas(16) InterpolationConstants {
        float interpolation_time = 0.0F;
        float inverse_source_width = 0.0F;
        float inverse_source_height = 0.0F;
        float scene_cut_threshold = 0.28F;
        float confidence_floor = 0.035F;
        float static_pixel_low = 0.006F;
        float static_pixel_high = 0.030F;
        float auto_mask_enabled = 0.0F;
        float debug_view = 0.0F;
        // Pads to a 16-byte multiple; the static_assert below is the guard.
        float padding[3]{};
    };

    static_assert(sizeof(MotionConstants) % 16 == 0);
    static_assert(sizeof(InterpolationConstants) % 16 == 0);

    void CreateShaders();
    void CreateResources(UINT source_width, UINT source_height);
    [[nodiscard]] LumaPyramid CreateLumaPyramid(UINT width, UINT height) const;
    void DispatchLumaPyramids(
        ID3D11ShaderResourceView* previous,
        ID3D11ShaderResourceView* current);
    void CreateUiMaskTexture();
    void ClearUiPersistence() noexcept;
    void DispatchUiPersistence(
        ID3D11ShaderResourceView* previous,
        ID3D11ShaderResourceView* current);
    FlowSurface CreateFlowSurface(UINT width, UINT height) const;
    // `luma_a`/`luma_b` follow matching order, not frame order: the backward
    // dispatches pass the two frames the other way round and must receive their
    // pyramids swapped to match.
    // `flow_prior` is only read by the coarse shader; pass null elsewhere.
    void DispatchFlow(
        ID3D11ComputeShader* shader,
        ID3D11ShaderResourceView* frame_a,
        ID3D11ShaderResourceView* frame_b,
        ID3D11ShaderResourceView* luma_a,
        ID3D11ShaderResourceView* luma_b,
        ID3D11ShaderResourceView* coarse_flow,
        ID3D11ShaderResourceView* flow_prior,
        ID3D11UnorderedAccessView* output,
        UINT width,
        UINT height);
    void DispatchSceneMetrics(
        ID3D11ShaderResourceView* previous,
        ID3D11ShaderResourceView* current);
    void DispatchFlowFilter(const FlowSurface& source, const FlowSurface& destination);
    void ClearComputeBindings() noexcept;
    void StoreError(const std::string& message) noexcept;

    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11ComputeShader> luma_shader_;
    winrt::com_ptr<ID3D11ComputeShader> coarse_shader_;
    winrt::com_ptr<ID3D11ComputeShader> refine_shader_;
    winrt::com_ptr<ID3D11ComputeShader> filter_shader_;
    winrt::com_ptr<ID3D11ComputeShader> scene_shader_;
    winrt::com_ptr<ID3D11ComputeShader> persistence_shader_;
    winrt::com_ptr<ID3D11PixelShader> interpolation_shader_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11Buffer> motion_constants_;
    winrt::com_ptr<ID3D11Buffer> interpolation_constants_;
    GpuTimestampCollector flow_timing_;

    FlowSurface forward_coarse_;
    FlowSurface backward_coarse_;
    FlowSurface forward_fine_;
    FlowSurface backward_fine_;
    // Outlier-filtered copies. Everything downstream of estimation reads these;
    // `forward_fine_`/`backward_fine_` are the raw estimator output and are only
    // ever the input to FilterFlow.
    FlowSurface forward_fine_filtered_;
    FlowSurface backward_fine_filtered_;
    FlowSurface scene_metrics_;
    LumaPyramid previous_luma_;
    LumaPyramid current_luma_;
    // Ping-pong so a single pass can read the previous state and write the new
    // one without aliasing a bound resource. Everything within one source pair
    // reads `ui_persistence_index_`; the pair's own update writes the other
    // slot and only becomes visible at the start of the next pair. That one-pair
    // lag is deliberate: a HUD that ticks must still be masked on the frame it
    // ticks, and only stops being masked if it keeps changing.
    FlowSurface ui_persistence_[2];
    std::size_t ui_persistence_index_ = 0;
    bool ui_persistence_pending_ = false;
    bool shaders_came_from_cache_ = false;
    winrt::com_ptr<ID3D11Texture2D> ui_mask_texture_;
    winrt::com_ptr<ID3D11ShaderResourceView> ui_mask_view_;
    std::vector<UiMaskRect> ui_mask_rects_;
    bool ui_mask_active_ = false;
    bool auto_ui_mask_enabled_ = false;

    UINT source_width_ = 0;
    UINT source_height_ = 0;
    UINT coarse_width_ = 0;
    UINT coarse_height_ = 0;
    UINT fine_width_ = 0;
    UINT fine_height_ = 0;
    bool performance_mode_ = false;
    bool temporal_prior_enabled_ = true;
    // True once a PreparePair has completed on the current surfaces, so
    // `backward_fine_filtered_` holds a flow field rather than whatever the
    // allocation left there. Cleared by anything that invalidates the field.
    bool flow_prior_available_ = false;
    // Defaults to the reference rate, so an interpolator that is never told a
    // period behaves exactly as it did before the search scaled. Every GPU test
    // in tests/ drives it that way.
    double source_period_seconds_ = 1.0 / kReferenceCoarseReachSourceFps;
    DebugView debug_view_ = DebugView::off;
    FlowScale flow_scale_setting_ = FlowScale::automatic;
    UINT flow_scale_ = 4;
    bool ready_ = false;
    std::string last_error_;
};

} // namespace osss
