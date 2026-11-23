// SPDX-License-Identifier: GPL-3.0-or-later
// t30_fakegame — 一个"假游戏"：走完真实 GDK 程序的启动序列
//
// 目的：在真 exe 到手之前，先用它验证**启动器**（tools/launch.sh）整条链路：
//   加载 shim -> 读配置 -> 建设备 -> 建队列 -> 分配内存 -> 异步用户 -> 退出
// 这样等真 exe 来了，出问题时能确定是"游戏的问题"还是"我们的启动器问题"。
#include <windows.h>
#include <d3d12.h>

#include <cstdio>
#include <cstring>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize(void);
HRESULT WINAPI XGameRuntimeUninitialize(void);
HRESULT WINAPI XGameGetXboxTitleId(unsigned* id);
void* WINAPI XMemAlloc(uint64_t attributes, size_t size);
void WINAPI XMemFree(void* p);
HRESULT WINAPI XTaskQueueCreate(int work_mode, int comp_mode, void** queue);
HRESULT WINAPI XTaskQueueSubmitCallback(void* queue, int port, void* context, void* callback);
bool WINAPI XTaskQueueDispatch(void* queue, int port, unsigned timeout_ms);
void WINAPI XTaskQueueCloseHandle(void* queue);
HRESULT WINAPI XPackageGetCurrentProcessPackageIdentifier(size_t size, char* buffer);
HRESULT WINAPI XUserAddAsync(int options, void* async_block);
HRESULT WINAPI XUserAddResult(void* async_block, void** user);
void WINAPI XUserCloseHandle(void* user);
HRESULT WINAPI XAsyncGetStatus(void* async_block, bool wait);

}

namespace {
struct Blk {
    void* queue;
    void* context;
    void (*callback)(Blk*);
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};

int g_fail = 0;
void step(const char* what, bool ok) {
    xbgtasix::logf("[game] %-38s %s", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}
void CALLBACK on_task(void*, bool) {}

}  // namespace

int main(int argc, char** argv) {
    // 和真实游戏一样，配置路径由外部给（不传就靠 exe 同目录的 MicrosoftGame.Config）
    if (argc > 1 && argv[1][0]) SetEnvironmentVariableA("XBGTASIX_CONFIG", argv[1]);
    if (argc > 2 && argv[2][0]) SetEnvironmentVariableA("XBGTASIX_PACKAGE_ROOT", argv[2]);
    xbgtasix::logf("[game] ===== 假游戏启动 %s =====", argc > 0 ? argv[0] : "?");

    // 1) 运行时初始化 + 读配置
    step("XGameRuntimeInitialize", SUCCEEDED(XGameRuntimeInitialize()));
    unsigned titleId = 0;
    step("XGameGetXboxTitleId", SUCCEEDED(XGameGetXboxTitleId(&titleId)) && titleId != 0);
    xbgtasix::logf("[game] TitleId = 0x%08lX", static_cast<unsigned long>(titleId));

    // 2) 包身份
    char pkg[256] = {0};
    step("XPackageGetCurrentProcessPackageIdentifier",
         SUCCEEDED(XPackageGetCurrentProcessPackageIdentifier(sizeof(pkg), pkg)));
    xbgtasix::logf("[game] 包身份 = %s", pkg);

    // 3) D3D12 设备（走 shim 的自动选卡）
    ID3D12Device* device = nullptr;
    const HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                                         reinterpret_cast<void**>(&device));
    step("D3D12CreateDevice", SUCCEEDED(hr) && device != nullptr);
    if (device) device->Release();

    // 4) 内存
    void* mem = XMemAlloc(0, 65536);
    step("XMemAlloc(64KB)", mem != nullptr);
    // 以前是 step("XMemFree", true)：恒真，等于没测。改成"确实拿到非空块并释放了"
    bool freed_non_null = false;
    if (mem) {
        std::memset(mem, 0x5A, 65536);
        XMemFree(mem);
        freed_non_null = true;
    }
    step("XMemFree", freed_non_null);

    // 5) 任务队列
    void* queue = nullptr;
    step("XTaskQueueCreate", SUCCEEDED(XTaskQueueCreate(0, 0, &queue)) && queue != nullptr);
    if (queue) {
        XTaskQueueSubmitCallback(queue, 0, nullptr, reinterpret_cast<void*>(&on_task));
        step("XTaskQueueDispatch", XTaskQueueDispatch(queue, 0, 100));
        XTaskQueueCloseHandle(queue);
    }

    // 6) 异步用户
    Blk b{};
    step("XUserAddAsync", SUCCEEDED(XUserAddAsync(0, &b)));
    step("XAsyncGetStatus(wait)", XAsyncGetStatus(&b, true) == S_OK);
    void* user = nullptr;
    step("XUserAddResult", SUCCEEDED(XUserAddResult(&b, &user)) && user != nullptr);
    if (user) XUserCloseHandle(user);

    // 7) 收尾
    XGameRuntimeUninitialize();
    xbgtasix::logf("[game] ===== 退出，failures=%d =====", g_fail);
    return g_fail ? 1 : 0;
}
