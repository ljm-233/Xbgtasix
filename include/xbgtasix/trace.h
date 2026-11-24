// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// xbgtasix: 共享的日志/未实现 API 策略
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace xbgtasix {

// XBGTASIX_UNIMPL=stop|log  （默认 stop：未实现就打印调用名并退出，避免"猜错语义继续跑"）
// XBGTASIX_LOG=<path>        （默认 exe 同目录 xbgtasix.log）
enum class UnimplMode { Stop, Log };

void logf(const char* fmt, ...);
void unimpl(const char* api);
UnimplMode unimpl_mode();

// XBGTASIX_WATCH_CTX=1：对游戏线程本地上下文里的计数 ctx+0xC04 装硬件写断点（Dr1），
// 谁写、写多少都会打一行 [xbgtasix]。第 4 堵墙（game+0x2237178）就是被它=0 卡住的。
void install_ctx_write_watchpoint();

// XBGTASIX_TRACE_AT=<rva>,<rva>：在游戏的两个 RVA 上下执行断点（Dr2/Dr3），命中时把
// rbx / rdi 当 C 字符串打出来。用来"看路径"，而不是靠反汇编猜路径。
void install_trace_breakpoints();

// XBGTASIX_INT3_AT=<rva>,<rva>：一次性 int3 探针（硬件执行断点在本机 wine 下不投递事件）。
// 命中时把 rbx / rdi 当 C 字符串打进日志，然后还原字节继续跑。
void install_int3_probes();

void set_game_root(const char* path);
const char* game_root();

// 由 DllMain 在进程早期调用：把主机盘符（G: 持久存储 / T: 临时盘）映射到 PC 目录。
// 实测游戏启动期会直接探测 G:\appdata0.rpf 与 T:\<guid>.dmp。
void install_console_drives();

} // namespace xbgtasix

#define XB_UNIMPL(api) ::xbgtasix::unimpl(api)
