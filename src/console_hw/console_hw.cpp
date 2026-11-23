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
#include <windows.h>

#include <cstdint>

#include "xbgtasix/trace.h"

namespace {

// 同一个 API 只详细记一次，避免每帧刷屏（与桩机制一致的手工版本）
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
__declspec(dllexport) uintptr_t AcpHalCreate(void*, void*, void*) {
    first_time("AcpHalCreate");
    return 0;   // 创建失败
}
__declspec(dllexport) uintptr_t AcpHalAllocateShapeContexts(void*, void*) {
    first_time("AcpHalAllocateShapeContexts");
    return 0;
}
__declspec(dllexport) uintptr_t AcpHalReleaseShapeContexts(void*, void*) {
    return 0;   // S_OK：释放路径保持幂等
}
__declspec(dllexport) uintptr_t ApuAlloc(void*, void*, void*, void*) {
    first_time("ApuAlloc");
    return 0;   // 分配不到 APU 内存
}
__declspec(dllexport) uintptr_t ApuCreateHeap(void*, void*, void*) {
    first_time("ApuCreateHeap");
    return 0;
}
__declspec(dllexport) uintptr_t ApuFree(void*, void*) { return 0; }
__declspec(dllexport) uintptr_t ApuIsVirtualAddressValid(void*, void*) {
    return 0;   // false：没有任何 APU 虚拟地址是有效的
}
__declspec(dllexport) uintptr_t ApuMapApuAddress(void*, void*, void*) {
    first_time("ApuMapApuAddress");
    return 0;
}
__declspec(dllexport) uintptr_t ApuMapVirtualAddress(void*, void*, void*) {
    first_time("ApuMapVirtualAddress");
    return 0;
}

// ============================ xapu.dll ============================
__declspec(dllexport) uintptr_t XApuConnect(void*, void*, void*) {
    first_time("XApuConnect");
    return 0x80004001u;   // E_NOTIMPL：连不上 APU
}
__declspec(dllexport) uintptr_t XApuDisconnect(void*, void*) { return 0; }
__declspec(dllexport) uintptr_t XApuEnqueueCommand(void*, void*, void*) {
    first_time("XApuEnqueueCommand");
    return 0x80004001u;
}
__declspec(dllexport) uintptr_t XApuDequeueResult(void*, void*, void*) {
    return 0x80004001u;
}
__declspec(dllexport) uintptr_t XApuGetResultCount(void*, void*) { return 0; }

// ============================ chad.dll ============================
__declspec(dllexport) uintptr_t XDspActivate(void*, void*, void*) {
    first_time("XDspActivate");
    return 0x80004001u;
}
__declspec(dllexport) uintptr_t XDspDeactivate(void*, void*) { return 0; }
__declspec(dllexport) uintptr_t XDspConnectWithMaximumStreamLimit(void*, void*, void*, void*) {
    first_time("XDspConnectWithMaximumStreamLimit");
    return 0x80004001u;
}
__declspec(dllexport) uintptr_t XDspDisconnect(void*, void*) { return 0; }
__declspec(dllexport) uintptr_t XDspSubmitCommand(void*, void*, void*) {
    first_time("XDspSubmitCommand");
    return 0x80004001u;
}

// ============================ xg_xs.dll ============================
// 纹理 swizzle：主机 GPU 用固定 swizzle 模式；PC 的 D3D12 自己决定平铺方式。
// 建议返回 0（线性/无 swizzle），让游戏按"未指定"处理。
__declspec(dllexport) uintptr_t XGSuggestSwizzleMode(void*, void*, void*, void*) {
    first_time("XGSuggestSwizzleMode");
    return 0;
}
__declspec(dllexport) uintptr_t XGCreateTextureComputer(void*, void*, void*) {
    first_time("XGCreateTextureComputer");
    return 0x80004001u;   // 没有主机纹理计算机
}

// ============================ XCRDAPI.dll ============================
// 只有一个 DStorageGetFactory：DirectStorage 的门面。
// PC 上真实现是 dstorage.dll（Win10 21H1+ 自带 DirectStorage 1.x），能转发就转发。
// 注意：这个函数的真实签名是 HRESULT DStorageGetFactory(REFIID riid, void** ppv)，
// 与 GDK 命名一致，所以这里是**真实现**而不是桩。
__declspec(dllexport) HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv) {
    using PFN = HRESULT(WINAPI*)(REFIID, void**);
    static PFN pfn = []() -> PFN {
        HMODULE h = LoadLibraryW(L"dstorage.dll");
        if (!h) h = LoadLibraryW(L"DStorageCore.dll");
        return h ? reinterpret_cast<PFN>(GetProcAddress(h, "DStorageGetFactory")) : nullptr;
    }();
    if (!pfn) {
        first_time("DStorageGetFactory（系统无 dstorage.dll）");
        if (ppv) *ppv = nullptr;
        return E_NOTIMPL;
    }
    const HRESULT hr = pfn(riid, ppv);
    // 打印 IID 与结果：GDK 的 dstorage 接口版本与 PC 的 DirectStorage 1.x 不一定同 IID，
    // 拿到的如果是 E_NOINTERFACE，游戏就会断言"factory 为空"。
    const auto* g = reinterpret_cast<const unsigned char*>(&riid);
    xbgtasix::logf("[console-hw] DStorageGetFactory iid={%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} "
                   "-> hr=0x%08lX out=%p",
                   *reinterpret_cast<const uint32_t*>(g),
                   *reinterpret_cast<const uint16_t*>(g + 4),
                   *reinterpret_cast<const uint16_t*>(g + 6), g[8], g[9], g[10], g[11], g[12],
                   g[13], g[14], g[15], static_cast<unsigned long>(hr),
                   ppv ? *ppv : nullptr);
    return hr;
}

}  // extern "C"
