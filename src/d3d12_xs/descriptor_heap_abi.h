// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d12.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>
#include <new>

namespace xbgtasix::d3d12::detail {

struct DescriptorHeapProxy;

struct DescriptorHeapVtable {
    HRESULT (WINAPI* query_interface)(DescriptorHeapProxy*, REFIID, void**);
    ULONG (WINAPI* add_ref)(DescriptorHeapProxy*);
    ULONG (WINAPI* release)(DescriptorHeapProxy*);
    HRESULT (WINAPI* get_private_data)(DescriptorHeapProxy*, REFGUID, UINT*, void*);
    HRESULT (WINAPI* set_private_data)(DescriptorHeapProxy*, REFGUID, UINT, const void*);
    HRESULT (WINAPI* set_private_data_interface)(DescriptorHeapProxy*, REFGUID, const IUnknown*);
    HRESULT (WINAPI* set_name)(DescriptorHeapProxy*, LPCWSTR);
    HRESULT (WINAPI* get_device)(DescriptorHeapProxy*, REFIID, void**);
    D3D12_DESCRIPTOR_HEAP_DESC* (WINAPI* get_desc)(DescriptorHeapProxy*, D3D12_DESCRIPTOR_HEAP_DESC*);
    SIZE_T (WINAPI* get_cpu_start)(DescriptorHeapProxy*);
    UINT64 (WINAPI* get_gpu_start)(DescriptorHeapProxy*);
};
static_assert(offsetof(DescriptorHeapVtable, get_cpu_start) == 9 * sizeof(void*));
static_assert(offsetof(DescriptorHeapVtable, get_gpu_start) == 10 * sizeof(void*));

struct DescriptorHeapProxy {
    const DescriptorHeapVtable* vtable;
    ID3D12DescriptorHeap* pc;
    IUnknown* xbox_device;
    std::atomic<ULONG> refs{1};
    // 主机"标题内存描述符堆"语义：CPU 句柄必须是标题预留 VA 里的地址（实测游戏会断言）。
    // virtual_base 非空时，GetCPUDescriptorHandleForHeapStart 返回它而不是 PC 的真实地址。
    // 尚未做句柄翻译：用这个句柄建视图时还需要把 offset 映射回 PC 堆（见 docs/rendering.md）。
    SIZE_T virtual_cpu_base = 0;
    mutable int logged_cpu_start = 0;   // 诊断：只记一次

    static bool supports(REFIID iid) {
        return iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) ||
            iid == __uuidof(ID3D12DeviceChild) || iid == __uuidof(ID3D12Pageable) ||
            iid == __uuidof(ID3D12DescriptorHeap);
    }

    static HRESULT WINAPI query_interface(DescriptorHeapProxy* self, REFIID iid, void** out) {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!DescriptorHeapProxy::supports(iid)) {
            const auto* g = reinterpret_cast<const unsigned char*>(&iid);
            xbgtasix::logf("[d3d12_xs] 堆 QI 被拒 iid={%08X-%04X-%04X-%02X%02X-...}（游戏在找别的接口）",
                           *reinterpret_cast<const uint32_t*>(g),
                           *reinterpret_cast<const uint16_t*>(g + 4),
                           *reinterpret_cast<const uint16_t*>(g + 6), g[8], g[9]);
            return E_NOINTERFACE;
        }
        add_ref(self);
        *out = self;
        return S_OK;
    }

    static ULONG WINAPI add_ref(DescriptorHeapProxy* self) {
        return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    static ULONG WINAPI release(DescriptorHeapProxy* self) {
        const ULONG refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (refs == 0) {
            self->pc->Release();
            self->xbox_device->Release();
            delete self;
        }
        return refs;
    }

    static HRESULT WINAPI get_private_data(DescriptorHeapProxy* self, REFGUID key, UINT* size,
                                            void* data) {
        return self->pc->GetPrivateData(key, size, data);
    }

    static HRESULT WINAPI set_private_data(DescriptorHeapProxy* self, REFGUID key, UINT size,
                                            const void* data) {
        return self->pc->SetPrivateData(key, size, data);
    }

    static HRESULT WINAPI set_private_data_interface(DescriptorHeapProxy* self, REFGUID key,
                                                      const IUnknown* object) {
        return self->pc->SetPrivateDataInterface(key, object);
    }

    static HRESULT WINAPI set_name(DescriptorHeapProxy* self, LPCWSTR name) {
        return self->pc->SetName(name);
    }

    static HRESULT WINAPI get_device(DescriptorHeapProxy* self, REFIID iid, void** out) {
        return self->xbox_device->QueryInterface(iid, out);
    }

    static D3D12_DESCRIPTOR_HEAP_DESC* WINAPI get_desc(DescriptorHeapProxy* self,
                                                      D3D12_DESCRIPTOR_HEAP_DESC* out) {
        *out = self->pc->GetDesc();
        return out;
    }

    // The title expects the handles in RAX with only the heap in RCX. Desktop
    // COM writes each handle into an explicit aggregate return buffer.
    static SIZE_T WINAPI get_cpu_start(DescriptorHeapProxy* self) {
        const SIZE_T v = self->virtual_cpu_base ? self->virtual_cpu_base
                                                : self->pc->GetCPUDescriptorHandleForHeapStart().ptr;
        if (!self->logged_cpu_start) {
            self->logged_cpu_start = 1;
            xbgtasix::logf("[d3d12_xs] 堆 proxy=%p 的 GetCPUDescriptorHandleForHeapStart -> %p"
                           "（virtual_base=%p，真实=%p）",
                           self, reinterpret_cast<void*>(v),
                           reinterpret_cast<void*>(self->virtual_cpu_base),
                           reinterpret_cast<void*>(self->pc->GetCPUDescriptorHandleForHeapStart().ptr));
        }
        return v;
    }

    static UINT64 WINAPI get_gpu_start(DescriptorHeapProxy* self) {
        if (self->virtual_cpu_base) return static_cast<UINT64>(self->virtual_cpu_base);
        return self->pc->GetGPUDescriptorHandleForHeapStart().ptr;
    }
};

// ---------------------------------------------------------------------------
// 句柄翻译
//
// 主机语义下描述符 CPU 句柄就是标题预留 VA 里的地址，游戏会直接用它建视图。
// PC 的描述符堆在驱动自己的内存里，两边地址不同。所以：
//   返回给游戏的是 arena 地址（virtual_base），
//   游戏拿它来建视图时，我们要换算成 PC 堆里的真实地址：
//       真实 = pc_real_base + (virtual - virtual_base)
// ---------------------------------------------------------------------------
struct DescriptorHandleMap {
    SIZE_T virtual_base;      // 返回给游戏的 CPU 句柄基址（标题 arena 里）
    SIZE_T virtual_size;
    SIZE_T real_base;         // PC 堆的 CPU 基址
    UINT64 virtual_gpu_base;  // 返回给游戏的 GPU 句柄（同样的 arena 地址）
    UINT64 real_gpu_base;     // PC 堆的 GPU 基址
    UINT increment;           // 该堆的描述符步长（PC 上通常 32）
};

inline std::vector<DescriptorHandleMap>& handle_maps() {
    static std::vector<DescriptorHandleMap> maps;
    return maps;
}

inline D3D12_CPU_DESCRIPTOR_HANDLE translate_cpu(D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (!h.ptr) return h;
    // **Xbox 的句柄 ABI 允许直接传描述符序号**（实测游戏传 0、1），
    // PC 却要求真实地址 —— 直接透传会让 D3D12 往地址 0 写描述符（崩在 0x4）。
    // 序号判定：小于 16 MB 的值不可能是任何堆的真实地址，按"序号 x 步长"换算。
    if (h.ptr < 0x1000000) {
        for (const auto& m : handle_maps()) {
            if (!m.real_base || !m.increment) continue;
            D3D12_CPU_DESCRIPTOR_HANDLE out{};
            out.ptr = m.real_base + h.ptr * m.increment;
            return out;
        }
    }
    for (const auto& m : handle_maps()) {
        if (h.ptr >= m.virtual_base && h.ptr < m.virtual_base + m.virtual_size) {
            D3D12_CPU_DESCRIPTOR_HANDLE out{};
            out.ptr = m.real_base + (h.ptr - m.virtual_base);
            return out;
        }
    }
    return h;
}

// GPU 句柄同理：游戏会把 GPU 句柄算进根描述符表，PC 侧要换成真实堆的 GPU 地址。
inline UINT64 translate_gpu(UINT64 h) {
    if (!h) return h;
    for (const auto& m : handle_maps()) {
        if (m.virtual_gpu_base == 0) continue;
        if (h >= m.virtual_gpu_base && h < m.virtual_gpu_base + m.virtual_size) {
            return m.real_gpu_base + (h - m.virtual_gpu_base);
        }
    }
    return h;
}

inline constexpr DescriptorHeapVtable descriptor_heap_vtable{
    DescriptorHeapProxy::query_interface, DescriptorHeapProxy::add_ref, DescriptorHeapProxy::release,
    DescriptorHeapProxy::get_private_data, DescriptorHeapProxy::set_private_data,
    DescriptorHeapProxy::set_private_data_interface, DescriptorHeapProxy::set_name,
    DescriptorHeapProxy::get_device, DescriptorHeapProxy::get_desc,
    DescriptorHeapProxy::get_cpu_start, DescriptorHeapProxy::get_gpu_start,
};

// PC 上"着色器可见 CBV/SRV/UAV 堆"的硬上限（D3D12 Tier-2 = 1,000,000）。
// 实测：游戏按可用显存算堆大小，RTX 5060 上要 1,245,184 项，
// 桌面 D3D12 直接 E_INVALIDARG（0x80070057）；同一份代码在 Intel 核显上只要
// 802,816 项就创建成功了。所以这里对请求量做钳制，并**大声记录**——
// 因为钳制会让游戏以为自己有更多描述符，是已知的有损折中（见 docs/rendering.md）。
inline UINT clamp_descriptor_count(ID3D12Device* pc, const D3D12_DESCRIPTOR_HEAP_DESC* desc) {
    constexpr UINT kPcShaderVisibleMax = 1000000;   // Tier-2 上限
    UINT n = desc->NumDescriptors;
    if (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
        if (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && n > kPcShaderVisibleMax) {
            xbgtasix::logf("[d3d12_xs] 描述符堆请求 %u 项 > PC 上限 %u → 钳制（游戏以为有 %u 项）",
                           n, kPcShaderVisibleMax, n);
            n = kPcShaderVisibleMax;
        }
    }
    (void)pc;
    return n;
}

inline HRESULT create_descriptor_heap_proxy(ID3D12Device* pc, IUnknown* xbox_device,
    const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID iid, void** out) {
    if (!out) return pc->CreateDescriptorHeap(desc, iid, nullptr);
    *out = nullptr;
    if (!DescriptorHeapProxy::supports(iid)) return E_NOINTERFACE;
    if (!desc) return E_INVALIDARG;

    D3D12_DESCRIPTOR_HEAP_DESC adjusted = *desc;
    adjusted.NumDescriptors = clamp_descriptor_count(pc, desc);
    const D3D12_DESCRIPTOR_HEAP_DESC* use = &adjusted;

    void* native = nullptr;
    const HRESULT hr = pc->CreateDescriptorHeap(use, __uuidof(ID3D12DescriptorHeap), &native);
    if (FAILED(hr)) return hr;
    if (!native) return E_FAIL;

    auto* heap = static_cast<ID3D12DescriptorHeap*>(native);
    // 从 xmem 的描述符 arena 里切一块：游戏预留了 VA 并断言 CPU 句柄落在其中
    SIZE_T virtual_base = 0;
    {
        using ReserveFn = void* (__cdecl*)(size_t, size_t);
        static ReserveFn reserve = [] {
            HMODULE m = GetModuleHandleW(L"xmem.dll");
            if (!m) return static_cast<ReserveFn>(nullptr);
            return reinterpret_cast<ReserveFn>(GetProcAddress(m, "XMemReserveDescriptorArena"));
        }();
        xbgtasix::logf("[d3d12_xs] 堆创建诊断：requested=%u flags=0x%x type=%d reserve_fn=%s",
                       desc->NumDescriptors, desc->Flags, static_cast<int>(desc->Type),
                       reserve ? "有" : "无（xmem 未加载或缺导出）");
        if (reserve && (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) &&
            desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
            const UINT inc = pc->GetDescriptorHandleIncrementSize(desc->Type);
            if (void* got = reserve(static_cast<size_t>(adjusted.NumDescriptors) * inc, 0x10000)) {
                virtual_base = reinterpret_cast<SIZE_T>(got);
                xbgtasix::logf("[d3d12_xs] 描述符堆 CPU 句柄使用标题 arena：%zu 项 x %u = %zu 字节 @ %p",
                               adjusted.NumDescriptors, inc,
                               static_cast<size_t>(adjusted.NumDescriptors) * inc,
                               reinterpret_cast<void*>(virtual_base));
            }
        }
    }
    auto* proxy = new (std::nothrow) DescriptorHeapProxy{&descriptor_heap_vtable, heap, xbox_device};
    if (proxy) proxy->virtual_cpu_base = virtual_base;
    if (proxy && virtual_base) {
        const UINT inc = pc->GetDescriptorHandleIncrementSize(desc->Type);
        handle_maps().push_back(DescriptorHandleMap{
            virtual_base, static_cast<SIZE_T>(adjusted.NumDescriptors) * inc,
            heap->GetCPUDescriptorHandleForHeapStart().ptr,
            static_cast<UINT64>(virtual_base),   // GPU 句柄也用同一 arena 值
            heap->GetGPUDescriptorHandleForHeapStart().ptr, inc});
    }
    if (!proxy) {
        heap->Release();
        return E_OUTOFMEMORY;
    }
    xbox_device->AddRef();
    *out = proxy;
    return hr;
}

inline ID3D12DescriptorHeap* unwrap_descriptor_heap(ID3D12DescriptorHeap* heap) {
    if (!heap) return nullptr;
    const DescriptorHeapVtable* vtable;
    std::memcpy(&vtable, heap, sizeof(vtable));
    if (vtable != &descriptor_heap_vtable) return heap;
    return reinterpret_cast<DescriptorHeapProxy*>(heap)->pc;
}

}  // namespace xbgtasix::d3d12::detail
