#include "renderer.h"

#include "motion_interpolator.h"
#include "window_catalog.h"

#include <d3d10.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace osss {
namespace {

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

constexpr char kPixelShaderSource[] = R"(
Texture2D<float4> previous_frame : register(t0);
Texture2D<float4> current_frame : register(t1);
SamplerState linear_clamp : register(s0);

cbuffer InterpolationConstants : register(b0) {
    float interpolation_alpha;
    float3 padding;
};

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    const float4 previous_color = previous_frame.Sample(linear_clamp, uv);
    const float4 current_color = current_frame.Sample(linear_clamp, uv);
    return lerp(previous_color, current_color, interpolation_alpha);
}
)";

winrt::com_ptr<ID3DBlob> CompileShader(
    const char* source,
    const char* entry_point,
    const char* profile) {
    winrt::com_ptr<ID3DBlob> bytecode;
    winrt::com_ptr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entry_point,
        profile,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        bytecode.put(),
        errors.put());

    if (FAILED(result)) {
        std::string message = "Shader compilation failed.";
        if (errors) {
            message.append(" ");
            message.append(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        throw std::runtime_error(message);
    }
    return bytecode;
}

UINT RectWidth(const RECT& bounds) {
    return static_cast<UINT>(std::max<LONG>(1, bounds.right - bounds.left));
}
RECT ToRect(const IntRect& bounds) {
    return RECT{bounds.x, bounds.y, bounds.x + bounds.width, bounds.y + bounds.height};
}
// The monitor a rect sits on, in virtual-screen coordinates. Fullscreen output
// has to cover an entire output exactly or DWM will not consider promoting it.
RECT MonitorBoundsFor(const RECT& bounds) {
    const HMONITOR monitor = MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        return info.rcMonitor;
    }
    return bounds;
}


UINT RectHeight(const RECT& bounds) {
    return static_cast<UINT>(std::max<LONG>(1, bounds.bottom - bounds.top));
}

} // namespace

Renderer::Renderer() = default;

Renderer::~Renderer() {
    ReleaseWindow();
}

void Renderer::InitializeDevice() {
    if (device_) {
        return;
    }

    constexpr UINT device_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    constexpr std::array feature_levels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL selected_level{};
    HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        device_flags,
        feature_levels.data(),
        static_cast<UINT>(feature_levels.size()),
        D3D11_SDK_VERSION,
        device_.put(),
        &selected_level,
        context_.put());

    if (result == E_INVALIDARG) {
        const D3D_FEATURE_LEVEL fallback_level = D3D_FEATURE_LEVEL_11_0;
        result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            device_flags,
            &fallback_level,
            1,
            D3D11_SDK_VERSION,
            device_.put(),
            &selected_level,
            context_.put());
    }
    winrt::check_hresult(result);

    winrt::com_ptr<ID3D10Multithread> multithread;
    winrt::check_hresult(context_->QueryInterface(__uuidof(ID3D10Multithread), multithread.put_void()));
    multithread->SetMultithreadProtected(TRUE);

    winrt::com_ptr<IDXGIDevice> dxgi_device;
    winrt::check_hresult(device_->QueryInterface(__uuidof(IDXGIDevice), dxgi_device.put_void()));
    winrt::com_ptr<IDXGIAdapter> base_adapter;
    winrt::check_hresult(dxgi_device->GetAdapter(base_adapter.put()));
    winrt::check_hresult(base_adapter->QueryInterface(__uuidof(IDXGIAdapter1), adapter_.put_void()));

    CreatePipeline();
    fusion_timing_ = std::make_unique<GpuTimestampCollector>(device_.get(), context_.get());
    try {
        motion_interpolator_ = std::make_unique<MotionInterpolator>(device_.get(), context_.get());
        upscaler_ = std::make_unique<Upscaler>(device_.get(), context_.get());
        upscaler_->SetSharpness(upscale_sharpness_);
        motion_initialization_error_.clear();
    } catch (const winrt::hresult_error& error) {
        motion_initialization_error_ = winrt::to_string(error.message());
    } catch (const std::exception& error) {
        motion_initialization_error_ = error.what();
    }
}

void Renderer::CreateOutputWindow(
    const RECT& bounds,
    const int max_multiplier,
    const double target_fps) {
    if (!device_) {
        throw std::logic_error("InitializeDevice must be called before CreateOutputWindow.");
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClassName;
    window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));

    if (RegisterClassExW(&window_class) == 0) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            winrt::throw_last_error();
        }
    } else {
        window_class_registered_ = true;
    }

    const std::wstring title =
        L"OSSS " + std::to_wstring(static_cast<long long>(std::lround(target_fps))) +
        L" FPS (" + std::to_wstring(max_multiplier) + L"x max)";
    // Overlay mode requires WS_EX_LAYERED, and that was measured rather than
    // assumed.
    //
    // It looks removable: the layered alpha below is 255 -- fully opaque -- so
    // it appears to buy nothing, and it is genuinely expensive, because a
    // layered window is composed by DWM through a redirection surface whatever
    // the swap chain asks for and can therefore never be promoted to
    // independent flip.
    //
    // It is also load-bearing. WS_EX_TRANSPARENT plus WM_NCHITTEST returning
    // HTTRANSPARENT does *not* pass a click to a window owned by another
    // process; only the layered-plus-transparent pair does. Measured directly
    // with tests/input_passthrough_smoke.cpp: without WS_EX_LAYERED it reports
    // "the generated-frame surface intercepted the target click", and with it
    // the same binary passes.
    //
    // Fullscreen mode takes exactly that trade in the other direction, which is
    // why it is a separate mode and not a tweak: it drops the layered style to
    // become promotable, and gives up click-through to do it. WS_EX_NOACTIVATE
    // is kept in both so the target never loses keyboard focus. See
    // src/output_mode.h.
    //
    // WS_EX_NOREDIRECTIONBITMAP stays off: it is for composition swap chains
    // created through DirectComposition, not CreateSwapChainForHwnd, and
    // pairing it with a layered window was contradictory.
    const bool layered = output_mode_ == OutputMode::overlay;
    const DWORD extended_style = layered
        ? (WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
           WS_EX_LAYERED)
        : (WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    // Fullscreen output covers the monitor the target is on, not the target.
    const RECT window_bounds = layered ? bounds : MonitorBoundsFor(bounds);
    output_window_ = CreateWindowExW(
        extended_style,
        kWindowClassName,
        title.c_str(),
        WS_POPUP,
        window_bounds.left,
        window_bounds.top,
        static_cast<int>(RectWidth(window_bounds)),
        static_cast<int>(RectHeight(window_bounds)),
        nullptr,
        nullptr,
        instance,
        this);
    if (!output_window_) {
        winrt::throw_last_error();
    }
    // Alpha 255: fully opaque. The layered style is carried for its input
    // behaviour, not for translucency. Never applied in fullscreen mode -- the
    // call would silently re-add the very attribute that blocks promotion.
    if (layered && !SetLayeredWindowAttributes(output_window_, 0, 255, LWA_ALPHA)) {
        winrt::throw_last_error();
    }

    last_bounds_ = window_bounds;
    output_width_ = RectWidth(window_bounds);
    output_height_ = RectHeight(window_bounds);
    CreateSwapChain(output_width_, output_height_);

    RegisterStopHotkey();
    RegisterToggleHotkey();
}

void Renderer::RegisterStopHotkey() {
    // Ctrl+Alt+F12 is not ours to assume: the Intel Graphics Command Center
    // hotkey service claims exactly that chord by default, and RegisterHotKey
    // then fails. It used to fail silently, leaving a topmost click-through
    // window with no way to close it while the banner still advertised the
    // shortcut. Walk a candidate list and report what actually took.
    struct Candidate {
        UINT modifiers;
        UINT key;
        const wchar_t* description;
    };
    static constexpr Candidate kCandidates[] = {
        {MOD_CONTROL | MOD_ALT, VK_F12, L"Ctrl+Alt+F12"},
        {MOD_CONTROL | MOD_SHIFT, VK_F12, L"Ctrl+Shift+F12"},
        {MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F12, L"Ctrl+Alt+Shift+F12"},
        {MOD_CONTROL | MOD_ALT, VK_END, L"Ctrl+Alt+End"},
    };

    stop_hotkey_registered_ = false;
    stop_hotkey_description_.clear();
    for (const auto& candidate : kCandidates) {
        if (RegisterHotKey(
                output_window_,
                kQuitHotkeyId,
                candidate.modifiers | MOD_NOREPEAT,
                candidate.key)) {
            stop_hotkey_registered_ = true;
            stop_hotkey_description_ = candidate.description;
            return;
        }
    }
}

void Renderer::RegisterToggleHotkey() {
    // Same walk-a-list approach as the stop chord above, and for the same
    // reason: none of these are ours to assume. F11 rather than F12 so a machine
    // that has one F12 chord free does not have to choose between stopping and
    // toggling, and the two lists never overlap.
    //
    // Failing to register is not an error. The toggle is a convenience; a
    // session with no free chord runs exactly as it did before this existed, and
    // the banner says so rather than advertising a shortcut that does nothing --
    // which is the specific bug the stop chord had.
    struct Candidate {
        UINT modifiers;
        UINT key;
        const wchar_t* description;
    };
    static constexpr Candidate kCandidates[] = {
        {MOD_CONTROL | MOD_ALT, VK_F11, L"Ctrl+Alt+F11"},
        {MOD_CONTROL | MOD_SHIFT, VK_F11, L"Ctrl+Shift+F11"},
        {MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F11, L"Ctrl+Alt+Shift+F11"},
        {MOD_CONTROL | MOD_ALT, VK_HOME, L"Ctrl+Alt+Home"},
    };

    toggle_hotkey_registered_ = false;
    toggle_hotkey_description_.clear();
    for (const auto& candidate : kCandidates) {
        if (RegisterHotKey(
                output_window_,
                kToggleHotkeyId,
                candidate.modifiers | MOD_NOREPEAT,
                candidate.key)) {
            toggle_hotkey_registered_ = true;
            toggle_hotkey_description_ = candidate.description;
            return;
        }
    }
}

void Renderer::PushCapturedFrame(ID3D11Texture2D* const source) {
    if (!source) {
        return;
    }
    StoreCapturedFrame(source, ++legacy_unique_sequence_);
    if (frame_history_.size() == 1) {
        static_cast<void>(SelectRealFrame(frame_history_.back().unique_sequence));
    } else {
        static_cast<void>(SelectFramePair(
            frame_history_[frame_history_.size() - 2].unique_sequence,
            frame_history_.back().unique_sequence));
    }
}

void Renderer::StoreCapturedFrame(
    ID3D11Texture2D* const source,
    const std::uint64_t unique_sequence) {
    if (!source) {
        return;
    }

    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    if (source_format_ == DXGI_FORMAT_UNKNOWN ||
        description.Width != source_width_ ||
        description.Height != source_height_ ||
        description.Format != source_format_) {
        CreateHistoryTextures(description);
    }

    if (FindHistoryFrame(unique_sequence)) {
        return;
    }

    HistoryFrame frame{};
    const auto reusable = std::find_if(
        recycled_history_frames_.begin(),
        recycled_history_frames_.end(),
        [this, &description](const HistoryFrame& candidate) {
            if (!candidate.texture ||
                candidate.texture.get() == previous_frame_.get() ||
                candidate.texture.get() == current_frame_.get()) {
                return false;
            }
            D3D11_TEXTURE2D_DESC candidate_description{};
            candidate.texture->GetDesc(&candidate_description);
            return candidate_description.Width == description.Width &&
                candidate_description.Height == description.Height &&
                candidate_description.Format == description.Format;
        });
    if (reusable != recycled_history_frames_.end()) {
        frame = std::move(*reusable);
        recycled_history_frames_.erase(reusable);
    } else {
        D3D11_TEXTURE2D_DESC owned_description = description;
        owned_description.MipLevels = 1;
        owned_description.ArraySize = 1;
        owned_description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
        owned_description.Usage = D3D11_USAGE_DEFAULT;
        owned_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        owned_description.CPUAccessFlags = 0;
        owned_description.MiscFlags = 0;
        winrt::check_hresult(device_->CreateTexture2D(
            &owned_description,
            nullptr,
            frame.texture.put()));
        winrt::check_hresult(device_->CreateShaderResourceView(
            frame.texture.get(),
            nullptr,
            frame.view.put()));
    }
    frame.unique_sequence = unique_sequence;
    context_->CopyResource(frame.texture.get(), source);
    frame_history_.push_back(std::move(frame));
    constexpr std::size_t maximum_history = 8;
    while (frame_history_.size() > maximum_history) {
        recycled_history_frames_.push_back(std::move(frame_history_.front()));
        frame_history_.pop_front();
    }
}

bool Renderer::SelectFramePair(
    const std::uint64_t previous_unique_sequence,
    const std::uint64_t current_unique_sequence) {
    if (previous_unique_sequence == current_unique_sequence) {
        return SelectRealFrame(current_unique_sequence);
    }
    if (has_frame_pair_ &&
        active_previous_sequence_ == previous_unique_sequence &&
        active_current_sequence_ == current_unique_sequence) {
        return true;
    }

    const HistoryFrame* previous = FindHistoryFrame(previous_unique_sequence);
    const HistoryFrame* current = FindHistoryFrame(current_unique_sequence);
    if (!previous || !current) {
        return false;
    }

    // The new pair continues the old one when it starts on the frame the old
    // one ended on. That is the only case in which the old pair's flow says
    // anything about this one, and it is decided here, before the sequences
    // below are overwritten, because only the renderer knows the sequences.
    const bool continues_previous_pair =
        has_frame_pair_ && active_current_sequence_ == previous_unique_sequence;

    previous_frame_ = previous->texture;
    previous_view_ = previous->view;
    current_frame_ = current->texture;
    current_view_ = current->view;
    active_previous_sequence_ = previous_unique_sequence;
    active_current_sequence_ = current_unique_sequence;
    has_current_frame_ = true;
    has_frame_pair_ = true;
    motion_ready_ = false;
    if (motion_enabled_ && motion_interpolator_) {
        ++motion_preparation_count_;
        motion_ready_ = motion_interpolator_->PreparePair(
            previous_view_.get(),
            current_view_.get(),
            source_width_,
            source_height_,
            continues_previous_pair);
    }
    return true;
}

bool Renderer::SelectRealFrame(const std::uint64_t unique_sequence) {
    if (has_current_frame_ && !has_frame_pair_ && active_current_sequence_ == unique_sequence) {
        return true;
    }
    const HistoryFrame* frame = FindHistoryFrame(unique_sequence);
    if (!frame) {
        return false;
    }
    previous_frame_ = frame->texture;
    previous_view_ = frame->view;
    current_frame_ = frame->texture;
    current_view_ = frame->view;
    active_previous_sequence_ = unique_sequence;
    active_current_sequence_ = unique_sequence;
    has_current_frame_ = true;
    has_frame_pair_ = false;
    motion_ready_ = false;
    return true;
}

bool Renderer::WaitForPresentationSlot(const DWORD timeout_milliseconds) {
    if (!frame_latency_waitable_object_) {
        presentation_slot_acquired_ = true;
        return true;
    }
    if (presentation_slot_acquired_) {
        return true;
    }

    const DWORD result = WaitForSingleObjectEx(
        frame_latency_waitable_object_,
        timeout_milliseconds,
        TRUE);
    if (result == WAIT_OBJECT_0) {
        presentation_slot_acquired_ = true;
        RefreshPresentationStatistics();
        return true;
    }
    if (result == WAIT_TIMEOUT || result == WAIT_IO_COMPLETION) {
        return false;
    }
    winrt::throw_last_error();
}

void Renderer::MarkPresentationSlotAcquired() noexcept {
    presentation_slot_acquired_ = true;
    RefreshPresentationStatistics();
}

bool Renderer::PresentationSlotAcquired() const noexcept {
    return presentation_slot_acquired_;
}

// Source-resolution colour target the fusion draws into when upscaling. Kept
// separate from the swap-chain back buffer because the upscaler has to receive
// native pixels: if fusion drew straight to an output-sized target the
// rasteriser would already have stretched it bilinearly, and sharpening that is
// a different and worse operation than upscaling the original.
bool Renderer::EnsureFusionTarget(const UINT width, const UINT height) noexcept {
    if (fusion_texture_ && fusion_width_ == width && fusion_height_ == height) {
        return true;
    }
    fusion_view_ = nullptr;
    fusion_target_ = nullptr;
    fusion_texture_ = nullptr;
    fusion_width_ = 0;
    fusion_height_ = 0;
    try {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        winrt::check_hresult(device_->CreateTexture2D(
            &description,
            nullptr,
            fusion_texture_.put()));
        winrt::check_hresult(device_->CreateRenderTargetView(
            fusion_texture_.get(),
            nullptr,
            fusion_target_.put()));
        winrt::check_hresult(device_->CreateShaderResourceView(
            fusion_texture_.get(),
            nullptr,
            fusion_view_.put()));
    } catch (...) {
        fusion_view_ = nullptr;
        fusion_target_ = nullptr;
        fusion_texture_ = nullptr;
        return false;
    }
    fusion_width_ = width;
    fusion_height_ = height;
    return true;
}

void Renderer::Render(const float alpha) {
    if (!presentation_slot_acquired_ && !WaitForPresentationSlot(1000)) {
        throw std::runtime_error("Timed out waiting for the swap chain presentation slot.");
    }
    if (!render_target_) {
        return;
    }

    // Decide before binding anything: the fusion draw goes to a native-sized
    // target when upscaling, and straight to the back buffer when not. Feeding
    // the upscaler a frame the rasteriser already stretched would ask it to
    // sharpen bilinear output, which is not the same operation at all.
    const bool upscaling =
        upscaler_ && has_current_frame_ && source_width_ != 0 && source_height_ != 0 &&
        upscale_mode_ != UpscaleMode::off &&
        (upscale_mode_ == UpscaleMode::always ||
         output_width_ != source_width_ || output_height_ != source_height_) &&
        EnsureFusionTarget(source_width_, source_height_);

    constexpr float black[] = {0.0F, 0.0F, 0.0F, 1.0F};
    ID3D11RenderTargetView* target = upscaling ? fusion_target_.get() : render_target_.get();
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->ClearRenderTargetView(target, black);
    if (upscaling) {
        context_->ClearRenderTargetView(render_target_.get(), black);
    }

    if (!has_current_frame_) {
        return;
    }

    const D3D11_VIEWPORT viewport{
        0.0F,
        0.0F,
        static_cast<float>(upscaling ? source_width_ : output_width_),
        static_cast<float>(upscaling ? source_height_ : output_height_),
        0.0F,
        1.0F,
    };
    context_->RSSetViewports(1, &viewport);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_shader_.get(), nullptr, 0);

    const float clamped_alpha = has_frame_pair_ ? std::clamp(alpha, 0.0F, 1.0F) : 1.0F;
    const bool motion_bound =
        has_frame_pair_ && motion_enabled_ && motion_ready_ && motion_interpolator_ &&
        motion_interpolator_->BindForDraw(
            previous_view_.get(),
            current_view_.get(),
            clamped_alpha);
    if (!motion_bound) {
        context_->PSSetShader(blend_pixel_shader_.get(), nullptr, 0);
        const float constants[4] = {clamped_alpha, 0.0F, 0.0F, 0.0F};
        context_->UpdateSubresource(interpolation_constants_.get(), 0, nullptr, constants, 0, 0);

        ID3D11ShaderResourceView* views[] = {previous_view_.get(), current_view_.get()};
        ID3D11SamplerState* samplers[] = {sampler_.get()};
        ID3D11Buffer* constant_buffers[] = {interpolation_constants_.get()};
        context_->PSSetShaderResources(0, 2, views);
        context_->PSSetSamplers(0, 1, samplers);
        context_->PSSetConstantBuffers(0, 1, constant_buffers);
    }
    if (fusion_timing_) {
        fusion_timing_->Begin();
    }
    context_->Draw(3, 0);
    if (fusion_timing_) {
        fusion_timing_->End();
    }

    if (motion_bound) {
        motion_interpolator_->UnbindAfterDraw();
    } else {
        ID3D11ShaderResourceView* empty_views[] = {nullptr, nullptr};
        context_->PSSetShaderResources(0, 2, empty_views);
    }

    if (upscaling) {
        // Unbound above, so the fusion result is safe to read as a texture.
        // A failure here leaves the back buffer cleared rather than torn, and
        // is reported through InterpolatorError rather than thrown: this runs
        // once per presented frame and must not become an exception path.
        (void)upscaler_->Draw(
            fusion_view_.get(),
            fusion_width_,
            fusion_height_,
            render_target_.get(),
            output_width_,
            output_height_);
    }
}

void Renderer::Present() {
    if (!swap_chain_ || !output_visible_) {
        return;
    }
    // The rational output clock decides which slots are submitted, and the
    // pre-render latency wait keeps a queued frame from adding hidden latency.
    // What is left is who decides *when* a submitted frame appears.
    //
    // Sync interval one hands that decision to the display: every present is
    // held to a vblank, so a target rate that is not a divisor of the refresh
    // rate cannot be paced. Each deadline rounds up to the next vblank and the
    // frame times alternate between one and two refresh periods. A 120 FPS
    // target on a 144 Hz panel is exactly that case, and it is the common one.
    //
    // Sync interval zero with ALLOW_TEARING lets the present return immediately,
    // so the clock's deadline sets handover time and this loop keeps its own
    // cadence. Because the overlay is layered, and therefore DWM-composed, DWM
    // still decides scan-out: this does not tear and does not drive a
    // variable-refresh panel the way a fullscreen swap chain would. It removes
    // our own quantization, which is the part an overlay controls.
    const UINT sync_interval = effective_present_mode_ == PresentMode::tearing ? 0U : 1U;
    const UINT flags =
        effective_present_mode_ == PresentMode::tearing ? DXGI_PRESENT_ALLOW_TEARING : 0U;
    winrt::check_hresult(swap_chain_->Present(sync_interval, flags));
    ++presentation_metrics_.submitted;
    presentation_slot_acquired_ = false;
    RefreshPresentationStatistics();
}

void Renderer::Show() {
    if (!output_window_ || output_visible_) {
        return;
    }
    ShowWindow(output_window_, SW_SHOWNOACTIVATE);
    SetWindowPos(
        output_window_,
        HWND_TOPMOST,
        last_bounds_.left,
        last_bounds_.top,
        static_cast<int>(RectWidth(last_bounds_)),
        static_cast<int>(RectHeight(last_bounds_)),
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    output_visible_ = true;
}

void Renderer::Hide() {
    if (!output_window_ || !output_visible_) {
        return;
    }
    ShowWindow(output_window_, SW_HIDE);
    output_visible_ = false;
}

bool Renderer::OutputVisible() const noexcept {
    return output_visible_;
}

void Renderer::FollowTarget(const HWND target) {
    const auto bounds = ExtendedWindowBounds(WindowHandle::FromNative(target));
    // Fullscreen output stays locked to the monitor, so it only moves when the
    // target crosses to another one. Tracking the target rect here would shrink
    // the window off the output and lose promotion for good.
    const RECT followed = (bounds && output_mode_ == OutputMode::fullscreen)
        ? MonitorBoundsFor(ToRect(*bounds))
        : (bounds ? ToRect(*bounds) : RECT{});
    if (!bounds || !output_window_) {
        return;
    }

    if (EqualRect(&last_bounds_, &followed)) {
        return;
    }
    last_bounds_ = followed;
    SetWindowPos(
        output_window_,
        HWND_TOPMOST,
        followed.left,
        followed.top,
        static_cast<int>(RectWidth(followed)),
        static_cast<int>(RectHeight(followed)),
        SWP_NOACTIVATE | (output_visible_ ? SWP_SHOWWINDOW : 0));
}

void Renderer::SetMotionEnabled(const bool enabled) noexcept {
    motion_enabled_ = enabled;
    motion_ready_ = false;
    if (motion_interpolator_) {
        motion_interpolator_->Reset();
    }
}

bool Renderer::SetUiMask(std::vector<UiMaskRect> rects) noexcept {
    try {
        ui_mask_rects_ = rects;
    } catch (...) {
        return false;
    }
    if (!motion_interpolator_) {
        // No motion backend: nothing to mask, but remember the regions so a
        // caller can still report what was requested.
        return true;
    }
    motion_ready_ = false;
    const bool uploaded = motion_interpolator_->SetUiMask(std::move(rects));
    if (has_frame_pair_ && motion_enabled_) {
        // Flow for the active pair was estimated against the old mask. Not a
        // continuation: the field it would be seeded from is its own.
        motion_ready_ = motion_interpolator_->PreparePair(
            previous_view_.get(),
            current_view_.get(),
            source_width_,
            source_height_);
    }
    return uploaded;
}

std::size_t Renderer::UiMaskRegionCount() const noexcept {
    return ui_mask_rects_.size();
}

void Renderer::SetAutoUiMaskEnabled(const bool enabled) noexcept {
    auto_ui_mask_enabled_ = enabled;
    if (motion_interpolator_) {
        motion_interpolator_->SetAutoUiMaskEnabled(enabled);
        motion_ready_ = false;
    }
}

bool Renderer::AutoUiMaskEnabled() const noexcept {
    return auto_ui_mask_enabled_;
}

void Renderer::SetFlowScale(const FlowScale scale) noexcept {
    flow_scale_ = scale;
    if (motion_interpolator_) {
        motion_interpolator_->SetFlowScale(scale);
        motion_ready_ = false;
    }
}

bool Renderer::SetPerformanceMode(const bool enabled) noexcept {
    performance_mode_ = enabled;
    if (!motion_interpolator_) {
        return true;
    }
    motion_ready_ = false;
    return motion_interpolator_->SetPerformanceMode(enabled);
}

void Renderer::SetTemporalPriorEnabled(const bool enabled) noexcept {
    temporal_prior_enabled_ = enabled;
    if (motion_interpolator_) {
        // Like SetSourcePeriod, this does not clear `motion_ready_`: the
        // prepared pair is still a valid estimate, and the setting only
        // changes how the next one is searched.
        motion_interpolator_->SetTemporalPriorEnabled(enabled);
    }
}

bool Renderer::TemporalPriorEnabled() const noexcept {
    return temporal_prior_enabled_;
}

void Renderer::SetSourcePeriod(const double seconds) noexcept {
    source_period_seconds_ = seconds;
    if (motion_interpolator_) {
        // Deliberately does not clear `motion_ready_`. The flow surfaces are
        // sized by the divisor, not by the search radius, so nothing about the
        // prepared pair goes stale: the next PreparePair simply searches further
        // or less far. Dropping the pair here would throw away a valid estimate
        // once per source-rate wobble, which is the opposite of the point.
        motion_interpolator_->SetSourcePeriod(seconds);
    }
}

unsigned Renderer::CoarseSearchPixels() const noexcept {
    return motion_interpolator_ ? motion_interpolator_->CoarseSearchPixels() : 0U;
}

void Renderer::SetDebugView(const DebugView view) noexcept {
    debug_view_ = view;
    if (motion_interpolator_) {
        motion_interpolator_->SetDebugView(view);
    }
}

DebugView Renderer::DebugViewSetting() const noexcept {
    return debug_view_;
}

bool Renderer::PerformanceMode() const noexcept {
    return performance_mode_;
}

FlowScale Renderer::FlowScaleSetting() const noexcept {
    return flow_scale_;
}

bool Renderer::PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            return false;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

bool Renderer::HasCapturedFrame() const noexcept {
    return has_current_frame_;
}

ID3D11Device* Renderer::Device() const noexcept {
    return device_.get();
}

std::wstring Renderer::AdapterName() const {
    if (!adapter_) {
        return L"unknown adapter";
    }
    DXGI_ADAPTER_DESC1 description{};
    winrt::check_hresult(adapter_->GetDesc1(&description));
    return description.Description;
}

std::string Renderer::InterpolatorDescription() const {
    if (!motion_enabled_ || !motion_interpolator_) {
        return "temporal blend fallback";
    }
    std::string description = "adaptive bidirectional HLSL optical flow (flow scale ";
    description += FlowScaleName(flow_scale_);
    if (performance_mode_) {
        description += ", performance";
    }
    if (!temporal_prior_enabled_) {
        // The default is on; only the departure from it is worth a word.
        description += ", no temporal prior";
    }
    description += ")";
    if (!ui_mask_rects_.empty()) {
        description += "; " + std::to_string(ui_mask_rects_.size()) + " UI mask region";
        description += ui_mask_rects_.size() == 1 ? "" : "s";
    }
    if (auto_ui_mask_enabled_) {
        description += "; automatic static-overlay detection";
    }
    return description;
}

std::string Renderer::InterpolatorError() const {
    if (!motion_initialization_error_.empty()) {
        return motion_initialization_error_;
    }
    if (motion_interpolator_) {
        return motion_interpolator_->LastError();
    }
    return {};
}

HWND Renderer::OutputWindow() const noexcept {
    return output_window_;
}

const std::wstring& Renderer::StopHotkeyDescription() const noexcept {
    return stop_hotkey_description_;
}

const std::wstring& Renderer::ToggleHotkeyDescription() const noexcept {
    return toggle_hotkey_description_;
}

std::uint64_t Renderer::GenerationToggleCount() const noexcept {
    return generation_toggle_count_;
}

HANDLE Renderer::FrameLatencyWaitableObject() const noexcept {
    return frame_latency_waitable_object_;
}

std::optional<std::chrono::steady_clock::time_point> Renderer::LastVblank() const noexcept {
    return last_vblank_;
}

const Renderer::PresentationMetrics& Renderer::PresentationStatistics() const noexcept {
    return presentation_metrics_;
}

std::uint64_t Renderer::MotionPreparationCount() const noexcept {
    return motion_preparation_count_;
}

void Renderer::PollGpuTimings() noexcept {
    if (fusion_timing_) {
        fusion_timing_->Poll();
    }
    if (motion_interpolator_) {
        motion_interpolator_->PollGpuTimings();
    }
}

GpuTimingStatistics Renderer::FlowGpuTiming() const noexcept {
    return motion_interpolator_
        ? motion_interpolator_->FlowGpuTiming()
        : GpuTimingStatistics{};
}

GpuTimingStatistics Renderer::FusionGpuTiming() const noexcept {
    return fusion_timing_ ? fusion_timing_->Statistics() : GpuTimingStatistics{};
}

LRESULT CALLBACK Renderer::WindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    Renderer* renderer = reinterpret_cast<Renderer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        renderer = static_cast<Renderer*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(renderer));
    }

    switch (message) {
    case WM_HOTKEY:
        if (wparam == kQuitHotkeyId) {
            PostQuitMessage(0);
            return 0;
        }
        if (wparam == kToggleHotkeyId && renderer) {
            // Only counted here. What the count means -- and whether generation
            // is on -- belongs to the presentation loop and FrameSelector, so
            // that a press is applied at a slot boundary rather than in the
            // middle of one.
            ++renderer->generation_toggle_count_;
            return 0;
        }
        break;
    case WM_SIZE:
        if (renderer && renderer->swap_chain_ && wparam != SIZE_MINIMIZED) {
            renderer->Resize(LOWORD(lparam), HIWORD(lparam));
        }
        return 0;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void Renderer::DetectTearingSupport() noexcept {
    if (tearing_support_detected_) {
        return;
    }
    tearing_support_detected_ = true;
    tearing_supported_ = false;
    if (!adapter_) {
        return;
    }
    // DXGI 1.5. Absent on Windows builds older than 1607, which is below the
    // stated platform minimum but costs nothing to tolerate: the QueryInterface
    // simply fails and the effective mode stays vsync.
    winrt::com_ptr<IDXGIFactory5> factory;
    if (FAILED(adapter_->GetParent(__uuidof(IDXGIFactory5), factory.put_void())) || !factory) {
        return;
    }
    BOOL allow_tearing = FALSE;
    if (FAILED(factory->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &allow_tearing,
            sizeof(allow_tearing)))) {
        return;
    }
    tearing_supported_ = allow_tearing != FALSE;
}

void Renderer::SetUpscaleMode(const UpscaleMode mode) noexcept {
    upscale_mode_ = mode;
}

UpscaleMode Renderer::UpscaleModeSetting() const noexcept {
    return upscale_mode_;
}

void Renderer::SetUpscaleSharpness(const float sharpness) noexcept {
    upscale_sharpness_ = sharpness;
    if (upscaler_) {
        upscaler_->SetSharpness(sharpness);
    }
}

GpuTimingStatistics Renderer::UpscaleGpuTiming() const noexcept {
    return upscaler_ ? upscaler_->GpuTiming() : GpuTimingStatistics{};
}

void Renderer::SetOutputMode(const OutputMode mode) noexcept {
    output_mode_ = mode;
}

OutputMode Renderer::OutputModeSetting() const noexcept {
    return output_mode_;
}

void Renderer::SetPresentMode(const PresentMode mode) noexcept {
    requested_present_mode_ = mode;
}

void Renderer::SetPacingMode(const PacingMode mode) noexcept {
    pacing_mode_ = mode;
}

PacingMode Renderer::PacingModeSetting() const noexcept {
    return pacing_mode_;
}

unsigned Renderer::MaximumFrameLatency() const noexcept {
    return maximum_frame_latency_;
}

PresentModeStatus Renderer::PresentStatus() const noexcept {
    PresentModeStatus status{};
    status.requested = requested_present_mode_;
    status.effective = effective_present_mode_;
    status.tearing_supported = tearing_supported_;
    // Necessary, not sufficient. The overlay is layered and therefore always
    // composed, so it can never be promoted however the swap chain is built.
    // Fullscreen mode clears that blocker and satisfies the two conditions
    // this class controls -- non-layered opaque window covering the output,
    // flip-model swap chain presenting without waiting on a vblank -- but DWM
    // still decides, and reports nothing. Anything composited above the window
    // (a notification, another overlay, OSSS's own HUD) silently demotes it.
    // Confirm with PresentMon: "Hardware: Independent Flip" against
    // "Composed: Flip".
    status.independent_flip_eligible =
        OutputModeCanReachIndependentFlip(output_mode_) &&
        effective_present_mode_ == PresentMode::tearing;
    return status;
}

void Renderer::CreateSwapChain(const UINT width, const UINT height) {
    if (frame_latency_waitable_object_) {
        CloseHandle(frame_latency_waitable_object_);
        frame_latency_waitable_object_ = nullptr;
    }
    swap_chain_2_ = nullptr;
    swap_chain_ = nullptr;

    winrt::com_ptr<IDXGIFactory2> factory;
    winrt::check_hresult(adapter_->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));

    DetectTearingSupport();
    // `automatic` prefers tearing wherever DXGI offers it, because that is the
    // mode in which the rational output clock decides when a frame is handed
    // over: under Present(1) the swap chain blocks until a vblank, so any target
    // rate that is not a divisor of the refresh rate beats against it.
    //
    // Scoped honestly: the output window is layered and therefore always
    // DWM-composed, so sync interval zero does not produce literal tearing or
    // independent flip here. What it does produce is a present that returns
    // immediately, which keeps this loop on its own rational cadence instead of
    // being dragged onto vblank multiples. That is the half of the problem OSSS
    // can fix from inside an overlay.
    effective_present_mode_ =
        (requested_present_mode_ != PresentMode::vsync && tearing_supported_)
            ? PresentMode::tearing
            : PresentMode::vsync;
    const bool use_tearing = effective_present_mode_ == PresentMode::tearing;

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.Stereo = FALSE;
    description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // Three buffers in tearing mode. With two, a present issued while the
    // previous one is still being scanned out has nowhere to go and the latency
    // wait serializes on scanout; the third buffer is what lets the output clock
    // keep its own cadence instead of inheriting the display's.
    //
    // Three buffers also whenever the loop renders a slot ahead of its
    // deadline (PacingMode::queued): slot k is being shown, slot k+1 is
    // rendered and waiting for its deadline, and slot k+2 needs somewhere to
    // draw. With two buffers the render-ahead would block on the waitable
    // object every other slot and the mode would silently degrade to paced.
    const bool render_ahead = PacingModeRendersAhead(pacing_mode_);
    description.BufferCount = (use_tearing || render_ahead) ? 3 : 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (use_tearing) {
        description.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    swap_chain_flags_ = description.Flags;

    winrt::check_hresult(factory->CreateSwapChainForHwnd(
        device_.get(),
        output_window_,
        &description,
        nullptr,
        nullptr,
        swap_chain_.put()));
    winrt::check_hresult(swap_chain_->QueryInterface(
        __uuidof(IDXGISwapChain2),
        swap_chain_2_.put_void()));
    // Maximum frame latency is what bounds how far the loop can run ahead of
    // the display, and therefore what it does to the frame-time distribution:
    //
    //   1  (paced, unpaced) the waitable object only signals once the previous
    //      present has been consumed, so a frame can never sit queued behind
    //      another and add a hidden slot of latency. The cost is that every
    //      slot's render is on the critical path -- a flow pass that overruns
    //      its deadline is a slot missed, not a slot delayed.
    //   2  (queued) one present may be outstanding while the next is rendered,
    //      so slot k+1 is drawn while slot k waits to be shown and its own
    //      deadline costs only a Present(). The frame-time distribution loses
    //      the render-time spikes and gains exactly one target slot of latency,
    //      taken on the media side by FrameSelector's lookahead rather than as
    //      an early present -- frames are still handed over at their deadlines.
    maximum_frame_latency_ = PacingModeMaximumFrameLatency(pacing_mode_);
    winrt::check_hresult(swap_chain_2_->SetMaximumFrameLatency(maximum_frame_latency_));
    frame_latency_waitable_object_ = swap_chain_2_->GetFrameLatencyWaitableObject();
    if (!frame_latency_waitable_object_) {
        throw std::runtime_error("DXGI did not return a frame-latency waitable object.");
    }
    presentation_slot_acquired_ = false;
    presentation_metrics_ = {};
    last_confirmed_present_id_ = 0;
    has_confirmed_present_id_ = false;
    factory->MakeWindowAssociation(output_window_, DXGI_MWA_NO_ALT_ENTER);
    CreateRenderTarget();
}

void Renderer::CreateRenderTarget() {
    winrt::com_ptr<ID3D11Texture2D> back_buffer;
    winrt::check_hresult(swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), back_buffer.put_void()));
    winrt::check_hresult(device_->CreateRenderTargetView(back_buffer.get(), nullptr, render_target_.put()));
}

void Renderer::CreatePipeline() {
    const auto vertex_bytecode = CompileShader(kVertexShaderSource, "main", "vs_5_0");
    const auto pixel_bytecode = CompileShader(kPixelShaderSource, "main", "ps_5_0");
    winrt::check_hresult(device_->CreateVertexShader(
        vertex_bytecode->GetBufferPointer(),
        vertex_bytecode->GetBufferSize(),
        nullptr,
        vertex_shader_.put()));
    winrt::check_hresult(device_->CreatePixelShader(
        pixel_bytecode->GetBufferPointer(),
        pixel_bytecode->GetBufferSize(),
        nullptr,
        blend_pixel_shader_.put()));

    D3D11_SAMPLER_DESC sampler_description{};
    sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_description.MinLOD = 0.0F;
    sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
    winrt::check_hresult(device_->CreateSamplerState(&sampler_description, sampler_.put()));

    D3D11_BUFFER_DESC constant_description{};
    constant_description.ByteWidth = 16;
    constant_description.Usage = D3D11_USAGE_DEFAULT;
    constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    winrt::check_hresult(device_->CreateBuffer(
        &constant_description,
        nullptr,
        interpolation_constants_.put()));
}

void Renderer::CreateHistoryTextures(const D3D11_TEXTURE2D_DESC& source_description) {
    frame_history_.clear();
    recycled_history_frames_.clear();
    previous_view_ = nullptr;
    current_view_ = nullptr;
    previous_frame_ = nullptr;
    current_frame_ = nullptr;
    source_width_ = source_description.Width;
    source_height_ = source_description.Height;
    source_format_ = source_description.Format;
    active_previous_sequence_ = 0;
    active_current_sequence_ = 0;
    has_current_frame_ = false;
    has_frame_pair_ = false;
    motion_ready_ = false;
    if (motion_interpolator_) {
        motion_interpolator_->Reset();
    }
}

const Renderer::HistoryFrame* Renderer::FindHistoryFrame(
    const std::uint64_t unique_sequence) const noexcept {
    const auto iterator = std::find_if(
        frame_history_.begin(),
        frame_history_.end(),
        [unique_sequence](const HistoryFrame& frame) {
            return frame.unique_sequence == unique_sequence;
        });
    return iterator == frame_history_.end() ? nullptr : &*iterator;
}

void Renderer::RefreshPresentationStatistics() noexcept {
    if (!swap_chain_) {
        return;
    }

    DXGI_FRAME_STATISTICS statistics{};
    const HRESULT result = swap_chain_->GetFrameStatistics(&statistics);
    if (result == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
        ++presentation_metrics_.statistics_disjoint;
        has_confirmed_present_id_ = false;
        return;
    }
    if (FAILED(result)) {
        return;
    }

    presentation_metrics_.statistics_available = true;
    presentation_metrics_.present_refresh_count = statistics.PresentRefreshCount;
    presentation_metrics_.sync_refresh_count = statistics.SyncRefreshCount;
    presentation_metrics_.sync_qpc_time = statistics.SyncQPCTime;

    // SyncQPCTime is the QPC value of the vblank that the reported present
    // scanned out on -- the only handle this process has on the display's own
    // clock. Map it into steady_clock so the output clock can phase-align to a
    // real vblank rather than to whatever instant the process happened to start.
    //
    // The mapping is anchored once and reused. Re-anchoring per sample would
    // fold the caller's scheduling jitter into the answer, which is the exact
    // quantity the alignment is trying to remove. Both clocks are QPC derived on
    // every supported Windows build, so a single anchor does not drift.
    if (statistics.SyncQPCTime.QuadPart > 0) {
        static const struct QpcAnchor {
            LONGLONG qpc = 0;
            LONGLONG frequency = 1;
            std::chrono::steady_clock::time_point steady{};
        } anchor = [] {
            QpcAnchor value{};
            LARGE_INTEGER frequency{};
            LARGE_INTEGER counter{};
            if (QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&counter) &&
                frequency.QuadPart > 0) {
                value.frequency = frequency.QuadPart;
                value.qpc = counter.QuadPart;
            }
            value.steady = std::chrono::steady_clock::now();
            return value;
        }();

        if (anchor.qpc != 0) {
            const long long ticks = statistics.SyncQPCTime.QuadPart - anchor.qpc;
            // Scale in two steps so a large tick count cannot overflow before
            // the division: QPC is typically 10 MHz, so seconds-worth of ticks
            // multiplied by a nanosecond numerator would wrap a 64-bit value
            // after only a few minutes of uptime.
            const long long whole_seconds = ticks / anchor.frequency;
            const long long remainder = ticks % anchor.frequency;
            const auto offset = std::chrono::seconds(whole_seconds) +
                std::chrono::nanoseconds(
                    (remainder * 1'000'000'000LL) / anchor.frequency);
            last_vblank_ = anchor.steady +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(offset);
        }
    }
    if (has_confirmed_present_id_ && statistics.PresentCount >= last_confirmed_present_id_) {
        presentation_metrics_.confirmed +=
            static_cast<std::uint64_t>(statistics.PresentCount - last_confirmed_present_id_);
    }
    last_confirmed_present_id_ = statistics.PresentCount;
    has_confirmed_present_id_ = true;
}

void Renderer::Resize(const UINT width, const UINT height) {
    if (!swap_chain_ || width == 0 || height == 0 ||
        (width == output_width_ && height == output_height_)) {
        return;
    }

    context_->OMSetRenderTargets(0, nullptr, nullptr);
    render_target_ = nullptr;
    winrt::check_hresult(swap_chain_->ResizeBuffers(
        0,
        width,
        height,
        DXGI_FORMAT_UNKNOWN,
        swap_chain_flags_));
    presentation_slot_acquired_ = false;
    output_width_ = width;
    output_height_ = height;
    CreateRenderTarget();
}

void Renderer::ReleaseWindow() noexcept {
    if (frame_latency_waitable_object_) {
        CloseHandle(frame_latency_waitable_object_);
        frame_latency_waitable_object_ = nullptr;
    }
    swap_chain_2_ = nullptr;
    if (output_window_) {
        if (toggle_hotkey_registered_) {
            UnregisterHotKey(output_window_, kToggleHotkeyId);
            toggle_hotkey_registered_ = false;
        }
        if (stop_hotkey_registered_) {
            UnregisterHotKey(output_window_, kQuitHotkeyId);
            stop_hotkey_registered_ = false;
        }
        if (IsWindow(output_window_)) {
            DestroyWindow(output_window_);
        }
        output_window_ = nullptr;
    }
    if (window_class_registered_) {
        UnregisterClassW(kWindowClassName, GetModuleHandleW(nullptr));
        window_class_registered_ = false;
    }
}

} // namespace osss
