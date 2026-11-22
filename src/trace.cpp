// xbgtasix: 日志 + 未实现 API 策略
#include "xbgtasix/trace.h"

#include <windows.h>
#include <winternl.h>   // RtlCaptureStackBackTrace

#include <mutex>
#include <set>
#include <cstring>
#include <string>

namespace xbgtasix {
namespace {

std::mutex g_mu;
std::set<std::string> g_seen;
char g_root[MAX_PATH] = {0};

// 用 Win32 句柄 + FILE_APPEND_DATA 打开：每次 WriteFile 一整行，
// 在这一个句柄上原子追加（fprintf 走 stdio 缓冲，多模块并发时会切碎行）。
// exe 目录不可写时（Program Files / Game Pass 安装目录）回落到 %LOCALAPPDATA%。
HANDLE g_handle = INVALID_HANDLE_VALUE;
bool g_open_failed = false;

bool try_open(const char* path) {
    g_handle = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_handle != INVALID_HANDLE_VALUE;
}

void ensure_open() {
    if (g_handle != INVALID_HANDLE_VALUE || g_open_failed) return;
    char path[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableA("XBGTASIX_LOG", path, MAX_PATH);
    if (n > 0) {
        if (try_open(path)) return;
        g_open_failed = true;
        return;
    }
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (char* slash = strrchr(exe, '\\')) *(slash + 1) = 0;
    _snprintf(path, MAX_PATH, "%sxbgtasix.log", exe);
    if (try_open(path)) return;

    char base[MAX_PATH] = {0};
    const DWORD bn = GetEnvironmentVariableA("XBGTASIX_PLS_ROOT", base, MAX_PATH);
    if (bn > 0) {
        _snprintf(path, MAX_PATH, "%s\\xbgtasix.log", base);
    } else if (GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH) > 0) {
        _snprintf(path, MAX_PATH, "%s\\Xbgtasix\\xbgtasix.log", base);
        // 目录可能不存在：逐级建一次，失败就放弃落盘（stderr 仍有输出）
        std::string d = std::string(base) + "\\Xbgtasix";
        CreateDirectoryA(d.c_str(), nullptr);
    }
    if (!try_open(path)) g_open_failed = true;
}

}  // namespace

UnimplMode unimpl_mode() {
    char buf[16] = {0};
    DWORD n = GetEnvironmentVariableA("XBGTASIX_UNIMPL", buf, sizeof(buf));
    if (n > 0 && _stricmp(buf, "log") == 0) return UnimplMode::Log;
    return UnimplMode::Stop;
}

void logf(const char* fmt, ...) {
    // 先在锁外把整行格式化好，再在锁内一次 WriteFile：
    // 原来持锁 fprintf+fflush，stderr 若接的是无人读的管道会整段卡死
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    const size_t len = strlen(line);

    fwrite(line, 1, len, stderr);
    fputc('\n', stderr);
    fflush(stderr);   // stderr 本来就不该在锁内 flush

    std::lock_guard<std::mutex> lock(g_mu);
    ensure_open();
    if (g_handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        char buf[2064];
        const int n = _snprintf(buf, sizeof(buf), "%s\n", line);
        WriteFile(g_handle, buf, static_cast<DWORD>(n > 0 ? n : 0), &written, nullptr);
    }
}

// 环境变量只读一次：桩是热路径，而且运行期改 env 会让行为不确定
UnimplMode unimpl_mode_cached() {
    static const UnimplMode mode = unimpl_mode();
    return mode;
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
    if (unimpl_mode_cached() == UnimplMode::Stop) {
        // 退出前先留下"谁调的"：只有 API 名对定位没帮助，栈比名字有用得多。
        void* frames[24] = {0};
        const USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
        for (USHORT i = 0; i < n; ++i) {
            logf("[xbgtasix]   #%-2u %p", i, frames[i]);
        }
        logf("[xbgtasix] stop (set XBGTASIX_UNIMPL=log to continue)");
        // ExitProcess 会等所有 DLL 的 DllMain(detach)：若我们此刻正持有 loader lock，
        // 就会互等死锁。TerminateProcess 跳过这些回调，适合"诊断性退出"。
        if (GetModuleHandleA("ntdll.dll")) {
            using PFN = VOID(WINAPI*)(HANDLE, UINT);
            auto fn = reinterpret_cast<PFN>(
                GetProcAddress(GetModuleHandleA("kernel32.dll"), "TerminateProcess"));
            if (fn) fn(GetCurrentProcess(), 70);
        }
        ExitProcess(70);
    }
}

void set_game_root(const char* path) {
    std::lock_guard<std::mutex> lock(g_mu);
    strncpy(g_root, path ? path : "", MAX_PATH - 1);
}

const char* game_root() { return g_root; }

}  // namespace xbgtasix
