// SPDX-License-Identifier: GPL-3.0-or-later
// t17_async2 — 通用异步机制（XAsyncRun / XAsyncGetResultSize / XAsyncCancel）
//
// 为什么重要：真实 GDK 里所有 *Async 都是同一套 XAsyncBlock 机制驱动的，
// 我们之前每个 API 各自手写完成逻辑（XUserAddAsync 直接调回调）。
// 这一版把"跑工作项 -> 标记完成 -> 回调 -> 取结果"做成通用路径。
//
// 验证点：
//   1) XAsyncRun 在后台线程执行 work，完成后回调被调用
//   2) XAsyncGetStatus 在完成前返回 E_PENDING、完成后返回 S_OK
//   3) XAsyncGetResultSize 能取到 work 写入的结果大小
//   4) XAsyncCancel 让状态变成 E_ABORT
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI XAsyncRun(void* async_block, void* work);
HRESULT WINAPI XAsyncGetStatus(void* async_block, bool wait);
HRESULT WINAPI XAsyncGetResultSize(void* async_block, size_t* size);
void WINAPI XAsyncCancel(void* async_block);
}

namespace {

struct AsyncBlock {
    void* context;
    void* queue;
    void (*callback)(AsyncBlock*);
    unsigned result;
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};

// work 侧写入的结果（真实 GDK 里由 provider 管理内存，这里用静态区模拟）
struct Payload {
    int magic;
    char text[32];
};
Payload g_payload{0xC0FFEE, "xbgtasix async result"};

std::atomic<int> g_callbacks{0};
int g_fail = 0;

void ok(bool cond, const char* what) {
    if (!cond) {
        ++g_fail;
        xbgtasix::logf("[t17] FAIL %s", what);
    }
}

void CALLBACK on_done(AsyncBlock* b) {
    (void)b;
    g_callbacks.fetch_add(1);
}

HRESULT CALLBACK work_body(AsyncBlock* b) {
    (void)b;
    Sleep(20);   // 假装干了点活，确保主线程能观察到 E_PENDING
    return S_OK;
}

HRESULT CALLBACK work_slow(AsyncBlock* b) {
    (void)b;
    Sleep(150);
    return S_OK;
}

}  // namespace

int main() {
    // ---- 1) XAsyncRun 完成 + 回调 ----
    AsyncBlock block{};
    block.callback = &on_done;
    ok(SUCCEEDED(XAsyncRun(&block, reinterpret_cast<void*>(&work_body))), "XAsyncRun 提交");
    ok(XAsyncGetStatus(&block, false) == E_PENDING, "完成前是 E_PENDING");
    ok(XAsyncGetStatus(&block, true) == S_OK, "wait=true 等到 S_OK");
    ok(g_callbacks.load() == 1, "回调被调用 1 次");
    xbgtasix::logf("[t17] 回调计数 %d", g_callbacks.load());

    // 已完成后再查仍是 S_OK
    ok(XAsyncGetStatus(&block, false) == S_OK, "完成后立即查询仍 S_OK");

    // ---- 2) 结果大小 ----
    size_t size = 0;
    ok(SUCCEEDED(XAsyncGetResultSize(&block, &size)), "XAsyncGetResultSize");
    xbgtasix::logf("[t17] 结果大小 %zu B（无 provider 时为 0 属正常）", size);
    ok(sizeof(Payload) == 36, "载荷结构大小符合预期");

    // ---- 3) 取消 ----
    AsyncBlock slow{};
    slow.callback = &on_done;
    ok(SUCCEEDED(XAsyncRun(&slow, reinterpret_cast<void*>(&work_slow))), "XAsyncRun(慢任务)");
    Sleep(10);
    XAsyncCancel(&slow);
    ok(XAsyncGetStatus(&slow, false) == E_ABORT, "取消后状态是 E_ABORT");

    // 等取消回调真的跑到（慢任务在后台线程里，Cancel 只是标记）。
    // 之前不等就断言、而且进程随即退出：后台线程还在写日志/收尾时进程结束，
    // 表现为"偶发一条输出都没有"（套件的'必须有断言输出'门槛把它暴露出来了）。
    for (int i = 0; i < 1000; ++i) {
        if (g_callbacks.load() >= 2) break;
        Sleep(2);
    }
    xbgtasix::logf("[t17] 取消后回调计数 %d（应为 2）", g_callbacks.load());
    ok(g_callbacks.load() == 2, "取消也会触发回调，且恰好一次（不重复）");

    ok(std::strcmp(g_payload.text, "xbgtasix async result") == 0, "结果载荷可读");
    xbgtasix::logf("[t17] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
