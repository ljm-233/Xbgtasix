// SPDX-License-Identifier: GPL-3.0-or-later
// trace_entry.h —— xgameruntime.dll 手写导出函数的统一入口追踪
//
// 为什么需要它：真实游戏在初始化约 1.45 秒后崩在自己的断言上（某个结构体
// +0xC 的 int 必须非 0），而 268 个手写导出一个调用记录都没有，根本看不出
// 游戏到底调了哪些 API、按什么顺序调。这里给每个导出函数体的第一行插一行
// XB_TRACE_ENTRY，就能拿到完整的入口序列。
//
// 语义与 console_hw.cpp 里的 trace_on() 一致：XBGTASIX_TRACE=1 时开启，
// 环境变量只读一次，最多记 400 行。**故意不跨 DLL 依赖**：console_hw 的实现
// 被编进 AcpHal / xapu / chad / xg_gens / XCRDAPI 五个模块，xgameruntime.dll
// 不该反向依赖它们（加载顺序与导出集都不一样），所以这里按同一语义各留一份。
//
// 关闭时零开销：每个调用点用 static const bool 缓存开关，未开启时成本只有
// 一次"已初始化标志"的读取 —— 不读环境变量、不求值参数、不调用日志函数。
#pragma once

#include <cstdint>
#include <type_traits>

namespace xbgtasix {

// XBGTASIX_TRACE=1 时返回 true。环境变量只读一次（C++11 起函数内 static 线程安全）。
bool trace_entry_enabled();

// 打一行 [trace] Api(0x.., 0x.., 0x.., 0x..)。内部限流 400 行，超出静默丢弃。
// 参数一律按 unsigned long long 传：x64 下前 4 个参数就是 rcx/rdx/r8/r9。
void trace_entry_log(const char* api, unsigned long long a1, unsigned long long a2,
                     unsigned long long a3, unsigned long long a4);

namespace trace_detail {

// 形参类型不统一（指针 / 整数 / 枚举 / 按值结构体 / 函数指针），这里统一折算成
// unsigned long long 再交给日志格式串，免得每个函数各写一遍转换：
//   - 指针（含函数指针）：取地址值（就是 x64 寄存器里的那个值）
//   - 整数 / bool / 枚举：直接零扩展
//   - 其它（按值传的结构体等）：记 0，不碰对象本身（不拷贝、不猜布局）
// 引用类型在插桩时已经按 0ULL 传入，走不到这里。
template <typename T>
unsigned long long tr_arg(const T& v) noexcept {
    (void)v;
    if constexpr (std::is_pointer_v<T>) {
        return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(v));
    } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
        return static_cast<unsigned long long>(v);
    } else {
        return 0ULL;
    }
}

}  // namespace trace_detail

}  // namespace xbgtasix

// 插桩宏：放在每个导出函数体的第一行。
// 按 x64 调用约定记录前 4 个形参（rcx/rdx/r8/r9），形参不足 4 个时补 0ULL。
#define XB_TRACE_ENTRY(api, a1, a2, a3, a4)                                         \
    do {                                                                            \
        static const bool xb_trace_on_ = ::xbgtasix::trace_entry_enabled();         \
        if (xb_trace_on_) {                                                         \
            ::xbgtasix::trace_entry_log(api, ::xbgtasix::trace_detail::tr_arg(a1),  \
                                        ::xbgtasix::trace_detail::tr_arg(a2),       \
                                        ::xbgtasix::trace_detail::tr_arg(a3),       \
                                        ::xbgtasix::trace_detail::tr_arg(a4));      \
        }                                                                           \
    } while (0)
