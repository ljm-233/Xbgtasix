// xgameruntime.dll shim — GDK 运行时入口
//
// 现在直接包含 GDK 真头文件（从 GDK_2610.0.9661.zip 里抽出来的 include/），
// 所以签名、枚举、struct 布局都由编译器强制与官方一致 —— 不再有 PROVISIONAL 猜测。
// XMem 不在公开 GDK 里（NDA），仍由 src/xmem 手写实现。
#include <windows.h>

#include "XGame.h"
#include "XGameRuntime.h"
#include "XStore.h"
#include "XSystem.h"
#include "XTaskQueue.h"
#include "XUser.h"

#include "xbgtasix/trace.h"

extern "C" {

// ---- 生命周期 ----

STDAPI XGameRuntimeInitialize() {
    xbgtasix::logf("[xgamert] XGameRuntimeInitialize()");
    return S_OK;
}

STDAPI XGameRuntimeInitializeWithOptions(const XGameRuntimeOptions* options) {
    xbgtasix::logf("[xgamert] XGameRuntimeInitializeWithOptions(source=%d)",
                 options ? static_cast<int>(options->gameConfigSource) : -1);
    return XGameRuntimeInitialize();
}

STDAPI_(void) XGameRuntimeUninitialize() {
    xbgtasix::logf("[xgamert] XGameRuntimeUninitialize()");
}

// ---- 任务队列（GDK 全线异步 API 的基础）----

STDAPI XTaskQueueCreate(XTaskQueueDispatchMode workDispatchMode,
                        XTaskQueueDispatchMode completionDispatchMode,
                        XTaskQueueHandle* queue) noexcept {
    xbgtasix::logf("[xgamert] XTaskQueueCreate(work=%u, completion=%u)",
                 static_cast<unsigned>(workDispatchMode),
                 static_cast<unsigned>(completionDispatchMode));
    if (queue) *queue = nullptr;
    XB_UNIMPL("XTaskQueueCreate");
    return E_NOTIMPL;
}

// ---- 用户 ----

STDAPI XUserAddAsync(XUserAddOptions options, XAsyncBlock* async) noexcept {
    (void)options;
    (void)async;
    XB_UNIMPL("XUserAddAsync");
    return E_NOTIMPL;
}

STDAPI XUserGetId(XUserHandle user, uint64_t* userId) noexcept {
    (void)user;
    if (userId) *userId = 0;
    XB_UNIMPL("XUserGetId");
    return E_NOTIMPL;
}

STDAPI_(void) XUserCloseHandle(XUserHandle user) noexcept {
    (void)user;
    XB_UNIMPL("XUserCloseHandle");
}

// ---- 商店 / 授权 ----

STDAPI XStoreQueryProductForCurrentGameAsync(const XStoreContextHandle storeContextHandle,
                                             XAsyncBlock* async) noexcept {
    (void)storeContextHandle;
    (void)async;
    XB_UNIMPL("XStoreQueryProductForCurrentGameAsync");
    return E_NOTIMPL;
}

// ---- 系统信息 ----

STDAPI_(XSystemDeviceType) XSystemGetDeviceType() noexcept {
    // 让分支走 PC 路径，比抛异常有用：主机专用分支本就不该在 PC 上执行
    xbgtasix::logf("[xgamert] XSystemGetDeviceType() -> Pc");
    return XSystemDeviceType::Pc;
}

STDAPI XGameGetXboxTitleId(uint32_t* titleId) noexcept {
    if (titleId) *titleId = 0;
    XB_UNIMPL("XGameGetXboxTitleId");
    return E_NOTIMPL;
}

}  // extern "C"
