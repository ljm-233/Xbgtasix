// xbgtasix: 日志 + 未实现 API 策略
#include "xbgtasix/trace.h"

#include <windows.h>

#include <mutex>
#include <set>
#include <string>

namespace xbgtasix {
namespace {

std::mutex g_mu;
FILE* g_file = nullptr;
std::set<std::string> g_seen;
char g_root[MAX_PATH] = {0};

void ensure_open() {
    if (g_file) return;
    char path[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableA("XBGTASIX_LOG", path, MAX_PATH);
    if (n == 0) {
        char exe[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        if (char* slash = strrchr(exe, '\\')) *(slash + 1) = 0;
        _snprintf(path, MAX_PATH, "%sxbgtasix.log", exe);
    }
    g_file = fopen(path, "a");
}

}  // namespace

UnimplMode unimpl_mode() {
    char buf[16] = {0};
    DWORD n = GetEnvironmentVariableA("XBGTASIX_UNIMPL", buf, sizeof(buf));
    if (n > 0 && _stricmp(buf, "log") == 0) return UnimplMode::Log;
    return UnimplMode::Stop;
}

void logf(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mu);
    ensure_open();
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    if (g_file) {
        va_start(ap, fmt);
        vfprintf(g_file, fmt, ap);
        va_end(ap);
        fputc('\n', g_file);
        fflush(g_file);
    }
}

void unimpl(const char* api) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_seen.insert(api).second) {
            // 只报第一次，避免日志被刷爆
            if (unimpl_mode() == UnimplMode::Log) return;
        }
    }
    logf("[xbgtasix] UNIMPLEMENTED: %s", api);
    if (unimpl_mode() == UnimplMode::Stop) {
        logf("[xbgtasix] stop (set XBGTASIX_UNIMPL=log to continue)");
        ExitProcess(70);
    }
}

void set_game_root(const char* path) {
    std::lock_guard<std::mutex> lock(g_mu);
    strncpy(g_root, path ? path : "", MAX_PATH - 1);
}

const char* game_root() { return g_root; }

}  // namespace xbgtasix
