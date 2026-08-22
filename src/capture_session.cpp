#include "capture_session.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <stdexcept>

namespace osss {
namespace {

constexpr char kSignatureShaderSource[] = R"(
Texture2D<float4> source_frame : register(t0);
RWStructuredBuffer<uint> signature_output : register(u0);

cbuffer SignatureConstants : register(b0) {
    uint source_width;
    uint source_height;
    uint2 signature_padding;
};

[numthreads(16, 16, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
    const uint2 cell = dispatch_id.xy;
    const uint2 cell_min = uint2(
        (cell.x * source_width) / 16,
        (cell.y * source_height) / 16);
    const uint2 cell_max = uint2(
        max(cell_min.x + 1, ((cell.x + 1) * source_width) / 16),
        max(cell_min.y + 1, ((cell.y + 1) * source_height) / 16));
    const uint2 span = max(uint2(1, 1), cell_max - cell_min);

    // This signature decides whether a captured frame is an exact duplicate of
    // the previous one, so a false match silently discards a real source frame.
    // Four point samples per cell covered ~0.2% of the surface, which merged
    // frames that differed only by sub-pixel motion: a 120 FPS source classified
    // ~15% of genuinely distinct frames as duplicates, starving the scheduler
    // and widening interpolation endpoint gaps. Hash every pixel in the cell
    // instead, with a stride that only engages on very large surfaces so the
    // per-frame cost stays bounded.
    const uint total_pixels = max(1u, source_width * source_height);
    const uint step = max(1u, (uint)ceil(sqrt((float)total_pixels / 1048576.0f)));

    uint hash = 2166136261u;
    for (uint y = cell_min.y; y < cell_max.y; y += step) {
        for (uint x = cell_min.x; x < cell_max.x; x += step) {
            const float4 texel = saturate(source_frame.Load(int3(x, y, 0)));
            const uint4 quantized = uint4(round(texel * 255.0f));
            const uint packed =
                quantized.x |
                (quantized.y << 8) |
                (quantized.z << 16) |
                (quantized.w << 24);
            hash = (hash ^ packed) * 16777619u;
        }
    }

    // Fold the span in so a cell that changes size cannot alias an old hash.
    hash = (hash ^ (span.x * 73856093u + span.y * 19349663u)) * 16777619u;
    signature_output[cell.y * 16 + cell.x] = hash;
}
)";

winrt::com_ptr<ID3DBlob> CompileSignatureShader() {
    winrt::com_ptr<ID3DBlob> bytecode;
    winrt::com_ptr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        kSignatureShaderSource,
        sizeof(kSignatureShaderSource) - 1,
        "osss_frame_signature.hlsl",
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        bytecode.put(),
        errors.put());
    if (FAILED(result)) {
        const std::string detail = errors
            ? std::string(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize())
            : "unknown shader compiler failure";
        throw std::runtime_error("Frame signature shader compilation failed: " + detail);
    }
    return bytecode;
}

std::int64_t QueryPerformanceCounterAsHundredNanoseconds(
    const LARGE_INTEGER counter,
    const LARGE_INTEGER frequency) noexcept {
    constexpr std::int64_t units_per_second = 10'000'000;
    const std::int64_t whole_seconds = counter.QuadPart / frequency.QuadPart;
    const std::int64_t remainder = counter.QuadPart % frequency.QuadPart;
    return (whole_seconds * units_per_second) +
        static_cast<std::int64_t>(
            (static_cast<long double>(remainder) * units_per_second) /
            static_cast<long double>(frequency.QuadPart));
}

struct SignatureConstants {
    UINT width = 0;
    UINT height = 0;
    UINT padding_0 = 0;
    UINT padding_1 = 0;
};

static_assert(sizeof(SignatureConstants) % 16 == 0);

} // namespace

CaptureSession::CaptureSession(ID3D11Device* const device) {
    if (!device) {
        throw std::invalid_argument("CaptureSession requires a Direct3D 11 device.");
    }
    device_.copy_from(device);
    device_->GetImmediateContext(context_.put());
    winrt_device_ = CreateWinrtDevice(device_.get());

    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter)) {
        winrt::throw_last_error();
    }
    qpc_anchor_100ns_ = QueryPerformanceCounterAsHundredNanoseconds(counter, frequency);
    clock_anchor_ = std::chrono::steady_clock::now();

    frame_available_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!frame_available_event_) {
        winrt::throw_last_error();
    }
    try {
        CreateSignaturePipeline();
    } catch (...) {
        CloseHandle(frame_available_event_);
        frame_available_event_ = nullptr;
        throw;
    }
}

CaptureSession::~CaptureSession() {
    Stop();
    if (frame_available_event_) {
        CloseHandle(frame_available_event_);
        frame_available_event_ = nullptr;
    }
}

void CaptureSession::Start(const HWND target, const double requested_fps) {
    if (!IsWindow(target)) {
        throw std::invalid_argument("The requested capture target is not a valid window.");
    }
    if (capture_session_) {
        throw std::logic_error("CaptureSession has already been started.");
    }

    {
        std::scoped_lock lock(mutex_);
        raw_frames_.clear();
        pending_signatures_.clear();
        classified_frames_.clear();
        last_unique_signature_.reset();
        last_unique_width_ = 0;
        last_unique_height_ = 0;
        latest_information_ = {};
        classified_frame_count_ = 0;
        duplicate_frame_count_ = 0;
        dropped_frame_count_ = 0;
        error_.clear();
    }

    capture_item_ = CreateCaptureItem(target);
    frame_pool_size_ = capture_item_.Size();
    if (frame_pool_size_.Width <= 0 || frame_pool_size_.Height <= 0) {
        throw std::runtime_error("The target window has no capturable surface. Restore it and try again.");
    }

    frame_pool_ = FramePool::CreateFreeThreaded(
        winrt_device_,
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        3,
        frame_pool_size_);
    capture_session_ = frame_pool_.CreateCaptureSession(capture_item_);

    requested_update_fps_.reset();
    if (std::isfinite(requested_fps) && requested_fps > 0.0) {
        if (const auto high_rate_session = capture_session_.try_as<
                winrt::Windows::Graphics::Capture::IGraphicsCaptureSession5>()) {
            try {
                // MinUpdateInterval is a floor on the spacing between delivered
                // frames, so it caps capture at 1/interval. Requesting exactly the
                // output cadence therefore suppresses any source frame that becomes
                // available a little sooner than one full interval after the last
                // one: a 120 FPS source measured 92 FPS captured at a 120 FPS
                // request, and 119 FPS at a 240 FPS request. Interpolating from
                // those thinned pairs widens the endpoint gap and is visible as
                // lost fine detail, so ask for headroom. WGC still only delivers on
                // content change, so headroom costs nothing on a slower source; the
                // throttle stays purely as a bound on runaway ones.
                const double capture_fps = requested_fps * kCaptureRateHeadroom;
                const auto interval = std::chrono::duration_cast<
                    winrt::Windows::Foundation::TimeSpan>(
                    std::chrono::duration<double>(1.0 / capture_fps));
                high_rate_session.MinUpdateInterval(interval);
                requested_update_fps_ = capture_fps;
            } catch (const winrt::hresult_error&) {
                // Older builds retain the system capture cadence.
            }
        }
    }

    try {
        capture_session_.IsCursorCaptureEnabled(false);
    } catch (const winrt::hresult_error&) {
        // Cursor control is best effort on the Windows 10 compatibility path.
    }

    frame_arrived_token_ = frame_pool_.FrameArrived({this, &CaptureSession::OnFrameArrived});
    event_registered_ = true;
    capture_session_.StartCapture();
}

void CaptureSession::Stop() noexcept {
    try {
        if (event_registered_ && frame_pool_) {
            frame_pool_.FrameArrived(frame_arrived_token_);
            event_registered_ = false;
        }
        if (capture_session_) {
            capture_session_.Close();
            capture_session_ = nullptr;
        }
        if (frame_pool_) {
            frame_pool_.Close();
            frame_pool_ = nullptr;
        }
        capture_item_ = nullptr;
        std::scoped_lock lock(mutex_);
        raw_frames_.clear();
        pending_signatures_.clear();
        classified_frames_.clear();
    } catch (...) {
        // Destructors and shutdown paths must not surface WinRT teardown errors.
    }
}

bool CaptureSession::ConsumeLatestAfter(
    const std::uint64_t known_sequence,
    CapturedFrameInfo& information,
    const std::function<void(ID3D11Texture2D*)>& consumer) {
    bool consumed = false;
    DrainClassifiedFramesAfter(
        known_sequence,
        [&](ID3D11Texture2D* texture, const CapturedFrameInfo& frame_information) {
            consumer(texture);
            information = frame_information;
            consumed = true;
        });
    return consumed;
}

std::size_t CaptureSession::DrainClassifiedFramesAfter(
    const std::uint64_t known_sequence,
    const std::function<void(ID3D11Texture2D*, const CapturedFrameInfo&)>& consumer) {
    SubmitQueuedSignatures();
    // Signature completion is queried with DONOTFLUSH below. Flush exactly
    // once here, on the main thread, so work submitted by the callback and
    // this drain reaches the GPU even when the output is hidden or no Present
    // has happened recently. The callback must remain a capture-only path.
    context_->Flush();
    CollectCompletedSignatures();

    std::deque<ClassifiedFrame> ready;
    {
        std::scoped_lock lock(mutex_);
        ready.swap(classified_frames_);
    }

    std::size_t consumed = 0;
    for (auto& classified : ready) {
        classified.frame.information.ingest = std::chrono::steady_clock::now();
        if (classified.frame.information.sequence > known_sequence) {
            consumer(classified.frame.texture.get(), classified.frame.information);
            ++consumed;
        }
        RecycleFrameTexture(std::move(classified.frame.texture));
    }
    return consumed;
}

std::string CaptureSession::Error() const {
    std::scoped_lock lock(mutex_);
    return error_;
}

std::uint64_t CaptureSession::CapturedFrameCount() const {
    std::scoped_lock lock(mutex_);
    return latest_information_.sequence;
}

std::uint64_t CaptureSession::ClassifiedFrameCount() const {
    std::scoped_lock lock(mutex_);
    return classified_frame_count_;
}

std::uint64_t CaptureSession::DuplicateFrameCount() const {
    std::scoped_lock lock(mutex_);
    return duplicate_frame_count_;
}

std::uint64_t CaptureSession::DroppedFrameCount() const {
    std::scoped_lock lock(mutex_);
    return dropped_frame_count_;
}

std::optional<double> CaptureSession::RequestedUpdateFps() const noexcept {
    return requested_update_fps_;
}

HANDLE CaptureSession::FrameAvailableEvent() const noexcept {
    return frame_available_event_;
}

winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice CaptureSession::CreateWinrtDevice(
    ID3D11Device* const device) {
    winrt::com_ptr<IDXGIDevice> dxgi_device;
    winrt::check_hresult(device->QueryInterface(__uuidof(IDXGIDevice), dxgi_device.put_void()));

    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put()));
    return inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
}

winrt::Windows::Graphics::Capture::GraphicsCaptureItem CaptureSession::CreateCaptureItem(
    const HWND target) {
    auto interop = winrt::get_activation_factory<
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForWindow(
        target,
        winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
        winrt::put_abi(item)));
    return item;
}

void CaptureSession::OnFrameArrived(
    const FramePool& sender,
    const winrt::Windows::Foundation::IInspectable&) noexcept {
    try {
        auto frame = sender.TryGetNextFrame();
        if (!frame) {
            return;
        }

        const auto content_size = frame.ContentSize();
        const auto system_relative_time = frame.SystemRelativeTime();
        const auto surface = frame.Surface();
        const auto access = surface.as<
            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> source;
        winrt::check_hresult(access->GetInterface(
            __uuidof(ID3D11Texture2D),
            source.put_void()));

        D3D11_TEXTURE2D_DESC source_description{};
        source->GetDesc(&source_description);

        QueuedFrame queued{};
        queued.texture = AcquireFrameTexture(source_description);
        context_->CopyResource(queued.texture.get(), source.get());
        queued.information.system_relative_time_100ns = system_relative_time.count();
        queued.information.media_time = MapSystemRelativeTime(system_relative_time.count());
        queued.information.arrival = std::chrono::steady_clock::now();
        queued.information.width = source_description.Width;
        queued.information.height = source_description.Height;

        {
            std::scoped_lock lock(mutex_);
            queued.information.sequence = latest_information_.sequence + 1;
            latest_information_ = queued.information;
            if (raw_frames_.size() >= kMaximumRawQueue) {
                auto dropped_texture = std::move(raw_frames_.front().texture);
                raw_frames_.pop_front();
                if (recycled_frames_.size() < kMaximumRawQueue + kMaximumPendingSignatures) {
                    recycled_frames_.push_back(std::move(dropped_texture));
                }
                ++dropped_frame_count_;
            }
            raw_frames_.push_back(std::move(queued));
        }
        if (frame_available_event_) {
            SetEvent(frame_available_event_);
        }

        frame.Close();

        if (content_size.Width > 0 && content_size.Height > 0 &&
            (content_size.Width != frame_pool_size_.Width ||
             content_size.Height != frame_pool_size_.Height)) {
            frame_pool_size_ = content_size;
            sender.Recreate(
                winrt_device_,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                3,
                frame_pool_size_);
        }
    } catch (const winrt::hresult_error& error) {
        StoreError(winrt::to_string(error.message()));
    } catch (const std::exception& error) {
        StoreError(error.what());
    } catch (...) {
        StoreError("Unknown error in the Windows Graphics Capture callback.");
    }
}

void CaptureSession::CreateSignaturePipeline() {
    const auto bytecode = CompileSignatureShader();
    winrt::check_hresult(device_->CreateComputeShader(
        bytecode->GetBufferPointer(),
        bytecode->GetBufferSize(),
        nullptr,
        signature_shader_.put()));

    D3D11_BUFFER_DESC output_description{};
    output_description.ByteWidth = static_cast<UINT>(
        kSignatureValueCount * sizeof(std::uint32_t));
    output_description.Usage = D3D11_USAGE_DEFAULT;
    output_description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    output_description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    output_description.StructureByteStride = sizeof(std::uint32_t);
    winrt::check_hresult(device_->CreateBuffer(
        &output_description,
        nullptr,
        signature_output_.put()));
    winrt::check_hresult(device_->CreateUnorderedAccessView(
        signature_output_.get(),
        nullptr,
        signature_output_view_.put()));

    D3D11_BUFFER_DESC constants_description{};
    constants_description.ByteWidth = sizeof(SignatureConstants);
    constants_description.Usage = D3D11_USAGE_DEFAULT;
    constants_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    winrt::check_hresult(device_->CreateBuffer(
        &constants_description,
        nullptr,
        signature_constants_.put()));
}

void CaptureSession::SubmitQueuedSignatures() {
    while (true) {
        QueuedFrame frame;
        {
            std::scoped_lock lock(mutex_);
            if (raw_frames_.empty() || pending_signatures_.size() >= kMaximumPendingSignatures) {
                break;
            }
            frame = std::move(raw_frames_.front());
            raw_frames_.pop_front();
        }

        D3D11_TEXTURE2D_DESC description{};
        frame.texture->GetDesc(&description);
        winrt::com_ptr<ID3D11ShaderResourceView> source_view;
        winrt::check_hresult(device_->CreateShaderResourceView(
            frame.texture.get(),
            nullptr,
            source_view.put()));
        SignatureResources resources = AcquireSignatureResources();

        const SignatureConstants constants{
            description.Width,
            description.Height,
            0,
            0,
        };
        context_->UpdateSubresource(signature_constants_.get(), 0, nullptr, &constants, 0, 0);
        ID3D11ShaderResourceView* views[] = {source_view.get()};
        ID3D11UnorderedAccessView* outputs[] = {signature_output_view_.get()};
        ID3D11Buffer* constant_buffers[] = {signature_constants_.get()};
        context_->CSSetShader(signature_shader_.get(), nullptr, 0);
        context_->CSSetShaderResources(0, 1, views);
        context_->CSSetUnorderedAccessViews(0, 1, outputs, nullptr);
        context_->CSSetConstantBuffers(0, 1, constant_buffers);
        context_->Dispatch(1, 1, 1);

        ID3D11ShaderResourceView* empty_views[] = {nullptr};
        ID3D11UnorderedAccessView* empty_outputs[] = {nullptr};
        context_->CSSetShaderResources(0, 1, empty_views);
        context_->CSSetUnorderedAccessViews(0, 1, empty_outputs, nullptr);
        context_->CSSetShader(nullptr, nullptr, 0);
        context_->CopyResource(resources.staging.get(), signature_output_.get());
        context_->End(resources.completion.get());

        std::scoped_lock lock(mutex_);
        pending_signatures_.push_back(SignatureWork{
            std::move(frame),
            std::move(resources.staging),
            std::move(resources.completion),
        });
    }
}

void CaptureSession::CollectCompletedSignatures() {
    while (true) {
        SignatureWork work;
        {
            std::scoped_lock lock(mutex_);
            if (pending_signatures_.empty()) {
                break;
            }
            const HRESULT query_result = context_->GetData(
                pending_signatures_.front().completion.get(),
                nullptr,
                0,
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (query_result == S_FALSE) {
                break;
            }
            winrt::check_hresult(query_result);
            work = std::move(pending_signatures_.front());
            pending_signatures_.pop_front();
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT map_result = context_->Map(
            work.staging.get(),
            0,
            D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT,
            &mapped);
        if (map_result == DXGI_ERROR_WAS_STILL_DRAWING) {
            std::scoped_lock lock(mutex_);
            pending_signatures_.push_front(std::move(work));
            break;
        }
        winrt::check_hresult(map_result);

        ClassifiedFrame classified{};
        classified.frame = std::move(work.frame);
        std::memcpy(
            classified.signature.data(),
            mapped.pData,
            classified.signature.size() * sizeof(std::uint32_t));
        context_->Unmap(work.staging.get(), 0);

        bool duplicate = false;
        {
            std::scoped_lock lock(mutex_);
            const bool same_dimensions =
                last_unique_width_ == classified.frame.information.width &&
                last_unique_height_ == classified.frame.information.height;
            duplicate =
                same_dimensions && last_unique_signature_.has_value() &&
                *last_unique_signature_ == classified.signature;
            classified.frame.information.duplicate = duplicate;
            if (duplicate) {
                ++duplicate_frame_count_;
            } else {
                last_unique_signature_ = classified.signature;
                last_unique_width_ = classified.frame.information.width;
                last_unique_height_ = classified.frame.information.height;
            }
            ++classified_frame_count_;
            classified_frames_.push_back(std::move(classified));
        }

        RecycleSignatureResources(SignatureResources{
            std::move(work.staging),
            std::move(work.completion),
        });
    }
}

winrt::com_ptr<ID3D11Texture2D> CaptureSession::AcquireFrameTexture(
    const D3D11_TEXTURE2D_DESC& source_description) {
    {
        std::scoped_lock lock(mutex_);
        for (auto iterator = recycled_frames_.begin(); iterator != recycled_frames_.end(); ++iterator) {
            D3D11_TEXTURE2D_DESC candidate_description{};
            (*iterator)->GetDesc(&candidate_description);
            if (candidate_description.Width == source_description.Width &&
                candidate_description.Height == source_description.Height &&
                candidate_description.Format == source_description.Format) {
                auto texture = std::move(*iterator);
                recycled_frames_.erase(iterator);
                return texture;
            }
        }
    }

    D3D11_TEXTURE2D_DESC owned_description = source_description;
    owned_description.MipLevels = 1;
    owned_description.ArraySize = 1;
    owned_description.SampleDesc = DXGI_SAMPLE_DESC{1, 0};
    owned_description.Usage = D3D11_USAGE_DEFAULT;
    owned_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    owned_description.CPUAccessFlags = 0;
    owned_description.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(device_->CreateTexture2D(
        &owned_description,
        nullptr,
        texture.put()));
    return texture;
}

void CaptureSession::RecycleFrameTexture(winrt::com_ptr<ID3D11Texture2D> texture) noexcept {
    if (!texture) {
        return;
    }
    try {
        std::scoped_lock lock(mutex_);
        if (recycled_frames_.size() < kMaximumRawQueue + kMaximumPendingSignatures) {
            recycled_frames_.push_back(std::move(texture));
        }
    } catch (...) {
    }
}

winrt::com_ptr<ID3D11Buffer> CaptureSession::CreateSignatureReadbackBuffer() const {
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = static_cast<UINT>(
        kSignatureValueCount * sizeof(std::uint32_t));
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Buffer> buffer;
    winrt::check_hresult(device_->CreateBuffer(&description, nullptr, buffer.put()));
    return buffer;
}

CaptureSession::SignatureResources CaptureSession::AcquireSignatureResources() {
    {
        std::scoped_lock lock(mutex_);
        if (!recycled_signature_resources_.empty()) {
            SignatureResources resources = std::move(recycled_signature_resources_.back());
            recycled_signature_resources_.pop_back();
            return resources;
        }
    }

    SignatureResources resources{};
    resources.staging = CreateSignatureReadbackBuffer();
    D3D11_QUERY_DESC query_description{};
    query_description.Query = D3D11_QUERY_EVENT;
    winrt::check_hresult(device_->CreateQuery(
        &query_description,
        resources.completion.put()));
    return resources;
}

void CaptureSession::RecycleSignatureResources(SignatureResources resources) noexcept {
    if (!resources.staging || !resources.completion) {
        return;
    }
    try {
        std::scoped_lock lock(mutex_);
        if (recycled_signature_resources_.size() < kMaximumPendingSignatures) {
            recycled_signature_resources_.push_back(std::move(resources));
        }
    } catch (...) {
    }
}

std::chrono::steady_clock::time_point CaptureSession::MapSystemRelativeTime(
    const std::int64_t system_relative_time_100ns) const noexcept {
    using HundredNanoseconds = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
    return clock_anchor_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        HundredNanoseconds{system_relative_time_100ns - qpc_anchor_100ns_});
}

void CaptureSession::StoreError(const std::string& message) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (error_.empty()) {
            error_ = message;
        }
    } catch (...) {
    }
}

} // namespace osss
