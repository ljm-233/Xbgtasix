// SPDX-License-Identifier: GPL-3.0-or-later
// t31_swapchain — 窗口交换链：主机没有 swapchain，PC 必须有
//
// 之前 30 个测试全是离屏渲染（render target -> readback），从没走过
// CreateSwapChainForHwnd + Present。而这正是"窗口出现但黑屏"最可能的原因，
// 所以要在 exe 到手前把这条路径验掉。
//
// 测试内容：
//   1) 建一个隐藏窗口（不需要显示，Present 也能工作）
//   2) 用 D3D12 命令队列建 flip-model 交换链
//   3) 清屏 -> Present 两次，检查返回值与帧序号推进
//   4) 检查 back buffer 能拿到 RTV
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdio>

#include "xbgtasix/trace.h"

namespace {

int g_fail = 0;
void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t31]   PASS  %-36s %s", what, detail);
    } else {
        xbgtasix::logf("[t31]   FAIL  %-36s %s", what, detail);
        ++g_fail;
    }
}

// 当前会话有没有"可见桌面"（服务会话/无交互桌面时为 false）。
// 用来区分：建窗口/建交换链失败是"环境不支持"，还是"我们的逻辑坏了"。
bool has_interactive_desktop() {
    const HWINSTA ws = GetProcessWindowStation();
    if (!ws) return false;
    USEROBJECTFLAGS uof{};
    DWORD need = 0;
    if (!GetUserObjectInformationA(ws, UOI_FLAGS, &uof, sizeof(uof), &need)) return false;
    return (uof.dwFlags & WSF_VISIBLE) != 0;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcA(hwnd, msg, wp, lp);
}

}  // namespace

int main() {
    // ---- 1) 隐藏窗口 ----
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "xbgtasix_swapchain_test";
    const ATOM atom = RegisterClassExA(&wc);
    ok(atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "RegisterClassEx");

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "xbgtasix", WS_OVERLAPPEDWINDOW,
                                0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    // 有交互桌面却建不出窗口 = 逻辑失败(1)；只有无交互桌面才允许 SKIP(2)
    const bool desktop = has_interactive_desktop();
    if (!hwnd && !desktop) {
        xbgtasix::logf("[t31]   SKIP  CreateWindowEx（当前会话没有可见桌面）");
        return 2;
    }
    ok(hwnd != nullptr, "CreateWindowEx（隐藏窗口，不 ShowWindow）");
    if (!hwnd) return 1;

    // ---- 2) D3D12 设备 + 队列 ----
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                                   reinterpret_cast<void**>(&dev));
    // 以前是 `ok(...); if (!dev) return 2;` —— 自相矛盾：断言已经记了 FAIL，
    // 退出码却是 SKIP。现在只有驱动明确回 DXGI_ERROR_UNSUPPORTED 才算环境问题。
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        xbgtasix::logf("[t31]   SKIP  D3D12CreateDevice（驱动明确不支持 D3D12）");
        return 2;
    }
    ok(dev != nullptr, "D3D12CreateDevice");
    if (!dev) {
        xbgtasix::logf("[t31] 本机有可用 GPU 却建不出设备 -> FAIL（不是环境问题）");
        DestroyWindow(hwnd);
        return 1;
    }

    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, reinterpret_cast<void**>(&queue));
    ok(SUCCEEDED(hr) && queue, "CreateCommandQueue");
    if (!queue) return 1;

    // ---- 3) 交换链 ----
    IDXGIFactory2* factory = nullptr;
    hr = CreateDXGIFactory2(0, IID_IDXGIFactory2, reinterpret_cast<void**>(&factory));
    ok(SUCCEEDED(hr) && factory, "CreateDXGIFactory2");

    IDXGISwapChain1* swap = nullptr;
    if (factory) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 320;
        sd.Height = 240;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;   // flip 模型必须显式指定
        hr = factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &swap);
        ok(SUCCEEDED(hr) && swap, "CreateSwapChainForHwnd(flip-discard)",
           SUCCEEDED(hr) ? "" : "hr 见日志");
        if (FAILED(hr)) {
            xbgtasix::logf("[t31] CreateSwapChainForHwnd hr=0x%08lx", static_cast<unsigned long>(hr));
        }
    }
    if (!swap) {
        // 窗口和设备都建出来了，交换链却建不出来 = 逻辑/运行时失败（不是环境问题）
        if (factory) factory->Release();
        queue->Release();
        dev->Release();
        DestroyWindow(hwnd);
        return 1;
    }

    // ---- 4) back buffer 拿 RTV + Present ----
    if (swap) {
        // flip 模型有多个 back buffer：必须按"当前索引"渲染，
        // 一直写 buffer 0 会得到 DXGI_ERROR_DEVICE_REMOVED（实测踩过）
        ID3D12Resource* buffers[2] = {nullptr, nullptr};
        ok(SUCCEEDED(swap->GetBuffer(0, IID_ID3D12Resource,
                                     reinterpret_cast<void**>(&buffers[0]))) && buffers[0],
           "GetBuffer(0)");
        ok(SUCCEEDED(swap->GetBuffer(1, IID_ID3D12Resource,
                                     reinterpret_cast<void**>(&buffers[1]))) && buffers[1],
           "GetBuffer(1)");
        ID3D12Resource* backbuffer = buffers[0];   // 兼容后面的空指针检查

        ID3D12DescriptorHeap* rtv_heap = nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 2;
        dev->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap, reinterpret_cast<void**>(&rtv_heap));
        ok(rtv_heap != nullptr, "CreateDescriptorHeap(RTV)");

        ID3D12CommandAllocator* alloc = nullptr;
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator,
                                    reinterpret_cast<void**>(&alloc));
        ID3D12GraphicsCommandList* list = nullptr;
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                               IID_ID3D12GraphicsCommandList, reinterpret_cast<void**>(&list));
        ok(alloc && list, "CommandAllocator + CommandList");

        int presented = 0;
        // Present 前后的 back buffer 索引：以前只断言 idx < 2（常量检查，恒真）。
        // 记录第一帧 Present 前后的索引，用它证明"帧真的推进了"。
        UINT idx_before_present = 0;
        UINT idx_after_present = 0;
        if (backbuffer && rtv_heap && alloc && list) {
            const UINT rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            dev->CreateRenderTargetView(buffers[0], nullptr, rtv0);
            D3D12_CPU_DESCRIPTOR_HANDLE rtv1 = rtv0;
            rtv1.ptr += rtv_inc;
            if (buffers[1]) dev->CreateRenderTargetView(buffers[1], nullptr, rtv1);

            // 需要 fence：重置 allocator 前必须确认 GPU 已用完上一帧，
            // 否则会拿到 DXGI_ERROR_DEVICE_REMOVED（0x887A0005，实测踩过）
            ID3D12Fence* fence = nullptr;
            dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence,
                             reinterpret_cast<void**>(&fence));
            HANDLE fev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            UINT64 fv = 0;

            IDXGISwapChain3* swap3 = nullptr;
            swap->QueryInterface(IID_IDXGISwapChain3, reinterpret_cast<void**>(&swap3));

            for (int frame = 0; frame < 2; ++frame) {
                alloc->Reset();
                list->Reset(alloc, nullptr);

                // 按当前 back buffer 索引取资源与 RTV
                const UINT bi = swap3 ? swap3->GetCurrentBackBufferIndex() : 0;
                ID3D12Resource* target = buffers[bi & 1] ? buffers[bi & 1] : buffers[0];
                D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv0;
                if (bi & 1) rtv = rtv1;
                xbgtasix::logf("[t31] frame %d -> back buffer %u", frame, bi);

                // flip 模型要求每帧回到 RENDER_TARGET 状态
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = target;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = frame == 0 ? D3D12_RESOURCE_STATE_COMMON
                                                     : D3D12_RESOURCE_STATE_PRESENT;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                list->ResourceBarrier(1, &b);

                const float color[4] = {frame == 0 ? 0.1f : 0.8f, 0.2f, 0.3f, 1.0f};
                list->ClearRenderTargetView(rtv, color, 0, nullptr);

                D3D12_RESOURCE_BARRIER p2{};
                p2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                p2.Transition.pResource = target;
                p2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                p2.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                p2.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                list->ResourceBarrier(1, &p2);

                list->Close();
                ID3D12CommandList* cl[] = {list};
                queue->ExecuteCommandLists(1, cl);

                if (frame == 0) idx_before_present = bi;
                hr = swap->Present(1, 0);   // vsync
                xbgtasix::logf("[t31] frame %d Present hr=0x%08lx", frame,
                               static_cast<unsigned long>(hr));
                if (SUCCEEDED(hr)) {
                    ++presented;
                    // Present 之后索引必须已经换到另一个 back buffer
                    if (frame == 0 && swap3) idx_after_present = swap3->GetCurrentBackBufferIndex();
                } else {
                    break;
                }

                if (fence) {   // 这一帧做完再进下一帧
                    ++fv;
                    queue->Signal(fence, fv);
                    if (fence->GetCompletedValue() < fv) {
                        fence->SetEventOnCompletion(fv, fev);
                        WaitForSingleObject(fev, 5000);
                    }
                }
            }
            if (fence) fence->Release();
            if (fev) CloseHandle(fev);
            const bool have_swap3 = swap3 != nullptr;
            if (swap3) swap3->Release();   // 先记下"有没有 swap3"，下面不许再用已释放的指针

            ok(presented >= 1, "Present 至少成功一次",
               presented >= 1 ? "" : "（无显示会话时会失败）");

            // 帧序号必须真的推进：比较 Present 前后（以前只查 idx < 2，那是常量检查）
            if (have_swap3) {
                ok(idx_after_present != idx_before_present,
                   "Present 前后 back buffer 索引发生变化（帧真的推进了）",
                   "");
                xbgtasix::logf("[t31] back buffer 索引：Present 前 = %u，Present 后 = %u",
                               idx_before_present, idx_after_present);
            }
        }

        if (list) list->Release();
        if (alloc) alloc->Release();
        if (rtv_heap) rtv_heap->Release();
        for (auto* b : buffers) if (b) b->Release();
        swap->Release();
    }
    if (factory) factory->Release();
    queue->Release();
    dev->Release();
    DestroyWindow(hwnd);

    xbgtasix::logf("[t31] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
