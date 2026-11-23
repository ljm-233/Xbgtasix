// SPDX-License-Identifier: GPL-3.0-or-later
// t27_composite — 复合队列（CreateComposite + GetPort）
//
// 复合队列本身不排队：提交给它的任务被转发到"来源端口"所在的队列，
// 排队与派发完全由来源队列负责。这是 GDK 里把"UI 队列"和"渲染队列"
// 拼成一个逻辑队列的用法。
//
// 验证点：
//   1) GetPort 给出非空端口句柄
//   2) 提交给复合队列的 Work 任务，真的落进来源队列（在来源队列上 Dispatch 才执行）
//   3) Completion 端口分别路由到另一个来源队列
//   4) 端口句柄能被 Close，且不影响来源队列继续工作
#include <windows.h>

#include <atomic>
#include <cstdio>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI XTaskQueueCreate(int work_mode, int comp_mode, void** queue);
HRESULT WINAPI XTaskQueueGetPort(void* queue, int port, void** port_handle);
HRESULT WINAPI XTaskQueueCreateComposite(void* work_port, void* comp_port, void** queue);
HRESULT WINAPI XTaskQueueSubmitCallback(void* queue, int port, void* context, void* callback);
bool WINAPI XTaskQueueDispatch(void* queue, int port, unsigned timeout_ms);
void WINAPI XTaskQueueCloseHandle(void* queue);
}

namespace {

std::atomic<int> g_work{0};
std::atomic<int> g_comp{0};
int g_fail = 0;

void ok(bool cond, const char* what) {
    if (cond) {
        xbgtasix::logf("[t27]   PASS  %s", what);
    } else {
        xbgtasix::logf("[t27]   FAIL  %s", what);
        ++g_fail;
    }
}

void CALLBACK on_work(void*, bool) { g_work.fetch_add(1); }
void CALLBACK on_comp(void*, bool) { g_comp.fetch_add(1); }

}  // namespace

int main() {
    // 两个来源队列：都用手动态，方便断言"任务到底落在谁身上"
    void *qa = nullptr, *qb = nullptr;
    ok(SUCCEEDED(XTaskQueueCreate(0 /*Manual*/, 0, &qa)) && qa, "创建来源队列 A");
    ok(SUCCEEDED(XTaskQueueCreate(0 /*Manual*/, 0, &qb)) && qb, "创建来源队列 B");

    void *portA = nullptr, *portB = nullptr;
    ok(SUCCEEDED(XTaskQueueGetPort(qa, 0 /*Work*/, &portA)) && portA, "GetPort(A, Work)");
    ok(SUCCEEDED(XTaskQueueGetPort(qb, 1 /*Completion*/, &portB)) && portB, "GetPort(B, Completion)");

    void* composite = nullptr;
    ok(SUCCEEDED(XTaskQueueCreateComposite(portA, portB, &composite)) && composite,
       "CreateComposite(A.work, B.completion)");

    // ---- 提交到复合队列的 Work：应落到 A ----
    ok(SUCCEEDED(XTaskQueueSubmitCallback(composite, 0, nullptr,
                                          reinterpret_cast<void*>(&on_work))),
       "向复合队列提交 Work");
    ok(!XTaskQueueDispatch(qa, 0, 0) ? false : true, "A 上能派发到转发来的任务");
    ok(g_work.load() == 1, "Work 回调由来源队列 A 执行");

    // ---- 提交到复合队列的 Completion：应落到 B ----
    ok(SUCCEEDED(XTaskQueueSubmitCallback(composite, 1, nullptr,
                                          reinterpret_cast<void*>(&on_comp))),
       "向复合队列提交 Completion");
    bool fired = false;
    for (int i = 0; i < 10 && !fired; ++i) fired = XTaskQueueDispatch(qb, 1, 20);
    xbgtasix::logf("[t27] B 上派发 completion：fired=%d comp_count=%d", fired ? 1 : 0,
                   g_comp.load());
    ok(fired && g_comp.load() == 1, "Completion 回调由来源队列 B 执行");

    // ---- 复合队列自己不该排队：在它上面 Dispatch 拿不到东西 ----
    ok(!XTaskQueueDispatch(composite, 0, 0), "复合队列自身不排队（Dispatch 返回 false）");

    // ---- 端口句柄可关闭，且不影响来源队列 ----
    XTaskQueueCloseHandle(portA);
    XTaskQueueCloseHandle(portB);
    ok(SUCCEEDED(XTaskQueueSubmitCallback(qa, 0, nullptr, reinterpret_cast<void*>(&on_work))),
       "关闭端口句柄后 A 仍可用");
    while (XTaskQueueDispatch(qa, 0, 0)) {}
    ok(g_work.load() == 2, "A 直接提交仍能执行");

    XTaskQueueCloseHandle(composite);
    XTaskQueueCloseHandle(qa);
    XTaskQueueCloseHandle(qb);
    xbgtasix::logf("[t27] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
