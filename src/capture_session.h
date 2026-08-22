#pragma once

#include <windows.h>

#include <d3d11.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <chrono>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace osss {

struct CapturedFrameInfo {
    std::uint64_t sequence = 0;
    std::int64_t system_relative_time_100ns = 0;
    std::chrono::steady_clock::time_point media_time{};
    std::chrono::steady_clock::time_point arrival{};
    std::chrono::steady_clock::time_point ingest{};
    UINT width = 0;
    UINT height = 0;
    bool duplicate = false;
};

class CaptureSession {
public:
    explicit CaptureSession(ID3D11Device* device);
    ~CaptureSession();

    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    void Start(HWND target, double requested_fps = 0.0);
    void Stop() noexcept;

    bool ConsumeLatestAfter(
        std::uint64_t known_sequence,
        CapturedFrameInfo& information,
        const std::function<void(ID3D11Texture2D*)>& consumer);
    std::size_t DrainClassifiedFramesAfter(
        std::uint64_t known_sequence,
        const std::function<void(ID3D11Texture2D*, const CapturedFrameInfo&)>& consumer);

    [[nodiscard]] std::string Error() const;
    [[nodiscard]] std::uint64_t CapturedFrameCount() const;
    [[nodiscard]] std::uint64_t ClassifiedFrameCount() const;
    [[nodiscard]] std::uint64_t DuplicateFrameCount() const;
    [[nodiscard]] std::uint64_t DroppedFrameCount() const;
    [[nodiscard]] std::optional<double> RequestedUpdateFps() const noexcept;
    [[nodiscard]] HANDLE FrameAvailableEvent() const noexcept;

private:
    using FramePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;

    static winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice CreateWinrtDevice(
        ID3D11Device* device);
    static winrt::Windows::Graphics::Capture::GraphicsCaptureItem CreateCaptureItem(HWND target);

    static constexpr std::size_t kSignatureWidth = 16;
    static constexpr std::size_t kSignatureHeight = 16;
    static constexpr std::size_t kSignatureValueCount = kSignatureWidth * kSignatureHeight;
    static constexpr std::size_t kMaximumRawQueue = 8;
    static constexpr std::size_t kMaximumPendingSignatures = 12;
    // Capture is requested this much faster than the output target so the
    // MinUpdateInterval throttle cannot suppress source frames the scheduler
    // needs. See the measurement in CaptureSession::Start.
    static constexpr double kCaptureRateHeadroom = 2.0;


    struct QueuedFrame {
        winrt::com_ptr<ID3D11Texture2D> texture;
        CapturedFrameInfo information{};
    };

    struct SignatureWork {
        QueuedFrame frame;
        winrt::com_ptr<ID3D11Buffer> staging;
        winrt::com_ptr<ID3D11Query> completion;
    };

    struct ClassifiedFrame {
        QueuedFrame frame;
        std::array<std::uint32_t, kSignatureValueCount> signature{};
    };

    struct SignatureResources {
        winrt::com_ptr<ID3D11Buffer> staging;
        winrt::com_ptr<ID3D11Query> completion;
    };

    void OnFrameArrived(const FramePool& sender, const winrt::Windows::Foundation::IInspectable&) noexcept;
    void CreateSignaturePipeline();
    void SubmitQueuedSignatures();
    void CollectCompletedSignatures();
    [[nodiscard]] winrt::com_ptr<ID3D11Texture2D> AcquireFrameTexture(
        const D3D11_TEXTURE2D_DESC& source_description);
    void RecycleFrameTexture(winrt::com_ptr<ID3D11Texture2D> texture) noexcept;
    [[nodiscard]] winrt::com_ptr<ID3D11Buffer> CreateSignatureReadbackBuffer() const;
    [[nodiscard]] SignatureResources AcquireSignatureResources();
    void RecycleSignatureResources(SignatureResources resources) noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point MapSystemRelativeTime(
        std::int64_t system_relative_time_100ns) const noexcept;
    void StoreError(const std::string& message) noexcept;

    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<ID3D11ComputeShader> signature_shader_;
    winrt::com_ptr<ID3D11Buffer> signature_output_;
    winrt::com_ptr<ID3D11UnorderedAccessView> signature_output_view_;
    winrt::com_ptr<ID3D11Buffer> signature_constants_;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrt_device_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem capture_item_{nullptr};
    FramePool frame_pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession capture_session_{nullptr};
    winrt::event_token frame_arrived_token_{};
    winrt::Windows::Graphics::SizeInt32 frame_pool_size_{};

    mutable std::mutex mutex_;
    std::deque<QueuedFrame> raw_frames_;
    std::deque<SignatureWork> pending_signatures_;
    std::deque<ClassifiedFrame> classified_frames_;
    std::vector<winrt::com_ptr<ID3D11Texture2D>> recycled_frames_;
    std::vector<SignatureResources> recycled_signature_resources_;
    std::optional<std::array<std::uint32_t, kSignatureValueCount>> last_unique_signature_;
    UINT last_unique_width_ = 0;
    UINT last_unique_height_ = 0;
    CapturedFrameInfo latest_information_{};
    std::string error_;
    std::optional<double> requested_update_fps_;
    std::chrono::steady_clock::time_point clock_anchor_{};
    std::int64_t qpc_anchor_100ns_ = 0;
    HANDLE frame_available_event_ = nullptr;
    std::uint64_t classified_frame_count_ = 0;
    std::uint64_t duplicate_frame_count_ = 0;
    std::uint64_t dropped_frame_count_ = 0;
    bool event_registered_ = false;
};

} // namespace osss
