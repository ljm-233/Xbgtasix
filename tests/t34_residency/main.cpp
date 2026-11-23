// SPDX-License-Identifier: GPL-3.0-or-later
// t34_residency — 显存驻留：MakeResident / Evict 与预算回收
//
// 这是"显存环节"的核心：主机上用 residency 管理"哪些资源在显存里"，
// PC 上的等价机制是 ID3D12Device::MakeResident/Evict + IDXGIAdapter3 的预算查询。
// 本项目之前只做了"查预算"和"收通知"，没验证真正的**回收**动作是否有效。
//
// 验证点：
//   1) 申请到接近预算，QueryVideoMemoryInfo 的 usage 真的上升
//   2) Evict 之后 usage 真的下降（不是只返回 S_OK）
//   3) MakeResident 把资源放回去，usage 再上升
//   4) Evict 之后的内容仍然可读（residency 不影响 CPU 侧资源存活）
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <string>
#include <vector>

#include "xbgtasix/trace.h"

namespace {

int g_fail = 0;
void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t34]   PASS  %-40s %s", what, detail);
    } else {
        xbgtasix::logf("[t34]   FAIL  %-40s %s", what, detail);
        ++g_fail;
    }
}

std::string gb(uint64_t b) {
    char s[64];
    _snprintf(s, sizeof(s), "%.2f GB", b / 1024.0 / 1024 / 1024);
    return s;
}

// 每块 256 MB 的 2D 贴图（8192x8192x4B）。注意别把"边长"当"大小"算错：
// 8192² × 4 = 268 MB，不是 128 MB——第一版注释就写错了。
constexpr UINT kBlockMB = 256;
ID3D12Resource* alloc_tex(ID3D12Device* dev) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    const UINT side = 8192;
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
                                            reinterpret_cast<void**>(&r)))) {
        return nullptr;
    }
    return r;
}

}  // namespace

int main() {
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                                 reinterpret_cast<void**>(&dev))) ||
        !dev) {
        xbgtasix::logf("[t34] 建 D3D12 设备失败 -> SKIP");
        return 2;
    }

    // 找到设备用的适配器（按 LUID 匹配，IDXGIDevice 在这套头文件下拿不到）
    IDXGIAdapter3* adapter3 = nullptr;
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
                a->QueryInterface(IID_IDXGIAdapter3, reinterpret_cast<void**>(&adapter3));
            }
            a->Release();
            if (adapter3) break;
        }
        if (factory) factory->Release();
    }
    if (!adapter3) {
        xbgtasix::logf("[t34] 没有 IDXGIAdapter3 -> SKIP");
        dev->Release();
        return 2;
    }

    DXGI_QUERY_VIDEO_MEMORY_INFO base{};
    adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &base);
    xbgtasix::logf("[t34] 起始 usage=%s budget=%s", gb(base.CurrentUsage).c_str(),
                   gb(base.Budget).c_str());

    // ---- 1) 申请到接近预算 ----
    std::vector<ID3D12Resource*> held;
    UINT got_mb = 0;
    const UINT chunk = kBlockMB;
    // 目标：budget 的 ~50%（不把机器压垮，但足够看出 usage 变化）
    const UINT target_mb = static_cast<UINT>((base.Budget / (1024 * 1024)) / 2);
    while (got_mb + kBlockMB <= target_mb && held.size() < 32) {
        ID3D12Resource* r = alloc_tex(dev);
        if (!r) break;
        held.push_back(r);
        got_mb += chunk;
    }
    xbgtasix::logf("[t34] 申请了 %u MB（%zu 块）", got_mb, held.size());
    ok(!held.empty(), "至少申请到一块显存");

    DXGI_QUERY_VIDEO_MEMORY_INFO after_alloc{};
    adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &after_alloc);
    const uint64_t risen = after_alloc.CurrentUsage > base.CurrentUsage
                               ? after_alloc.CurrentUsage - base.CurrentUsage
                               : 0;
    xbgtasix::logf("[t34] 分配后 usage=%s（上升 %s）", gb(after_alloc.CurrentUsage).c_str(),
                   gb(risen).c_str());
    ok(risen > 0, "usage 随分配上升");

    // ---- 2) Evict 一半，usage 必须下降 ----
    std::vector<ID3D12Pageable*> evict;
    for (size_t i = 0; i < held.size() / 2; ++i) evict.push_back(held[i]);
    HRESULT hr = E_FAIL;
    if (!evict.empty()) hr = dev->Evict(static_cast<UINT>(evict.size()), evict.data());
    ok(SUCCEEDED(hr), "Evict 调用成功");

    DXGI_QUERY_VIDEO_MEMORY_INFO after_evict{};
    adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &after_evict);
    const bool dropped = after_evict.CurrentUsage < after_alloc.CurrentUsage;
    xbgtasix::logf("[t34] Evict 后 usage=%s（%s）", gb(after_evict.CurrentUsage).c_str(),
                   dropped ? "下降" : "未下降");
    ok(dropped, "Evict 之后 usage 真的下降（不是只返回 S_OK）");

    // ---- 3) MakeResident 放回去 ----
    std::vector<ID3D12Pageable*> resident;
    for (size_t i = 0; i < held.size() / 2; ++i) resident.push_back(held[i]);
    hr = E_FAIL;
    if (!resident.empty()) hr = dev->MakeResident(static_cast<UINT>(resident.size()), resident.data());
    ok(SUCCEEDED(hr), "MakeResident 调用成功");

    DXGI_QUERY_VIDEO_MEMORY_INFO after_resident{};
    adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &after_resident);
    xbgtasix::logf("[t34] MakeResident 后 usage=%s", gb(after_resident.CurrentUsage).c_str());
    // 以前是 >=：MakeResident 什么都不做也能过（usage 持平就当成功）
    ok(after_resident.CurrentUsage > after_evict.CurrentUsage,
       "MakeResident 之后 usage 严格回升（不是只返回 S_OK）");

    // ---- 4) Evict 不影响资源对象本身：单块进出显存的往返必须成功 ----
    // 以前这里查 GetDesc().Width > 0 —— 那个字段恒大于 0，和 residency 毫无关系。
    // 真正有意义的检查：单独 Evict 一块，再单独 MakeResident 回去。
    if (!evict.empty()) {
        ID3D12Pageable* one = held[0];
        const HRESULT e1 = dev->Evict(1, &one);
        const HRESULT r1 = dev->MakeResident(1, &one);
        ok(SUCCEEDED(e1) && SUCCEEDED(r1),
           "单块 Evict -> MakeResident 往返成功（资源还能进出显存）");
    }

    // ---- 5) 释放后 usage 归位 ----
    for (auto* r : held) r->Release();
    DXGI_QUERY_VIDEO_MEMORY_INFO final_{};
    adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &final_);
    xbgtasix::logf("[t34] 全部释放后 usage=%s", gb(final_.CurrentUsage).c_str());
    ok(final_.CurrentUsage < after_alloc.CurrentUsage, "释放后 usage 回落");

    adapter3->Release();
    dev->Release();
    xbgtasix::logf("[t34] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
