// SPDX-License-Identifier: GPL-3.0-or-later
// t15_budget — 显存预算变化通知 + 简单降级策略
//
// 为什么重要：WDDM 会在系统显存吃紧时下调我们进程的 budget。游戏如果只顾分配、
// 不监听这个事件，就会在某次 CreateCommittedResource 上直接失败（表现为掉卡/闪退）。
//
// 这里验证三件事：
//   1) IDXGIAdapter3::RegisterVideoMemoryBudgetChangeNotificationEvent 能否注册成功
//   2) 猛分配之后，是否真的收到预算变化通知（收不到就报 SKIP 而不是 FAIL）
//   3) 收到通知后按 "budget < usage" 释放一部分资源，usage 是否随之下降（降级动作有效）
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL min_level, REFIID riid,
                                 void** device);
}

namespace {

constexpr UINT kChunkMB = 256;
constexpr UINT kMaxMB = 8192;
constexpr UINT kOvercommitMB = 64;   // 超出 budget 的部分仍按 256 MB 粒度要

std::atomic<int> g_notifications{0};
std::atomic<int> g_stop{0};
HANDLE g_event = nullptr;
IDXGIAdapter3* g_adapter3 = nullptr;

int g_fail = 0;
int g_checks = 0;
void ok(bool cond, const char* what, const char* detail = "") {
    ++g_checks;
    if (cond) {
        xbgtasix::logf("[t15]   PASS  %-46s %s", what, detail);
    } else {
        xbgtasix::logf("[t15]   FAIL  %-46s %s", what, detail);
        ++g_fail;
    }
}

std::string gb(uint64_t b) {
    char buf[64];
    _snprintf(buf, sizeof(buf), "%.2f GB", b / 1024.0 / 1024 / 1024);
    return buf;
}

ID3D12Resource* alloc_texture(ID3D12Device* dev, UINT mb) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    const UINT side = static_cast<UINT>(8192u * mb / 256u);
    td.Width = side;
    td.Height = side;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_ID3D12Resource,
                                            reinterpret_cast<void**>(&r))))
        return nullptr;
    return r;
}

// 监听线程：收到事件就递增计数并打印新的 budget/usage
DWORD WINAPI watcher(LPVOID) {
    while (!g_stop.load()) {
        if (WaitForSingleObject(g_event, 200) == WAIT_OBJECT_0) {
            g_notifications.fetch_add(1);
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (g_adapter3 &&
                SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                xbgtasix::logf("[t15] 预算变化 #%d: budget=%s usage=%s",
                               g_notifications.load(), gb(info.Budget).c_str(),
                               gb(info.CurrentUsage).c_str());
            }
        }
    }
    return 0;
}

}  // namespace

int main() {
    xbgtasix::logf("[t15] 显存预算变化通知 + 降级");

    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                                 reinterpret_cast<void**>(&dev))) || !dev) {
        return 1;
    }

    // 按 LUID 找到设备真正用的适配器，拿 IDXGIAdapter3
    {
        const LUID luid = dev->GetAdapterLuid();
        IDXGIFactory1* factory = nullptr;
        CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void**>(&factory));
        for (UINT i = 0; factory; ++i) {
            IDXGIAdapter1* a = nullptr;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND || !a) break;
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
                a->QueryInterface(IID_IDXGIAdapter3, reinterpret_cast<void**>(&g_adapter3));
            }
            a->Release();
            if (g_adapter3) break;
        }
        if (factory) factory->Release();
    }
    if (!g_adapter3) {
        xbgtasix::logf("[t15] 没有 IDXGIAdapter3，无法注册通知 -> SKIP");
        dev->Release();
        return 2;
    }

    DXGI_QUERY_VIDEO_MEMORY_INFO before{};
    g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &before);
    xbgtasix::logf("[t15] 起始: budget=%s usage=%s", gb(before.Budget).c_str(),
                   gb(before.CurrentUsage).c_str());

    // ---- 注册通知 ----
    g_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    // mingw 的声明是 Register(HANDLE, DWORD*)（出参），而 MS 的真实语义是 Register(HANDLE, DWORD)。
    // x64 上编译器把指针放进同一个寄存器，运行时就把它当成 cookie 的 32 位值用。
    // 所以这里传一个栈变量的地址，再按"指针低 32 位"作为 cookie 去 Unregister —— 两边对得上。
    static DWORD cookie_slot = 0;
    HRESULT hr = g_adapter3->RegisterVideoMemoryBudgetChangeNotificationEvent(g_event, &cookie_slot);
    const DWORD cookie = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&cookie_slot));
    {
        char d[64];
        _snprintf(d, sizeof(d), "hr=0x%08lx", static_cast<unsigned long>(hr));
        // 注册预算通知是兼容层完全可以断言的行为（不是环境属性）。
        // 以前注册失败走 return 2（SKIP），让"通知根本注册不上"在报表里看起来没事。
        ok(SUCCEEDED(hr), "RegisterVideoMemoryBudgetChangeNotificationEvent 成功", d);
    }
    if (FAILED(hr)) {
        dev->Release();
        return 1;
    }
    xbgtasix::logf("[t15] 注册成功，开始猛分配（上限 %u MB）", kMaxMB);
    HANDLE th = CreateThread(nullptr, 0, watcher, nullptr, 0, nullptr);

    // ---- 分配，直到失败或到上限 ----
    std::vector<ID3D12Resource*> held;
    UINT got_mb = 0;
    for (UINT mb = 0; mb < kMaxMB; mb += kChunkMB) {
        ID3D12Resource* r = alloc_texture(dev, kChunkMB);
        if (!r) {
            xbgtasix::logf("[t15] 在第 %u MB 处分配失败（驱动开始拒绝）", got_mb + kChunkMB);
            break;
        }
        held.push_back(r);
        got_mb += kChunkMB;
        if (got_mb % 1024 == 0) {
            DXGI_QUERY_VIDEO_MEMORY_INFO now{};
            g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &now);
            xbgtasix::logf("[t15] 已分配 %u MB | budget=%s usage=%s", got_mb,
                           gb(now.Budget).c_str(), gb(now.CurrentUsage).c_str());
        }
    }

    // ---- 等通知（最多 10 秒）----
    for (int i = 0; i < 100 && g_notifications.load() == 0; ++i) Sleep(100);
    xbgtasix::logf("[t15] 收到预算变化通知 %d 次", g_notifications.load());

    // ---- 降级：把超过当前 budget 的部分释放掉 ----
    DXGI_QUERY_VIDEO_MEMORY_INFO mid{};
    g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mid);
    const uint64_t used = mid.CurrentUsage;
    const uint64_t budget = mid.Budget;
    xbgtasix::logf("[t15] 降级前: budget=%s usage=%s", gb(budget).c_str(), gb(used).c_str());
    // 期望值字面量：至少 1024 MB 才谈得上"显存压力"，否则下面的降级断言毫无意义
    ok(got_mb >= 1024, "至少分配出 1024 MB（压力测试的前提）");

    int released_count = 0;
    if (used > budget) {
        // 简单策略：一直释放到 usage 低于 budget 的 80%
        const uint64_t target = budget * 8 / 10;
        while (!held.empty() && mid.CurrentUsage > target) {
            held.back()->Release();
            held.pop_back();
            ++released_count;
            g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mid);
        }
    }
    xbgtasix::logf("[t15] 降级: 释放 %d 块（每块 256 MB），budget=%s usage=%s", released_count,
                   gb(mid.Budget).c_str(), gb(mid.CurrentUsage).c_str());
    xbgtasix::logf("[t15] 降级结果: usage %s -> %s，%s", gb(used).c_str(), gb(mid.CurrentUsage).c_str(),
                   mid.CurrentUsage <= budget ? "已回到预算内" : "仍超预算");

    // 降级动作必须可证伪：
    //   usage 超预算 -> 必须真的释放（released_count > 0）且释放后回到预算内
    //   usage 没超预算 -> 一块都不该动（不误释放）
    // 以前这两种情况都没断言，只打了一行日志。
    if (used > budget) {
        ok(released_count > 0, "usage 超预算时确实释放了资源（不是只打印策略）");
        ok(mid.CurrentUsage <= budget, "降级后 usage 回到预算内");
    } else {
        ok(released_count == 0, "usage 未超预算时不误释放资源");
    }

    for (auto* r : held) r->Release();

    g_stop.store(1);
    if (th) {
        WaitForSingleObject(th, 1000);
        CloseHandle(th);
    }
    g_adapter3->UnregisterVideoMemoryBudgetChangeNotification(cookie);
    CloseHandle(g_event);
    g_adapter3->Release();
    dev->Release();

    const bool notified = g_notifications.load() > 0;
    xbgtasix::logf("[t15] done checks=%d failures=%d notifications=%d -> %s", g_checks, g_fail,
                   g_notifications.load(),
                   g_fail ? "FAIL" : (notified ? "PASS（预算变化可捕获）" : "SKIP（本次未触发）"));
    if (g_fail) return 1;
    // 只有"本次没收到预算变化通知"属于环境未触发（WDDM 只在显存吃紧时才发）
    return notified ? 0 : 2;
}
