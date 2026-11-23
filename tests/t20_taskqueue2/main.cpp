// SPDX-License-Identifier: GPL-3.0-or-later
// t20_taskqueue2 — waiter / monitor / 引用计数 / 进程级队列
//
// 补齐 XTaskQueue 的调度闭环：
//   waiter   = 队列有活时 signal 调用方给的 HANDLE（不是替它跑回调）
//   monitor  = 队列有活时回调通知（Manual 模式的宿主靠它决定何时 Dispatch）
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "xbgtasix/trace.h"

// 与 GDK 一致：注册令牌是按值传递的 8 字节结构
struct Token {
    unsigned long long token;
};

extern "C" {
HRESULT WINAPI XTaskQueueCreate(int work_mode, int comp_mode, void** queue);
HRESULT WINAPI XTaskQueueSubmitCallback(void* queue, int port, void* context, void* callback);
HRESULT WINAPI XTaskQueueSubmitDelayedCallback(void* queue, int port, unsigned delay_ms, void* context,
                                               void* callback);
HRESULT WINAPI XTaskQueueTerminate(void* queue, bool wait, void* context, void* callback);
bool WINAPI XTaskQueueDispatch(void* queue, int port, unsigned timeout_ms);
HRESULT WINAPI XTaskQueueRegisterWaiter(void* queue, int port, HANDLE wait, void* ctx, void* cb,
                                        void* token);
void WINAPI XTaskQueueUnregisterWaiter(void* queue, Token token);
HRESULT WINAPI XTaskQueueRegisterMonitor(void* queue, void* ctx, void* cb, void* token);
void WINAPI XTaskQueueUnregisterMonitor(void* queue, Token token);
HRESULT WINAPI XTaskQueueDuplicateHandle(void* queue, void** dup);
void WINAPI XTaskQueueCloseHandle(void* queue);
void WINAPI XTaskQueueSetCurrentProcessTaskQueue(void* queue);
bool WINAPI XTaskQueueGetCurrentProcessTaskQueue(void** queue);
}

namespace {

std::atomic<int> g_runs{0};
std::atomic<int> g_monitor_calls{0};
std::atomic<int> g_monitor_port{-1};
int g_fail = 0;

void ok(bool cond, const char* what) {
    if (cond) {
        xbgtasix::logf("[t20]   PASS  %s", what);
    } else {
        xbgtasix::logf("[t20]   FAIL  %s", what);
        ++g_fail;
    }
}

void CALLBACK on_work(void* ctx, bool canceled) {
    (void)ctx;
    (void)canceled;
    g_runs.fetch_add(1);
}

void CALLBACK on_monitor(void* ctx, void* queue, int port) {
    (void)ctx;
    (void)queue;
    g_monitor_calls.fetch_add(1);
    g_monitor_port.store(port);
}

}  // namespace

int main() {
    void* q = nullptr;
    ok(SUCCEEDED(XTaskQueueCreate(0 /*Manual*/, 0, &q)) && q, "XTaskQueueCreate");

    // ---- waiter：提交后 event 必须被 signal ----
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    Token wtok{0};
    ok(SUCCEEDED(XTaskQueueRegisterWaiter(q, 0, ev, nullptr,
                                          reinterpret_cast<void*>(&on_work), &wtok)),
       "RegisterWaiter");
    ok(SUCCEEDED(XTaskQueueSubmitCallback(q, 0, nullptr, reinterpret_cast<void*>(&on_work))),
       "SubmitCallback");
    const DWORD w = WaitForSingleObject(ev, 500);
    ok(w == WAIT_OBJECT_0, "waiter 的 event 被 signal");
    xbgtasix::logf("[t20] waiter 返回值 %lu", w);

    // ---- monitor：提交时被通知，且带正确的 port ----
    Token mtok{0};
    ok(SUCCEEDED(XTaskQueueRegisterMonitor(q, nullptr, reinterpret_cast<void*>(&on_monitor), &mtok)),
       "RegisterMonitor");
    ok(SUCCEEDED(XTaskQueueSubmitCallback(q, 0, nullptr, reinterpret_cast<void*>(&on_work))),
       "SubmitCallback(2)");
    xbgtasix::logf("[t20] monitor 调用 %d 次，port=%d", g_monitor_calls.load(),
                   g_monitor_port.load());
    ok(g_monitor_calls.load() == 1, "monitor 被通知 1 次");
    ok(g_monitor_port.load() == 0, "monitor 收到的是 Work 端口");

    // ---- Manual 模式：回调仍必须靠 Dispatch 驱动 ----
    while (XTaskQueueDispatch(q, 0, 0)) {}
    ok(g_runs.load() == 2, "两个回调都被 Dispatch 执行");

    // ---- 注销后不再 signal ----
    XTaskQueueUnregisterWaiter(q, wtok);   // 按值传 token（结构体 8 字节，走寄存器）
    XTaskQueueUnregisterMonitor(q, mtok);
    ResetEvent(ev);
    ok(SUCCEEDED(XTaskQueueSubmitCallback(q, 0, nullptr, reinterpret_cast<void*>(&on_work))),
       "SubmitCallback(3)");
    ok(WaitForSingleObject(ev, 100) == WAIT_TIMEOUT, "注销后 event 不再被 signal");
    ok(g_monitor_calls.load() == 1, "注销后 monitor 不再被调用");
    while (XTaskQueueDispatch(q, 0, 0)) {}

    // ---- DuplicateHandle：关一个，另一个还能用 ----
    void* dup = nullptr;
    ok(SUCCEEDED(XTaskQueueDuplicateHandle(q, &dup)) && dup == q, "DuplicateHandle");
    XTaskQueueCloseHandle(dup);
    ok(SUCCEEDED(XTaskQueueSubmitCallback(q, 0, nullptr, reinterpret_cast<void*>(&on_work))),
       "关掉重复句柄后原句柄仍可用");
    while (XTaskQueueDispatch(q, 0, 0)) {}
    ok(g_runs.load() == 4, "回调总数 4");

    // ---- 进程级默认队列 ----
    XTaskQueueSetCurrentProcessTaskQueue(q);
    void* got = nullptr;
    ok(XTaskQueueGetCurrentProcessTaskQueue(&got) && got == q, "GetCurrentProcessTaskQueue 往返");

    XTaskQueueSetCurrentProcessTaskQueue(nullptr);
    ok(!XTaskQueueGetCurrentProcessTaskQueue(&got), "清空后返回 false");

    // ---- 多 waiter：注销一个不能影响另一个（下标错位会静默丢 signal）----
    HANDLE evA = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE evB = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    Token ta{0}, tb{0};
    ok(SUCCEEDED(XTaskQueueRegisterWaiter(q, 0, evA, nullptr, nullptr, &ta)), "waiter A");
    ok(SUCCEEDED(XTaskQueueRegisterWaiter(q, 0, evB, nullptr, nullptr, &tb)), "waiter B");
    XTaskQueueUnregisterWaiter(q, ta);
    ResetEvent(evB);
    ok(SUCCEEDED(XTaskQueueSubmitCallback(q, 0, nullptr, reinterpret_cast<void*>(&on_work))), "提交");
    ok(WaitForSingleObject(evB, 300) == WAIT_OBJECT_0, "注销 A 之后 B 仍被 signal");
    XTaskQueueUnregisterWaiter(q, tb);
    while (XTaskQueueDispatch(q, 0, 0)) {}

    // ---- 延迟提交：不能崩，且到点才可用 ----
    ok(SUCCEEDED(XTaskQueueSubmitDelayedCallback(q, 0, 30, nullptr,
                                                 reinterpret_cast<void*>(&on_work))),
       "SubmitDelayedCallback");
    ok(!XTaskQueueDispatch(q, 0, 0), "延迟任务不会立刻可派发");
    bool fired = false;
    for (int i = 0; i < 20 && !fired; ++i) fired = XTaskQueueDispatch(q, 0, 30);
    ok(fired, "延迟任务到点被派发");

    // ---- 终止：残留任务不能把进程挂住 ----
    ok(SUCCEEDED(XTaskQueueSubmitCallback(q, 0, nullptr, reinterpret_cast<void*>(&on_work))), "残留任务");
    ok(SUCCEEDED(XTaskQueueTerminate(q, true, nullptr, nullptr)), "Terminate(wait)");
    // 终止之后残留任务不许再被派发（GDK 语义：终止即丢弃待办）。
    // 这条以前完全没有，于是"Terminate 只把 stopped 置位、待办仍留在队列里"能蒙过去。
    ok(!XTaskQueueDispatch(q, 0, 0), "Terminate 后不再派发（残留任务被丢弃）");

    XTaskQueueCloseHandle(q);
    CloseHandle(evA);
    CloseHandle(evB);
    CloseHandle(ev);
    xbgtasix::logf("[t20] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
