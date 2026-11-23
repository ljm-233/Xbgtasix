// SPDX-License-Identifier: GPL-3.0-or-later
// host_stall.h —— "仅主机侧停顿"的记账（给 guest 的 Sleep 用）
//
// 问题
// ----
// 主机的图形管线是原生代码建的；到 PC 上，每条 PSO 都变成一次**驱动编译**，
// 冷缓存下 25–60 s 都发生在标题的主线程里。标题自己有一个开机 hang check：
// 一个线程循环 Sleep(1000)，连续 30 轮主线程进度标记没变化就写地址 0 自杀
// （某一轮实际超过 2 s 就把计数重来）。
//
// 于是"我们的驱动编译"会被标题误判成"主线程卡死"，30 秒后被它自己杀掉。
// 这不是异常、没有错误码，看上去像崩溃。参考实现（Leonida PR #7858）的做法相同。
//
// 做法
// ----
//  1. 主机侧长操作用 scope 标出来，记进一个单调累加器（QPC 纳秒）。**进行中**的
//     停顿也要计入，否则 watchdog 会在停顿期间把"醒着的时间"攒够而提前返回；
//  2. 主模块 IAT 里的 Sleep 换成 thunk：n >= 1 s 时，必须在**停顿之外**累计满
//     n ms 才返回；
//  3. 结果：watchdog 那一轮被拉长到"停顿 + 1 s"，标题自己那条"> 2 s 就重数"的
//     规则生效，30 轮计数被重置，进程活下来。
//
// 边界（都很保守）
// ----------------
//  * n < 1 s 的 Sleep 原样转发：watchdog 每轮是 1000 ms，短睡眠改它只会引入差异。
//  * xmem.dll 没加载 / 导出取不到时，scope 是空操作，不影响任何现有行为。
//  * thunk 里只做时间计算与真实 Sleep，不写日志（避免把 IO 引入 watchdog 线程）。
//
// 实现在 xmem.dll（`src/host_stall/host_stall.cpp`）：它是主 exe 的静态导入，
// DllMain 最早执行，钩子能赶在 watchdog 线程起来之前装好。
#pragma once

#include <windows.h>

namespace xbgtasix::host_stall {

// 跨 DLL 的调用表。xmem.dll 导出 `XbgtasixHostStallApi()` 返回它的地址；
// 用一张表而不是三个导出，是为了只给 xmem.dll 增加**一个**导出符号。
struct vtable {
    void (*begin)();
    void (*end)();
    int (*installed)();   // 1 = Sleep 钩子已就位
};

// 由 xmem.dll 实现（编在它里面）；装钩子是它的 DllMain 的职责，
// 其它 DLL 只通过下面的 api() 取调用表，不要直接调这两个。
bool install();
int installed();

inline const vtable* api() {
    static const vtable* cached = [] () -> const vtable* {
        HMODULE xmem = GetModuleHandleA("xmem.dll");
        if (!xmem) return nullptr;
        using Fn = const vtable* (*)();
        auto fn = reinterpret_cast<Fn>(
            reinterpret_cast<void*>(GetProcAddress(xmem, "XbgtasixHostStallApi")));
        return fn ? fn() : nullptr;
    }();
    return cached;
}

// 记账区间：构造开始，析构把这期间算作一次"仅主机侧停顿"。
// 不分配内存、不抛异常；xmem 不在时就什么都不做。
class scope {
public:
    scope() : api_(api()), active_(api_ != nullptr) {
        if (active_) api_->begin();
    }
    ~scope() {
        if (active_) api_->end();
    }
    scope(const scope&) = delete;
    scope& operator=(const scope&) = delete;

private:
    const vtable* api_;
    bool active_;
};

}  // namespace xbgtasix::host_stall
