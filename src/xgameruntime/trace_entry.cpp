// SPDX-License-Identifier: GPL-3.0-or-later
// trace_entry.cpp —— xgameruntime.dll 手写导出函数的统一入口追踪（实现）
//
// 只看 XBGTASIX_TRACE=1 的进程。为什么单独一份而不复用 console_hw 的实现：
// console_hw.cpp 会被编进 AcpHal / xapu / chad / xg_gens / XCRDAPI 五个 DLL，
// xgameruntime.dll 不能跨 DLL 依赖它，所以按同样的语义（只读一次 env、400 行上限）
// 在本模块内自带一份。
#include <windows.h>

#include <atomic>

#include "xbgtasix/trace.h"

#include "trace_entry.h"

namespace xbgtasix {
namespace {

// 整局最多记这么多行：初始化阶段的前 400 次进入足够还原调用顺序，又不会把
// xbgtasix.log 刷到没法读（trace 是排障用的，不是性能剖析）。
constexpr int kMaxTraceLines = 400;

bool read_trace_env() {
    char b[8] = {0};
    const DWORD n = GetEnvironmentVariableA("XBGTASIX_TRACE", b, sizeof(b));
    return n > 0 && b[0] == '1';
}

// 计数用 atomic：两个线程同时进入时也不会把行数打过上限
// （logf 自己带锁，但"自增 -> 判断"这一步必须原子）。
std::atomic<int> g_trace_lines{0};

}  // namespace

bool trace_entry_enabled() {
    // static const bool：只读一次环境变量，之后每次调用只是一次已初始化标志读取。
    static const bool on = read_trace_env();
    return on;
}

void trace_entry_log(const char* api, unsigned long long a1, unsigned long long a2,
                     unsigned long long a3, unsigned long long a4) {
    if (!trace_entry_enabled()) return;
    // 超过上限就静默丢弃（不写"已达上限"的尾巴，保证总行数严格不超过 400）
    if (g_trace_lines.fetch_add(1, std::memory_order_relaxed) >= kMaxTraceLines) return;
    xbgtasix::logf("[trace] %s(0x%llx, 0x%llx, 0x%llx, 0x%llx)", api ? api : "?",
                   a1, a2, a3, a4);
}

}  // namespace xbgtasix
