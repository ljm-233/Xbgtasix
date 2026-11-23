// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d12.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#include "xbgtasix/trace.h"

namespace xbgtasix::d3d12 {

// ID3D12Device11 adds CreateSampler2 to Device10. Spell out the IID because
// some supported MinGW headers stop at ID3D12Device10.
inline constexpr GUID kDevice11 = {0x5405c344, 0xd457, 0x444e,
                                    {0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39}};
inline constexpr std::size_t kDeviceSlots = 140;
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

// Xbox calls slot 48 with the command queue IID; on PC that slot is
// OpenExistingHeapFromAddress. Keep the desktop form intact and translate
// only the observed queue form: (type, priority, flags, node mask).
inline HRESULT WINAPI queue_or_open_heap(DeviceProxy* self, const void* desc,
                                           REFIID iid, void** out) {
    if (iid != __uuidof(ID3D12CommandQueue)) {
        if (self->version < 3) {
            if (out) *out = nullptr;
            return E_NOINTERFACE;
        }
        return static_cast<ID3D12Device3*>(self->pc)->OpenExistingHeapFromAddress(desc, iid, out);
    }
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
        xbgtasix::logf("[d3d12_xs] slot 48 unsupported queue type=%d flags=0x%x",
                       xbox.type, xbox.flags);
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
    xbgtasix::logf("[d3d12_xs] slot 48 -> CreateCommandQueue type=%d priority=%d "
                   "flags=0x%x->0x%x nodeMask=0x%x hr=0x%08lx proxy=%s", xbox.type,
                   xbox.priority, xbox.flags, flags, xbox.node_mask,
                   static_cast<unsigned long>(hr), *out ? "yes" : "no");
    return (*out || FAILED(hr)) ? hr : E_OUTOFMEMORY;
}

// Xbox frame pacing occupies device slots 78?81. These calls are not present
// in the desktop device vtable; keeping them in a small process-local clock is
// enough to preserve the ABI and to keep the title's startup thread moving.
struct FrameState {
    std::mutex mutex;
    UINT interval_us = 0;
    bool active = false;
    std::array<bool, 3> scheduled{};
    UINT64 next_token = 1;
};

inline FrameState& frame_state() {
    static FrameState state;
    return state;
}

inline HRESULT WINAPI set_frame_interval_x(DeviceProxy*, void*, UINT interval_us,
                                           UINT periodicity, UINT flags) {
    auto& state = frame_state();
    {
        std::lock_guard lock(state.mutex);
        state.interval_us = interval_us;
        state.active = interval_us != 0;
        if (!state.active) state.scheduled.fill(false);
    }
    xbgtasix::logf("[d3d12_xs] device slot 78 SetFrameIntervalX interval=%u periodicity=%u flags=0x%x",
                   interval_us, periodicity, flags);
    return S_OK;
}

inline HRESULT WINAPI schedule_frame_event_x(DeviceProxy*, UINT type, UINT offset_us,
                                              const void* block, UINT flags) {
    auto& state = frame_state();
    if (type >= state.scheduled.size()) return E_INVALIDARG;
    {
        std::lock_guard lock(state.mutex);
        if (flags & 1u) state.scheduled[type] = false;
        else {
            state.active = true;
            state.scheduled[type] = true;
        }
    }
    xbgtasix::logf("[d3d12_xs] device slot 79 ScheduleFrameEventX type=%u offset=%u block=%p flags=0x%x",
                   type, offset_us, block, flags);
    return S_OK;
}

inline HRESULT WINAPI wait_frame_event_x(DeviceProxy*, UINT type, UINT timeout_ms,
                                          const void* block, UINT flags, UINT64* token) {
    auto& state = frame_state();
    if (type >= state.scheduled.size()) return E_INVALIDARG;
    UINT64 issued = 0;
    {
        std::lock_guard lock(state.mutex);
        if (token && *token != 0) issued = *token;
        if (issued == 0) issued = state.next_token++;
        if (token) *token = issued;
    }
    xbgtasix::logf("[d3d12_xs] device slot 80 WaitFrameEventX type=%u timeout=%u block=%p flags=0x%x token=0x%llx",
                   type, timeout_ms, block, flags, static_cast<unsigned long long>(issued));
    return S_OK;
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

inline HRESULT WINAPI device_extension_stub(DeviceProxy*, UINT64 a, UINT64 b, UINT64 c,
                                             UINT64 d) {
    xbgtasix::logf("[d3d12_xs] unmodeled Xbox device extension called a=0x%llx b=0x%llx "
                   "c=0x%llx d=0x%llx -> S_OK", static_cast<unsigned long long>(a),
                   static_cast<unsigned long long>(b), static_cast<unsigned long long>(c),
                   static_cast<unsigned long long>(d));
    return S_OK;
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

template <std::size_t N>
__attribute__((naked)) void WINAPI forward_queue_this() {
    __asm__ volatile("movq 8(%%rcx), %%rcx\n\t"
                     "movq (%%rcx), %%r11\n\t"
                     "jmp *%c0(%%r11)" :: "i"(N * 8));
}

struct CommandQueueProxy {
    const Slot* vtable;
    ID3D12CommandQueue* pc;
    std::atomic<ULONG> refs{1};
};

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

inline Slot command_queue_slot(std::size_t n) {
    if (n == 0) return reinterpret_cast<Slot>(&queue_query_interface);
    if (n == 1) return reinterpret_cast<Slot>(&queue_add_ref);
    if (n == 2) return reinterpret_cast<Slot>(&queue_release);
    if (n == 10) return reinterpret_cast<Slot>(&queue_execute);
    return nullptr;
}

template <std::size_t... N>
std::array<Slot, sizeof...(N)> make_command_queue_vtable(std::index_sequence<N...>) {
    std::array<Slot, sizeof...(N)> v{forward_queue_this<N>...};
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
    static std::atomic<unsigned> seen[4] = {};
    const std::size_t w = n / 64;
    const std::size_t bit = n % 64;
    if (w >= 4) return;
    const unsigned old = seen[w].fetch_or(1u << bit);
    if ((old & (1u << bit)) == 0) xbgtasix::logf("[d3d12_xs] 槽位探针 slot=%zu", n);
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

template <std::size_t N>
Slot device_slot() {
    if constexpr (N == 0) return reinterpret_cast<Slot>(&query_interface);
    if constexpr (N == 1) return reinterpret_cast<Slot>(&add_ref);
    if constexpr (N == 2) return reinterpret_cast<Slot>(&release);
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
    if constexpr (N >= 82) return reinterpret_cast<Slot>(&device_extension_stub);
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
