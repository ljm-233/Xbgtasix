// t14_vram — 显存预算：问驱动要额度，再实测能真分配多少
//
// 三件事：
//   1) DXGI QueryVideoMemoryInfo：驱动给的 Budget / CurrentUsage / Available（本地 + 非本地）
//   2) 实测：按 256 MB 递增分配贴图，直到失败 —— 看"账面上"和"真能拿到"差多少
//   3) D3D12_ARCHITECTURE：UMA（核显共享）还是独立显存，决定预算模型该怎么算
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL min_level, REFIID riid,
                                 void** device);
}

namespace {

constexpr UINT kChunkMB = 256;
constexpr UINT kMaxTestMB = 6144;   // 最多试 6 GB，别把机器拖死

std::string fmt_mb(uint64_t bytes) {
    char buf[64];
    _snprintf(buf, sizeof(buf), "%.1f GB", bytes / 1024.0 / 1024 / 1024);
    return buf;
}

int g_fail = 0;

ID3D12Resource* alloc_texture(ID3D12Device* dev, UINT mb) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    // 256 MB / 4 B/px = 64M 像素 -> 8192x8192
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

}  // namespace

int main() {
    xbgtasix::logf("[t14] 显存预算");

    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                                 reinterpret_cast<void**>(&dev))) || !dev) {
        xbgtasix::logf("[t14] 建设备失败");
        return 1;
    }

    // ---- 1) 架构：UMA 还是独立 ----
    D3D12_FEATURE_DATA_ARCHITECTURE arch{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch)))) {
        xbgtasix::logf("[t14] 架构: UMA=%d  缓存一致=%d", arch.UMA ? 1 : 0,
                       arch.CacheCoherentUMA ? 1 : 0);
    }

    // ---- 2) 驱动给的额度 ----
    // 不走 device->QueryInterface(IDXGIDevice)（在这套头/运行时组合下会失败），
    // 改用 DXGI 工厂枚举 + LUID 匹配，顺便拿到 Adapter3 的预算接口。
    IDXGIAdapter3* adapter3 = nullptr;
    {
        const LUID devLuid = dev->GetAdapterLuid();
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void**>(&factory))) &&
            factory) {
            for (UINT i = 0;; ++i) {
                IDXGIAdapter1* a = nullptr;
                if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
                if (!a) break;
                DXGI_ADAPTER_DESC1 d{};
                a->GetDesc1(&d);
                if (d.AdapterLuid.LowPart == devLuid.LowPart &&
                    d.AdapterLuid.HighPart == devLuid.HighPart) {
                    a->QueryInterface(IID_IDXGIAdapter3, reinterpret_cast<void**>(&adapter3));
                    xbgtasix::logf("[t14] 按 LUID 匹配到适配器（index %u）", i);
                }
                a->Release();
                if (adapter3) break;
            }
            factory->Release();
        }
        if (!adapter3) xbgtasix::logf("[t14] 没拿到 IDXGIAdapter3，只有实测数据");
    }

    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal{};
    if (adapter3 &&
        SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
        SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal))) {
        xbgtasix::logf("[t14] 本地显存: budget=%s usage=%s 可预留=%s",
                       fmt_mb(local.Budget).c_str(), fmt_mb(local.CurrentUsage).c_str(),
                       fmt_mb(local.AvailableForReservation).c_str());
        xbgtasix::logf("[t14] 非本地  : budget=%s usage=%s",
                       fmt_mb(nonlocal.Budget).c_str(), fmt_mb(nonlocal.CurrentUsage).c_str());
    }

    // ---- 3) 实测：按块递增分配，看真能拿到多少 ----
    std::vector<ID3D12Resource*> held;
    UINT got_mb = 0;
    for (UINT mb = 0; mb < kMaxTestMB; mb += kChunkMB) {
        ID3D12Resource* r = alloc_texture(dev, kChunkMB);
        if (!r) {
            xbgtasix::logf("[t14] 分配在第 %u MB 处失败", mb + kChunkMB);
            break;
        }
        held.push_back(r);
        got_mb += kChunkMB;
        if ((mb / kChunkMB) % 4 == 3) xbgtasix::logf("[t14] 已分配 %u MB", got_mb);
    }
    xbgtasix::logf("[t14] 实测可分配: %u MB（%.1f GB）", got_mb, got_mb / 1024.0);

    if (local.Budget) {
        const double ratio = 100.0 * (got_mb / 1024.0) / (local.Budget / 1024.0 / 1024.0 / 1024.0);
        xbgtasix::logf("[t14] 实测/账面 = %.1f%%", ratio);
    }
    if (got_mb == 0) {
        xbgtasix::logf("[t14] FAIL 一块都没分出去");
        ++g_fail;
    }

    for (auto* r : held) r->Release();
    if (adapter3) adapter3->Release();
    dev->Release();

    xbgtasix::logf("[t14] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
