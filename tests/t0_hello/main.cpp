// t0_hello — 测试靶子
//
// 目的：在没有游戏 exe 的情况下，验证"导入 shim DLL → 初始化 → 建 D3D12 设备 → 出帧"整条链路。
// 它故意从 xgameruntime.dll / xmem.dll / d3d12_xs.dll 取符号，而不是直接用系统 d3d12。
//
//   t0_hello.exe --smoke   不开窗，只验证 初始化 + XMem + 设备创建
//   t0_hello.exe           开窗，清屏三帧后退出
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "xbshim/trace.h"

extern "C" {
// xgameruntime.dll
HRESULT WINAPI XGameRuntimeInitialize(void);
// xmem.dll
void* __cdecl XMemAlloc(uint64_t attributes, size_t size);
void __cdecl XMemFree(void* ptr);
// d3d12_xs.dll
HRESULT WINAPI D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL min_level, REFIID riid,
                                 void** device);
}

namespace {

constexpr UINT kWidth = 960;
constexpr UINT kHeight = 540;
constexpr UINT kFrameCount = 3;

int g_failures = 0;

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        xbshim::logf("[t0] FAIL %s hr=0x%08lx", what, static_cast<unsigned long>(hr));
        ++g_failures;
    }
}

void check_bool(bool ok, const char* what) {
    if (!ok) {
        xbshim::logf("[t0] FAIL %s", what);
        ++g_failures;
    }
}

// ---------------- window ----------------

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

HWND create_window(HINSTANCE inst) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = "xbshim_t0";
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExA(0, wc.lpszClassName, "xbshim t0_hello", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           nullptr, nullptr, inst, nullptr);
}

// ---------------- gpu ----------------

struct Gpu {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12Fence* fence = nullptr;
    IDXGISwapChain1* swapchain = nullptr;
    ID3D12Resource* backbuffer = nullptr;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;
    UINT rtv_size = 0;

    void release() {
        if (backbuffer) backbuffer->Release();
        if (swapchain) swapchain->Release();
        if (fence) fence->Release();
        if (rtv_heap) rtv_heap->Release();
        if (list) list->Release();
        if (allocator) allocator->Release();
        if (queue) queue->Release();
        if (device) device->Release();
        if (fence_event) CloseHandle(fence_event);
    }
};

bool create_pipeline(Gpu& g, HWND hwnd) {
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                            reinterpret_cast<void**>(&g.device)),
          "D3D12CreateDevice");
    if (!g.device) return false;

    LUID luid = g.device->GetAdapterLuid();
    xbshim::logf("[t0] device ok, adapter luid=%08lx:%08lx, nodes=%u",
                 static_cast<unsigned long>(luid.HighPart), static_cast<unsigned long>(luid.LowPart),
                 g.device->GetNodeCount());

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(g.device->CreateCommandQueue(&qd, IID_ID3D12CommandQueue,
                                       reinterpret_cast<void**>(&g.queue)),
          "CreateCommandQueue");
    check(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator,
                                           reinterpret_cast<void**>(&g.allocator)),
          "CreateCommandAllocator");
    check(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator, nullptr,
                                      IID_ID3D12GraphicsCommandList,
                                      reinterpret_cast<void**>(&g.list)),
          "CreateCommandList");
    if (g.list) check(g.list->Close(), "CommandList::Close");

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 2;
    check(g.device->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap,
                                         reinterpret_cast<void**>(&g.rtv_heap)),
          "CreateDescriptorHeap");
    g.rtv_size = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (!g.queue) return false;

    if (!hwnd) {
        // smoke：拿到设备就够，后面的 swapchain 需要窗口
        return true;
    }

    IDXGIFactory4* factory = nullptr;
    check(CreateDXGIFactory1(IID_IDXGIFactory4, reinterpret_cast<void**>(&factory)), "CreateDXGIFactory1");
    if (!factory) return false;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = kWidth;
    sd.Height = kHeight;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    check(factory->CreateSwapChainForHwnd(g.queue, hwnd, &sd, nullptr, nullptr, &g.swapchain),
          "CreateSwapChainForHwnd");
    factory->Release();
    if (!g.swapchain) return false;

    check(g.swapchain->GetBuffer(0, IID_ID3D12Resource, reinterpret_cast<void**>(&g.backbuffer)),
          "SwapChain::GetBuffer");
    if (!g.backbuffer) return false;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateRenderTargetView(g.backbuffer, nullptr, rtv);

    check(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence,
                                reinterpret_cast<void**>(&g.fence)),
          "CreateFence");
    g.fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    return true;
}

bool render_frames(Gpu& g) {
    if (!g.list || !g.queue || !g.swapchain || !g.fence) return false;

    for (UINT i = 0; i < kFrameCount; ++i) {
        check(g.allocator->Reset(), "Allocator::Reset");
        check(g.list->Reset(g.allocator, nullptr), "List::Reset");

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g.backbuffer;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g.list->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const FLOAT color[4] = {0.05f, 0.10f, 0.18f, 1.0f};
        g.list->ClearRenderTargetView(rtv, color, 0, nullptr);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g.list->ResourceBarrier(1, &barrier);

        check(g.list->Close(), "List::Close");
        ID3D12CommandList* lists[] = {g.list};
        g.queue->ExecuteCommandLists(1, lists);
        check(g.swapchain->Present(0, 0), "Present");

        ++g.fence_value;
        check(g.queue->Signal(g.fence, g.fence_value), "Queue::Signal");
        if (g.fence->GetCompletedValue() < g.fence_value) {
            g.fence->SetEventOnCompletion(g.fence_value, g.fence_event);
            WaitForSingleObject(g.fence_event, 5000);
        }
        xbshim::logf("[t0] frame %u presented", i + 1);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool smoke = (argc > 1 && std::strcmp(argv[1], "--smoke") == 0);
    xbshim::logf("[t0] t0_hello start (smoke=%d) pid=%lu", smoke ? 1 : 0, GetCurrentProcessId());

    check(XGameRuntimeInitialize(), "XGameRuntimeInitialize");

    // XMem 语义：64KB 对齐
    void* p = XMemAlloc(1ull, 65536);
    check_bool(p != nullptr, "XMemAlloc(64KiB)");
    if (p) {
        check_bool((reinterpret_cast<uintptr_t>(p) & 0xFFFF) == 0, "XMemAlloc 64KiB alignment");
        std::memset(p, 0xAB, 65536);
        XMemFree(p);
        xbshim::logf("[t0] xmem roundtrip ok");
    }

    // smoke：只验证设备创建（不开窗，可在无桌面环境跑）
    if (smoke) {
        Gpu probe;
        const bool ok = create_pipeline(probe, nullptr);
        probe.release();
        xbshim::logf("[t0] smoke done with %d failure(s)", g_failures);
        return (ok && g_failures == 0) ? 0 : 1;
    }

    // 开窗模式：设备 + swapchain
    HINSTANCE inst = GetModuleHandleA(nullptr);
    HWND hwnd = create_window(inst);
    if (!hwnd) {
        xbshim::logf("[t0] CreateWindow failed: %lu", GetLastError());
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    Gpu g2;
    bool ok = create_pipeline(g2, hwnd) && render_frames(g2);
    g2.release();
    DestroyWindow(hwnd);
    xbshim::logf("[t0] window run %s, %d failure(s)", ok ? "ok" : "failed", g_failures);
    return (ok && g_failures == 0) ? 0 : 1;
}
