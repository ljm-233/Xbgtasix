// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#include "xbgtasix/host_stall.h"
#include "xbgtasix/trace.h"

#include "descriptor_heap_abi.h"
#include "root_sig.h"   // 消费端根签名剥离（blob 里的主机位）   // 句柄翻译表（描述符 CPU 句柄：arena 地址 <-> PC 堆地址）
#include "descriptor_heap_abi.h"
#include "root_sig.h"   // 消费端根签名剥离（blob 里的主机位）
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
//
// ⚠ 谁来实现主机 PSO 创建（60–67 这一段里的 graphics/compute pipeline state）：
//   真实的 `CreateGraphics/ComputePipelineState` 在这一层就是一次**驱动编译**，冷缓存
//   几十秒。必须用 `xbgtasix::host_stall::scope` 把它包起来（见 include/xbgtasix/host_stall.h），
//   否则标题的开机 hang check 会在 30 s 后把进程杀掉（它数的是"主线程没进展的轮数"，
//   而驱动编译在我们线程里，它看不见进展）。参考 Leonida PR #7858 的同一机制。
//   60–67 这一段现在由下方 `host_pso_slot_stub` 接管：**参数 dump 桩**（默认 E_NOTIMPL），
//   唯一真正调用驱动的地方是 env 开关下的可行性实验，那里已经用 host_stall::scope 包好。
template <std::size_t N>
HRESULT WINAPI device_extension_stub_n(DeviceProxy*, UINT64 a = 0, UINT64 b = 0, UINT64 c = 0,
                                       UINT64 d = 0) {
    static std::atomic<int> budget{40};
    if (budget.fetch_sub(1) > 0) {
        // 记调用方返回地址（模块内 RVA）：同一槽位在二进制里有多处调用点，
        // 只有知道是谁调的，才能反汇编出真实签名与语义。
        const auto* caller = static_cast<const unsigned char*>(__builtin_return_address(0));
        HMODULE mod = nullptr;
        unsigned long long rva = reinterpret_cast<unsigned long long>(caller);
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(caller), &mod) && mod) {
            rva = reinterpret_cast<unsigned long long>(caller) -
                  reinterpret_cast<unsigned long long>(mod);
        }
        xbgtasix::logf("[d3d12_xs] 未建模的主机扩展 slot=%zu 调用方=+0x%llX "
                       "a=0x%llx b=0x%llx c=0x%llx d=0x%llx -> E_NOTIMPL",
                       N, rva, static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                       static_cast<unsigned long long>(c), static_cast<unsigned long long>(d));
        // 光有指针值认不出方法。主机把"结构体 in/out"塞进参数里（实测 slot 70 的两个
        // 参数相隔 0x40），把前 32 B 打出来，才有机会从字段反推它是哪一个方法。
        // 逐个 VirtualQuery，未提交就跳过 —— 绝不为一条日志去读野指针。
        // 0x40 字节（实测 slot 70 的两个结构相隔 0x40，按 64 B 打全），分两行。
        auto dump64 = [&](const char* tag, UINT64 addr) {
            if (addr < 0x10000) return;
            MEMORY_BASIC_INFORMATION m{};
            if (VirtualQuery(reinterpret_cast<const void*>(addr), &m, sizeof(m)) == 0) return;
            if (m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return;
            const auto* p = reinterpret_cast<const unsigned char*>(addr);
            for (int half = 0; half < 2; ++half) {
                char hex[3 * 32 + 1] = {0};
                for (int i = 0; i < 32; ++i) {
                    snprintf(hex + i * 3, 4, "%02x ", static_cast<unsigned>(p[half * 32 + i]));
                }
                xbgtasix::logf("[d3d12_xs]   slot=%zu %s+0x%02X@%p: %s", N, tag, half * 32,
                               reinterpret_cast<const void*>(addr + half * 32), hex);
            }
        };
        dump64("a", a);
        dump64("b", b);
        dump64("c", c);
    }
    // 诊断开关（默认关）：把未建模槽一律当成功，用来回答"这个槽在不在崩溃路径上"。
    // 只用于实验，**不是**修复手段——项目规则是"明确失败"，不是伪造成功。
    static const bool unimpl_as_ok = [] {
        const char* v = std::getenv("XBGTASIX_UNIMPL_S_OK");
        return v && *v == '1';
    }();
    return unimpl_as_ok ? S_OK : E_NOTIMPL;
}

// ===========================================================================
// 主机 PSO 槽（55 / 60 / 61 / 62 / 66 / 67）：参数 dump 桩 + 预留实验入口
// ===========================================================================
//
// 证据分级（把"参考实现说的"和"本机实测的"分开写，以后不要再混）：
//
//  [参考] 槽位身份：omonkeyz/xbox-emulator（GPL-3.0，与本仓库同许可）
//         `crates/compat/d3d12_xs/device_wrap.rs` 末尾 `vtable()` 的注释：
//           55 = 派生 PSO（base PSO + 状态补丁）
//           60 = Xbox graphics PSO create
//           61 = 扩展采样器创建
//           62 = Xbox compute PSO create
//           66 / 67 = PSO 序列化 / 反序列化（参考实现用自研 TBPS 容器）
//  [实测] 本仓库 build/ 下 **202 个 .log** 里没有任何一次 slot=60/61/62 的调用：
//         未建模槽只出现过 7/8/9/17/18/20/27/36/39/70/127/133。真机最后停在
//         game+0x247AF2D（XboxBvhBuilder 构造后的自身断言），之前最后一条 D3D 调用是
//         槽 19 CreateUnorderedAccessView（build/real-veh.log 末尾）—— 也就是
//         **根本还没走到 PSO 创建这一步**。
//  [实测] 静态侧也给不出正证据：在 120 MB 的无符号标题 EXE 里 `call [reg+0x1e0]`
//         有 185 处、`call [reg+0x1f0]` 有 373 处，但"基址寄存器就是设备指针"这件事
//         无法只靠字节模式证明（同一位移在任何对象上都可能出现）。
//         ⇒ **签名、参数个数、desc 布局全部未实测。**
//
// 所以这一段的实现分级是：
//   55 / 60 / 61 / 62 / 66 / 67：**桩**，返回 E_NOTIMPL，但必须打出足够定位的信息 ——
//       槽号、调用方模块内 RVA、8 个参数、可读指针的前 64 B、以及（60/62）对每个参数
//       做一次"按桌面 desc 布局读是否完全合法"的形状自检：不合法时给出**第一个非法字段
//       在 desc 内的偏移**，这正是下一次真机实验要对照的数据。
//   真正的 desc 翻译/创建**默认不做**：布局未知时按猜想去解引用等于把野指针喂给驱动
//   （实测 slot 70 同号转发直接崩在 D3D12Core）。唯一的例外是 env 开关
//   `XBGTASIX_HOST_PSO_TRANSLATE=1` 下的**图形 PSO 可行性实验**：形状自检通过才用桌面
//   布局原样试建一次，记 HRESULT 后立刻 Release，**不把指针写回游戏的 out 参数**
//   （位置未知），桩的返回值始终是 E_NOTIMPL。
//
// 两个**独立的后续依赖**（拿到槽 60 的参数也不会自动解决）：
//   1) 主机 PSO desc 是否与桌面 D3D12_GRAPHICS_PIPELINE_STATE_DESC 同构；
//   2) 主机着色器字节码格式 —— docs/d3d12xs-evidence.md 记录 shaders.rpf 里扫不到
//      DXBC/DXIL/SHDR/RDEF 魔数；桌面管线只吃 DXIL，格式不对时 desc 再正确也是
//      E_INVALIDARG（见 docs/graphics-pitfalls.md 第 2 条）。
inline constexpr std::size_t kHostDerivedPsoSlot = 55;
inline constexpr std::size_t kHostGraphicsPsoSlot = 60;
inline constexpr std::size_t kHostSamplerXSlot = 61;
inline constexpr std::size_t kHostComputePsoSlot = 62;
inline constexpr std::size_t kHostPsoSerializeSlot = 66;
inline constexpr std::size_t kHostPsoDeserializeSlot = 67;

constexpr bool is_host_pso_slot(std::size_t slot) {
    return slot == kHostDerivedPsoSlot || slot == kHostGraphicsPsoSlot ||
           slot == kHostSamplerXSlot || slot == kHostComputePsoSlot ||
           slot == kHostPsoSerializeSlot || slot == kHostPsoDeserializeSlot;
}

// 只用于日志里的可读标签；括号里的"参考"= 来自参考实现的槽表，**不是**本机实测。
inline const char* host_pso_slot_name(std::size_t slot) {
    switch (slot) {
        case kHostDerivedPsoSlot: return "派生PSO(base+patch, 参考)";
        case kHostGraphicsPsoSlot: return "graphicsPSO(参考)";
        case kHostSamplerXSlot: return "samplerX(参考)";
        case kHostComputePsoSlot: return "computePSO(参考)";
        case kHostPsoSerializeSlot: return "PSO序列化(参考)";
        case kHostPsoDeserializeSlot: return "PSO反序列化(参考)";
        default: return "?";
    }
}

// ---- 只读"这段内存能不能安全读"：绝不为一条日志去碰野指针 ----
// 返回从 addr 起**可连续读**的字节数（want 与当前已提交区域剩余长度的较小值）。
// 未提交页、PAGE_NOACCESS、PAGE_GUARD 一律返回 0。
inline std::size_t host_readable_span(UINT64 addr, std::size_t want,
                                      MEMORY_BASIC_INFORMATION* out) {
    if (addr < 0x10000) return 0;
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(reinterpret_cast<const void*>(addr), &m, sizeof(m)) || m.RegionSize == 0)
        return 0;
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    const UINT64 base = reinterpret_cast<UINT64>(m.BaseAddress);
    const UINT64 avail = static_cast<UINT64>(m.RegionSize) - (addr - base);
    if (out) *out = m;
    return avail < want ? static_cast<std::size_t>(avail) : want;
}

inline bool host_is_module_address(UINT64 addr) {
    if (addr < 0x10000) return false;
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(addr)), &mod))
        return false;
    return mod != nullptr;
}

// GUID 的版本位（Data3 的 bits 12..15）与变体位（byte8 高 2 位 = 10）。主机扩展方法
// 经常把 REFIID 直接当参数；先认出"这像不像一个 IID"，日志才读得下去。
// 返回 0 = 不像 GUID；1..8 = 版本号。
inline unsigned host_guid_version(const unsigned char* p16) {
    const unsigned data3 = static_cast<unsigned>(p16[6]) | (static_cast<unsigned>(p16[7]) << 8);
    const unsigned version = (data3 >> 12) & 0xF;
    if (version < 1 || version > 8) return 0;
    if ((p16[8] & 0xC0) != 0x80) return 0;
    return version;
}

// ---- 桌面 desc 的"形状自检" ----
// 这不是在断言"主机 desc 就是桌面布局"，而是把最省事的假设变成一个**可判定**的问题：
// 如果这块内存按桌面 D3D12_GRAPHICS/COMPUTE_PIPELINE_STATE_DESC 读，所有小枚举、布尔、
// 指针都落在合法域里，那么它与桌面布局同构的可能性就很高，值得逐字段比对；否则日志会
// 指出第一个不合法字段的**偏移**，那正是下次真机实验要看的差异位置。
// 注意：形状自检通过 ≠ 能创建成功（还有着色器字节码格式这一关）。
enum class PsoShape : int {
    kPlausible = 0,
    kNotReadable,
    kBadRootSignature,
    kBadShaderBytecode,
    kBadStreamOutput,
    kBadBlend,
    kBadRasterizer,
    kBadDepthStencil,
    kBadInputLayout,
    kBadStripCut,
    kBadTopology,
    kBadRenderTargets,
    kBadRtvFormat,
    kBadDsvFormat,
    kBadSample,
    kBadNodeMask,
    kBadCachedPso,
    kBadFlags,
};

inline const char* pso_shape_name(PsoShape shape) {
    switch (shape) {
        case PsoShape::kPlausible: return "通过";
        case PsoShape::kNotReadable: return "内存不可读";
        case PsoShape::kBadRootSignature: return "根签名指针不像 COM 对象";
        case PsoShape::kBadShaderBytecode: return "着色器字节码指针/长度非法";
        case PsoShape::kBadStreamOutput: return "流输出描述非法";
        case PsoShape::kBadBlend: return "混合状态枚举越界";
        case PsoShape::kBadRasterizer: return "光栅化状态枚举越界";
        case PsoShape::kBadDepthStencil: return "深度/模板状态枚举越界";
        case PsoShape::kBadInputLayout: return "输入布局非法";
        case PsoShape::kBadStripCut: return "IBStripCutValue 越界";
        case PsoShape::kBadTopology: return "PrimitiveTopologyType 越界";
        case PsoShape::kBadRenderTargets: return "NumRenderTargets 越界";
        case PsoShape::kBadRtvFormat: return "RTVFormats 里有非法格式";
        case PsoShape::kBadDsvFormat: return "DSVFormat 非法";
        case PsoShape::kBadSample: return "SampleDesc 非法";
        case PsoShape::kBadNodeMask: return "NodeMask 非法";
        case PsoShape::kBadCachedPso: return "CachedPSO 指针/长度非法";
        case PsoShape::kBadFlags: return "Flags 越界";
    }
    return "?";
}

struct PsoShapeResult {
    PsoShape shape = PsoShape::kPlausible;
    std::size_t offset = 0;  // 第一个可疑字段在 desc 内的偏移
    UINT64 value = 0;        // 该字段的值（或可疑指针）
};

inline PsoShapeResult pso_shape_bad(PsoShape shape, std::size_t offset, UINT64 value) {
    return PsoShapeResult{shape, offset, value};
}

inline PsoShapeResult host_check_bytecode(const D3D12_SHADER_BYTECODE& code,
                                          std::size_t base) {
    constexpr SIZE_T kMaxShaderBytes = 16u << 20;  // 宽松上界：正常 DXIL 远小于 16 MB
    if (!code.pShaderBytecode) {
        if (code.BytecodeLength != 0)
            return pso_shape_bad(PsoShape::kBadShaderBytecode,
                                 base + offsetof(D3D12_SHADER_BYTECODE, pShaderBytecode),
                                 code.BytecodeLength);
        return {};
    }
    if (code.BytecodeLength == 0 || code.BytecodeLength > kMaxShaderBytes)
        return pso_shape_bad(PsoShape::kBadShaderBytecode,
                             base + offsetof(D3D12_SHADER_BYTECODE, BytecodeLength),
                             code.BytecodeLength);
    if (!host_readable_span(reinterpret_cast<UINT64>(code.pShaderBytecode), 4, nullptr))
        return pso_shape_bad(PsoShape::kBadShaderBytecode,
                             base + offsetof(D3D12_SHADER_BYTECODE, pShaderBytecode),
                             reinterpret_cast<UINT64>(code.pShaderBytecode));
    return {};
}

// COM 对象的判据：前 8 B 是模块内地址（vtable 指针）。主机的 root signature 也可能
// 是"句柄"而不是对象指针 —— 那种情况下这里会不合格，正好是我们要在日志里看到的差异。
inline PsoShapeResult host_check_root_signature(ID3D12RootSignature* root, std::size_t base) {
    if (!root) return {};  // 空根签名交给桌面自己回 E_INVALIDARG，不算"形状不合法"
    const UINT64 addr = reinterpret_cast<UINT64>(root);
    if (host_readable_span(addr, 8, nullptr) < 8)
        return pso_shape_bad(PsoShape::kBadRootSignature, base, addr);
    UINT64 vtable_ptr = 0;
    std::memcpy(&vtable_ptr, root, sizeof(vtable_ptr));
    if (!host_is_module_address(vtable_ptr))
        return pso_shape_bad(PsoShape::kBadRootSignature, base, vtable_ptr);
    return {};
}

inline PsoShapeResult host_check_cached_pso(const D3D12_CACHED_PIPELINE_STATE& cached,
                                            std::size_t base) {
    constexpr SIZE_T kMaxCachedBytes = 64u << 20;
    if (!cached.pCachedBlob) {
        if (cached.CachedBlobSizeInBytes != 0)
            return pso_shape_bad(PsoShape::kBadCachedPso,
                                 base + offsetof(D3D12_CACHED_PIPELINE_STATE, pCachedBlob),
                                 cached.CachedBlobSizeInBytes);
        return {};
    }
    if (cached.CachedBlobSizeInBytes == 0 || cached.CachedBlobSizeInBytes > kMaxCachedBytes)
        return pso_shape_bad(
            PsoShape::kBadCachedPso,
            base + offsetof(D3D12_CACHED_PIPELINE_STATE, CachedBlobSizeInBytes),
            cached.CachedBlobSizeInBytes);
    if (!host_readable_span(reinterpret_cast<UINT64>(cached.pCachedBlob),
                            static_cast<std::size_t>(cached.CachedBlobSizeInBytes), nullptr))
        return pso_shape_bad(PsoShape::kBadCachedPso,
                             base + offsetof(D3D12_CACHED_PIPELINE_STATE, pCachedBlob),
                             reinterpret_cast<UINT64>(cached.pCachedBlob));
    return {};
}

// 枚举的合法域直接取自 d3d12.h 的首/末枚举值（不是魔法数字），越小越不容易误判。
// MinGW 的 d3d12.h 里 FILL_MODE 是 2..3、CULL_MODE 是 1..3、COMPARISON_FUNC 是 1..8。
inline PsoShapeResult check_graphics_pso_shape(const void* p) {
    constexpr std::size_t kSize = sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC);
    if (!p || host_readable_span(reinterpret_cast<UINT64>(p), kSize, nullptr) < kSize)
        return pso_shape_bad(PsoShape::kNotReadable, 0, reinterpret_cast<UINT64>(p));
    const auto& d = *static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(p);

    // 1) 根签名
    {
        const auto r = host_check_root_signature(
            d.pRootSignature, offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, pRootSignature));
        if (r.shape != PsoShape::kPlausible) return r;
    }
    // 2) 五个着色器阶段（VS/PS/DS/HS/GS 在结构里是连续数组）
    {
        const std::size_t base = offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, VS);
        const PsoShapeResult stages[] = {
            host_check_bytecode(d.VS, base + 0 * sizeof(D3D12_SHADER_BYTECODE)),
            host_check_bytecode(d.PS, base + 1 * sizeof(D3D12_SHADER_BYTECODE)),
            host_check_bytecode(d.DS, base + 2 * sizeof(D3D12_SHADER_BYTECODE)),
            host_check_bytecode(d.HS, base + 3 * sizeof(D3D12_SHADER_BYTECODE)),
            host_check_bytecode(d.GS, base + 4 * sizeof(D3D12_SHADER_BYTECODE)),
        };
        for (const auto& r : stages)
            if (r.shape != PsoShape::kPlausible) return r;
    }
    // 3) 流输出
    {
        constexpr std::size_t kBase =
            offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, StreamOutput);
        const auto& so = d.StreamOutput;
        if (so.NumEntries > 64)
            return pso_shape_bad(PsoShape::kBadStreamOutput,
                                 kBase + offsetof(D3D12_STREAM_OUTPUT_DESC, NumEntries),
                                 so.NumEntries);
        if (so.NumEntries && (!so.pSODeclaration ||
                              !host_readable_span(reinterpret_cast<UINT64>(so.pSODeclaration),
                                                  sizeof(D3D12_SO_DECLARATION_ENTRY) *
                                                      static_cast<std::size_t>(so.NumEntries),
                                                  nullptr)))
            return pso_shape_bad(PsoShape::kBadStreamOutput,
                                 kBase + offsetof(D3D12_STREAM_OUTPUT_DESC, pSODeclaration),
                                 reinterpret_cast<UINT64>(so.pSODeclaration));
        if (so.NumStrides > 64)
            return pso_shape_bad(PsoShape::kBadStreamOutput,
                                 kBase + offsetof(D3D12_STREAM_OUTPUT_DESC, NumStrides),
                                 so.NumStrides);
        if (so.NumStrides && (!so.pBufferStrides ||
                              !host_readable_span(reinterpret_cast<UINT64>(so.pBufferStrides),
                                                  sizeof(UINT) *
                                                      static_cast<std::size_t>(so.NumStrides),
                                                  nullptr)))
            return pso_shape_bad(PsoShape::kBadStreamOutput,
                                 kBase + offsetof(D3D12_STREAM_OUTPUT_DESC, pBufferStrides),
                                 reinterpret_cast<UINT64>(so.pBufferStrides));
        if (so.RasterizedStream > 7)  // 宽松上界：D3D12 实际只用到 0..3
            return pso_shape_bad(PsoShape::kBadStreamOutput,
                                 kBase + offsetof(D3D12_STREAM_OUTPUT_DESC, RasterizedStream),
                                 so.RasterizedStream);
    }
    // 4) 混合状态
    {
        constexpr std::size_t kBlend =
            offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, BlendState);
        constexpr std::size_t kRt = kBlend + offsetof(D3D12_BLEND_DESC, RenderTarget);
        constexpr std::size_t kRtStride = sizeof(D3D12_RENDER_TARGET_BLEND_DESC);
        if (d.BlendState.AlphaToCoverageEnable > 1)
            return pso_shape_bad(PsoShape::kBadBlend,
                                 kBlend + offsetof(D3D12_BLEND_DESC, AlphaToCoverageEnable),
                                 d.BlendState.AlphaToCoverageEnable);
        if (d.BlendState.IndependentBlendEnable > 1)
            return pso_shape_bad(PsoShape::kBadBlend,
                                 kBlend + offsetof(D3D12_BLEND_DESC, IndependentBlendEnable),
                                 d.BlendState.IndependentBlendEnable);
        for (UINT i = 0; i < 8; ++i) {
            // IndependentBlendEnable 为 FALSE 时只有 RT[0] 生效，RT[1..7] 允许为 0
            // （很多标题就是这么初始化的）——只校验"真正会被用到"的那一份，
            // 否则合法 desc 会被误判，日志反而误导下一个人。
            if (i != 0 && !d.BlendState.IndependentBlendEnable) continue;
            const auto& rt = d.BlendState.RenderTarget[i];
            const std::size_t b = kRt + static_cast<std::size_t>(i) * kRtStride;
            if (rt.BlendEnable > 1)
                return pso_shape_bad(PsoShape::kBadBlend,
                                     b + offsetof(D3D12_RENDER_TARGET_BLEND_DESC, BlendEnable),
                                     rt.BlendEnable);
            if (rt.LogicOpEnable > 1)
                return pso_shape_bad(PsoShape::kBadBlend,
                                     b + offsetof(D3D12_RENDER_TARGET_BLEND_DESC, LogicOpEnable),
                                     rt.LogicOpEnable);
            if (rt.SrcBlend < D3D12_BLEND_ZERO || rt.SrcBlend > D3D12_BLEND_INV_ALPHA_FACTOR)
                return pso_shape_bad(PsoShape::kBadBlend,
                                     b + offsetof(D3D12_RENDER_TARGET_BLEND_DESC, SrcBlend),
                                     rt.SrcBlend);
            if (rt.DestBlend < D3D12_BLEND_ZERO || rt.DestBlend > D3D12_BLEND_INV_ALPHA_FACTOR)
                return pso_shape_bad(PsoShape::kBadBlend,
                                     b + offsetof(D3D12_RENDER_TARGET_BLEND_DESC, DestBlend),
                                     rt.DestBlend);
            if (rt.BlendOp < D3D12_BLEND_OP_ADD || rt.BlendOp > D3D12_BLEND_OP_MAX)
                return pso_shape_bad(PsoShape::kBadBlend,
                                     b + offsetof(D3D12_RENDER_TARGET_BLEND_DESC, BlendOp),
                                     rt.BlendOp);
            if (rt.SrcBlendAlpha < D3D12_BLEND_ZERO ||
                rt.SrcBlendAlpha > D3D12_BLEND_INV_ALPHA_FACTOR)
                return pso_shape_bad(
                    PsoShape::kBadBlend,
                    b + offsetof(D3D12_RENDER_TARGET_BLEND_DESC, SrcBlendAlpha),
                    rt.SrcBlendAlpha);
            if (rt.DestBlendAlpha < D3D12_BLEND_ZERO ||
                rt.DestBlendAlpha > D3D12_BLEND_INV_ALPHA_FACTOR)
                return pso_shape_bad(
                    PsoShape::kBadBlend,
                    b + offsetof(D3D12_RENDER_TARGET_BLEND_DESC, DestBlendAlpha),
                    rt.DestBlendAlpha);
            if (rt.BlendOpAlpha < D3D12_BLEND_OP_ADD || rt.BlendOpAlpha > D3D12_BLEND_OP_MAX)
                return pso_shape_bad(PsoShape::kBadBlend,
                                     b + offsetof(D3D12_RENDER_TARGET_BLEND_DESC, BlendOpAlpha),
                                     rt.BlendOpAlpha);
            if (rt.LogicOp > D3D12_LOGIC_OP_OR_INVERTED)
                return pso_shape_bad(PsoShape::kBadBlend,
                                     b + offsetof(D3D12_RENDER_TARGET_BLEND_DESC, LogicOp),
                                     rt.LogicOp);
            if (rt.RenderTargetWriteMask & ~0x0Fu)
                return pso_shape_bad(
                    PsoShape::kBadBlend,
                    b + offsetof(D3D12_RENDER_TARGET_BLEND_DESC, RenderTargetWriteMask),
                    rt.RenderTargetWriteMask);
        }
    }
    // 5) 光栅化状态
    {
        constexpr std::size_t kBase =
            offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, RasterizerState);
        const auto& rs = d.RasterizerState;
        if (rs.FillMode < D3D12_FILL_MODE_WIREFRAME || rs.FillMode > D3D12_FILL_MODE_SOLID)
            return pso_shape_bad(PsoShape::kBadRasterizer,
                                 kBase + offsetof(D3D12_RASTERIZER_DESC, FillMode), rs.FillMode);
        if (rs.CullMode < D3D12_CULL_MODE_NONE || rs.CullMode > D3D12_CULL_MODE_BACK)
            return pso_shape_bad(PsoShape::kBadRasterizer,
                                 kBase + offsetof(D3D12_RASTERIZER_DESC, CullMode), rs.CullMode);
        if (rs.FrontCounterClockwise > 1 || rs.DepthClipEnable > 1 ||
            rs.MultisampleEnable > 1 || rs.AntialiasedLineEnable > 1 ||
            rs.ConservativeRaster > 1)
            return pso_shape_bad(PsoShape::kBadRasterizer, kBase,
                                 static_cast<UINT64>(rs.FrontCounterClockwise) |
                                     (static_cast<UINT64>(rs.DepthClipEnable) << 8) |
                                     (static_cast<UINT64>(rs.MultisampleEnable) << 16) |
                                     (static_cast<UINT64>(rs.AntialiasedLineEnable) << 24) |
                                     (static_cast<UINT64>(rs.ConservativeRaster) << 32));
        // ForcedSampleCount 只允许 0/1/2/4/8（D3D12 的合法集合）
        switch (rs.ForcedSampleCount) {
            case 0: case 1: case 2: case 4: case 8: break;
            default:
                return pso_shape_bad(
                    PsoShape::kBadRasterizer,
                    kBase + offsetof(D3D12_RASTERIZER_DESC, ForcedSampleCount),
                    rs.ForcedSampleCount);
        }
    }
    // 6) 深度/模板状态
    {
        constexpr std::size_t kBase =
            offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, DepthStencilState);
        const auto& ds = d.DepthStencilState;
        // DepthEnable / StencilEnable 为 FALSE 时对应功能不用，标题常把相关字段留 0；
        // 这种 desc 桌面是收的，所以只在功能开启时校验对应枚举（避免误判合法 desc）。
        if (ds.DepthEnable > 1)
            return pso_shape_bad(PsoShape::kBadDepthStencil,
                                 kBase + offsetof(D3D12_DEPTH_STENCIL_DESC, DepthEnable),
                                 ds.DepthEnable);
        if (ds.DepthWriteMask > D3D12_DEPTH_WRITE_MASK_ALL)
            return pso_shape_bad(PsoShape::kBadDepthStencil,
                                 kBase + offsetof(D3D12_DEPTH_STENCIL_DESC, DepthWriteMask),
                                 ds.DepthWriteMask);
        if (ds.DepthEnable &&
            (ds.DepthFunc < D3D12_COMPARISON_FUNC_NEVER ||
             ds.DepthFunc > D3D12_COMPARISON_FUNC_ALWAYS))
            return pso_shape_bad(PsoShape::kBadDepthStencil,
                                 kBase + offsetof(D3D12_DEPTH_STENCIL_DESC, DepthFunc),
                                 ds.DepthFunc);
        if (ds.StencilEnable > 1)
            return pso_shape_bad(PsoShape::kBadDepthStencil,
                                 kBase + offsetof(D3D12_DEPTH_STENCIL_DESC, StencilEnable),
                                 ds.StencilEnable);
        const D3D12_DEPTH_STENCILOP_DESC* faces[] = {&ds.FrontFace, &ds.BackFace};
        const std::size_t face_offsets[] = {
            kBase + offsetof(D3D12_DEPTH_STENCIL_DESC, FrontFace),
            kBase + offsetof(D3D12_DEPTH_STENCIL_DESC, BackFace)};
        for (int f = 0; f < 2 && ds.StencilEnable; ++f) {
            const auto& face = *faces[f];
            const std::size_t b = face_offsets[f];
            const D3D12_COMPARISON_FUNC func = face.StencilFunc;
            if (func < D3D12_COMPARISON_FUNC_NEVER || func > D3D12_COMPARISON_FUNC_ALWAYS)
                return pso_shape_bad(
                    PsoShape::kBadDepthStencil,
                    b + offsetof(D3D12_DEPTH_STENCILOP_DESC, StencilFunc), func);
            const D3D12_STENCIL_OP ops[] = {face.StencilFailOp, face.StencilDepthFailOp,
                                            face.StencilPassOp};
            if (ops[0] < D3D12_STENCIL_OP_KEEP || ops[0] > D3D12_STENCIL_OP_DECR ||
                ops[1] < D3D12_STENCIL_OP_KEEP || ops[1] > D3D12_STENCIL_OP_DECR ||
                ops[2] < D3D12_STENCIL_OP_KEEP || ops[2] > D3D12_STENCIL_OP_DECR)
                return pso_shape_bad(
                    PsoShape::kBadDepthStencil,
                    b + offsetof(D3D12_DEPTH_STENCILOP_DESC, StencilFailOp),
                    static_cast<UINT64>(ops[0]) | (static_cast<UINT64>(ops[1]) << 8) |
                        (static_cast<UINT64>(ops[2]) << 16));
        }
    }
    // 7) 输入布局
    {
        constexpr std::size_t kBase =
            offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, InputLayout);
        if (d.InputLayout.NumElements > 128)
            return pso_shape_bad(PsoShape::kBadInputLayout,
                                 kBase + offsetof(D3D12_INPUT_LAYOUT_DESC, NumElements),
                                 d.InputLayout.NumElements);
        if (d.InputLayout.NumElements &&
            (!d.InputLayout.pInputElementDescs ||
             !host_readable_span(
                 reinterpret_cast<UINT64>(d.InputLayout.pInputElementDescs),
                 sizeof(D3D12_INPUT_ELEMENT_DESC) *
                     static_cast<std::size_t>(d.InputLayout.NumElements),
                 nullptr)))
            return pso_shape_bad(PsoShape::kBadInputLayout,
                                 kBase + offsetof(D3D12_INPUT_LAYOUT_DESC, pInputElementDescs),
                                 reinterpret_cast<UINT64>(d.InputLayout.pInputElementDescs));
    }
    // 8) 其余小字段
    if (d.IBStripCutValue > D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF)
        return pso_shape_bad(
            PsoShape::kBadStripCut,
            offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, IBStripCutValue), d.IBStripCutValue);
    if (d.PrimitiveTopologyType > D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH)
        return pso_shape_bad(PsoShape::kBadTopology,
                             offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, PrimitiveTopologyType),
                             d.PrimitiveTopologyType);
    if (d.NumRenderTargets > 8)
        return pso_shape_bad(PsoShape::kBadRenderTargets,
                             offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, NumRenderTargets),
                             d.NumRenderTargets);
    for (UINT i = 0; i < d.NumRenderTargets; ++i) {
        const UINT fmt = static_cast<UINT>(d.RTVFormats[i]);
        // 0 = DXGI_FORMAT_UNKNOWN：有 RTV 时桌面一定拒绝，属于"形状不合法"
        if (fmt == 0 || fmt > 0xC0)
            return pso_shape_bad(
                PsoShape::kBadRtvFormat,
                offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, RTVFormats) +
                    static_cast<std::size_t>(i) * sizeof(DXGI_FORMAT),
                fmt);
    }
    {
        const UINT fmt = static_cast<UINT>(d.DSVFormat);
        if (fmt > 0xC0)
            return pso_shape_bad(PsoShape::kBadDsvFormat,
                                 offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, DSVFormat), fmt);
    }
    {
        const UINT n = d.SampleDesc.Count;
        if (n != 1 && n != 2 && n != 4 && n != 8 && n != 16)
            return pso_shape_bad(PsoShape::kBadSample,
                                 offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, SampleDesc) +
                                     offsetof(DXGI_SAMPLE_DESC, Count),
                                 n);
        if (d.SampleDesc.Quality > 0xFFFFu)
            return pso_shape_bad(PsoShape::kBadSample,
                                 offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, SampleDesc) +
                                     offsetof(DXGI_SAMPLE_DESC, Quality),
                                 d.SampleDesc.Quality);
    }
    if (d.NodeMask & ~1u)  // 单节点主机；实测槽 8 的队列描述符 nodeMask=1
        return pso_shape_bad(PsoShape::kBadNodeMask,
                             offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, NodeMask),
                             d.NodeMask);
    {
        const auto r = host_check_cached_pso(
            d.CachedPSO, offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, CachedPSO));
        if (r.shape != PsoShape::kPlausible) return r;
    }
    if (d.Flags & ~0xFu)  // 宽松上界：桌面只有 DEBUG=0x1，主机可能多几位
        return pso_shape_bad(PsoShape::kBadFlags,
                             offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, Flags), d.Flags);
    return {};
}

inline PsoShapeResult check_compute_pso_shape(const void* p) {
    constexpr std::size_t kSize = sizeof(D3D12_COMPUTE_PIPELINE_STATE_DESC);
    if (!p || host_readable_span(reinterpret_cast<UINT64>(p), kSize, nullptr) < kSize)
        return pso_shape_bad(PsoShape::kNotReadable, 0, reinterpret_cast<UINT64>(p));
    const auto& d = *static_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(p);
    {
        const auto r = host_check_root_signature(
            d.pRootSignature, offsetof(D3D12_COMPUTE_PIPELINE_STATE_DESC, pRootSignature));
        if (r.shape != PsoShape::kPlausible) return r;
    }
    {
        const auto r =
            host_check_bytecode(d.CS, offsetof(D3D12_COMPUTE_PIPELINE_STATE_DESC, CS));
        if (r.shape != PsoShape::kPlausible) return r;
    }
    if (d.NodeMask & ~1u)
        return pso_shape_bad(PsoShape::kBadNodeMask,
                             offsetof(D3D12_COMPUTE_PIPELINE_STATE_DESC, NodeMask), d.NodeMask);
    {
        const auto r = host_check_cached_pso(
            d.CachedPSO, offsetof(D3D12_COMPUTE_PIPELINE_STATE_DESC, CachedPSO));
        if (r.shape != PsoShape::kPlausible) return r;
    }
    if (d.Flags & ~0xFu)
        return pso_shape_bad(PsoShape::kBadFlags,
                             offsetof(D3D12_COMPUTE_PIPELINE_STATE_DESC, Flags), d.Flags);
    return {};
}

// ---- 实验入口（默认关）：XBGTASIX_HOST_PSO_TRANSLATE=1 ----
// 每次调用都读一次环境变量（不是 static 缓存）：这样"先跑默认、再开开关"的实验
// 在同一进程内也成立（测试里要用到），PSO 创建本来就不是热路径。
inline bool host_pso_translate_requested() {
    char value[8] = {0};
    return GetEnvironmentVariableA("XBGTASIX_HOST_PSO_TRANSLATE", value, sizeof(value)) > 0 &&
           value[0] == '1';
}

// 图形 PSO 的**可行性实验**：形状自检通过时，用桌面布局**原样**试建一次
// （不做任何字段改写 —— 在布局未知时任何改写都是猜测），记 HRESULT 后立刻 Release。
// **不写回游戏的 out 参数**：它的位置/语义未知，把指针塞进来源不明的内存比明确失败
// 危险得多。因此这个函数不改变桩的返回值（仍是 E_NOTIMPL）。
inline void host_pso_try_create_graphics(DeviceProxy* self, UINT64 desc_addr) {
    if (!self->pc) {
        xbgtasix::logf("[d3d12_xs] slot=60 实验直通跳过：没有真实桌面设备（self->pc 为空）");
        return;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::memcpy(&desc, reinterpret_cast<const void*>(desc_addr), sizeof(desc));
    ID3D12PipelineState* created = nullptr;
    HRESULT hr = E_FAIL;
    {
        // 驱动编译可能几十秒；不标成"仅主机侧停顿"会被标题的开机 hang check 杀掉
        xbgtasix::host_stall::scope stall;
        hr = static_cast<ID3D12Device*>(self->pc)->CreateGraphicsPipelineState(
            &desc, __uuidof(ID3D12PipelineState),
            reinterpret_cast<void**>(&created));
    }
    xbgtasix::logf("[d3d12_xs] slot=60 实验直通 CreateGraphicsPipelineState("
                   "VS=%lluB PS=%lluB NumRT=%u RTV0=%d DSV=%d sample=%u/%u nodeMask=0x%x "
                   "cached=%lluB) -> hr=0x%08lx pso=%p（已释放；桩仍返回 E_NOTIMPL）",
                   static_cast<unsigned long long>(desc.VS.BytecodeLength),
                   static_cast<unsigned long long>(desc.PS.BytecodeLength),
                   desc.NumRenderTargets, static_cast<int>(desc.RTVFormats[0]),
                   static_cast<int>(desc.DSVFormat), desc.SampleDesc.Count,
                   desc.SampleDesc.Quality, desc.NodeMask,
                   static_cast<unsigned long long>(desc.CachedPSO.CachedBlobSizeInBytes),
                   static_cast<unsigned long>(hr), static_cast<void*>(created));
    if (created) created->Release();
}

// 主机 PSO 槽的统一桩：8 个参数全部记录，指针能读就 dump 64 B，然后**明确失败**。
// 参数 5..8（e..h）在调用方只传了更少参数时读到的是调用方的影子空间/栈 ——
// 这是安全的（只是读内存），但**可能不是真参数**，日志里要按"疑似"看待。
template <std::size_t N>
HRESULT WINAPI host_pso_slot_stub(DeviceProxy* self, UINT64 a = 0, UINT64 b = 0, UINT64 c = 0,
                                  UINT64 d = 0, UINT64 e = 0, UINT64 f = 0, UINT64 g = 0,
                                  UINT64 h = 0) {
    static_assert(is_host_pso_slot(N), "host_pso_slot_stub 只用于 55/60/61/62/66/67");
    static std::atomic<int> detailed{24};  // 每个槽前 24 次调用打完整 dump
    static std::atomic<UINT64> calls{0};
    const UINT64 call_index = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const UINT64 args[8] = {a, b, c, d, e, f, g, h};
    (void)self;

    if (detailed.fetch_sub(1, std::memory_order_relaxed) > 0) {
        // 调用方返回地址必须在本函数自己的栈帧里取：__builtin_return_address(0)
        // 放进 helper 会变成 helper 的调用者。所以这里重复了通用桩里那几行。
        const auto* caller = static_cast<const unsigned char*>(__builtin_return_address(0));
        HMODULE caller_module = nullptr;
        unsigned long long rva = reinterpret_cast<unsigned long long>(caller);
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(caller), &caller_module) &&
            caller_module) {
            rva = reinterpret_cast<unsigned long long>(caller) -
                  reinterpret_cast<unsigned long long>(caller_module);
        }
        xbgtasix::logf("[d3d12_xs] 主机 PSO 槽 slot=%zu（%s）第 %llu 次调用 调用方=+0x%llX "
                       "a=0x%llx b=0x%llx c=0x%llx d=0x%llx e=0x%llx f=0x%llx g=0x%llx "
                       "h=0x%llx -> E_NOTIMPL（签名未实测，见 device_abi.h 该段注释）",
                       N, host_pso_slot_name(N), static_cast<unsigned long long>(call_index),
                       rva, static_cast<unsigned long long>(a),
                       static_cast<unsigned long long>(b),
                       static_cast<unsigned long long>(c),
                       static_cast<unsigned long long>(d),
                       static_cast<unsigned long long>(e),
                       static_cast<unsigned long long>(f),
                       static_cast<unsigned long long>(g),
                       static_cast<unsigned long long>(h));
        for (int i = 0; i < 8; ++i) {
            const UINT64 addr = args[i];
            if (addr < 0x10000) continue;  // 小整数/空指针：不是可指向的地址
            MEMORY_BASIC_INFORMATION m{};
            const std::size_t span = host_readable_span(addr, 64, &m);
            const char tag = static_cast<char>('a' + i);
            if (span == 0) {
                xbgtasix::logf("[d3d12_xs]   slot=%zu %c=0x%llx 不可读（未提交/守卫页/"
                               "非用户地址），跳过 dump",
                               N, tag, static_cast<unsigned long long>(addr));
                continue;
            }
            unsigned char bytes[64] = {0};
            std::memcpy(bytes, reinterpret_cast<const void*>(addr), span);
            UINT64 first8 = 0;
            if (span >= 8) std::memcpy(&first8, bytes, sizeof(first8));
            const bool module_like = host_is_module_address(first8);
            const unsigned guid_version = span >= 16 ? host_guid_version(bytes) : 0;
            char note[192] = {0};
            if (module_like) {
                std::snprintf(note, sizeof(note),
                              " | 首 8 B=0x%llx 在模块内（疑似 vtable/函数指针）",
                              static_cast<unsigned long long>(first8));
            } else if (guid_version != 0) {
                std::snprintf(note, sizeof(note),
                              " | 前 16 B 像 GUID（版本 %u，疑似 REFIID 参数）", guid_version);
            }
            xbgtasix::logf("[d3d12_xs]   slot=%zu %c=0x%llx 区域基址=%p 大小=0x%llx "
                           "保护=0x%lx 可读=0x%zx B%s",
                           N, tag, static_cast<unsigned long long>(addr), m.BaseAddress,
                           static_cast<unsigned long long>(m.RegionSize),
                           static_cast<unsigned long>(m.Protect), span, note);
            for (std::size_t off = 0; off < 64 && off < span; off += 16) {
                char hex[3 * 16 + 1] = {0};
                for (std::size_t k = 0; k < 16 && off + k < span; ++k) {
                    std::snprintf(hex + k * 3, 4, "%02x ",
                                  static_cast<unsigned>(bytes[off + k]));
                }
                xbgtasix::logf("[d3d12_xs]     slot=%zu %c+0x%02zx: %s", N, tag, off, hex);
            }
        }
        // 60/62：对每个参数做"按桌面 desc 布局读是否合法"的自检。
        // 通过 = 最可能的 desc 参数；不合格 = 直接给出第一个非法字段的偏移与值。
        if constexpr (N == kHostGraphicsPsoSlot || N == kHostComputePsoSlot) {
            for (int i = 0; i < 8; ++i) {
                const UINT64 addr = args[i];
                if (addr < 0x10000) continue;
                const PsoShapeResult shape =
                    N == kHostGraphicsPsoSlot
                        ? check_graphics_pso_shape(reinterpret_cast<const void*>(addr))
                        : check_compute_pso_shape(reinterpret_cast<const void*>(addr));
                const char tag = static_cast<char>('a' + i);
                if (shape.shape == PsoShape::kPlausible) {
                    xbgtasix::logf("[d3d12_xs]   slot=%zu %c=0x%llx 形状自检=通过"
                                   "（按桌面 %s 读全部合法 → 最可能的 desc 参数）",
                                   N, tag, static_cast<unsigned long long>(addr),
                                   N == kHostGraphicsPsoSlot
                                       ? "GRAPHICS_PIPELINE_STATE_DESC"
                                       : "COMPUTE_PIPELINE_STATE_DESC");
                } else if (shape.shape != PsoShape::kNotReadable) {
                    xbgtasix::logf("[d3d12_xs]   slot=%zu %c=0x%llx 形状自检=不合格 "
                                   "原因=%s 字段偏移=+0x%zx 值=0x%llx",
                                   N, tag, static_cast<unsigned long long>(addr),
                                   pso_shape_name(shape.shape), shape.offset,
                                   static_cast<unsigned long long>(shape.value));
                }
            }
        }
    } else if (call_index % 256 == 0) {
        xbgtasix::logf("[d3d12_xs] 主机 PSO 槽 slot=%zu 已调用 %llu 次"
                       "（完整 dump 只打每个槽的前 24 次）",
                       N, static_cast<unsigned long long>(call_index));
    }

    // 唯一的真实创建入口：默认关，形状自检通过才试，且结果只写日志、不返回给调用方。
    if constexpr (N == kHostGraphicsPsoSlot) {
        if (host_pso_translate_requested()) {
            const PsoShapeResult shape =
                check_graphics_pso_shape(reinterpret_cast<const void*>(a));
            if (shape.shape == PsoShape::kPlausible) {
                host_pso_try_create_graphics(self, a);
            } else {
                xbgtasix::logf("[d3d12_xs] slot=60 实验直通跳过：a=0x%llx 形状自检=不合格 "
                               "原因=%s 字段偏移=+0x%zx 值=0x%llx",
                               static_cast<unsigned long long>(a),
                               pso_shape_name(shape.shape), shape.offset,
                               static_cast<unsigned long long>(shape.value));
            }
        }
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

// 命令列表的槽位转发：**不用裸汇编**。
// 设备侧同样的写法被证明在 MinGW 下不可靠（t41 崩在 D3D12Core），
// 这里也换成 C 透传。实测崩溃点 d3d12_xs+0x23EB4 就落在这类跳板里
// （forward_list_this<189>）。
// 边界：浮点参数会丢（命令列表方法没有）；超过 9 个参数丢尾部（D3D12 命令列表最多 8 个）。
// PC 的 ID3D12GraphicsCommandList7 只有约 70 个方法；主机的命令列表 vtable 到 600+。
// 把主机高位槽（>= 80）按同号转发 = 读越界后跳到垃圾地址（实测崩在 forward_list_slot<189>）。
// 未建模的高位槽一律记一次并返回 S_OK（命令列表扩展方法多为 void 语义，返回 S_OK 最安全）。
template <std::size_t N>
HRESULT WINAPI list_extension_stub(void* self, uintptr_t a1 = 0, uintptr_t a2 = 0,
                                   uintptr_t a3 = 0) {
    static std::atomic<int> budget{40};
    if (budget.fetch_sub(1) > 0) {
        const auto* caller = static_cast<const unsigned char*>(__builtin_return_address(0));
        HMODULE mod = nullptr;
        unsigned long long rva = reinterpret_cast<unsigned long long>(caller);
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(caller), &mod) && mod) {
            rva = reinterpret_cast<unsigned long long>(caller) -
                  reinterpret_cast<unsigned long long>(mod);
        }
        xbgtasix::logf("[d3d12_xs] 未建模的命令列表扩展 slot=%zu 调用方=+0x%llX "
                       "a=0x%llx b=0x%llx c=0x%llx -> S_OK", N, rva,
                       static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                       static_cast<unsigned long long>(a3));
    }
    (void)self;
    return S_OK;
}

template <std::size_t N>
uintptr_t WINAPI forward_list_slot(void* self, uintptr_t a1 = 0, uintptr_t a2 = 0,
                                   uintptr_t a3 = 0, uintptr_t a4 = 0, uintptr_t a5 = 0,
                                   uintptr_t a6 = 0, uintptr_t a7 = 0, uintptr_t a8 = 0,
                                   uintptr_t a9 = 0) {
    auto* proxy = static_cast<CommandListProxy*>(self);
    using Fn = uintptr_t (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                             uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    const auto vt = *reinterpret_cast<void***>(static_cast<ID3D12CommandList*>(proxy->pc));
    return reinterpret_cast<Fn>(vt[N])(proxy->pc, a1, a2, a3, a4, a5, a6, a7, a8, a9);
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
    // 高位槽不转发（PC vtable 没有），低位按 C 透传
    std::array<Slot, sizeof...(N)> v{
        ((N < 80) ? reinterpret_cast<Slot>(&forward_list_slot<N>)
                  : reinterpret_cast<Slot>(&list_extension_stub<N>))...};
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
    // 先把"谁调的、原始参数"记下来，再进 present_one_plane ——
    // 否则内部一崩，就没有任何线索了（上一次就是这么丢的）。
    {
        const auto* caller = static_cast<const unsigned char*>(__builtin_return_address(0));
        HMODULE mod = nullptr;
        unsigned long long rva = reinterpret_cast<unsigned long long>(caller);
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(caller), &mod) && mod) {
            rva = reinterpret_cast<unsigned long long>(caller) -
                  reinterpret_cast<unsigned long long>(mod);
        }
        static std::atomic<int> budget{8};
        if (budget.fetch_sub(1) > 0) {
            xbgtasix::logf("[d3d12_xs] 队列槽30 PresentX 调用方=+0x%llX planes=%u ptr=%p params=%p",
                           rva, plane_count, planes, params);
        }
    }
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
    // ⚠️ 未定论的开放问题：静态反汇编显示"主机槽 = 桌面槽 + 4"（9 个点交叉验证：
    // 8=CreateCommandQueue/PC4、14=CreateDescriptorHeap/PC10、15=IncrementSize/PC11、
    // 19=UAV/PC15、20=RTV/PC16、21=DSV/PC17、25=AllocInfo/PC21、26=CustomHeap/PC22、
    // 36=CreateFence/PC32），但**实测按 -4 转发会让设备在更早处被移除**（0x887a0005），
    // 而同号转发能推进到 game+0x247AF2D。两者矛盾，暂按同号（能跑通的状态），
    // 待逐槽用 call-site RVA + 参数断言确认后再改。见 docs/host-vtable-offset.md。
    const std::size_t pc_index = N;
    auto fn = reinterpret_cast<Fn>(pc_vtable[pc_index]);
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
            // 注意：dev3 只在函数末尾释放一次。这里若再 Release 会二次释放（GPT 指出的 bug）。
            if (SUCCEEDED(hr)) {
                dev3->Release();
                return hr;
            }
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

// 主机槽位 127 = 用**已观测参数**对照出来的 CreateCommittedResource3
// （ID3D12Device10，10 个参数）。证据来自调用点 game+0x193E1B4：
//     RDX = {1,0,0,1,1}      -> D3D12_HEAP_PROPERTIES(DEFAULT)
//     R8D = 8                -> D3D12_HEAP_FLAG_ALLOW_DISPLAY
//     R9  = 资源描述指针      -> 按 D3D12_RESOURCE_DESC1 解读
//     [rsp+0x20] = 3         -> D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS
//     [rsp+0x28..0x40]       -> clear value / 空 protected session / 0 个 castable 格式
//     [rsp+0x48]/[0x50]      -> IID_ID3D12Resource / 输出指针
//
// MinGW 头文件没有 ID3D12Device10，但有 ID3D12Device8::CreateCommittedResource2
// （签名少 NumCastableFormats/pCastableFormats 两项，而我们观测到的正是 0 和空指针），
// 所以降级用它，语义等价。
inline HRESULT WINAPI create_committed_resource3_x(
    DeviceProxy* self, const D3D12_HEAP_PROPERTIES* heap_props, UINT heap_flags,
    const D3D12_RESOURCE_DESC1* desc1, UINT layout, const D3D12_CLEAR_VALUE* clear,
    void* protected_session, UINT num_castable, const void* castable, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!desc1) return E_INVALIDARG;

    xbgtasix::logf("[d3d12_xs] 槽127 CreateCommittedResource3: heapProps=%p flags=0x%x "
                   "desc1=%p layout=%u clear=%p session=%p castable=%u/%p",
                   static_cast<const void*>(heap_props), heap_flags,
                   static_cast<const void*>(desc1), layout,
                   static_cast<const void*>(clear), protected_session, num_castable, castable);
    xbgtasix::logf("[d3d12_xs] 槽127 desc1: dim=%d align=0x%llx w=%llu h=%u arr=%u mip=%u fmt=%d "
                   "samples=%u/0x%x flags=0x%x",
                   static_cast<int>(desc1->Dimension),
                   static_cast<unsigned long long>(desc1->Alignment),
                   static_cast<unsigned long long>(desc1->Width), desc1->Height,
                   desc1->DepthOrArraySize, desc1->MipLevels, static_cast<int>(desc1->Format),
                   desc1->SampleDesc.Count, desc1->SampleDesc.Quality,
                   static_cast<unsigned>(desc1->Flags));

    // 主机资源标志含 PC 不认的位（实测 0x20000），先剥到标准位
    D3D12_RESOURCE_DESC1 pc_desc = *desc1;
    const auto original_flags = pc_desc.Flags;
    pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(pc_desc.Flags & 0x3F);
    if (original_flags != pc_desc.Flags) {
        xbgtasix::logf("[d3d12_xs] 槽127 剥离主机资源标志 0x%x -> 0x%x",
                       static_cast<unsigned>(original_flags), static_cast<unsigned>(pc_desc.Flags));
    }

    ID3D12Device8* dev8 = nullptr;
    const HRESULT hr_qi = static_cast<ID3D12Device*>(self->pc)->QueryInterface(IID_PPV_ARGS(&dev8));
    if (FAILED(hr_qi) || !dev8) {
        xbgtasix::logf("[d3d12_xs] 槽127 取 ID3D12Device8 失败 hr=0x%08lx",
                       static_cast<unsigned long>(hr_qi));
        return FAILED(hr_qi) ? hr_qi : E_NOINTERFACE;
    }

    // 初始布局：3 = UNORDERED_ACCESS。PC 上对应资源状态；失败再退 COMMON。
    auto try_create = [&](D3D12_RESOURCE_STATES state, UINT flags) {
        D3D12_HEAP_PROPERTIES props{};
        if (heap_props) props = *heap_props;
        else { props.Type = D3D12_HEAP_TYPE_DEFAULT; props.CreationNodeMask = 1; props.VisibleNodeMask = 1; }
        return dev8->CreateCommittedResource2(&props, static_cast<D3D12_HEAP_FLAGS>(flags),
                                              &pc_desc, state, clear, nullptr, riid, out);
    };
    HRESULT hr = try_create(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, heap_flags);
    if (FAILED(hr)) {
        xbgtasix::logf("[d3d12_xs] 槽127 首次失败 hr=0x%08lx（state=UNORDERED_ACCESS flags=0x%x）",
                       static_cast<unsigned long>(hr), heap_flags);
        hr = try_create(D3D12_RESOURCE_STATE_COMMON, heap_flags);
    }
    if (FAILED(hr) && (heap_flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY)) {
        hr = try_create(D3D12_RESOURCE_STATE_COMMON,
                        heap_flags & ~static_cast<UINT>(D3D12_HEAP_FLAG_ALLOW_DISPLAY));
        xbgtasix::logf("[d3d12_xs] 槽127 去掉 ALLOW_DISPLAY 重试 hr=0x%08lx",
                       static_cast<unsigned long>(hr));
    }
    dev8->Release();
    xbgtasix::logf("[d3d12_xs] 槽127 -> hr=0x%08lx out=%p",
                   static_cast<unsigned long>(hr), out ? *out : nullptr);
    return hr;
}

// 主机槽位 25 = GetResourceAllocationInfo，但 **ABI 是反的**：
//   RCX  = D3D12_RESOURCE_ALLOCATION_INFO* 输出缓冲   ← 不是 this！
//   RDX  = 设备 this
//   R8D  = visibleMask            R9D = numResourceDescs
//   [rsp+0x20] = 资源描述指针
// 证据：调用点 game+0x344182 是 `call *0xC8(%rax)`（0xC8/8 = 25），
// 且调用方第一个动作就是把返回的 Alignment 和 0x1000 比较，不等则清零重查一次。
//
// 通用转发会把 RCX 里的输出缓冲当成设备代理 → 崩。必须显式适配。
// 桌面签名是 (this, visibleMask, numDescs, pDescs) 并**按值返回**结构，
// 所以这里把返回结构写回调用方的缓冲，再把缓冲地址返回（主机约定）。
inline D3D12_RESOURCE_ALLOCATION_INFO* WINAPI get_allocation_info_x(
    D3D12_RESOURCE_ALLOCATION_INFO* out, DeviceProxy* self, UINT visible_mask,
    UINT num_resource_descs, const D3D12_RESOURCE_DESC* descs) {
    if (!out) return out;
    static std::atomic<int> budget{24};
    const bool log = budget.fetch_sub(1) > 0;
    if (!num_resource_descs || !descs) {
        out->SizeInBytes = 0;
        out->Alignment = 0;
        return out;
    }
    // 资源标志转换与实际创建路径保持一致（剥掉 PC 不认的主机位）
    D3D12_RESOURCE_DESC masked[8]{};
    const UINT n = num_resource_descs < 8 ? num_resource_descs : 8;
    for (UINT i = 0; i < n; ++i) {
        masked[i] = descs[i];
        masked[i].Flags = static_cast<D3D12_RESOURCE_FLAGS>(masked[i].Flags & 0x3F);
    }
    if (log) {
        xbgtasix::logf("[d3d12_xs] 槽25 GetResourceAllocationInfo: out=%p visible=0x%x num=%u "
                       "desc0={dim=%d w=%llu h=%u fmt=%d flags=0x%x->0x%x align=0x%llx}",
                       static_cast<void*>(out), visible_mask, num_resource_descs,
                       static_cast<int>(descs[0].Dimension),
                       static_cast<unsigned long long>(descs[0].Width), descs[0].Height,
                       static_cast<int>(descs[0].Format), static_cast<unsigned>(descs[0].Flags),
                       static_cast<unsigned>(masked[0].Flags),
                       static_cast<unsigned long long>(descs[0].Alignment));
    }
    const D3D12_RESOURCE_ALLOCATION_INFO info =
        // 注意：必须传 masked（剥离主机标志后的），传原始 descs 会拿到
        // SizeInBytes = 0xFFFFFFFFFFFFFFFF（桌面认为描述无效）
        static_cast<ID3D12Device*>(self->pc)->GetResourceAllocationInfo(visible_mask, n, masked);
    out->SizeInBytes = info.SizeInBytes;
    out->Alignment = info.Alignment;
    if (log) {
        xbgtasix::logf("[d3d12_xs] 槽25 -> SizeInBytes=%llu Alignment=0x%llx",
                       static_cast<unsigned long long>(info.SizeInBytes),
                       static_cast<unsigned long long>(info.Alignment));
    }
    return out;
}

// 主机槽 15 = GetDescriptorHandleIncrementSize（桌面槽 11）。
// 实测调用点 game+0x2478E2F 把返回值存进全局 0x1475E7000，之后既当描述符步进
// （`add %rax,%rdi`）又当 slot 70 后 `clwb` 的回写长度。同号透传会打到桌面
// CreateUnorderedAccessView 返回 S_OK=0 → **步进变成 0 → 16 个描述符全写同一地址**。
// 标题自己的描述符 arena 是按 64 B/条目记账的（802816 项 x 64），所以按 0x40 返回。
inline UINT WINAPI get_descriptor_increment_x(DeviceProxy* self, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    constexpr UINT kHostIncrement = 0x40;
    const UINT pc_value = static_cast<ID3D12Device*>(self->pc)->GetDescriptorHandleIncrementSize(type);
    // 实验开关（默认关）：0x40 这个值当初是为了让"标题按 64 B/条目记账"的 arena 对上，
    // 但它被**无条件**用于所有堆类型。实测 slot 70 的目标句柄里两个描述符索引相隔 0x20，
    // 与 0x40 矛盾，所以这里可以按驱动真值走一段，用崩溃点是否移动来判定。
    //   XBGTASIX_INCREMENT_PC=1 -> 全部用驱动值
    //   XBGTASIX_INCREMENT_PC=2 -> 只有非 CBV_SRV_UAV 堆用驱动值
    static const int mode = [] {
        const char* v = std::getenv("XBGTASIX_INCREMENT_PC");
        return v ? std::atoi(v) : 0;
    }();
    UINT answer = kHostIncrement;
    if (mode == 1) {
        answer = pc_value;
    } else if (mode == 2 && type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        answer = pc_value;
    }
    static std::atomic<int> budget{16};
    if (budget.fetch_sub(1) > 0) {
        xbgtasix::logf("[d3d12_xs] 槽15 GetDescriptorHandleIncrementSize(type=%d) -> 0x%x"
                       "（桌面值 0x%x；原策略 64B；实验模式=%d）",
                       static_cast<int>(type), answer, pc_value, mode);
    }
    return answer;
}

// 消费端根签名剥离：序列化时我们把主机位写回了 blob（主机侧反序列化器需要），
// 但桌面 CreateRootSignature 不收带主机位的 blob（实测 0x80004005 E_FAIL）。
// 这里在调用桌面之前把 RTS0 段的主机位清掉。
// 安全性：blob 不是合法 DXBC / 没有 RTS0 / 本来就没有主机位时，**一个字节都不改**，
// 只是原样透传 —— 所以即使这个槽不是 CreateRootSignature 也不会破坏调用。
template <std::size_t N>
uintptr_t WINAPI forward_slot_strip_root(DeviceProxy* proxy, uintptr_t a1 = 0, uintptr_t a2 = 0,
                                         uintptr_t a3 = 0, uintptr_t a4 = 0, uintptr_t a5 = 0,
                                         uintptr_t a6 = 0, uintptr_t a7 = 0, uintptr_t a8 = 0,
                                         uintptr_t a9 = 0) {
    // 根签名创建的参数形状：(nodeMask, pBlob, blobSizeInBytes, riid, ppv) —— blob 在 a2/a3。
    // 对 a1..a3 里所有"看起来像 DXBC 且带主机位"的缓冲区尝试剥位（会在拷贝上做）。
    auto* blob = reinterpret_cast<std::uint8_t*>(a2);
    const auto blob_size = static_cast<std::size_t>(a3);
    if (blob && blob_size >= 32 && blob_size <= (64u << 20) &&
        std::memcmp(blob, "DXBC", 4) == 0) {
        std::uint32_t bits = 0;
        std::size_t at = 0;
        // 直接就地剥（这块内存是游戏传进来的 blob，桌面只读它，就地改是可接受的：
        // 主机侧拿到的仍是同一份数据，但主机位已经是"要传给桌面"的值；
        // 若游戏之后还要用原始位，它会重新序列化）。
        if (xbgtasix::rootsig::strip_blob_for_pc(blob, blob_size, &bits, &at)) {
            static std::atomic<int> n{0};
            const int k = n.fetch_add(1, std::memory_order_relaxed);
            if (k < 16) {
                xbgtasix::logf("[d3d12_xs] 消费端根签名剥离：blob=%p size=%zu 主机位=0x%x 位置=+%zu"
                               "（桌面不收带主机位的 blob）", static_cast<void*>(blob), blob_size,
                               bits, at);
            }
        }
    }
    using Fn = uintptr_t (*)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                             uintptr_t, uintptr_t, uintptr_t, uintptr_t);
    const auto vt = *reinterpret_cast<void***>(static_cast<ID3D12Device*>(proxy->pc));
    const std::size_t pc_index = N;
    return reinterpret_cast<Fn>(vt[pc_index])(proxy->pc, a1, a2, a3, a4, a5, a6, a7, a8, a9);
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
    if constexpr (N == 127) return reinterpret_cast<Slot>(&create_committed_resource3_x);
    if constexpr (N == 25) return reinterpret_cast<Slot>(&get_allocation_info_x);
    // 根签名创建（主机槽号未定论：12 与 16 都挂上，剥离函数对非根签名数据是无操作）
    if constexpr (N == 12 || N == 16) return reinterpret_cast<Slot>(&forward_slot_strip_root<N>);
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
    // 主机 PSO 相关槽：55/60/61/62/66/67 走带完整参数 dump 的专用桩（仍是 E_NOTIMPL，
    // 但打出槽号、调用方 RVA、8 个参数、可读指针的前 64 B 与桌面 desc 形状自检）。
    if constexpr (is_host_pso_slot(N))
        return reinterpret_cast<Slot>(&host_pso_slot_stub<N>);
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
