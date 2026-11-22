#pragma once
// xbshim: 共享的日志/未实现 API 策略
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace xbshim {

// XBSHIM_UNIMPL=stop|log  （默认 stop：未实现就打印调用名并退出，避免"猜错语义继续跑"）
// XBSHIM_LOG=<path>        （默认 exe 同目录 xbshim.log）
enum class UnimplMode { Stop, Log };

void logf(const char* fmt, ...);
void unimpl(const char* api);
UnimplMode unimpl_mode();

void set_game_root(const char* path);
const char* game_root();

} // namespace xbshim

#define XB_UNIMPL(api) ::xbshim::unimpl(api)
