// api_impl.cpp —— 主机 GDK 运行时的“API 实现提供者”内部入口点。
//
// 为什么有这四个名字：标题（game_scarlett_master.exe）在引导时**按名字**从
// xgameruntime.dll 解析这几个没写在公开头文件里的导出，用来给它的 API 客户端拿到
// 实现对象。真 Xbox GDK 的 xgameruntime.dll 导出它们；我们的 shim 以前没有，于是：
//
//   LoadLibraryExW(L"xgameruntime.dll") -> 拿到（已加载的）我们的 shim
//   GetProcAddress("QueryApiImpl")        -> 有
//   GetProcAddress("InitializeApiImpl")   -> 无
//   GetProcAddress("InitializeApiImplEx") -> 无
//   GetProcAddress("InitializeApiImplEx2")-> 无
//   GetProcAddress("UninitializeApiImpl") -> 无
//   -> 引导函数返回 0x89240101，标题里 `js` 跳去 int3（0x80000003）
//      at game_scarlett_master.exe+0x5A91C21 —— 就是 Wine 上“482 ms 就死”的那一枪。
//
// 证据见 docs/wine-host.md：WINEDEBUG=+module 的 LdrGetProcedureAddress 四连 warn，
// 以及 game+0x5A91C21 / game+0x55E29DC 的反汇编。
//
// 这四个名字**不在 GDK 公开面**里，语义只从标题侧的调用点反推：
//   InitializeApiImpl     (uint64 a, uint64 b, uint64 flags)   rcx/rdx/r8
//   InitializeApiImplEx   同上（flags 里带 2/6 之类的位）
//   InitializeApiImplEx2  同上（flags 里带 8 位）
//   UninitializeApiImpl   (void)
// 标题在 InitializeApiImplEx2 之后会把返回值当“对象指针”用（见 docs/wine-host.md
// 的下一堵墙），那部分还没有实现。这里先给“成功且不抛”的最小实现：
// 返回值 0（S_OK），让引导走过这一步，后面的失败点才能真正暴露出来。

#include <windows.h>

#include <atomic>

#include "xbgtasix/platform.h"
#include "xbgtasix/trace.h"

namespace {

// 四个入口共用的记录：谁被调过、在哪个宿主上、参数是什么。
// 只记一次（标题在引导里只会问一次），不刷屏。
void log_once(const char* which, unsigned long long a, unsigned long long b,
              unsigned long long c) {
    xbgtasix::logf("[xgamert] %s(a=%016llX b=%016llX flags=%llX) 宿主=%s%s%s", which, a, b, c,
                   xbgtasix::host_platform_name(),
                   xbgtasix::host_is_wine() ? " " : "",
                   xbgtasix::host_is_wine() ? xbgtasix::host_wine_version().c_str() : "");
}

}  // namespace

extern "C" {

// 返回值语义未知，取 HRESULT：S_OK 让引导继续。
STDAPI InitializeApiImpl(unsigned long long a, unsigned long long b, unsigned long long flags) {
    log_once("InitializeApiImpl", a, b, flags);
    return S_OK;
}

STDAPI InitializeApiImplEx(unsigned long long a, unsigned long long b, unsigned long long flags) {
    log_once("InitializeApiImplEx", a, b, flags);
    return S_OK;
}

STDAPI InitializeApiImplEx2(unsigned long long a, unsigned long long b, unsigned long long flags) {
    log_once("InitializeApiImplEx2", a, b, flags);
    // 标题在这一步之后才会去读 ctx+0xC04（第 4 堵墙）。趁现在把写断点装上 ——
    // 这时 ctx（TLS +0x7F0）已经存在，早于它太早（DllMain 时期还是空的）。
    xbgtasix::install_ctx_write_watchpoint();
    xbgtasix::install_trace_breakpoints();
    xbgtasix::install_int3_probes();
    return S_OK;
}

STDAPI UninitializeApiImpl(void) {
    xbgtasix::logf("[xgamert] UninitializeApiImpl()");
    return S_OK;
}

}  // extern "C"

// ---------------------------------------------------------------------------
// QueryApiImpl：标题要的"实现对象"
//
// 现象（docs/wine-host.md 第 4 节，实测）：引导一过，标题就
//   1455de012: call 0x1455de048      ; 内部 call [0x146e04eb0] = QueryApiImpl(?, ?, &出参)
//   1455de01b: mov  0x40(%rsp),%rcx  ; rcx = 出参
//   1455de020: mov  (%rcx),%rax      ; ← 桩返回 NULL 就在这儿崩
//   1455de023: mov  0xe8(%rax),%rax  ; vtable[29]
//   1455de037: mov  0x10(%rax),%rax  ; vtable[2]（看着像 Release）
//
// 所以它要一个 COM 式对象：对象头是 vtable 指针，vtable 至少有 30 槽。
// 这里给一个**哑对象**：槽 2 = Release 返回 0，其余槽第一次被调用时记一行
// "vtable[N] 被调用" 然后返回 S_OK —— 目的不是让它跑通，是让下一堵墙露出来，
// 每跑一轮就多知道一个槽的形状。
//
// 记一次而不是每次都记：标题在崩溃前可能把同一个槽调几百次，日志会淹掉。
// ---------------------------------------------------------------------------
namespace {

constexpr int kApiVtableSlots = 40;   // 已知 0xE8/8 = 29 是最低要求，留余量
constexpr int kReleaseSlot = 0x10 / 8;
constexpr int kWorkSlot = 0xE8 / 8;

// 对象本体给 256 字节：标题可能往里写（引用计数/上下文指针），只给 8 字节会踩坏后面的全局量。
alignas(16) unsigned char g_api_object[256];
void* g_api_vtable[kApiVtableSlots];
std::atomic<bool> g_api_ready{false};
std::atomic<bool> g_slot_seen[kApiVtableSlots];

long long api_slot_called(int index, unsigned long long a, unsigned long long b,
                          unsigned long long c) {
    if (!g_slot_seen[index].exchange(true))
        xbgtasix::logf("[xgamert] QueryApi 对象 vtable[%d] (off 0x%X) 首次调用 a=%016llX b=%016llX "
                       "c=%016llX -> S_OK", index, index * 8, a, b, c);
    return 0;   // S_OK：先当"成功了"，谁知道它要什么，等下一轮的日志说话
}

}  // namespace

// vtable 的 40 个槽：每个槽一个独立函数体，只能这样才能从里面知道自己是谁。
#define XB_API_SLOT(n)                                                        \
    long long api_slot_##n(void*, unsigned long long a, unsigned long long b,  \
                           unsigned long long c) {                            \
        return api_slot_called(n, a, b, c);                                   \
    }
XB_API_SLOT(0)  XB_API_SLOT(1)  XB_API_SLOT(2)  XB_API_SLOT(3)
XB_API_SLOT(4)  XB_API_SLOT(5)  XB_API_SLOT(6)  XB_API_SLOT(7)
XB_API_SLOT(8)  XB_API_SLOT(9)  XB_API_SLOT(10) XB_API_SLOT(11)
XB_API_SLOT(12) XB_API_SLOT(13) XB_API_SLOT(14) XB_API_SLOT(15)
XB_API_SLOT(16) XB_API_SLOT(17) XB_API_SLOT(18) XB_API_SLOT(19)
XB_API_SLOT(20) XB_API_SLOT(21) XB_API_SLOT(22) XB_API_SLOT(23)
XB_API_SLOT(24) XB_API_SLOT(25) XB_API_SLOT(26) XB_API_SLOT(27)
XB_API_SLOT(28) XB_API_SLOT(29) XB_API_SLOT(30) XB_API_SLOT(31)
XB_API_SLOT(32) XB_API_SLOT(33) XB_API_SLOT(34) XB_API_SLOT(35)
XB_API_SLOT(36) XB_API_SLOT(37) XB_API_SLOT(38) XB_API_SLOT(39)
#undef XB_API_SLOT

namespace {

void build_api_vtable() {
    if (g_api_ready.exchange(true)) return;
    void** vt = g_api_vtable;
    vt[0] = reinterpret_cast<void*>(&api_slot_0);
    vt[1] = reinterpret_cast<void*>(&api_slot_1);
    vt[2] = reinterpret_cast<void*>(&api_slot_2);
    vt[3] = reinterpret_cast<void*>(&api_slot_3);
    vt[4] = reinterpret_cast<void*>(&api_slot_4);
    vt[5] = reinterpret_cast<void*>(&api_slot_5);
    vt[6] = reinterpret_cast<void*>(&api_slot_6);
    vt[7] = reinterpret_cast<void*>(&api_slot_7);
    vt[8] = reinterpret_cast<void*>(&api_slot_8);
    vt[9] = reinterpret_cast<void*>(&api_slot_9);
    vt[10] = reinterpret_cast<void*>(&api_slot_10);
    vt[11] = reinterpret_cast<void*>(&api_slot_11);
    vt[12] = reinterpret_cast<void*>(&api_slot_12);
    vt[13] = reinterpret_cast<void*>(&api_slot_13);
    vt[14] = reinterpret_cast<void*>(&api_slot_14);
    vt[15] = reinterpret_cast<void*>(&api_slot_15);
    vt[16] = reinterpret_cast<void*>(&api_slot_16);
    vt[17] = reinterpret_cast<void*>(&api_slot_17);
    vt[18] = reinterpret_cast<void*>(&api_slot_18);
    vt[19] = reinterpret_cast<void*>(&api_slot_19);
    vt[20] = reinterpret_cast<void*>(&api_slot_20);
    vt[21] = reinterpret_cast<void*>(&api_slot_21);
    vt[22] = reinterpret_cast<void*>(&api_slot_22);
    vt[23] = reinterpret_cast<void*>(&api_slot_23);
    vt[24] = reinterpret_cast<void*>(&api_slot_24);
    vt[25] = reinterpret_cast<void*>(&api_slot_25);
    vt[26] = reinterpret_cast<void*>(&api_slot_26);
    vt[27] = reinterpret_cast<void*>(&api_slot_27);
    vt[28] = reinterpret_cast<void*>(&api_slot_28);
    vt[29] = reinterpret_cast<void*>(&api_slot_29);
    vt[30] = reinterpret_cast<void*>(&api_slot_30);
    vt[31] = reinterpret_cast<void*>(&api_slot_31);
    vt[32] = reinterpret_cast<void*>(&api_slot_32);
    vt[33] = reinterpret_cast<void*>(&api_slot_33);
    vt[34] = reinterpret_cast<void*>(&api_slot_34);
    vt[35] = reinterpret_cast<void*>(&api_slot_35);
    vt[36] = reinterpret_cast<void*>(&api_slot_36);
    vt[37] = reinterpret_cast<void*>(&api_slot_37);
    vt[38] = reinterpret_cast<void*>(&api_slot_38);
    vt[39] = reinterpret_cast<void*>(&api_slot_39);
    // 对象头 = vtable 指针
    *reinterpret_cast<void**>(g_api_object) = static_cast<void*>(g_api_vtable);
}

}  // namespace

extern "C" {

// 出参在 r8（见 docs/wine-host.md 4 节的反汇编：rsi/rdi 是前两个入参，r8 是 &对象）。
// 真实语义未知；这里只做"给一个能安全解引用的对象"。
STDAPI QueryApiImpl(unsigned long long api_id, unsigned long long version, void** out) {
    xbgtasix::install_ctx_write_watchpoint();   // 幂等：装过就只返回
    xbgtasix::install_trace_breakpoints();
    xbgtasix::install_int3_probes();
    xbgtasix::logf("[xgamert] QueryApiImpl(id=%016llX ver=%016llX) -> 哑对象（槽 %d 干活、%d 释放）",
                   api_id, version, kWorkSlot, kReleaseSlot);
    if (!out) return E_POINTER;
    build_api_vtable();
    *out = static_cast<void*>(g_api_object);
    return S_OK;
}

}  // extern "C"
