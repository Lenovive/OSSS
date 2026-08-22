#include "test_animation_backends.h"

#include <d3d9.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace osss {
namespace {

using Microsoft::WRL::ComPtr;

[[noreturn]] void ThrowFailure(const char* const operation, const HRESULT result) {
    std::ostringstream message;
    message << operation << " failed (HRESULT 0x" << std::hex << std::uppercase
            << static_cast<unsigned long>(result) << ").";
    throw std::runtime_error(message.str());
}

void Check(const HRESULT result, const char* const operation) {
    if (FAILED(result)) {
        ThrowFailure(operation, result);
    }
}

void CheckFrameSize(
    const std::span<const std::uint32_t> pixels,
    const std::uint32_t width,
    const std::uint32_t height) {
    if (pixels.size() != static_cast<std::size_t>(width) * height) {
        throw std::invalid_argument("Test-animation frame dimensions do not match the backend.");
    }
}

class Direct3D9Backend final : public TestAnimationBackend {
public:
    Direct3D9Backend(const HWND window, const std::uint32_t width, const std::uint32_t height)
        : width_(width), height_(height) {
        Check(Direct3DCreate9Ex(D3D_SDK_VERSION, direct3d_.GetAddressOf()), "Direct3DCreate9Ex");

        parameters_.BackBufferWidth = width;
        parameters_.BackBufferHeight = height;
        parameters_.BackBufferFormat = D3DFMT_X8R8G8B8;
        parameters_.BackBufferCount = 2;
        parameters_.MultiSampleType = D3DMULTISAMPLE_NONE;
        parameters_.SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters_.hDeviceWindow = window;
        parameters_.Windowed = TRUE;
        parameters_.EnableAutoDepthStencil = FALSE;
        parameters_.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

        HRESULT result = direct3d_->CreateDeviceEx(
            D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL,
            window,
            D3DCREATE_HARDWARE_VERTEXPROCESSING,
            &parameters_,
            nullptr,
            device_.GetAddressOf());
        if (FAILED(result)) {
            result = direct3d_->CreateDeviceEx(
                D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL,
                window,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                &parameters_,
                nullptr,
                device_.ReleaseAndGetAddressOf());
        }
        Check(result, "IDirect3D9Ex::CreateDeviceEx");
        Check(
            device_->CreateOffscreenPlainSurface(
                width,
                height,
                D3DFMT_X8R8G8B8,
                D3DPOOL_SYSTEMMEM,
                upload_surface_.GetAddressOf(),
                nullptr),
            "IDirect3DDevice9Ex::CreateOffscreenPlainSurface");
    }

    void Present(const std::span<const std::uint32_t> pixels) override {
        CheckFrameSize(pixels, width_, height_);
        D3DLOCKED_RECT locked{};
        Check(upload_surface_->LockRect(&locked, nullptr, 0), "IDirect3DSurface9::LockRect");
        for (std::uint32_t row = 0; row < height_; ++row) {
            std::memcpy(
                static_cast<std::byte*>(locked.pBits) + static_cast<std::size_t>(row) * locked.Pitch,
                pixels.data() + static_cast<std::size_t>(row) * width_,
                static_cast<std::size_t>(width_) * sizeof(std::uint32_t));
        }
        Check(upload_surface_->UnlockRect(), "IDirect3DSurface9::UnlockRect");

        ComPtr<IDirect3DSurface9> back_buffer;
        Check(
            device_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, back_buffer.GetAddressOf()),
            "IDirect3DDevice9Ex::GetBackBuffer");
        Check(
            device_->UpdateSurface(upload_surface_.Get(), nullptr, back_buffer.Get(), nullptr),
            "IDirect3DDevice9Ex::UpdateSurface");
        Check(device_->PresentEx(nullptr, nullptr, nullptr, nullptr, 0), "IDirect3DDevice9Ex::PresentEx");
    }

    [[nodiscard]] std::wstring_view RuntimeName() const noexcept override {
        return L"Direct3D 9Ex";
    }

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    D3DPRESENT_PARAMETERS parameters_{};
    ComPtr<IDirect3D9Ex> direct3d_;
    ComPtr<IDirect3DDevice9Ex> device_;
    ComPtr<IDirect3DSurface9> upload_surface_;
};

class Direct3D10Backend final : public TestAnimationBackend {
public:
    Direct3D10Backend(const HWND window, const std::uint32_t width, const std::uint32_t height)
        : width_(width), height_(height) {
        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferDesc.Width = width;
        description.BufferDesc.Height = height;
        // DXGI 1.0-era D3D10 swap chains do not expose BGRA render-target
        // support consistently across current drivers. Use the universally
        // required RGBA format and swizzle the shared BGRA reference pixels.
        description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.OutputWindow = window;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        HRESULT result = D3D10CreateDeviceAndSwapChain(
            nullptr,
            D3D10_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D10_CREATE_DEVICE_BGRA_SUPPORT,
            D3D10_SDK_VERSION,
            &description,
            swap_chain_.GetAddressOf(),
            device_.GetAddressOf());
        if (FAILED(result)) {
            result = D3D10CreateDeviceAndSwapChain(
                nullptr,
                D3D10_DRIVER_TYPE_WARP,
                nullptr,
                D3D10_CREATE_DEVICE_BGRA_SUPPORT,
                D3D10_SDK_VERSION,
                &description,
                swap_chain_.ReleaseAndGetAddressOf(),
                device_.ReleaseAndGetAddressOf());
        }
        Check(result, "D3D10CreateDeviceAndSwapChain");
        Check(
            swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer_.GetAddressOf())),
            "IDXGISwapChain::GetBuffer (Direct3D 10)");
    }

    void Present(const std::span<const std::uint32_t> pixels) override {
        CheckFrameSize(pixels, width_, height_);
        rgba_pixels_.resize(pixels.size());
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            const std::uint32_t pixel = pixels[index];
            rgba_pixels_[index] = (pixel & 0xFF00FF00U) |
                ((pixel & 0x00FF0000U) >> 16U) |
                ((pixel & 0x000000FFU) << 16U);
        }
        device_->UpdateSubresource(
            back_buffer_.Get(),
            0,
            nullptr,
            rgba_pixels_.data(),
            width_ * sizeof(std::uint32_t),
            0);
        Check(swap_chain_->Present(0, 0), "IDXGISwapChain::Present (Direct3D 10)");
    }

    [[nodiscard]] std::wstring_view RuntimeName() const noexcept override {
        return L"Direct3D 10";
    }

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    ComPtr<ID3D10Device> device_;
    ComPtr<IDXGISwapChain> swap_chain_;
    ComPtr<ID3D10Texture2D> back_buffer_;
    std::vector<std::uint32_t> rgba_pixels_;
};

class Direct3D11Backend final : public TestAnimationBackend {
public:
    Direct3D11Backend(const HWND window, const std::uint32_t width, const std::uint32_t height)
        : width_(width), height_(height) {
        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferDesc.Width = width;
        description.BufferDesc.Height = height;
        description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.OutputWindow = window;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        constexpr std::array<D3D_FEATURE_LEVEL, 1> feature_levels{D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL created_level{};

        HRESULT result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            feature_levels.data(),
            static_cast<UINT>(feature_levels.size()),
            D3D11_SDK_VERSION,
            &description,
            swap_chain_.GetAddressOf(),
            device_.GetAddressOf(),
            &created_level,
            context_.GetAddressOf());
        if (FAILED(result)) {
            result = D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                feature_levels.data(),
                static_cast<UINT>(feature_levels.size()),
                D3D11_SDK_VERSION,
                &description,
                swap_chain_.ReleaseAndGetAddressOf(),
                device_.ReleaseAndGetAddressOf(),
                &created_level,
                context_.ReleaseAndGetAddressOf());
        }
        Check(result, "D3D11CreateDeviceAndSwapChain");
        Check(
            swap_chain_->GetBuffer(0, IID_PPV_ARGS(back_buffer_.GetAddressOf())),
            "IDXGISwapChain::GetBuffer (Direct3D 11)");
    }

    void Present(const std::span<const std::uint32_t> pixels) override {
        CheckFrameSize(pixels, width_, height_);
        context_->UpdateSubresource(
            back_buffer_.Get(),
            0,
            nullptr,
            pixels.data(),
            width_ * sizeof(std::uint32_t),
            0);
        Check(swap_chain_->Present(0, 0), "IDXGISwapChain::Present (Direct3D 11)");
    }

    [[nodiscard]] std::wstring_view RuntimeName() const noexcept override {
        return L"Direct3D 11";
    }

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swap_chain_;
    ComPtr<ID3D11Texture2D> back_buffer_;
};

class Direct3D12Backend final : public TestAnimationBackend {
public:
    Direct3D12Backend(const HWND window, const std::uint32_t width, const std::uint32_t height)
        : width_(width), height_(height) {
        Check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory_.GetAddressOf())), "CreateDXGIFactory2");

        HRESULT result = D3D12CreateDevice(
            nullptr,
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(device_.GetAddressOf()));
        if (FAILED(result)) {
            ComPtr<IDXGIAdapter> warp_adapter;
            Check(factory_->EnumWarpAdapter(IID_PPV_ARGS(warp_adapter.GetAddressOf())), "EnumWarpAdapter");
            result = D3D12CreateDevice(
                warp_adapter.Get(),
                D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(device_.ReleaseAndGetAddressOf()));
        }
        Check(result, "D3D12CreateDevice");

        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Check(
            device_->CreateCommandQueue(&queue_description, IID_PPV_ARGS(queue_.GetAddressOf())),
            "ID3D12Device::CreateCommandQueue");

        DXGI_SWAP_CHAIN_DESC1 swap_description{};
        swap_description.Width = width;
        swap_description.Height = height;
        swap_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swap_description.SampleDesc.Count = 1;
        swap_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_description.BufferCount = kBufferCount;
        swap_description.Scaling = DXGI_SCALING_STRETCH;
        swap_description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swap_description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> initial_swap_chain;
        Check(
            factory_->CreateSwapChainForHwnd(
                queue_.Get(),
                window,
                &swap_description,
                nullptr,
                nullptr,
                initial_swap_chain.GetAddressOf()),
            "IDXGIFactory4::CreateSwapChainForHwnd");
        Check(initial_swap_chain.As(&swap_chain_), "Query IDXGISwapChain3");
        Check(factory_->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation");
        for (UINT index = 0; index < kBufferCount; ++index) {
            Check(
                swap_chain_->GetBuffer(index, IID_PPV_ARGS(back_buffers_[index].GetAddressOf())),
                "IDXGISwapChain3::GetBuffer (Direct3D 12)");
        }

        Check(
            device_->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(command_allocator_.GetAddressOf())),
            "ID3D12Device::CreateCommandAllocator");
        Check(
            device_->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                command_allocator_.Get(),
                nullptr,
                IID_PPV_ARGS(command_list_.GetAddressOf())),
            "ID3D12Device::CreateCommandList");
        Check(command_list_->Close(), "ID3D12GraphicsCommandList::Close");

        D3D12_RESOURCE_DESC texture_description{};
        texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_description.Width = width;
        texture_description.Height = height;
        texture_description.DepthOrArraySize = 1;
        texture_description.MipLevels = 1;
        texture_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texture_description.SampleDesc.Count = 1;
        texture_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        UINT rows = 0;
        UINT64 row_bytes = 0;
        device_->GetCopyableFootprints(
            &texture_description,
            0,
            1,
            0,
            &footprint_,
            &rows,
            &row_bytes,
            &upload_size_);
        if (rows != height || row_bytes < static_cast<UINT64>(width) * sizeof(std::uint32_t)) {
            throw std::runtime_error("Direct3D 12 returned an invalid upload footprint.");
        }

        D3D12_HEAP_PROPERTIES upload_heap{};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        upload_heap.CreationNodeMask = 1;
        upload_heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC upload_description{};
        upload_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_description.Width = upload_size_;
        upload_description.Height = 1;
        upload_description.DepthOrArraySize = 1;
        upload_description.MipLevels = 1;
        upload_description.SampleDesc.Count = 1;
        upload_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Check(
            device_->CreateCommittedResource(
                &upload_heap,
                D3D12_HEAP_FLAG_NONE,
                &upload_description,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(upload_.GetAddressOf())),
            "ID3D12Device::CreateCommittedResource (upload)");
        const D3D12_RANGE no_read{0, 0};
        Check(upload_->Map(0, &no_read, reinterpret_cast<void**>(&mapped_upload_)), "ID3D12Resource::Map");

        Check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.GetAddressOf())),
            "ID3D12Device::CreateFence");
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event_) {
            ThrowFailure("CreateEventW (Direct3D 12 fence)", HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    ~Direct3D12Backend() override {
        try {
            WaitForSubmittedWork();
        } catch (const std::exception&) {
        }
        if (upload_ && mapped_upload_) {
            upload_->Unmap(0, nullptr);
            mapped_upload_ = nullptr;
        }
        if (fence_event_) {
            CloseHandle(fence_event_);
            fence_event_ = nullptr;
        }
    }

    void Present(const std::span<const std::uint32_t> pixels) override {
        CheckFrameSize(pixels, width_, height_);
        WaitForSubmittedWork();
        for (std::uint32_t row = 0; row < height_; ++row) {
            std::memcpy(
                mapped_upload_ + footprint_.Offset +
                    static_cast<std::size_t>(row) * footprint_.Footprint.RowPitch,
                pixels.data() + static_cast<std::size_t>(row) * width_,
                static_cast<std::size_t>(width_) * sizeof(std::uint32_t));
        }

        Check(command_allocator_->Reset(), "ID3D12CommandAllocator::Reset");
        Check(
            command_list_->Reset(command_allocator_.Get(), nullptr),
            "ID3D12GraphicsCommandList::Reset");
        const UINT buffer_index = swap_chain_->GetCurrentBackBufferIndex();
        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition.pResource = back_buffers_[buffer_index].Get();
        to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        command_list_->ResourceBarrier(1, &to_copy);

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = back_buffers_[buffer_index].Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload_.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint_;
        command_list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

        std::swap(to_copy.Transition.StateBefore, to_copy.Transition.StateAfter);
        command_list_->ResourceBarrier(1, &to_copy);
        Check(command_list_->Close(), "ID3D12GraphicsCommandList::Close (frame)");
        ID3D12CommandList* command_lists[]{command_list_.Get()};
        queue_->ExecuteCommandLists(1, command_lists);
        Check(swap_chain_->Present(0, 0), "IDXGISwapChain3::Present (Direct3D 12)");
        last_submitted_fence_ = next_fence_value_++;
        Check(queue_->Signal(fence_.Get(), last_submitted_fence_), "ID3D12CommandQueue::Signal");
    }

    [[nodiscard]] std::wstring_view RuntimeName() const noexcept override {
        return L"Direct3D 12";
    }

private:
    void WaitForSubmittedWork() {
        if (last_submitted_fence_ == 0 ||
            fence_->GetCompletedValue() >= last_submitted_fence_) {
            return;
        }
        Check(
            fence_->SetEventOnCompletion(last_submitted_fence_, fence_event_),
            "ID3D12Fence::SetEventOnCompletion");
        if (WaitForSingleObject(fence_event_, 5000) != WAIT_OBJECT_0) {
            throw std::runtime_error("Timed out waiting for the Direct3D 12 upload fence.");
        }
    }

    static constexpr UINT kBufferCount = 2;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_chain_;
    std::array<ComPtr<ID3D12Resource>, kBufferCount> back_buffers_;
    ComPtr<ID3D12CommandAllocator> command_allocator_;
    ComPtr<ID3D12GraphicsCommandList> command_list_;
    ComPtr<ID3D12Resource> upload_;
    ComPtr<ID3D12Fence> fence_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    UINT64 upload_size_ = 0;
    std::byte* mapped_upload_ = nullptr;
    HANDLE fence_event_ = nullptr;
    UINT64 next_fence_value_ = 1;
    UINT64 last_submitted_fence_ = 0;
};

} // namespace

std::unique_ptr<TestAnimationBackend> CreateTestAnimationBackend(
    const TestGraphicsApi api,
    const HWND window,
    const std::uint32_t width,
    const std::uint32_t height) {
    if (!window || !IsWindow(window)) {
        throw std::invalid_argument("A valid window is required for a test-animation backend.");
    }
    switch (api) {
    case TestGraphicsApi::direct3d9:
        return std::make_unique<Direct3D9Backend>(window, width, height);
    case TestGraphicsApi::direct3d10:
        return std::make_unique<Direct3D10Backend>(window, width, height);
    case TestGraphicsApi::direct3d11:
        return std::make_unique<Direct3D11Backend>(window, width, height);
    case TestGraphicsApi::direct3d12:
        return std::make_unique<Direct3D12Backend>(window, width, height);
    }
    throw std::invalid_argument("Unknown Direct3D test-animation backend.");
}

} // namespace osss
