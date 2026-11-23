// SPDX-License-Identifier: GPL-3.0-or-later
// t9_async — 验证 XTaskQueue 真的在派发回调、XUser 流程能走通
//
// 这两块之前是桩（一调用就退出）。现在：
//   - XTaskQueueCreate(ThreadPool, ThreadPool) 起真线程
//   - 提交 64 个回调，等全部执行完（超时即失败）
//   - XUserAddAsync -> XUserAddResult -> XUserGetId -> XUserCloseHandle
// 断言：64 个回调都跑到、计数准确、user id 非 0、Terminate 的回调被调用。
#include <windows.h>

#include <atomic>
#include <cstdio>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize(void);
HRESULT WINAPI XTaskQueueCreate(int work_mode, int comp_mode, void** queue);
HRESULT WINAPI XTaskQueueSubmitCallback(void* queue, int port, void* context, void* callback);
HRESULT WINAPI XTaskQueueTerminate(void* queue, bool wait, void* context, void* callback);
void WINAPI XTaskQueueCloseHandle(void* queue);
HRESULT WINAPI XUserAddAsync(int options, void* async_block);
HRESULT WINAPI XUserAddResult(void* async_block, void** user);
HRESULT WINAPI XUserGetId(void* user, uint64_t* id);
void WINAPI XUserCloseHandle(void* user);
}

namespace {

std::atomic<int> g_calls{0};
std::atomic<int> g_terminate_calls{0};

int g_fail = 0;

void ok(bool cond, const char* what) {
    if (!cond) {
        ++g_fail;
        xbgtasix::logf("[t9] FAIL %s", what);
    }
}

void CALLBACK on_work(void* context, bool canceled) {
    (void)context;
    (void)canceled;
    g_calls.fetch_add(1);
}

// 终止回调的签名与普通回调不同：只有一个 context 参数，没有 canceled
void CALLBACK on_terminate(void* context) {
    (void)context;
    g_terminate_calls.fetch_add(1);
}

// 最小的 XAsyncBlock 形状（只需要 callback 字段）
struct AsyncBlock {
    void* context;
    void* queue;
    void (*callback)(AsyncBlock*);
    unsigned result;
    unsigned internal[16];
};

}  // namespace

int main() {
    xbgtasix::logf("[t9] start");
    ok(SUCCEEDED(XGameRuntimeInitialize()), "XGameRuntimeInitialize");

    // ---- 任务队列 ----
    void* queue = nullptr;
    ok(SUCCEEDED(XTaskQueueCreate(1 /*ThreadPool*/, 1, &queue)) && queue, "XTaskQueueCreate");
    if (!queue) return 1;

    constexpr int kTasks = 64;
    for (int i = 0; i < kTasks; ++i) {
        ok(SUCCEEDED(XTaskQueueSubmitCallback(queue, 0 /*Work*/, nullptr,
                                              reinterpret_cast<void*>(&on_work))),
           "XTaskQueueSubmitCallback");
    }

    // 等回调跑完（最多 5 秒）
    for (int i = 0; i < 500 && g_calls.load() < kTasks; ++i) Sleep(10);
    ok(g_calls.load() == kTasks, "64 个 Work 回调全部执行");
    xbgtasix::logf("[t9] Work 回调执行数 = %d / %d", g_calls.load(), kTasks);

    // Completion 端口也要能用
    g_calls.store(0);
    for (int i = 0; i < 8; ++i) {
        ok(SUCCEEDED(XTaskQueueSubmitCallback(queue, 1 /*Completion*/, nullptr,
                                              reinterpret_cast<void*>(&on_work))),
           "Submit(Completion)");
    }
    for (int i = 0; i < 500 && g_calls.load() < 8; ++i) Sleep(10);
    ok(g_calls.load() == 8, "Completion 端口回调");
    xbgtasix::logf("[t9] Completion 回调执行数 = %d / 8", g_calls.load());

    // ---- Terminate ----
    ok(SUCCEEDED(XTaskQueueTerminate(queue, true, nullptr,
                                     reinterpret_cast<void*>(&on_terminate))),
       "XTaskQueueTerminate");
    ok(g_terminate_calls.load() == 1, "Terminate 回调被调用");
    XTaskQueueCloseHandle(queue);
    xbgtasix::logf("[t9] 队列已关闭");

    // ---- 用户 ----
    AsyncBlock block{};
    ok(SUCCEEDED(XUserAddAsync(0, &block)), "XUserAddAsync");
    void* user = nullptr;
    ok(SUCCEEDED(XUserAddResult(&block, &user)) && user, "XUserAddResult");
    uint64_t id = 0;
    ok(SUCCEEDED(XUserGetId(user, &id)), "XUserGetId");
    ok(id != 0, "user id 非 0");
    xbgtasix::logf("[t9] 本地用户 id = %llu", static_cast<unsigned long long>(id));
    XUserCloseHandle(user);

    xbgtasix::logf("[t9] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
