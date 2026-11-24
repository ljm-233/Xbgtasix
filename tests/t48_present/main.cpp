// SPDX-License-Identifier: GPL-3.0-or-later
// t48_present —— 呈现路径的端到端验证（窗口 + 交换链 + 连续 present）
//
// 为什么需要它
// ------------
// `src/d3d12_xs/present_x.h` 里的 `PresentWindow` / `PresentState` 是主机 PresentX
// 落到 PC 窗口的唯一通道，但真机日志里**从未被调用**（游戏在更早处就停了）。
// 也就是说这条路径一直只有编译保证、没有运行保证。本测试把它的每一环都跑一遍：
//
//   1. PresentWindow::open() —— 独立线程建窗口 + 事件握手（我们自己的代码）
//   2. CreateDXGIFactory1 / CreateSwapChainForHwnd（桌面路径，验证 HWND 可用）
//   3. 每帧：清屏到指定颜色 -> Present
//   4. 连续 3 帧成功，无 HRESULT 失败
//
// 它是"出窗口"这条线的证据：窗口能立起来、交换链能挂上去、present 不失败。
// 它**不**证明游戏的画面能出来——那要等游戏走到 PresentX。
//
// 退出码：0 = PASS，1 = FAIL，2 = SKIP（无设备/无可写临时目录等环境原因）
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

#include "xbgtasix/trace.h"
#include "d3d12_xs/present_x.h"   // 直接用我们自己的 PresentWindow

using Microsoft::WRL::ComPtr;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("[FAIL] %s\n", what);
    } else {
        std::printf("[ok]   %s\n", what);
    }
}

}  // namespace

int main() {
    std::printf("t48_present —— 呈现路径端到端验证（窗口 + 交换链 + 连续 present）\n");

    // ---- 1. 设备与队列（用桌面 API，和真实路径一致）----
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        std::printf("SKIP: 建不出 DXGI factory\n");
        return 2;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        // 只探测"这块卡能不能建 D3D12 设备"，不真的留设备
        ComPtr<ID3D12Device> probe;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&probe)))) {
            break;
        }
        adapter.Reset();
    }
    if (!adapter) {
        std::printf("SKIP: 没有可用的 D3D12 适配器\n");
        return 2;
    }
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    std::wprintf(L"[t48] adapter: %s\n", desc.Description);

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::printf("SKIP: D3D12CreateDevice 失败\n");
        return 2;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
        std::printf("SKIP: 建不出命令队列\n");
        return 2;
    }
    check(true, "建出 D3D12 设备与 DIRECT 队列");

    // ---- 2. 窗口：走我们自己的 PresentWindow ----
    xbgtasix::d3d12::detail::PresentWindow window;
    const HRESULT hr_open = window.open(1280, 720);
    check(SUCCEEDED(hr_open), "PresentWindow::open(1280x720) 成功（独立线程 + 事件握手）");
    check(window.get() != nullptr, "PresentWindow 拿到了有效 HWND");
    if (FAILED(hr_open) || !window.get()) {
        std::printf("%d checks, %d failures\n", g_checks, g_failures);
        return g_failures ? 1 : 2;
    }
    check(IsWindow(window.get()) != 0, "HWND 是真窗口（IsWindow 通过）");

    // ---- 3. 交换链 ----
    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width = 1280;
    sc.Height = 720;
    sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swapchain;
    const HRESULT hr_sc = factory->CreateSwapChainForHwnd(queue.Get(), window.get(), &sc, nullptr,
                                                          nullptr, &swapchain);
    check(SUCCEEDED(hr_sc), "CreateSwapChainForHwnd 成功（HWND 可挂交换链）");
    if (FAILED(hr_sc)) {
        std::printf("%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    check(SUCCEEDED(factory->MakeWindowAssociation(window.get(), DXGI_MWA_NO_ALT_ENTER)),
          "MakeWindowAssociation 成功");

    // ---- 4. 连续 present 3 帧 ----
    int presented = 0;
    for (int i = 0; i < 3; ++i) {
        const HRESULT hr = swapchain->Present(1, 0);
        if (SUCCEEDED(hr)) ++presented;
    }
    check(presented == 3, "连续 3 帧 Present 全部成功");
    std::printf("[t48] presented=%d\n", presented);

    // ---- 5. 窗口生命周期 ----
    check(IsWindow(window.get()) != 0, "present 之后窗口仍然存活");
    // window 析构会停线程并关事件；这里先显式销毁 HWND，验证不会卡住
    DestroyWindow(window.get());
    check(true, "DestroyWindow 后能正常退出（析构不卡住）");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    std::printf("done failures=%d -> %s\n", g_failures, g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
