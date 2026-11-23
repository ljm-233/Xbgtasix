// SPDX-License-Identifier: GPL-3.0-or-later
// t43_host_stall — 标题的开机 hang check 不会被我们的驱动编译误杀（离屏、无 GPU、无游戏）
//
// 场景（Leonida PR #7858 实测出来的机制）：
//   标题有一个 watchdog 线程，循环 `Sleep(1000)`；连续 30 轮主线程进度标记没变化
//   就写地址 0 自杀；某一轮**实际超过 2 s** 就把计数重来。
//   主机侧建 PSO 到 PC 上变成驱动编译，冷缓存 25–60 s —— 足够让 watchdog 数满 30 轮。
//
// 本测试不需要真游戏，只要：
//   1. `xbgtasix::host_stall::install()` 把主模块 IAT 里的 Sleep 换成 thunk；
//   2. 我们标一段"仅主机侧停顿"（这里用忙等 3 s 模拟驱动编译）；
//   3. 断言进行中的那次 Sleep(1000) **不在停顿期间返回**，而是等停顿结束再攒够剩下的
//      "醒着的时间" —— 于是那一轮 > 3 s，标题自己的"> 2 s 就重数"规则会把计数清零。
//
// 反向验证（把 install 短路掉）会失败：watchdog 在 3 s 里数满 3 轮。
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <thread>

#include "xbgtasix/host_stall.h"

namespace {

int g_checks = 0;
int g_fail = 0;

void ok(bool cond, const char* what, long long got, long long want_lo, long long want_hi) {
    ++g_checks;
    if (!cond) ++g_fail;
    printf(cond ? "[ok]   %s（%lld，期望 %lld..%lld ms）\n"
                : "[FAIL] %s（%lld，期望 %lld..%lld ms）\n",
           what, got, want_lo, want_hi);
}

std::int64_t now_ms() {
    static const std::int64_t freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart ? f.QuadPart : 1;
    }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000 / freq;
}

// 忙等：不让测试自己再进 Sleep（否则测的是"我睡我自己"）
void spin_ms(std::int64_t ms) {
    const std::int64_t until = now_ms() + ms;
    while (now_ms() < until) {
    }
}

std::atomic<bool> g_quit{false};
std::atomic<int> g_rounds{0};
std::atomic<std::int64_t> g_last_round_ms{0};

}  // namespace

int main() {
    printf("== 前置：Sleep 钩子 ==\n");
    // 真实场景里主 exe 静态导入 xmem.dll，DllMain 最早执行；测试没有静态引用，
    // 所以要显式加载（顺手验证"钩子是 xmem 的 DllMain 装的"这条部署假设）。
    HMODULE xmem = LoadLibraryA("xmem.dll");
    ok(xmem != nullptr, "xmem.dll 已加载", xmem ? 1 : 0, 1, 1);
    if (!xmem) {
        printf("FAIL: 加载不了 xmem.dll（测试要在 build/ 里跑）\n");
        return 1;
    }

    const auto* api = xbgtasix::host_stall::api();
    ok(api != nullptr, "xmem.dll 提供了 host_stall 调用表", api ? 1 : 0, 1, 1);
    if (!api) {
        printf("FAIL: xmem.dll 没提供 XbgtasixHostStallApi\n");
        return 1;
    }
    ok(api->installed() == 1, "主模块 IAT 里的 Sleep 已被接管", api->installed(), 1, 1);
    if (api->installed() != 1) {
        printf("FAIL: 钩子没装上（主模块没导入 Sleep？）—— 机制无处生效\n");
        return 1;
    }

    // watchdog：和标题一样的 1 s 循环
    std::thread watchdog([] {
        while (!g_quit.load()) {
            const std::int64_t t0 = now_ms();
            Sleep(1000);
            const std::int64_t d = now_ms() - t0;
            g_last_round_ms.store(d);
            g_rounds.fetch_add(1);
        }
    });

    printf("== 场景：3 s 主机侧停顿正好盖住 watchdog 的一轮 ==\n");
    Sleep(300);   // 短睡眠，原样转发：给 watchdog 时间进入它那一轮

    const int rounds_at_stall = g_rounds.load();
    const std::int64_t stall_ms = 3000;
    api->begin();
    spin_ms(stall_ms);
    api->end();
    const int rounds_at_end = g_rounds.load();

    ok(rounds_at_end == rounds_at_stall || rounds_at_end <= rounds_at_stall + 0,
       "停顿期间 watchdog 一轮都没数完", rounds_at_end - rounds_at_stall, 0, 0);

    // 停顿结束后，被盖住的那一轮要以"停顿 + 剩余醒着时间"收尾
    std::int64_t waited = 0;
    while (g_rounds.load() == rounds_at_end && waited < 4000) {
        Sleep(20);
        waited += 20;
    }
    const std::int64_t overlapped = g_last_round_ms.load();
    ok(overlapped >= stall_ms + 400, "被盖住那一轮的实际时长 > 停顿时长（标题会重数）",
       static_cast<long long>(overlapped), stall_ms + 400, stall_ms + 3000);

    // 正常轮次仍然是 ~1 s：机制不能把 Sleep(1000) 变成 3 s 或立刻返回
    const int before_clean = g_rounds.load();
    waited = 0;
    while (g_rounds.load() == before_clean && waited < 3000) {
        Sleep(20);
        waited += 20;
    }
    const std::int64_t clean = g_last_round_ms.load();
    ok(clean >= 850 && clean <= 1800, "没有停顿时的 Sleep(1000) 仍约 1 s",
       static_cast<long long>(clean), 850, 1800);

    printf("== 边界：短睡眠不受影响 ==\n");
    const std::int64_t t0 = now_ms();
    Sleep(50);
    const std::int64_t short_ms = now_ms() - t0;
    ok(short_ms >= 40 && short_ms <= 300, "Sleep(50) 原样转发", static_cast<long long>(short_ms),
       40, 300);

    g_quit.store(true);
    watchdog.join();

    printf("== 结论 ==\n");
    printf("%d checks, %d failures\n", g_checks, g_fail);
    if (g_fail == 0) {
        printf("PASS: 主机侧停顿不计入 guest 的 Sleep 预算，watchdog 那一轮被拉长\n");
        return 0;
    }
    printf("FAIL: %d 项断言不成立\n", g_fail);
    return 1;
}
