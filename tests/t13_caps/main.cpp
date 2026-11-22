// t13_caps — 显卡适配：枚举适配器、报告能力等级、对照本项目需求给出 PASS/FAIL
//
// 解决两个真实问题：
//   1) 笔记本/多卡机器上，D3D12CreateDevice(nullptr) 常选中核显 —— 这里显式挑"独显/显存最大"
//   2) "这块卡能不能跑 GTA6 的需求" 目前靠猜 —— 这里把需求（mesh shader / DXR / 98 槽 bindless）
//      变成可断言的检查项
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL min_level, REFIID riid,
                                 void** device);
HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void** out);
}

namespace {

// 来自 docs/shader-corpus.md 的实测需求
constexpr UINT kNeededBindlessSlots = 98;   // 语料里绑定槽的 p99
constexpr bool kNeedsMeshShader = true;     // 34.8% 是 NGG
constexpr bool kNeedsRaytracing = true;     // 64 个 BVH64

int g_fail = 0;
int g_skip = 0;

void verdict(bool ok, const char* what, const std::string& detail) {
    if (ok) {
        xbgtasix::logf("[t13]   PASS  %-34s %s", what, detail.c_str());
    } else {
        xbgtasix::logf("[t13]   FAIL  %-34s %s", what, detail.c_str());
        ++g_fail;
    }
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

struct AdapterInfo {
    std::string name;
    UINT vendor = 0, device = 0;
    SIZE_T vram = 0;
    bool software = false;
    IDXGIAdapter1* adapter = nullptr;
};

std::vector<AdapterInfo> enum_adapters() {
    std::vector<AdapterInfo> out;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void**>(&factory))) || !factory)
        return out;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        if (!a) break;
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(a->GetDesc1(&d))) {
            AdapterInfo info;
            char name[256] = {0};
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
            info.name = name;
            info.vendor = d.VendorId;
            info.device = d.DeviceId;
            info.vram = d.DedicatedVideoMemory;
            info.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
            info.adapter = a;
            out.push_back(info);
        } else {
            a->Release();
        }
    }
    factory->Release();
    return out;
}

const char* vendor_name(UINT id) {
    switch (id) {
        case 0x10DE: return "NVIDIA";
        case 0x1002: case 0x1022: return "AMD";
        case 0x8086: return "Intel";
        case 0x1414: return "Microsoft";
        default: return "其他";
    }
}

}  // namespace

int main() {
    xbgtasix::logf("[t13] 显卡适配报告");

    // ---- 1) 枚举适配器 ----
    auto adapters = enum_adapters();
    xbgtasix::logf("[t13] 适配器 %zu 个：", adapters.size());
    for (size_t i = 0; i < adapters.size(); ++i) {
        const auto& a = adapters[i];
        xbgtasix::logf("[t13]   [%zu] %-42s %-7s vendor=0x%04X dev=0x%04X VRAM=%.1f GB%s",
                       i, a.name.c_str(), vendor_name(a.vendor), a.vendor, a.device,
                       a.vram / 1024.0 / 1024 / 1024, a.software ? "  (软件/模拟)" : "");
    }
    if (adapters.empty()) {
        xbgtasix::logf("[t13] 没有可用适配器");
        return 1;
    }

    // ---- 2) 选卡：非软件、显存最大 ----
    int best = -1;
    for (size_t i = 0; i < adapters.size(); ++i) {
        if (adapters[i].software) continue;
        if (best < 0 || adapters[i].vram > adapters[best].vram) best = static_cast<int>(i);
    }
    if (best < 0) best = 0;
    xbgtasix::logf("[t13] 选择: [%d] %s（显存 %.1f GB）", best, adapters[best].name.c_str(),
                   adapters[best].vram / 1024.0 / 1024 / 1024);

    // ---- 3) 在默认适配器和选定适配器上各建一次设备，确认差异 ----
    ID3D12Device* dev_default = nullptr;
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                      reinterpret_cast<void**>(&dev_default));
    LUID luid_default{};
    if (dev_default) luid_default = dev_default->GetAdapterLuid();

    ID3D12Device* dev = nullptr;
    const HRESULT hr = D3D12CreateDevice(adapters[best].adapter, D3D_FEATURE_LEVEL_11_0,
                                         IID_ID3D12Device, reinterpret_cast<void**>(&dev));
    if (FAILED(hr) || !dev) {
        xbgtasix::logf("[t13] 在选定适配器上建设备失败 hr=0x%08lx", static_cast<unsigned long>(hr));
        ++g_fail;
        for (auto& a : adapters) if (a.adapter) a.adapter->Release();
        if (dev_default) dev_default->Release();
        return 1;
    }
    const LUID luid = dev->GetAdapterLuid();
    const bool same = luid.LowPart == luid_default.LowPart && luid.HighPart == luid_default.HighPart;
    xbgtasix::logf("[t13] 默认适配器 LUID=%08lX:%08lX  选定=%08lX:%08lX  %s",
                   static_cast<unsigned long>(luid_default.HighPart),
                   static_cast<unsigned long>(luid_default.LowPart),
                   static_cast<unsigned long>(luid.HighPart),
                   static_cast<unsigned long>(luid.LowPart),
                   same ? "（同一块卡）" : "（默认选错了卡，显式指定更好）");

    // ---- 4) 能力查询 ----
    D3D12_FEATURE_DATA_D3D12_OPTIONS opt{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 opt1{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7{};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof(opt));
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opt1, sizeof(opt1));
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5));
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7));

    xbgtasix::logf("[t13] 能力等级：");
    xbgtasix::logf("[t13]   ResourceBindingTier=%d  ResourceHeapTier=%d  WaveOps=%d",
                   static_cast<int>(opt.ResourceBindingTier), static_cast<int>(opt.ResourceHeapTier),
                   opt1.WaveOps ? 1 : 0);
    xbgtasix::logf("[t13]   RaytracingTier=%d  MeshShaderTier=%d  SamplerFeedbackTier=%d",
                   static_cast<int>(opt5.RaytracingTier), static_cast<int>(opt7.MeshShaderTier),
                   static_cast<int>(opt7.SamplerFeedbackTier));
    xbgtasix::logf("[t13]   MaxGPUVirtualAddressBitsPerResource=%u  TextureDataPlacementAlign=%u",
                   opt.MaxGPUVirtualAddressBitsPerResource, opt1.TotalLaneCount);

    // ---- 5) 对照需求 ----
    xbgtasix::logf("[t13] 对照本项目的需求（来自 21,522 条着色器语料）：");
    verdict(opt.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2,
            "描述符表容量（Tier >= 2）",
            fmt("tier=%d，需要放下 %u 个绑定槽/PSO", static_cast<int>(opt.ResourceBindingTier),
                kNeededBindlessSlots));
    verdict(opt1.WaveOps, "Wave 内建操作（wave 级着色器）", fmt("WaveOps=%d", opt1.WaveOps ? 1 : 0));
    verdict(!kNeedsRaytracing || opt5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0,
            "光追 DXR 1.0", fmt("tier=%d（需要 10/11）", static_cast<int>(opt5.RaytracingTier)));
    verdict(!kNeedsMeshShader || opt7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1,
            "Mesh Shader", fmt("tier=%d（需要 10）", static_cast<int>(opt7.MeshShaderTier)));

    // 98 个描述符能不能真建出来（不只是看 tier）
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kNeededBindlessSlots;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12DescriptorHeap* heap = nullptr;
        const HRESULT hh = dev->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap,
                                                     reinterpret_cast<void**>(&heap));
        verdict(SUCCEEDED(hh), "实际创建 98 槽 GPU 可见堆", fmt("hr=0x%08lx", static_cast<unsigned long>(hh)));
        if (heap) heap->Release();
    }

    dev->Release();
    if (dev_default) dev_default->Release();
    for (auto& a : adapters) if (a.adapter) a.adapter->Release();

    xbgtasix::logf("[t13] 结果: %d 项不满足 / 共 5 项检查", g_fail);
    return g_fail ? 1 : 0;
}
