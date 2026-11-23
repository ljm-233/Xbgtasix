// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace xbgtasix::d3d12::detail {

// Public Microsoft samples supply a token, resource count and resource array.
// The Xbox queue ABI places those members at offsets 0, 8 and 16.
struct PresentPlaneHeader {
    UINT64 token;
    UINT resource_count;
    ID3D12Resource* const* resources;
};
static_assert(offsetof(PresentPlaneHeader, resources) == 16);

template <typename T>
class PresentCom {
public:
    PresentCom() = default;
    PresentCom(const PresentCom&) = delete;
    PresentCom& operator=(const PresentCom&) = delete;
    ~PresentCom() { if (ptr_) ptr_->Release(); }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    T** out() { return &ptr_; }
private:
    T* ptr_ = nullptr;
};

class PresentWindow {
public:
    ~PresentWindow() {
        if (thread_) {
            SetEvent(stop_);
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
        }
        if (ready_) CloseHandle(ready_);
        if (stop_) CloseHandle(stop_);
    }

    HRESULT open(UINT width, UINT height) {
        width_ = static_cast<int>(width);
        height_ = static_cast<int>(height);
        if (width_ > 1280) {
            height_ = std::max(1, MulDiv(height_, 1280, width_));
            width_ = 1280;
        }
        if (height_ > 720) {
            width_ = std::max(1, MulDiv(width_, 720, height_));
            height_ = 720;
        }
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_) return HRESULT_FROM_WIN32(GetLastError());
        ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready_) return HRESULT_FROM_WIN32(GetLastError());
        thread_ = CreateThread(nullptr, 0, &run, this, 0, nullptr);
        if (!thread_) return HRESULT_FROM_WIN32(GetLastError());
        if (WaitForSingleObject(ready_, INFINITE) != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(GetLastError());
        return window_error_;
    }

    HWND get() const { return window_; }

private:
    static DWORD WINAPI run(void* context) {
        auto& self = *static_cast<PresentWindow*>(context);
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT bounds{0, 0, self.width_, self.height_};
        if (AdjustWindowRectEx(&bounds, style, FALSE, 0)) {
            self.window_ = CreateWindowExW(0, L"STATIC", L"Xbgtasix", style,
                CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        }
        self.window_error_ = self.window_ ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        SetEvent(self.ready_);
        if (!self.window_) return 0;

        ShowWindow(self.window_, SW_SHOWNOACTIVATE);
        while (MsgWaitForMultipleObjects(1, &self.stop_, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (IsWindow(self.window_)) DestroyWindow(self.window_);
        return 0;
    }

    HANDLE stop_ = nullptr;
    HANDLE ready_ = nullptr;
    HANDLE thread_ = nullptr;
    HWND window_ = nullptr;
    HRESULT window_error_ = E_FAIL;
    int width_ = 0;
    int height_ = 0;
};

class PresentState {
public:
    HRESULT initialize(ID3D12CommandQueue* queue, ID3D12Device* device,
                       const D3D12_RESOURCE_DESC& source) {
        HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(allocator_.out()));
        if (FAILED(hr)) return hr;
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.get(),
            nullptr, IID_PPV_ARGS(list_.out()));
        if (FAILED(hr)) return hr;
        hr = list_->Close();
        if (FAILED(hr)) return hr;
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.out()));
        if (FAILED(hr)) return hr;

        hr = window_.open(static_cast<UINT>(source.Width), source.Height);
        if (FAILED(hr)) return hr;
        PresentCom<IDXGIFactory2> factory;
        hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.out()));
        if (FAILED(hr)) return hr;
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(source.Width);
        desc.Height = source.Height;
        desc.Format = source.Format;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        PresentCom<IDXGISwapChain1> swap;
        hr = factory->CreateSwapChainForHwnd(queue, window_.get(), &desc, nullptr, nullptr, swap.out());
        if (FAILED(hr)) return hr;
        hr = factory->MakeWindowAssociation(window_.get(), DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(hr)) return hr;
        hr = swap->QueryInterface(IID_PPV_ARGS(chain_.out()));
        if (FAILED(hr)) return hr;
        mode_ = desc;
        return S_OK;
    }

    HRESULT present(ID3D12CommandQueue* queue, ID3D12Resource* source,
                    const D3D12_RESOURCE_DESC& desc) {
        if (!IsWindow(window_.get())) return DXGI_ERROR_INVALID_CALL;
        if (FAILED(copy_error_)) return copy_error_;
        if (desc.Width != mode_.Width || desc.Height != mode_.Height || desc.Format != mode_.Format) {
            const HRESULT hr = chain_->ResizeBuffers(mode_.BufferCount,
                static_cast<UINT>(desc.Width), desc.Height, desc.Format, 0);
            if (FAILED(hr)) return hr;
            mode_.Width = static_cast<UINT>(desc.Width);
            mode_.Height = desc.Height;
            mode_.Format = desc.Format;
        }

        PresentCom<ID3D12Resource> buffer;
        HRESULT hr = chain_->GetBuffer(chain_->GetCurrentBackBufferIndex(),
            IID_PPV_ARGS(buffer.out()));
        if (FAILED(hr)) return hr;
        hr = allocator_->Reset();
        if (FAILED(hr)) return hr;
        hr = list_->Reset(allocator_.get(), nullptr);
        if (FAILED(hr)) return hr;

        // Xbox samples hand the surface over in PRESENT (the same state value
        // as COMMON). The copy runs on the game's original queue and restores
        // both resources before DXGI takes the back buffer.
        D3D12_RESOURCE_BARRIER barriers[]{
            transition(source, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(buffer.get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        list_->ResourceBarrier(2, barriers);
        list_->CopyResource(buffer.get(), source);
        for (auto& barrier : barriers)
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list_->ResourceBarrier(2, barriers);
        hr = list_->Close();
        if (FAILED(hr)) return hr;

        ID3D12CommandList* lists[]{list_.get()};
        queue->ExecuteCommandLists(1, lists);
        hr = queue->Signal(fence_.get(), ++fence_value_);
        if (SUCCEEDED(hr)) {
            // Passing no event performs a blocking wait. The command
            // allocator and both surfaces stay valid through completion.
            hr = fence_->SetEventOnCompletion(fence_value_, nullptr);
            if (SUCCEEDED(hr) && fence_->GetCompletedValue() == UINT64_MAX)
                hr = DXGI_ERROR_DEVICE_REMOVED;
        }
        if (FAILED(hr)) {
            copy_error_ = hr;
            return hr;
        }
        return chain_->Present(1, 0);
    }

private:
    static D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER result{};
        result.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        result.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        return result;
    }

    PresentWindow window_;
    PresentCom<IDXGISwapChain3> chain_;
    PresentCom<ID3D12CommandAllocator> allocator_;
    PresentCom<ID3D12GraphicsCommandList> list_;
    PresentCom<ID3D12Fence> fence_;
    DXGI_SWAP_CHAIN_DESC1 mode_{};
    UINT64 fence_value_ = 0;
    HRESULT copy_error_ = S_OK;
};

inline bool supported_present_texture(const D3D12_RESOURCE_DESC& desc) {
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        desc.Width != 0 && desc.Width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        desc.Height != 0 && desc.Height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
        desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 &&
        (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
}

}  // namespace xbgtasix::d3d12::detail
