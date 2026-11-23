// SPDX-License-Identifier: GPL-3.0-or-later
// console_hw.cpp —— 主机硬件抽象层（APU / DSP / GPU swizzle / DirectStorage 门面）
//
// 为什么需要这一组 DLL：游戏 exe 的导入表里有 5 个模块**不在包里**，
// 因为它们在真实主机上来自系统分区：
//
//   AcpHal.dll   APU（音频处理器）HAL：ApuAlloc / ApuCreateHeap / ApuMapApuAddress …
//   xapu.dll     APU 命令队列：XApuConnect / XApuEnqueueCommand / XApuDequeueResult
//   chad.dll     音频 DSP：XDspActivate / XDspSubmitCommand / XDspConnect…
//   xg_xs.dll    Xbox GPU 纹理 swizzle：XGSuggestSwizzleMode / XGCreateTextureComputer
//   XCRDAPI.dll  只有一个 DStorageGetFactory（DirectStorage 门面）
//
// PC 上这些硬件都不存在。这里的策略分两类：
//   - **能转发的转发**：DStorageGetFactory → 系统的 dstorage.dll
//   - **不能的诚实失败**：APU/DSP 类返回失败码，让游戏走"没有音频硬件"的分支，
//     而不是拿着空句柄继续跑（那样会在更晚的地方炸）
//
// ABI 说明：这些函数的**签名我们无从得知**（主机头文件在 NDA 内）。
// 统一按"返回 uintptr_t"处理是安全的：x64 下调用方多传参数不影响，
// 而 uintptr_t 既能装指针也能装 int/HRESULT。取值原则：
//   创建/连接类 -> 返回 0（null）或 E_NOTIMPL，让调用方立刻知道不可用
//   释放/断开类 -> 返回 S_OK（幂等，避免清理路径报错）
// WIN32_LEAN_AND_MEAN 下 windows.h 不一定带进 IUnknown 的声明，显式包含
#include <unknwn.h>

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

#include "xbgtasix/trace.h"

// 注意：这里**不用** __declspec(dllexport)。本文件会被编进 5 个不同的 DLL，
// 每个 DLL 只应导出自己的那一组符号（由 build.sh 生成的 <模块>.def 决定）。
// 用 dllexport 会让每个 DLL 都导出全部 22 个符号（实测如此），
// 既不符合"一个模块一份接口"的设计，也让导出数对账失去意义。

namespace {

// 同一个 API 只详细记一次，避免每帧刷屏（与桩机制一致的手工版本）
// 全量调用追踪：XBGTASIX_TRACE=1 时把每次进入的 API 与参数记下来。
// 游戏崩在"某个 bool 检查返回 false"时，只有完整序列能看出是哪一步。
bool trace_on() {
    static int on = -1;
    if (on < 0) {
        char b[8] = {0};
        on = (GetEnvironmentVariableA("XBGTASIX_TRACE", b, sizeof(b)) > 0 && b[0] == '1') ? 1 : 0;
    }
    return on == 1;
}

#define TRACE_CALL(name, a1, a2, a3)                                    \
    do {                                                                \
        if (trace_on()) {                                               \
            static int n_ = 0;                                          \
            if (++n_ <= 400)                                           \
                xbgtasix::logf("[trace] %s(%p, %p, %p)", name,           \
                               reinterpret_cast<void*>(a1),              \
                               reinterpret_cast<void*>(a2),              \
                               reinterpret_cast<void*>(a3));             \
        }                                                               \
    } while (0)

bool first_time(const char* api) {
    static CRITICAL_SECTION cs = {};
    static bool inited = false;
    if (!inited) {
        InitializeCriticalSection(&cs);
        inited = true;
    }
    static const char* seen[64] = {};
    static int n = 0;
    EnterCriticalSection(&cs);
    bool first = true;
    for (int i = 0; i < n; ++i) {
        if (seen[i] == api) {
            first = false;
            break;
        }
    }
    if (first && n < 64) seen[n++] = api;
    LeaveCriticalSection(&cs);
    if (first) xbgtasix::logf("[console-hw] %s：PC 上没有对应硬件，返回不可用", api);
    return first;
}

}  // namespace

extern "C" {

// ============================ AcpHal.dll ============================
// APU = 主机的音频/计算加速单元。PC 上不存在，分配一律失败。
uintptr_t AcpHalCreate(void*, void*, void*) { TRACE_CALL("AcpHalCreate", 0, 0, 0);
    first_time("AcpHalCreate");
    return 0;   // 创建失败
}
uintptr_t AcpHalAllocateShapeContexts(void*, void*) { TRACE_CALL("AcpHalAllocateShapeContexts", 0, 0, 0);
    first_time("AcpHalAllocateShapeContexts");
    return 0;
}
uintptr_t AcpHalReleaseShapeContexts(void*, void*) { TRACE_CALL("AcpHalReleaseShapeContexts", 0, 0, 0);
    return 0;   // S_OK：释放路径保持幂等
}
uintptr_t ApuAlloc(void*, void*, void*, void*) { TRACE_CALL("ApuAlloc", 0, 0, 0);
    first_time("ApuAlloc");
    return 0;   // 分配不到 APU 内存
}
uintptr_t ApuCreateHeap(void*, void*, void*) { TRACE_CALL("ApuCreateHeap", 0, 0, 0);
    first_time("ApuCreateHeap");
    return 0;
}
uintptr_t ApuFree(void*, void*) { TRACE_CALL("ApuFree", 0, 0, 0); return 0; }
uintptr_t ApuIsVirtualAddressValid(void*, void*) { TRACE_CALL("ApuIsVirtualAddressValid", 0, 0, 0);
    return 0;   // false：没有任何 APU 虚拟地址是有效的
}
uintptr_t ApuMapApuAddress(void*, void*, void*) { TRACE_CALL("ApuMapApuAddress", 0, 0, 0);
    first_time("ApuMapApuAddress");
    return 0;
}
uintptr_t ApuMapVirtualAddress(void*, void*, void*) { TRACE_CALL("ApuMapVirtualAddress", 0, 0, 0);
    first_time("ApuMapVirtualAddress");
    return 0;
}

// ============================ xapu.dll ============================
uintptr_t XApuConnect(void*, void*, void*) { TRACE_CALL("XApuConnect", 0, 0, 0);
    first_time("XApuConnect");
    return 0x80004001u;   // E_NOTIMPL：连不上 APU
}
uintptr_t XApuDisconnect(void*, void*) { TRACE_CALL("XApuDisconnect", 0, 0, 0); return 0; }
uintptr_t XApuEnqueueCommand(void*, void*, void*) { TRACE_CALL("XApuEnqueueCommand", 0, 0, 0);
    first_time("XApuEnqueueCommand");
    return 0x80004001u;
}
uintptr_t XApuDequeueResult(void*, void*, void*) { TRACE_CALL("XApuDequeueResult", 0, 0, 0);
    return 0x80004001u;
}
uintptr_t XApuGetResultCount(void*, void*) { TRACE_CALL("XApuGetResultCount", 0, 0, 0); return 0; }

// ============================ chad.dll ============================
uintptr_t XDspActivate(void*, void*, void*) { TRACE_CALL("XDspActivate", 0, 0, 0);
    first_time("XDspActivate");
    return 0x80004001u;
}
uintptr_t XDspDeactivate(void*, void*) { TRACE_CALL("XDspDeactivate", 0, 0, 0); return 0; }
uintptr_t XDspConnectWithMaximumStreamLimit(void*, void*, void*, void*) { TRACE_CALL("XDspConnectWithMaximumStreamLimit", 0, 0, 0);
    first_time("XDspConnectWithMaximumStreamLimit");
    return 0x80004001u;
}
uintptr_t XDspDisconnect(void*, void*) { TRACE_CALL("XDspDisconnect", 0, 0, 0); return 0; }
uintptr_t XDspSubmitCommand(void*, void*, void*) { TRACE_CALL("XDspSubmitCommand", 0, 0, 0);
    first_time("XDspSubmitCommand");
    return 0x80004001u;
}

// ============================ xg_xs.dll ============================
// 纹理 swizzle：主机 GPU 用固定 swizzle 模式；PC 的 D3D12 自己决定平铺方式。
// 建议返回 0（线性/无 swizzle），让游戏按"未指定"处理。
uintptr_t XGSuggestSwizzleMode(void*, void*, void*, void*) { TRACE_CALL("XGSuggestSwizzleMode", 0, 0, 0);
    first_time("XGSuggestSwizzleMode");
    return 0;
}
uintptr_t XGCreateTextureComputer(void*, void*, void*) { TRACE_CALL("XGCreateTextureComputer", 0, 0, 0);
    first_time("XGCreateTextureComputer");
    return 0x80004001u;   // 没有主机纹理计算机
}

}  // extern "C"

// ============================ XCRDAPI.dll ============================
// 游戏要的是 GDK 的 IDStorageFactory（IID 62959D11-…），
// 而 PC 上所有 DirectStorage 版本（1.0.0/1.0.2/1.3.0，实测）用的都是
// IID 6924ea0c-…，直接转发必然 E_NOINTERFACE（实测 hr=0x80004002）。
// GDK 的 NuGet 包里也没有 dstorage 头文件，接口布局不公开。
//
// 因此这里返回一个**包装对象**：vtable 逐个槽位转发给 PC 的工厂，
// 同时把每个槽位的调用（序号 + 参数）记进日志。这样即使两版布局有偏移，
// 也能从日志反推游戏实际用的是哪几个槽位、传什么参数，再据此对齐。
namespace {

const GUID kGdkFactoryIid = {0x62959D11, 0xD45F, 0x4884,
                             {0xA0, 0xA3, 0xF7, 0x86, 0x2E, 0xA2, 0x43, 0xAE}};
const GUID kPcFactoryIid = {0x6924EA0C, 0xC3CD, 0x4826,
                            {0xB1, 0x0A, 0xF6, 0x4F, 0x4E, 0xD9, 0x27, 0xC1}};

struct Wrap {
    void** vt;        // 必须是第一个成员：对象指针即 IUnknown*
    IUnknown* pc;     // PC 的真实工厂；系统没有 dstorage.dll 时为空
};

// 每槽位首调时的详细信息 + 全部调用的简表（上限 200 行）：
// 只记首次会漏掉"设置完暂存缓冲后紧跟着建队列/开文件"这类序列。
bool slot_seen(size_t slot) {
    static std::atomic<unsigned> mask{0};
    if (slot >= 32) return false;
    const unsigned bit = 1u << slot;
    const unsigned old = mask.fetch_or(bit);
    return (old & bit) == 0;
}

bool slot_budget() {
    static std::atomic<int> left{200};
    return left.fetch_sub(1) > 0;
}

template <size_t N>
uintptr_t __stdcall wrap_thunk(Wrap* w, uintptr_t a1 = 0, uintptr_t a2 = 0, uintptr_t a3 = 0,
                               uintptr_t a4 = 0) {
    if (slot_seen(N)) {
        // 参数里如果有可读字符串就一起打出来：GDK 的接口布局不公开，
        // 靠"槽位号 + 参数形态"反推它在做什么（路径 / 描述符 / 队列）。
        auto sniff = [](uintptr_t p) -> const char* {
            static char buf[4][128];
            static int which = 0;
            if (!p) return "";
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) == 0) return "";
            if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return "";
            const auto* b = reinterpret_cast<const unsigned char*>(p);
            char* out = buf[which++ & 3];
            // UTF-16 形式的宽字符串最常见（路径）
            bool wide_ok = true;
            for (int i = 0; i < 60; i += 2) {
                if (b[i + 1] != 0) { wide_ok = false; break; }
                if (b[i] == 0) break;
            }
            if (wide_ok && b[0]) {
                char* d = out;
                for (int i = 0; i < 120 && b[i]; i += 2) *d++ = static_cast<char>(b[i]);
                *d = 0;
                return out;
            }
            bool asc_ok = true;
            for (int i = 0; i < 60 && b[i]; ++i) {
                if (b[i] < 32 || b[i] > 126) { asc_ok = false; break; }
            }
            if (asc_ok && b[0]) {
                for (int i = 0; i < 120 && b[i]; ++i) out[i] = static_cast<char>(b[i]);
                out[60] = 0;
                return out;
            }
            return "";
        };
        xbgtasix::logf("[xcrdapi] factory vtable[%zu] a1=%p(%s) a2=%p(%s) a3=%p(%s) a4=%p", N,
                       reinterpret_cast<void*>(a1), sniff(a1), reinterpret_cast<void*>(a2),
                       sniff(a2), reinterpret_cast<void*>(a3), sniff(a3),
                       reinterpret_cast<void*>(a4));
    }
    if (slot_budget()) {
        xbgtasix::logf("[xcrdapi]   call slot=%zu a1=%p a2=%p a3=%p a4=%p", N,
                       reinterpret_cast<void*>(a1), reinterpret_cast<void*>(a2),
                       reinterpret_cast<void*>(a3), reinterpret_cast<void*>(a4));
    }
    // **不盲目转发**：实测把 GDK 的 slot 8 转发给 PC 的实现会直接崩在
    // DStorageCore.dll（两版槽位顺序不同）。在弄清每个槽位的真实含义之前，
    // 先只记账并返回"成功"，看游戏是否把它当可选操作。
    (void)w; (void)a1; (void)a2; (void)a3; (void)a4;
    // 对照实验用：XBGTASIX_FACTORY_FAIL=1 时所有槽位返回 E_NOTIMPL，
    // 用来判断"游戏是否在意这个槽位的返回值"（崩溃点会不会变）。
    static int fail_mode = -1;
    if (fail_mode < 0) {
        char b[8] = {0};
        fail_mode = (GetEnvironmentVariableA("XBGTASIX_FACTORY_FAIL", b, sizeof(b)) > 0 && b[0] == '1') ? 1 : 0;
    }
    if (fail_mode) return 0x80004001u;
    // 对照：若游戏把这个返回值当"句柄"存进结构体（实测断言检查的是 obj+0xC 非 0），
    // 返回 0 会被当成失败。0x1000 在 HRESULT 语义下仍是成功码（>= 0）。
    static int handle_mode = -1;
    if (handle_mode < 0) {
        char b[8] = {0};
        handle_mode = (GetEnvironmentVariableA("XBGTASIX_FACTORY_HANDLE", b, sizeof(b)) > 0 && b[0] == '1') ? 1 : 0;
    }
    return handle_mode ? 0x1000u : 0;
}

// QueryInterface 特化：不管要什么 IID 都把自己交出去，让游戏继续走
uintptr_t __stdcall wrap_qi(Wrap* w, const GUID*, void** out) {
    if (out) *out = w;
    xbgtasix::logf("[xcrdapi] factory->QueryInterface 已放行（返回包装对象）");
    return 0;   // S_OK
}
uintptr_t __stdcall wrap_addref(Wrap*) { return 1; }
uintptr_t __stdcall wrap_release(Wrap*) { return 1; }

template <size_t... I>
constexpr std::array<void*, 32> make_vtable(std::index_sequence<I...>) {
    return {reinterpret_cast<void*>(&wrap_thunk<I>)...};
}

void** factory_vtable() {
    static void** vt = []() -> void** {
        auto arr = make_vtable(std::make_index_sequence<32>{});
        arr[0] = reinterpret_cast<void*>(&wrap_qi);
        arr[1] = reinterpret_cast<void*>(&wrap_addref);
        arr[2] = reinterpret_cast<void*>(&wrap_release);
        auto* heap = new std::array<void*, 32>(arr);
        return heap->data();
    }();
    return vt;
}

}  // namespace

extern "C" HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv) {
    using PFN = HRESULT(WINAPI*)(REFIID, void**);
    static PFN pfn = []() -> PFN {
        HMODULE h = LoadLibraryW(L"dstorage.dll");
        if (!h) h = LoadLibraryW(L"DStorageCore.dll");
        return h ? reinterpret_cast<PFN>(GetProcAddress(h, "DStorageGetFactory")) : nullptr;
    }();
    if (!ppv) return E_POINTER;

    // PC 的真实工厂（只建一次）
    static IUnknown* pc = nullptr;
    if (!pc && pfn) {
        IUnknown* got = nullptr;
        if (SUCCEEDED(pfn(kPcFactoryIid, reinterpret_cast<void**>(&got)))) pc = got;
    }

    const auto* g = reinterpret_cast<const unsigned char*>(&riid);
    const bool wants_gdk = memcmp(&riid, &kGdkFactoryIid, sizeof(GUID)) == 0;
    xbgtasix::logf("[xcrdapi] DStorageGetFactory iid={%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}"
                   " %s，PC 实现=%s",
                   *reinterpret_cast<const uint32_t*>(g), *reinterpret_cast<const uint16_t*>(g + 4),
                   *reinterpret_cast<const uint16_t*>(g + 6), g[8], g[9], g[10], g[11], g[12], g[13],
                   g[14], g[15], wants_gdk ? "（GDK 版）" : "", pc ? "有" : "无");

    static Wrap wrapper{factory_vtable(), nullptr};
    wrapper.pc = pc;
    *ppv = &wrapper;
    return S_OK;
}
