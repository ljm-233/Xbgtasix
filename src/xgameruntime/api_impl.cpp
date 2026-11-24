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
    return S_OK;
}

STDAPI UninitializeApiImpl(void) {
    xbgtasix::logf("[xgamert] UninitializeApiImpl()");
    return S_OK;
}

}  // extern "C"
