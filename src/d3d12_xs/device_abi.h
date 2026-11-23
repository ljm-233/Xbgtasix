// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#include "xbgtasix/trace.h"

#include "descriptor_heap_abi.h"   // 句柄翻译表（描述符 CPU 句柄：arena 地址 <-> PC 堆地址）
#include "descriptor_heap_abi.h"
#include "frame_events.h"
#include "present_x.h"

namespace xbgtasix::d3d12 {

// ID3D12Device11 adds CreateSampler2 to Device10. Spell out the IID because
// some supported MinGW headers stop at ID3D12Device10.
inline constexpr GUID kDevice11 = {0x5405c344, 0xd457, 0x444e,
                                    {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};
inline constexpr std::size_t kDeviceSlots = 140;
inline constexpr std::size_t kXboxCreateCommandQueueSlot = 8;
inline constexpr std::size_t kCustomHeapPropertiesSlot = 26;
inline constexpr std::size_t kQueueOrOpenHeapSlot = 48;
inline constexpr std::size_t kXboxCreateCommandListSlot = 73;
inline constexpr GUID kObservedXboxCommandListIid = {0xc3827890, 0x4024, 0x474c,
    {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
inline constexpr GUID kObservedXboxCommandList5Iid = {0x55050859, 0x4024, 0x474c,
    {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
inline constexpr std::size_t kCommandListSlots = 600;
inline int device_version(REFIID iid) {
    if (iid == __uuidof(ID3D12Device)) return 0;
    if (iid == __uuidof(ID3D12Device1)) return 1;
    if (iid == __uuidof(ID3D12Device2)) return 2;
    if (iid == __uuidof(ID3D12Device3)) return 3;
    if (iid == __uuidof(ID3D12Device4)) return 4;
    if (iid == __uuidof(ID3D12Device5)) return 5;
    if (iid == __uuidof(ID3D12Device6)) return 6;
    if (iid == __uuidof(ID3D12Device7)) return 7;
    if (iid == __uuidof(ID3D12Device8)) return 8;
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (iid == __uuidof(ID3D12Device9)) return 9;
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (iid == __uuidof(ID3D12Device10)) return 10;
#endif
    if (iid == kDevice11) return 11;
    return -1;
}

namespace detail {

inline void* wrap_command_queue(ID3D12CommandQueue* owned_pc_ref);

using Slot = void (WINAPI*)();

struct DeviceProxy {
    const Slot* vtable;
    IUnknown* pc;
    int version;
    std::atomic<ULONG> refs{1};
    FrameEvents frame_events{};
};
static_assert(sizeof(void*) == 8 && offsetof(DeviceProxy, vtable) == 0 &&
              offsetof(DeviceProxy, pc) == 8);

inline ULONG WINAPI add_ref(DeviceProxy* self) {
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

inline ULONG WINAPI release(DeviceProxy* self) {
    const ULONG refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) {
        IUnknown* pc = self->pc;
        delete self;
        pc->Release();
    }
    return refs;
}

inline HRESULT WINAPI query_interface(DeviceProxy* self, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    const int version = device_version(iid);
    if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12Object) &&
        (version < 0 || version > self->version)) {
        // Do not expose a raw desktop interface through the Xbox proxy.
        return E_NOINTERFACE;
    }
    *out = self;
    add_ref(self);
    return S_OK;
}

inline D3D12_HEAP_PROPERTIES* WINAPI custom_heap_properties(
    D3D12_HEAP_PROPERTIES* out, DeviceProxy* self, UINT node_mask, D3D12_HEAP_TYPE heap_type) {
    // Measured Xbox entry: RCX is the hidden return buffer, RDX is the device.
    // The desktop virtual method has the same slot but receives those in reverse order.
    using PcCall = D3D12_HEAP_PROPERTIES* (WINAPI*)(IUnknown*, D3D12_HEAP_PROPERTIES*,
                                                   UINT, D3D12_HEAP_TYPE);
    const Slot* pc_vtable;
    std::memcpy(&pc_vtable, self->pc, sizeof(pc_vtable));
    return reinterpret_cast<PcCall>(pc_vtable[kCustomHeapPropertiesSlot])(
        self->pc, out, node_mask, heap_type);
}

inline HRESULT create_wrapped_command_queue(DeviceProxy* self, const void* desc, void** out,
                                            std::size_t slot) {
    auto* pc = static_cast<ID3D12Device*>(self->pc);

    if (!out) return E_POINTER;
    *out = nullptr;
    if (!desc) return E_INVALIDARG;
    struct QueueDesc {
        INT type;
        INT priority;
        UINT flags;
        UINT node_mask;
    } xbox{};
    static_assert(sizeof(QueueDesc) == 16);
    std::memcpy(&xbox, desc, sizeof(xbox));

    constexpr UINT kDisableTimeout = D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT;
    constexpr UINT kObservedXboxExtraFlag = 0x2;
    UINT flags = xbox.flags;
    if (xbox.type == D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        (flags & ~kDisableTimeout) == kObservedXboxExtraFlag) {
        // No desktop equivalent for the extra flag on this observed compute
        // queue. Preserve the documented timeout bit, if set, and log the change.
        flags &= ~kObservedXboxExtraFlag;
    }
    if ((xbox.type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
         xbox.type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
         xbox.type != D3D12_COMMAND_LIST_TYPE_COPY) || (flags & ~kDisableTimeout)) {
        xbgtasix::logf("[d3d12_xs] slot %zu unsupported queue type=%d flags=0x%x",
                       slot, xbox.type, xbox.flags);
        return E_INVALIDARG;
    }
    const D3D12_COMMAND_QUEUE_DESC translated{
        static_cast<D3D12_COMMAND_LIST_TYPE>(xbox.type), xbox.priority,
        static_cast<D3D12_COMMAND_QUEUE_FLAGS>(flags), xbox.node_mask};
    void* native_queue = nullptr;
    const HRESULT hr = pc->CreateCommandQueue(&translated, __uuidof(ID3D12CommandQueue), &native_queue);
    if (SUCCEEDED(hr) && native_queue) {
        *out = wrap_command_queue(static_cast<ID3D12CommandQueue*>(native_queue));
    }
    xbgtasix::logf("[d3d12_xs] slot %zu -> CreateCommandQueue type=%d priority=%d "
                   "flags=0x%x->0x%x nodeMask=0x%x hr=0x%08lx proxy=%s", slot, xbox.type,
                   xbox.priority, xbox.flags, flags, xbox.node_mask,
                   static_cast<unsigned long>(hr), *out ? "yes" : "no");
    return (*out || FAILED(hr)) ? hr : E_OUTOFMEMORY;
}

// Slot 8 has the same CreateCommandQueue signature on both platforms. The
// title's direct queue needs the proxy for Xbox queue extensions such as PresentX.
inline HRESULT WINAPI create_command_queue_x(DeviceProxy* self, const void* desc,
                                              REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != __uuidof(ID3D12CommandQueue) && iid != __uuidof(IUnknown) &&
        iid != __uuidof(ID3D12Object) && iid != __uuidof(ID3D12DeviceChild) &&
        iid != __uuidof(ID3D12Pageable)) return E_NOINTERFACE;
    return create_wrapped_command_queue(self, desc, out, kXboxCreateCommandQueueSlot);
}

// Xbox calls slot 48 with the command queue IID. Preserve the desktop slot's
// OpenExistingHeapFromAddress behavior for its existing callers.
inline HRESULT WINAPI queue_or_open_heap(DeviceProxy* self, const void* desc,
                                           REFIID iid, void** out) {
    if (iid == __uuidof(ID3D12CommandQueue))
        return create_wrapped_command_queue(self, desc, out, kQueueOrOpenHeapSlot);
    if (self->version < 3) {
        if (out) *out = nullptr;
        return E_NOINTERFACE;
    }
    return static_cast<ID3D12Device3*>(self->pc)->OpenExistingHeapFromAddress(desc, iid, out);
}

inline HRESULT WINAPI create_descriptor_heap_x(DeviceProxy* self,
    const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID iid, void** out) {
    const HRESULT hr = create_descriptor_heap_proxy(static_cast<ID3D12Device*>(self->pc),
        reinterpret_cast<IUnknown*>(self), desc, iid, out);
    if (desc) {
        xbgtasix::logf("[d3d12_xs] device slot 14 CreateDescriptorHeap type=%u descriptors=%u "
                       "flags=0x%x nodeMask=0x%x hr=0x%08lx", static_cast<unsigned>(desc->Type),
                       desc->NumDescriptors, static_cast<unsigned>(desc->Flags), desc->NodeMask,
                       static_cast<unsigned long>(hr));
    }
    return hr;
}

inline HRESULT WINAPI set_frame_interval_x(DeviceProxy* self, IUnknown* output, UINT interval_us,
                                           UINT periodicity, UINT flags) {
    const HRESULT hr = output || flags != 0 ? E_NOTIMPL : S_OK;
    if (SUCCEEDED(hr)) self->frame_events.set_interval(interval_us);
    xbgtasix::logf("[d3d12_xs] device slot 78 SetFrameIntervalX interval=%u periodicity=%u "
                   "output=%p flags=0x%x hr=0x%08lx",
                   interval_us, periodicity, output, flags, static_cast<unsigned long>(hr));
    return hr;
}

inline HRESULT WINAPI schedule_frame_event_x(DeviceProxy* self, UINT type, UINT offset_us,
                                              const void* block, UINT flags) {
    const HRESULT hr = block || flags != 0 ? E_NOTIMPL : self->frame_events.schedule(type, offset_us);
    xbgtasix::logf("[d3d12_xs] device slot 79 ScheduleFrameEventX type=%u offset=%u block=%p "
                   "flags=0x%x hr=0x%08lx", type, offset_us, block, flags,
                   static_cast<unsigned long>(hr));
    return hr;
}

inline HRESULT WINAPI wait_frame_event_x(DeviceProxy* self, UINT type, UINT timeout_ms,
                                          const void* block, UINT flags, UINT64* token) {
    const HRESULT hr = block || flags != 0 ? E_NOTIMPL : self->frame_events.wait(type, timeout_ms, token);
    const UINT64 issued = SUCCEEDED(hr) ? *token : 0;
    xbgtasix::logf("[d3d12_xs] device slot 80 WaitFrameEventX type=%u timeout=%u block=%p "
                   "flags=0x%x token=0x%llx hr=0x%08lx", type, timeout_ms, block, flags,
                   static_cast<unsigned long long>(issued), static_cast<unsigned long>(hr));
    return hr;
}

inline HRESULT WINAPI frame_statistics_x(DeviceProxy*, UINT64 token, UINT type,
                                         UINT* count, void* stats) {
    if (!count) return E_POINTER;
    if (*count != 0 && stats) {
        std::memset(stats, 0, 0x20);
        *static_cast<UINT*>(stats) = type;
    }
    if (*count != 0) *count = 1;
    xbgtasix::logf("[d3d12_xs] device slot 81 GetFrameStatisticsX token=0x%llx type=%u count=%u",
                   static_cast<unsigned long long>(token), type, *count);
    return S_OK;
}

// 主机扩展桩：**必须带槽号**。以前只打参数、不打槽号，排查时根本没法定位是哪个方法
// （实测日志里出现 a=0x18ede4d0000（看着像描述符 arena 地址）b=... c=0xffffffff，
// 却不知道它属于哪个槽）。签名统一按 4 个 u64 收参数 —— 对"不知道签名"的桩这是唯一
// 安全的做法；但**不能**用它替代已知签名的方法（返回值类型不同，见每个已建模槽位）。
template <std::size_t N>
HRESULT WINAPI device_extension_stub_n(DeviceProxy*, UINT64 a = 0, UINT64 b = 0, UINT64 c = 0,
                                       UINT64 d = 0) {
    static std::atomic<int> budget{40};
    if (budget.fetch_sub(1) > 0) {
        xbgtasix::logf("[d3d12_xs] 未建模的主机扩展 slot=%zu a=0x%llx b=0x%llx c=0x%llx d=0x%llx"
                       " -> E_NOTIMPL",
                       N, static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                       static_cast<unsigned long long>(c), static_cast<unsigned long long>(d));
    }
    return E_NOTIMPL;
}

// The Xbox command-list interface keeps the ordinary D3D12 list methods at
// their observed positions and adds console methods after the desktop list.
// The game reaches slots 131/132 immediately after creation; those methods are
// state/setup hooks on the console and have no desktop equivalent. They are
// deliberately harmless no-ops until their state semantics are established.
struct CommandListProxy;

struct ObservedCommandListDesc {
    UINT type;
    UINT unknown_04;
    UINT node_mask;
    UINT unknown_0c;
};
static_assert(sizeof(ObservedCommandListDesc) == 16);

inline bool is_xbox_command_list_iid(REFIID iid) {
    return iid == kObservedXboxCommandListIid || iid == kObservedXboxCommandList5Iid;
}

inline ULONG WINAPI list_add_ref(CommandListProxy* self);
inline ULONG WINAPI list_release(CommandListProxy* self);
inline HRESULT WINAPI list_query_interface(CommandListProxy* self, REFIID iid, void** out);
inline HRESULT WINAPI list_close(CommandListProxy* self);
inline HRESULT WINAPI list_reset(CommandListProxy* self, ID3D12CommandAllocator* allocator,
                                 ID3D12PipelineState* initial_pso);
inline void WINAPI list_slot_131(CommandListProxy* self, UINT reg, UINT value, UINT flags,
                                 UINT64 address);
inline void WINAPI list_slot_132(CommandListProxy* self, UINT value, UINT flags, UINT unused,
                                 UINT64 address);

template <std::size_t N>
__attribute__((naked)) void WINAPI forward_list_this() {
    __asm__ volatile("movq 8(%%rcx), %%rcx\n\t"
                     "movq (%%rcx), %%r11\n\t"
                     "jmp *%c0(%%r11)" :: "i"(N * 8));
}

struct CommandListProxy {
    const Slot* vtable;
    ID3D12GraphicsCommandList* pc;
    std::atomic<ULONG> refs{1};
};
static_assert(offsetof(CommandListProxy, vtable) == 0 && offsetof(CommandListProxy, pc) == 8);

inline ULONG WINAPI list_add_ref(CommandListProxy* self) {
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

inline ULONG WINAPI list_release(CommandListProxy* self) {
    const ULONG refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) {
        auto* pc = self->pc;
        delete self;
        pc->Release();
    }
    return refs;
}

inline HRESULT WINAPI list_query_interface(CommandListProxy* self, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (is_xbox_command_list_iid(iid) || iid == __uuidof(IUnknown) ||
        iid == __uuidof(ID3D12Object) || iid == __uuidof(ID3D12DeviceChild) ||
        iid == __uuidof(ID3D12CommandList) || iid == __uuidof(ID3D12GraphicsCommandList)) {
        *out = self;
        list_add_ref(self);
        return S_OK;
    }
    void* native = nullptr;
    const HRESULT hr = self->pc->QueryInterface(iid, &native);
    if (SUCCEEDED(hr) && native) {
        static_cast<IUnknown*>(native)->Release();
        *out = self;
        list_add_ref(self);
        return S_OK;
    }
    return hr;
}

inline HRESULT WINAPI list_close(CommandListProxy* self) {
    xbgtasix::logf("[d3d12_xs] Xbox command list Close");
    return self->pc->Close();
}

inline HRESULT WINAPI list_reset(CommandListProxy* self, ID3D12CommandAllocator* allocator,
                                 ID3D12PipelineState* initial_pso) {
    xbgtasix::logf("[d3d12_xs] Xbox command list Reset allocator=%p pso=%p",
                   static_cast<void*>(allocator), static_cast<void*>(initial_pso));
    return self->pc->Reset(allocator, initial_pso);
}

inline void WINAPI list_set_descriptor_heaps(CommandListProxy* self, UINT count,
                                              ID3D12DescriptorHeap* const* heaps) {
    // D3D12 can bind at most one heap of each shader-visible heap type.
    std::array<ID3D12DescriptorHeap*, 2> native{};
    if (count > native.size() || (count != 0 && !heaps)) return;
    for (UINT i = 0; i < count; ++i) native[i] = unwrap_descriptor_heap(heaps[i]);
    self->pc->SetDescriptorHeaps(count, native.data());
}

inline void WINAPI list_slot_131(CommandListProxy*, UINT reg, UINT value, UINT flags,
                                 UINT64 address) {
    xbgtasix::logf("[d3d12_xs] Xbox command list slot 131 reg=0x%x value=0x%x flags=0x%x "
                   "address=0x%llx (no desktop equivalent)", reg, value, flags,
                   static_cast<unsigned long long>(address));
}

inline void WINAPI list_slot_132(CommandListProxy*, UINT value, UINT flags, UINT unused,
                                 UINT64 address) {
    xbgtasix::logf("[d3d12_xs] Xbox command list slot 132 value=0x%x flags=0x%x unused=0x%x "
                   "address=0x%llx (no desktop equivalent)", value, flags, unused,
                   static_cast<unsigned long long>(address));
}

inline Slot command_list_slot(std::size_t n) {
    if (n == 0) return reinterpret_cast<Slot>(&list_query_interface);
    if (n == 1) return reinterpret_cast<Slot>(&list_add_ref);
    if (n == 2) return reinterpret_cast<Slot>(&list_release);
    if (n == 9) return reinterpret_cast<Slot>(&list_close);
    if (n == 10) return reinterpret_cast<Slot>(&list_reset);
    if (n == 28) return reinterpret_cast<Slot>(&list_set_descriptor_heaps);
    if (n == 131) return reinterpret_cast<Slot>(&list_slot_131);
    if (n == 132) return reinterpret_cast<Slot>(&list_slot_132);
    return nullptr;
}

template <std::size_t... N>
std::array<Slot, sizeof...(N)> make_command_list_vtable(std::index_sequence<N...>) {
    std::array<Slot, sizeof...(N)> v{forward_list_this<N>...};
    ((v[N] = command_list_slot(N) ? command_list_slot(N) : v[N]), ...);
    return v;
}

inline const std::array<Slot, kCommandListSlots>& command_list_vtable() {
    static const auto vtable = make_command_list_vtable(
        std::make_index_sequence<kCommandListSlots>{});
    return vtable;
}

inline void* wrap_command_list(ID3D12GraphicsCommandList* owned_pc_ref) {
    auto* proxy = new (std::nothrow) CommandListProxy{command_list_vtable().data(), owned_pc_ref};
    if (!proxy) owned_pc_ref->Release();
    return proxy;
}

inline bool is_command_list_proxy(ID3D12CommandList* list) {
    if (!list) return false;
    const Slot* vtable = nullptr;
    std::memcpy(&vtable, list, sizeof(vtable));
    return vtable == command_list_vtable().data();
}

inline ID3D12CommandList* unwrap_command_list(ID3D12CommandList* list) {
    if (!is_command_list_proxy(list)) return list;
    auto* proxy = reinterpret_cast<CommandListProxy*>(list);
    return static_cast<ID3D12CommandList*>(proxy->pc);
}

inline constexpr std::size_t kCommandQueueSlots = 64;
struct CommandQueueProxy;

inline ULONG WINAPI queue_add_ref(CommandQueueProxy* self);
inline ULONG WINAPI queue_release(CommandQueueProxy* self);
inline HRESULT WINAPI queue_query_interface(CommandQueueProxy* self, REFIID iid, void** out);
inline void WINAPI queue_execute(CommandQueueProxy* self, UINT count,
                                 ID3D12CommandList* const* lists);

struct CommandQueueProxy {
    const Slot* vtable;
    ID3D12CommandQueue* pc;
    std::atomic<ULONG> refs{1};
    std::mutex present_mutex{};
    std::unique_ptr<PresentState> presentation{};
};
static_assert(offsetof(CommandQueueProxy, vtable) == 0);

template <std::size_t N>
uintptr_t WINAPI forward_queue_slot(CommandQueueProxy* self, uintptr_t a1 = 0, uintptr_t a2 = 0,
                                     uintptr_t a3 = 0, uintptr_t a4 = 0, uintptr_t a5 = 0,
                                     uintptr_t a6 = 0, uintptr_t a7 = 0, uintptr_t a8 = 0,
                                     uintptr_t a9 = 0) {
    using Fn = uintptr_t (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                             uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    const Slot* pc_vtable;
    std::memcpy(&pc_vtable, self->pc, sizeof(pc_vtable));
    return reinterpret_cast<Fn>(pc_vtable[N])(self->pc, a1, a2, a3, a4, a5, a6, a7, a8, a9);
}

inline ULONG WINAPI queue_add_ref(CommandQueueProxy* self) {
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

inline ULONG WINAPI queue_release(CommandQueueProxy* self) {
    const ULONG refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) {
        auto* pc = self->pc;
        delete self;
        pc->Release();
    }
    return refs;
}

inline HRESULT WINAPI queue_query_interface(CommandQueueProxy* self, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) ||
        iid == __uuidof(ID3D12DeviceChild) || iid == __uuidof(ID3D12CommandQueue)) {
        *out = self;
        queue_add_ref(self);
        return S_OK;
    }
    void* native = nullptr;
    const HRESULT hr = self->pc->QueryInterface(iid, &native);
    if (SUCCEEDED(hr) && native) {
        static_cast<IUnknown*>(native)->Release();
        *out = self;
        queue_add_ref(self);
        return S_OK;
    }
    return hr;
}

inline void WINAPI queue_execute(CommandQueueProxy* self, UINT count,
                                 ID3D12CommandList* const* lists) {
    if (!lists && count != 0) return;
    std::array<ID3D12CommandList*, 64> local{};
    std::unique_ptr<ID3D12CommandList*[]> heap;
    ID3D12CommandList** native_lists = local.data();
    if (count > local.size()) {
        heap.reset(new (std::nothrow) ID3D12CommandList*[count]);
        if (!heap) return;
        native_lists = heap.get();
    }
    for (UINT i = 0; i < count; ++i) native_lists[i] = unwrap_command_list(lists[i]);
    xbgtasix::logf("[d3d12_xs] Xbox command queue ExecuteCommandLists count=%u", count);
    self->pc->ExecuteCommandLists(count, native_lists);
}

inline HRESULT present_one_plane(CommandQueueProxy* self, UINT plane_count,
                                 const void* planes, const void* params) {
    if (params || plane_count > 1) return E_NOTIMPL;
    std::lock_guard lock(self->present_mutex);
    if (plane_count == 0) {
        self->presentation.reset();
        return S_OK;
    }
    if (!planes) return E_INVALIDARG;
    PresentPlaneHeader plane{};
    std::memcpy(&plane, planes, sizeof(plane));
    if (plane.resource_count > 1) return E_NOTIMPL;
    if (plane.resource_count == 0 || !plane.resources || !*plane.resources) return E_INVALIDARG;
    if (self->pc->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_NOTIMPL;

    auto* source = *plane.resources;
    const auto desc = source->GetDesc();
    if (!supported_present_texture(desc)) return E_NOTIMPL;
    PresentCom<ID3D12Device> queue_device;
    HRESULT hr = self->pc->GetDevice(IID_PPV_ARGS(queue_device.out()));
    if (FAILED(hr)) return hr;
    PresentCom<ID3D12Device> source_device;
    hr = source->GetDevice(IID_PPV_ARGS(source_device.out()));
    if (FAILED(hr)) return hr;
    if (source_device.get() != queue_device.get()) return E_INVALIDARG;

    if (!self->presentation) {
        std::unique_ptr<PresentState> candidate(new (std::nothrow) PresentState);
        if (!candidate) return E_OUTOFMEMORY;
        hr = candidate->initialize(self->pc, queue_device.get(), desc);
        if (FAILED(hr)) return hr;
        self->presentation = std::move(candidate);
    }
    return self->presentation->present(self->pc, source, desc);
}

inline HRESULT WINAPI queue_present_x(CommandQueueProxy* self, UINT plane_count,
                                      const void* planes, const void* params) {
    const HRESULT hr = present_one_plane(self, plane_count, planes, params);
    static std::atomic<unsigned> calls{0};
    const unsigned call = calls.fetch_add(1, std::memory_order_relaxed);
    if (call < 8)
        xbgtasix::logf("[d3d12_xs] queue slot 30 PresentX planes=%u params=%p hr=0x%08lx",
                       plane_count, params, static_cast<unsigned long>(hr));
    return hr;
}

inline Slot command_queue_slot(std::size_t n) {
    if (n == 0) return reinterpret_cast<Slot>(&queue_query_interface);
    if (n == 1) return reinterpret_cast<Slot>(&queue_add_ref);
    if (n == 2) return reinterpret_cast<Slot>(&queue_release);
    if (n == 10) return reinterpret_cast<Slot>(&queue_execute);
    if (n == 30) return reinterpret_cast<Slot>(&queue_present_x);
    return nullptr;
}

template <std::size_t... N>
std::array<Slot, sizeof...(N)> make_command_queue_vtable(std::index_sequence<N...>) {
    std::array<Slot, sizeof...(N)> v{reinterpret_cast<Slot>(&forward_queue_slot<N>)...};
    ((v[N] = command_queue_slot(N) ? command_queue_slot(N) : v[N]), ...);
    return v;
}

inline const std::array<Slot, kCommandQueueSlots>& command_queue_vtable() {
    static const auto vtable = make_command_queue_vtable(
        std::make_index_sequence<kCommandQueueSlots>{});
    return vtable;
}

inline void* wrap_command_queue(ID3D12CommandQueue* owned_pc_ref) {
    auto* proxy = new (std::nothrow) CommandQueueProxy{command_queue_vtable().data(), owned_pc_ref};
    if (!proxy) owned_pc_ref->Release();
    return proxy;
}

inline HRESULT WINAPI create_command_list_x(DeviceProxy* self, const void* desc,
    ID3D12CommandAllocator* allocator, ID3D12PipelineState* initial_pso,
    const GUID* iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!desc || !allocator || !iid) return E_INVALIDARG;
    ObservedCommandListDesc args{};
    std::memcpy(&args, desc, sizeof(args));
    const bool xbox_list = is_xbox_command_list_iid(*iid);
    const bool desktop_list = *iid == __uuidof(ID3D12GraphicsCommandList);
    if (!xbox_list && !desktop_list) {
        xbgtasix::logf("[d3d12_xs] Xbox slot 73 -> unrecognized command-list IID (E_NOINTERFACE)");
        return E_NOINTERFACE;
    }
    xbgtasix::logf("[d3d12_xs] Xbox slot 73 CreateCommandListX desc=%08x/%08x/%08x/%08x "
                   "initialPSO=%s requestedABI=%s", args.type, args.unknown_04, args.node_mask,
                   args.unknown_0c, initial_pso ? "present" : "null", xbox_list ? "Xbox" : "desktop");
    if ((args.type != D3D12_COMMAND_LIST_TYPE_DIRECT &&
         args.type != D3D12_COMMAND_LIST_TYPE_COMPUTE &&
         args.type != D3D12_COMMAND_LIST_TYPE_COPY) || args.node_mask == 0) {
        return E_INVALIDARG;
    }
    if (args.unknown_04 != 2 || args.unknown_0c != 0) {
        xbgtasix::logf("[d3d12_xs] Xbox slot 73 preserving unmodeled descriptor words "
                       "word1=0x%x word3=0x%x", args.unknown_04, args.unknown_0c);
    }
    auto* pc = static_cast<ID3D12Device*>(self->pc);
    const UINT pc_node_mask = pc->GetNodeCount() == 1 ? 0 : args.node_mask;
    void* pc_list = nullptr;
    const HRESULT hr = pc->CreateCommandList(pc_node_mask,
        static_cast<D3D12_COMMAND_LIST_TYPE>(args.type), allocator, initial_pso,
        __uuidof(ID3D12GraphicsCommandList), &pc_list);
    xbgtasix::logf("[d3d12_xs] Xbox slot 73 -> desktop CreateCommandList nodeMask=0x%x->0x%x "
                   "hr=0x%08lx", args.node_mask, pc_node_mask,
                   static_cast<unsigned long>(hr));
    if (FAILED(hr) || !pc_list) return FAILED(hr) ? hr : E_FAIL;
    *out = wrap_command_list(static_cast<ID3D12GraphicsCommandList*>(pc_list));
    return *out ? hr : E_OUTOFMEMORY;
}

// Slots whose ABI is shared pass through to the real device. A tail jump means
// we replace only "this" and preserve all other registers, stack arguments and
// return values. These leaf thunks have no stack frame or generated code pages.
// A slot with a differing signature must get its own adapter instead.
// 槽位转发：**不用裸汇编**。
//
// 原来这里是 `__attribute__((naked))` 的 asm 跳板（改 this 后 jmp pc->vtable[N]），
// 实测它在真实调用下会崩（t41 在 D3D12Core 内 AV），而同样语义的 C 版本正常。
// 原因大概是内联汇编在 MinGW 下与 SEH/unwind 或寄存器分配冲突；不值得深究，
// 直接换成"固定参数个数的 C 透传"：x64 下 this 走 rcx，前 4 个整数参数走
// rcx/rdx/r8/r9，其余在栈上 —— C 函数按同样规则收下再原样传出去即可。
// 已知边界：浮点参数会丢（设备方法基本没有）；参数超过 9 个会丢尾部（D3D12 里没有）。
template <std::size_t N>
uintptr_t WINAPI forward_slot(void* self, uintptr_t a1 = 0, uintptr_t a2 = 0, uintptr_t a3 = 0,
                              uintptr_t a4 = 0, uintptr_t a5 = 0, uintptr_t a6 = 0,
                              uintptr_t a7 = 0, uintptr_t a8 = 0, uintptr_t a9 = 0) {
    auto* proxy = static_cast<DeviceProxy*>(self);
    using Fn = uintptr_t (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                             uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    const auto pc_vtable = *reinterpret_cast<void***>(proxy->pc);
    auto fn = reinterpret_cast<Fn>(pc_vtable[N]);
    return fn(proxy->pc, a1, a2, a3, a4, a5, a6, a7, a8, a9);
}

// 诊断探针：XBGTASIX_SLOTPROBE=1 时把进过的槽位号记一次（每个槽位只记首次），
// 然后照常转发。用来确认"调用方实际用的是哪套 vtable 布局"。
inline bool slot_probe_on() {
    static const bool on = [] {
        char b[8] = {0};
        return GetEnvironmentVariableA("XBGTASIX_SLOTPROBE", b, sizeof(b)) > 0 && b[0] == '1';
    }();
    return on;
}

inline void log_slot_once(std::size_t n) {
    static std::atomic<UINT64> seen[4] = {};
    const std::size_t w = n / 64;
    const std::size_t bit = n % 64;
    if (w >= 4) return;
    const UINT64 mask = UINT64{1} << bit;
    if ((seen[w].fetch_or(mask, std::memory_order_relaxed) & mask) == 0)
        xbgtasix::logf("[d3d12_xs] 槽位探针 slot=%zu", n);
}

template <std::size_t N>
uintptr_t WINAPI slot_probe_forward(void* self, uintptr_t a1 = 0, uintptr_t a2 = 0,
                                    uintptr_t a3 = 0, uintptr_t a4 = 0, uintptr_t a5 = 0,
                                    uintptr_t a6 = 0, uintptr_t a7 = 0, uintptr_t a8 = 0,
                                    uintptr_t a9 = 0) {
    log_slot_once(N);
    auto* proxy = static_cast<DeviceProxy*>(self);
    using Fn = uintptr_t (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                             uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    const auto pc_vtable = *reinterpret_cast<void***>(proxy->pc);
    return reinterpret_cast<Fn>(pc_vtable[N])(proxy->pc, a1, a2, a3, a4, a5, a6, a7, a8, a9);
}

// 建视图 / 拷贝描述符：把游戏传进来的 arena 句柄翻译成 PC 堆里的真实地址。
//
// **注意：这些函数目前没有绑到任何槽位。**
// 原因是主机设备 vtable 的方法顺序与 PC 头文件不同（实测主机 14 = CreateDescriptorHeap、
// 48 = CreateCommandQueueX、73 = 命令列表创建），按 PC 索引硬套会把 A 方法当 B 方法调。
// 待用 XBGTASIX_SLOTPROBE=1 在真机上逐个确认主机槽位后再绑定。
// 实测：不翻译的话游戏一建视图就崩在 D3D12Core（AV）。
inline void log_view_handle(const char* what, D3D12_CPU_DESCRIPTOR_HANDLE given,
                            D3D12_CPU_DESCRIPTOR_HANDLE mapped) {
    static std::atomic<int> budget{40};
    if (budget.fetch_sub(1) > 0) {
        xbgtasix::logf("[d3d12_xs] 视图创建诊断 %s: 传入=%p 翻译后=%p", what,
                       reinterpret_cast<void*>(given.ptr), reinterpret_cast<void*>(mapped.ptr));
    }
}

inline void WINAPI create_cbv(DeviceProxy* self, const D3D12_CONSTANT_BUFFER_VIEW_DESC* d,
                              D3D12_CPU_DESCRIPTOR_HANDLE h) {
    auto* pc = static_cast<ID3D12Device*>(self->pc);
    const auto mapped = translate_cpu(h);
    log_view_handle("CBV", h, mapped);
    pc->CreateConstantBufferView(d, mapped);
}
inline void WINAPI create_srv(DeviceProxy* self, ID3D12Resource* r,
                              const D3D12_SHADER_RESOURCE_VIEW_DESC* d,
                              D3D12_CPU_DESCRIPTOR_HANDLE h) {
    const auto mapped = translate_cpu(h);
    log_view_handle("SRV", h, mapped);
    static_cast<ID3D12Device*>(self->pc)->CreateShaderResourceView(r, d, mapped);
}
inline void WINAPI create_uav(DeviceProxy* self, ID3D12Resource* r,
                              const D3D12_UNORDERED_ACCESS_VIEW_DESC* d,
                              D3D12_CPU_DESCRIPTOR_HANDLE h) {
    const auto mapped = translate_cpu(h);
    log_view_handle("UAV", h, mapped);
    static_cast<ID3D12Device*>(self->pc)->CreateUnorderedAccessView(r, nullptr, d, mapped);
}
inline void WINAPI create_rtv(DeviceProxy* self, ID3D12Resource* r,
                              const D3D12_RENDER_TARGET_VIEW_DESC* d,
                              D3D12_CPU_DESCRIPTOR_HANDLE h) {
    const auto mapped = translate_cpu(h);
    log_view_handle("RTV", h, mapped);
    static_cast<ID3D12Device*>(self->pc)->CreateRenderTargetView(r, d, mapped);
}
inline void WINAPI create_dsv(DeviceProxy* self, ID3D12Resource* r,
                              const D3D12_DEPTH_STENCIL_VIEW_DESC* d,
                              D3D12_CPU_DESCRIPTOR_HANDLE h) {
    const auto mapped = translate_cpu(h);
    log_view_handle("DSV", h, mapped);
    static_cast<ID3D12Device*>(self->pc)->CreateDepthStencilView(r, d, mapped);
}
inline void WINAPI create_sampler(DeviceProxy* self, const D3D12_SAMPLER_DESC* d,
                                  D3D12_CPU_DESCRIPTOR_HANDLE h) {
    const auto mapped = translate_cpu(h);
    log_view_handle("Sampler", h, mapped);
    static_cast<ID3D12Device*>(self->pc)->CreateSampler(d, mapped);
}
inline void WINAPI copy_descriptors_simple(DeviceProxy* self,
                                           D3D12_CPU_DESCRIPTOR_HANDLE dst, UINT dst_range,
                                           D3D12_CPU_DESCRIPTOR_HANDLE src, UINT src_range,
                                           UINT count) {
    static_cast<ID3D12Device*>(self->pc)->CopyDescriptorsSimple(
        count, translate_cpu(dst), translate_cpu(src),
        static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(dst_range));
}

// 主机槽位 19 = CreateUnorderedAccessView（五参：(this, pResource, pCounter, pDesc, Dest)）。
// 证据来自崩溃点 game+0x1647616 前的调用序列：
//     mov (%rcx),%rax ; mov %r8,0x20(%rsp) ; xor %r8d,%r8d ; call *0x98(%rax)
// 即 pCounter 被显式清零、目标句柄走第 5 个参数位（栈上）。
inline HRESULT WINAPI create_uav_x(DeviceProxy* self, ID3D12Resource* resource,
                                   ID3D12Resource* counter,
                                   const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc,
                                   D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    const auto mapped = translate_cpu(dest);
    static std::atomic<int> budget{20};
    if (budget.fetch_sub(1) > 0) {
        xbgtasix::logf("[d3d12_xs] 槽19 CreateUnorderedAccessView res=%p counter=%p desc=%p "
                       "dest=%p -> %p", static_cast<void*>(resource), static_cast<void*>(counter),
                       static_cast<const void*>(desc), reinterpret_cast<void*>(dest.ptr),
                       reinterpret_cast<void*>(mapped.ptr));
    }
    static_cast<ID3D12Device*>(self->pc)->CreateUnorderedAccessView(resource, counter, desc, mapped);
    return S_OK;
}

// 主机槽位 133：**用已有虚拟地址创建 D3D12 资源**。
//
// 调用点证据（GPT 给的 game+0x1E8684C，vtable+0x428 = 槽 133）：
//   RCX = 设备 this
//   RDX = 已映射区间的起始地址
//   R8  = 栈上构造的资源描述
//   R9D = 0xFFFFFFFF
//   [rsp+0x20] = 0
//   [rsp+0x28] = IID_ID3D12Resource
//   [rsp+0x30] = 输出指针（成功后游戏存到 game+0x75DF108）
//
// 注意：0xFFFFFFFF **不是**资源状态，绝不能直接当 D3D12_RESOURCE_STATES 传给桌面。
// 这里统一用 COMMON。
//
// 实现策略（由保真到降级）：
//   1) `OpenExistingHeapFromAddress(基址)` → 在它上面 CreatePlacedResource(offset=0)：
//      资源与游戏那片内存**真正共享**，语义最接近主机
//   2) 退化为 committed buffer（只验证对象 ABI，数据不共享，会记一条警告）
inline HRESULT WINAPI create_resource_from_address_x(DeviceProxy* self, void* base_addr,
                                                     const D3D12_RESOURCE_DESC* desc, UINT a4,
                                                     UINT a5, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    xbgtasix::logf("[d3d12_xs] 槽133 用地址建资源 base=%p desc=%p a4=0x%x a5=0x%x riid=%p",
                   base_addr, static_cast<const void*>(desc), a4, a5,
                   static_cast<const void*>(&riid));
    if (!desc) return E_INVALIDARG;
    auto* pc = static_cast<ID3D12Device*>(self->pc);
    xbgtasix::logf("[d3d12_xs] 槽133 desc: dim=%d align=0x%llx w=%llu h=%u fmt=%d flags=0x%x",
                   static_cast<int>(desc->Dimension),
                   static_cast<unsigned long long>(desc->Alignment),
                   static_cast<unsigned long long>(desc->Width), desc->Height,
                   static_cast<int>(desc->Format), static_cast<unsigned>(desc->Flags));

    // 主机资源标志里含 PC 不认的位（实测 flags=0x20004，其中 0x20000 就是主机专属，
    // 桌面 D3D12 直接 E_INVALIDARG）。只保留标准位（低 6 位）再往下走。
    constexpr D3D12_RESOURCE_FLAGS kPcResourceFlagMask =
        static_cast<D3D12_RESOURCE_FLAGS>(0x3F);
    D3D12_RESOURCE_DESC pc_desc = *desc;
    const auto original_flags = pc_desc.Flags;
    pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(pc_desc.Flags & kPcResourceFlagMask);
    if (original_flags != pc_desc.Flags) {
        xbgtasix::logf("[d3d12_xs] 槽133 剥离主机资源标志 0x%x -> 0x%x",
                       static_cast<unsigned>(original_flags), static_cast<unsigned>(pc_desc.Flags));
    }

    // 1) 首选：把这片内存当成"已存在的堆"，在上面放资源（真正共享数据）
    ID3D12Device3* dev3 = nullptr;
    if (SUCCEEDED(pc->QueryInterface(IID_PPV_ARGS(&dev3))) && dev3) {
        ID3D12Heap* heap = nullptr;
        const HRESULT hr_heap = dev3->OpenExistingHeapFromAddress(base_addr, IID_PPV_ARGS(&heap));
        if (SUCCEEDED(hr_heap) && heap) {
            const HRESULT hr = pc->CreatePlacedResource(
                heap, 0, &pc_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, riid, out);
            heap->Release();
            xbgtasix::logf("[d3d12_xs] 槽133 -> placed resource hr=0x%08lx out=%p（与标题内存共享）",
                           static_cast<unsigned long>(hr), out ? *out : nullptr);
            dev3->Release();
            if (SUCCEEDED(hr)) return hr;
        } else {
            xbgtasix::logf("[d3d12_xs] 槽133 OpenExistingHeapFromAddress 失败 hr=0x%08lx"
                           "（该地址不是已提交的私有分配？）",
                           static_cast<unsigned long>(hr_heap));
        }
        dev3->Release();
    }

    // 2) 降级：committed buffer —— 只能验证对象 ABI，数据不共享
    D3D12_RESOURCE_DESC buffer_desc = pc_desc;
    if (buffer_desc.Dimension == D3D12_RESOURCE_DIMENSION_UNKNOWN)
        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    D3D12_HEAP_PROPERTIES props{};
    props.Type = D3D12_HEAP_TYPE_DEFAULT;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    const HRESULT hr = pc->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr, riid, out);
    xbgtasix::logf("[d3d12_xs] 槽133 -> committed 降级 hr=0x%08lx out=%p"
                   "（**数据不与标题内存共享**，仅验证对象 ABI）",
                   static_cast<unsigned long>(hr), out ? *out : nullptr);
    return hr;
}

template <std::size_t N>
Slot device_slot() {
    if constexpr (N == 0) return reinterpret_cast<Slot>(&query_interface);
    if constexpr (N == 1) return reinterpret_cast<Slot>(&add_ref);
    if constexpr (N == 2) return reinterpret_cast<Slot>(&release);
    if constexpr (N == kXboxCreateCommandQueueSlot)
        return reinterpret_cast<Slot>(&create_command_queue_x);
    if constexpr (N == 14) return reinterpret_cast<Slot>(&create_descriptor_heap_x);
    if constexpr (N == 19) return reinterpret_cast<Slot>(&create_uav_x);
    if constexpr (N == 133) return reinterpret_cast<Slot>(&create_resource_from_address_x);
    if constexpr (N == kCustomHeapPropertiesSlot)
        return reinterpret_cast<Slot>(&custom_heap_properties);
    if constexpr (N == kQueueOrOpenHeapSlot)
        return reinterpret_cast<Slot>(&queue_or_open_heap);
    if constexpr (N == kXboxCreateCommandListSlot)
        return reinterpret_cast<Slot>(&create_command_list_x);
    if constexpr (N == 78) return reinterpret_cast<Slot>(&set_frame_interval_x);
    if constexpr (N == 79) return reinterpret_cast<Slot>(&schedule_frame_event_x);
    if constexpr (N == 80) return reinterpret_cast<Slot>(&wait_frame_event_x);
    if constexpr (N == 81) return reinterpret_cast<Slot>(&frame_statistics_x);
    // 主机扩展槽（>= 44）在 PC 设备上**不存在**，按同号转发会把参数喂给别的方法
    // （实测 slot 70 直接崩在 D3D12Core）。未实现的一律走"明确失败 + 记一次"，
    // 让游戏走它自己的降级/报错路径，而不是在驱动内部炸掉。
    if constexpr (N >= 44) return reinterpret_cast<Slot>(&device_extension_stub_n<N>);
    if (slot_probe_on()) return reinterpret_cast<Slot>(&slot_probe_forward<N>);  // 仅诊断用
    return reinterpret_cast<Slot>(&forward_slot<N>);
}

template <std::size_t... N>
std::array<Slot, sizeof...(N)> make_vtable(std::index_sequence<N...>) {
    return {device_slot<N>()...};
}

}  // namespace detail

inline void* wrap_device(IUnknown* owned_pc_ref, int version) {
    static const auto vtable = detail::make_vtable(std::make_index_sequence<kDeviceSlots>{});
    auto* proxy = new (std::nothrow) detail::DeviceProxy{vtable.data(), owned_pc_ref, version};
    if (!proxy) owned_pc_ref->Release();
    return proxy;
}

}  // namespace xbgtasix::d3d12
