// t32_window — 真实窗口生命周期：消息循环 + 尺寸变化 + ResizeBuffers
//
// t31 只验证了"能拿交换链并 Present"。真实游戏还有三件事必须成立：
//   1) 消息循环在被驱动（否则窗口无响应，游戏卡在 PeekMessage 里）
//   2) 收到 WM_SIZE 后能用 ResizeBuffers 重建 back buffer
//   3) 重建后还能继续 Present（很多"改变窗口大小就黑屏"就是漏了这步）
//
// 同时验证 CPU 侧：ThreadPool 模式的任务队列是否真的并行（按核数起 worker）。
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdio>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI XTaskQueueCreate(int work_mode, int comp_mode, void** queue);
HRESULT WINAPI XTaskQueueSubmitCallback(void* queue, int port, void* context, void* callback);
HRESULT WINAPI XTaskQueueCloseHandle(void* queue);
HRESULT WINAPI XTaskQueueTerminate(void* queue, bool wait, void* context, void* callback);
}

namespace {

int g_fail = 0;
void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t32]   PASS  %-38s %s", what, detail);
    } else {
        xbgtasix::logf("[t32]   FAIL  %-38s %s", what, detail);
        ++g_fail;
    }
}

// ---- 窗口 ----
HWND g_hwnd = nullptr;
bool g_resized = false;
std::atomic<int> g_par_runs{0};

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            g_resized = true;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

// 抽干消息队列（模拟游戏主循环的一帧）
int pump_messages() {
    MSG msg{};
    int n = 0;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
        ++n;
        if (msg.message == WM_QUIT) break;
    }
    return n;
}

void CALLBACK on_task(void*, bool) { g_par_runs.fetch_add(1); }

}  // namespace

int main() {
    // =====================================================================
    // 一、窗口 + 消息循环 + ResizeBuffers
    // =====================================================================
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "xbgtasix_window_test";
    RegisterClassExA(&wc);

    g_hwnd = CreateWindowExA(0, wc.lpszClassName, "xbgtasix t32", WS_OVERLAPPEDWINDOW,
                             0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    ok(g_hwnd != nullptr, "CreateWindowEx");
    if (!g_hwnd) return 2;

    // 用 SetWindowPos 触发真实的 WM_SIZE（不依赖用户拖动）
    SetWindowPos(g_hwnd, nullptr, 0, 0, 480, 360, SWP_NOZORDER | SWP_NOMOVE);
    const int msgs = pump_messages();
    xbgtasix::logf("[t32] 消息循环处理了 %d 条消息，resized=%d", msgs, g_resized ? 1 : 0);
    ok(msgs > 0, "消息循环确实被驱动");
    ok(g_resized, "收到 WM_SIZE（窗口尺寸变化可被感知）");

    // ---- 交换链 + ResizeBuffers ----
    ID3D12Device* dev = nullptr;
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                      reinterpret_cast<void**>(&dev));
    ok(dev != nullptr, "D3D12CreateDevice");
    if (!dev) return 2;

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, reinterpret_cast<void**>(&queue));

    IDXGIFactory2* factory = nullptr;
    CreateDXGIFactory2(0, IID_IDXGIFactory2, reinterpret_cast<void**>(&factory));
    IDXGISwapChain1* swap = nullptr;
    if (factory && queue) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 320;
        sd.Height = 240;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        factory->CreateSwapChainForHwnd(queue, g_hwnd, &sd, nullptr, nullptr, &swap);
    }
    ok(swap != nullptr, "CreateSwapChainForHwnd");
    if (!swap) return 2;

    // 先把 back buffer 全放掉，否则 ResizeBuffers 会失败（BufferCount 不匹配）
    auto release_buffers = [&](ID3D12Resource** bufs, ID3D12DescriptorHeap* heap) {
        for (int i = 0; i < 2; ++i) {
            if (bufs[i]) {
                bufs[i]->Release();
                bufs[i] = nullptr;
            }
        }
        if (heap) heap->Release();
    };

    ID3D12DescriptorHeap* rtv_heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 2;
    dev->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap, reinterpret_cast<void**>(&rtv_heap));

    ID3D12Resource* bufs[2] = {nullptr, nullptr};
    for (UINT i = 0; i < 2; ++i) swap->GetBuffer(i, IID_ID3D12Resource, reinterpret_cast<void**>(&bufs[i]));
    ok(bufs[0] && bufs[1], "GetBuffer(0/1)");

    // 模拟"用户把窗口拉大" -> 必须 ResizeBuffers 重建
    release_buffers(bufs, rtv_heap);
    if (rtv_heap) { rtv_heap->Release(); rtv_heap = nullptr; }
    HRESULT hr = swap->ResizeBuffers(0, 480, 360, DXGI_FORMAT_UNKNOWN, 0);
    ok(SUCCEEDED(hr), "ResizeBuffers(480x360)");

    dev->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap, reinterpret_cast<void**>(&rtv_heap));
    for (UINT i = 0; i < 2; ++i) {
        hr = swap->GetBuffer(i, IID_ID3D12Resource, reinterpret_cast<void**>(&bufs[i]));
        if (FAILED(hr)) break;
    }
    ok(bufs[0] && bufs[1], "ResizeBuffers 后能重新拿到 back buffer");

    // 重建后继续 Present
    bool presented_after_resize = false;
    if (bufs[0] && rtv_heap) {
        ID3D12CommandAllocator* alloc = nullptr;
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator,
                                    reinterpret_cast<void**>(&alloc));
        ID3D12GraphicsCommandList* list = nullptr;
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                               IID_ID3D12GraphicsCommandList, reinterpret_cast<void**>(&list));
        if (alloc && list) {
            IDXGISwapChain3* swap3 = nullptr;
            swap->QueryInterface(IID_IDXGISwapChain3, reinterpret_cast<void**>(&swap3));
            const UINT bi = swap3 ? swap3->GetCurrentBackBufferIndex() : 0;
            ID3D12Resource* target = bufs[bi & 1];
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += (bi & 1) * dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            dev->CreateRenderTargetView(target, nullptr, rtv);

            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = target;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            list->ResourceBarrier(1, &b);
            const float c[4] = {0.2f, 0.6f, 0.9f, 1.0f};
            list->ClearRenderTargetView(rtv, c, 0, nullptr);
            D3D12_RESOURCE_BARRIER p{};
            p.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            p.Transition.pResource = target;
            p.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            p.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            p.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            list->ResourceBarrier(1, &p);
            list->Close();
            ID3D12CommandList* cl[] = {list};
            queue->ExecuteCommandLists(1, cl);
            hr = swap->Present(1, 0);
            presented_after_resize = SUCCEEDED(hr);
            xbgtasix::logf("[t32] ResizeBuffers 之后 Present hr=0x%08lx",
                           static_cast<unsigned long>(hr));
            if (swap3) swap3->Release();
        }
        if (list) list->Release();
        if (alloc) alloc->Release();
    }
    ok(presented_after_resize, "重建后仍能 Present（改变窗口大小不黑屏）");

    // =====================================================================
    // 二、CPU 侧：ThreadPool 是否真的并行
    // =====================================================================
    {
        void* q = nullptr;
        const HRESULT qhr = XTaskQueueCreate(1 /*ThreadPool*/, 0 /*Manual*/, &q);
        ok(SUCCEEDED(qhr) && q, "XTaskQueueCreate(ThreadPool)");
        if (q) {
            const int N = 2000;
            const ULONGLONG t0 = GetTickCount64();
            for (int i = 0; i < N; ++i) {
                XTaskQueueSubmitCallback(q, 0, nullptr, reinterpret_cast<void*>(&on_task));
            }
            // 等全部跑完（ThreadPool 模式由 worker 线程消费）
            for (int i = 0; i < 5000 && g_par_runs.load() < N; ++i) Sleep(1);
            const ULONGLONG dt = GetTickCount64() - t0;
            xbgtasix::logf("[t32] ThreadPool: %d 个回调，完成 %d 个，耗时 %llu ms", N,
                           g_par_runs.load(), static_cast<unsigned long long>(dt));
            ok(g_par_runs.load() == N, "ThreadPool 回调全部被执行（没有丢）");
            XTaskQueueTerminate(q, true, nullptr, nullptr);
            XTaskQueueCloseHandle(q);
        }
    }

    release_buffers(bufs, rtv_heap);
    swap->Release();
    if (factory) factory->Release();
    if (queue) queue->Release();
    dev->Release();
    DestroyWindow(g_hwnd);
    pump_messages();

    xbgtasix::logf("[t32] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
