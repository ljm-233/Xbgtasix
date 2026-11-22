// t10_integration — 用一个"最小 GDK 程序"的形状去验证兼容层
//
// 与 t9 的区别：t9 测的是单点 API；这里走一遍真实 GDK 程序的启动序列，并且
// 用**真数据**做断言：
//   1) 读 MicrosoftGame.Config（环境变量指向游戏包里的那份）-> TitleId 必须是 0x7952987A
//   2) Manual 队列：3 个回调只能被 XTaskQueueDispatch 一个个驱动
//   3) 延迟提交：50ms 后才会被派发
//   4) XAsyncGetStatus：XUserAddAsync 完成后必须是 S_OK（而不是 E_PENDING）
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize(void);
HRESULT WINAPI XGameGetXboxTitleId(uint32_t* title_id);
HRESULT WINAPI XTaskQueueCreate(int work_mode, int comp_mode, void** queue);
HRESULT WINAPI XTaskQueueSubmitCallback(void* queue, int port, void* context, void* callback);
HRESULT WINAPI XTaskQueueSubmitDelayedCallback(void* queue, int port, unsigned delay_ms, void* context,
                                               void* callback);
bool WINAPI XTaskQueueDispatch(void* queue, int port, unsigned timeout_ms);
HRESULT WINAPI XTaskQueueTerminate(void* queue, bool wait, void* context, void* callback);
void WINAPI XTaskQueueCloseHandle(void* queue);
HRESULT WINAPI XUserAddAsync(int options, void* async_block);
HRESULT WINAPI XUserAddResult(void* async_block, void** user);
HRESULT WINAPI XUserGetId(void* user, uint64_t* id);
void WINAPI XUserCloseHandle(void* user);
HRESULT WINAPI XAsyncGetStatus(void* async_block, bool wait);
}

namespace {

std::atomic<int> g_calls{0};
std::atomic<int> g_delayed{0};
int g_fail = 0;

void ok(bool cond, const char* what) {
    if (!cond) {
        ++g_fail;
        xbgtasix::logf("[t10] FAIL %s", what);
    }
}

void CALLBACK on_work(void* c, bool canceled) {
    (void)c;
    (void)canceled;
    g_calls.fetch_add(1);
}

void CALLBACK on_delayed(void* c, bool canceled) {
    (void)c;
    (void)canceled;
    g_delayed.fetch_add(1);
}

struct AsyncBlock {
    void* context;
    void* queue;
    void (*callback)(AsyncBlock*);
    unsigned result;
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};

}  // namespace

int main(int argc, char** argv) {
    // 用真游戏配置做输入（命令行第一个参数就是路径）
    const char* cfg = argc > 1 ? argv[1] : "";
    if (cfg[0]) {
        SetEnvironmentVariableA("XBGTASIX_CONFIG", cfg);
        xbgtasix::logf("[t10] 配置: %s", cfg);
    }

    ok(SUCCEEDED(XGameRuntimeInitialize()), "XGameRuntimeInitialize");

    // ---- 1) 真配置里的 TitleId ----
    uint32_t title = 0;
    ok(SUCCEEDED(XGameGetXboxTitleId(&title)), "XGameGetXboxTitleId");
    xbgtasix::logf("[t10] TitleId = 0x%08lX", static_cast<unsigned long>(title));
    ok(title == 0x7952987A, "TitleId 等于配置里的 7952987A");

    // ---- 2) Manual 队列：必须靠 Dispatch 驱动 ----
    void* queue = nullptr;
    ok(SUCCEEDED(XTaskQueueCreate(0 /*Manual*/, 0, &queue)) && queue, "XTaskQueueCreate(Manual)");
    if (!queue) return 1;

    for (int i = 0; i < 3; ++i) {
        ok(SUCCEEDED(XTaskQueueSubmitCallback(queue, 0, nullptr,
                                              reinterpret_cast<void*>(&on_work))),
           "Submit(Work)");
    }
    // 给线程一点时间：Manual 模式下不该有人执行
    Sleep(50);
    ok(g_calls.load() == 0, "Manual 模式下线程不得自动执行");

    int dispatched = 0;
    while (XTaskQueueDispatch(queue, 0, 100)) ++dispatched;
    xbgtasix::logf("[t10] Manual 派发 %d 次，回调计数 %d", dispatched, g_calls.load());
    ok(dispatched == 3, "Dispatch 驱动了 3 个回调");
    ok(g_calls.load() == 3, "回调计数 3");

    // ---- 3) 延迟提交 ----
    ok(SUCCEEDED(XTaskQueueSubmitDelayedCallback(queue, 0, 50, nullptr,
                                                 reinterpret_cast<void*>(&on_delayed))),
       "SubmitDelayed");
    // 立刻派发应该拿不到（延迟 50ms）
    const bool early = XTaskQueueDispatch(queue, 0, 0);
    ok(!early, "延迟回调不应立刻可用");
    bool fired = false;
    for (int i = 0; i < 20 && !fired; ++i) fired = XTaskQueueDispatch(queue, 0, 50);
    xbgtasix::logf("[t10] 延迟回调 fired=%d", fired ? 1 : 0);
    ok(fired && g_delayed.load() == 1, "延迟回调按时触发");

    ok(SUCCEEDED(XTaskQueueTerminate(queue, true, nullptr, nullptr)), "Terminate");
    XTaskQueueCloseHandle(queue);

    // ---- 4) XAsync 状态 ----
    AsyncBlock block{};
    ok(SUCCEEDED(XUserAddAsync(0, &block)), "XUserAddAsync");
    ok(XAsyncGetStatus(&block, false) == S_OK, "XAsyncGetStatus 立即 S_OK（不是 E_PENDING）");
    void* user = nullptr;
    ok(SUCCEEDED(XUserAddResult(&block, &user)) && user, "XUserAddResult");
    uint64_t uid = 0;
    ok(SUCCEEDED(XUserGetId(user, &uid)) && uid != 0, "XUserGetId");
    xbgtasix::logf("[t10] 用户 id = %llu", static_cast<unsigned long long>(uid));
    XUserCloseHandle(user);

    xbgtasix::logf("[t10] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
