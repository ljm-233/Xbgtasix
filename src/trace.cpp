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
        // 显式路径打不开时不能直接放弃：调用方（启动器）可能给了个相对/不可写路径。
        // 继续走下面的回落链，至少还能落一份日志。
        fprintf(stderr, "[xbgtasix] 警告：XBGTASIX_LOG=%s 打不开，改用回落路径\n", path);
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

// ---------- 栈帧符号化 ----------
// ASLR 下裸地址每重启一次就变、没有 PDB 时根本没法定位"谁调了未实现 API"。
// 把帧换算成 模块文件名+0xRVA 之后，只要事后对同一份 exe/dll 跑 objdump -d，
// 就能把 RVA 对回具体代码位置。
constexpr DWORD kNameW = 512;      // 模块全路径（宽字符）缓冲
constexpr size_t kNameU8 = 1600;   // UTF-8 文件名缓冲（最坏 3 字节/宽字符 + '\0'）

// 取模块的"仅文件名"（UTF-8）。日志里不需要长路径，而且各机器安装路径不同，
// 只有文件名在排障时才有意义。
bool module_short_name_utf8(HMODULE mod, char* out, size_t out_size) {
    if (!mod || !out || out_size == 0) return false;
    wchar_t wpath[kNameW] = {0};
    const DWORD nw = GetModuleFileNameW(mod, wpath, kNameW);
    if (nw == 0) return false;          // 句柄无效/路径取不到：交给调用方兜底
    if (nw >= kNameW) return false;     // 路径过长被截断：丢了文件名还不如直接兜底
    const DWORD len = nw;               // 未截断时 wpath[nw] 已经是 '\0'
    // 只保留文件名：同时认 '\\' 和 '/'，别的地方灌进来的路径可能是混合分隔符
    const wchar_t* base = wpath;
    for (DWORD i = 0; i < len; ++i) {
        if (wpath[i] == L'\\' || wpath[i] == L'/') base = wpath + i + 1;
    }
    if (*base == L'\0') return false;
    // 宽字符 -> UTF-8（项目日志统一 UTF-8）。cchWideChar 传 -1 让 API 自己算长度，
    // 不要手工按 n-1 传，否则会掉最后一个字符；返 byte 数含结尾 '\0'，直接可用。
    const int nb = WideCharToMultiByte(CP_UTF8, 0, base, -1, out, static_cast<int>(out_size),
                                       nullptr, nullptr);
    if (nb <= 0) return false;   // 缓冲不足（名字过长）时同样走兜底
    return true;
}

// 打一帧：#<序号> <模块文件名>+0x<RVA>；解析不到模块时打 <unknown>+0x<原始地址>。
// 注意：本函数不持有 g_mu —— logf 内部自己加锁，外面再套一把会死锁。
void log_frame(unsigned index, void* addr) {
    const uintptr_t u = reinterpret_cast<uintptr_t>(addr);
    if (addr) {
        HMODULE mod = nullptr;
        // GetModuleHandleExW 第一参数声明是 LPCWSTR，但 FROM_ADDRESS 模式下传的其实是
        // "地址"而不是字符串；按 Windows API 惯例直接 reinterpret_cast 强转（API 内部
        // 不会解引用它），这也是微软文档示例的标准写法。
        const BOOL ok = GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(addr), &mod);
        if (ok && mod) {
            char name[kNameU8] = {0};
            if (module_short_name_utf8(mod, name, sizeof(name))) {
                logf("[xbgtasix]   #%-2u %s+0x%llX", index, name,
                     static_cast<unsigned long long>(u - reinterpret_cast<uintptr_t>(mod)));
                return;
            }
        }
    }
    // 解析不到模块（JIT 生成代码、手工映射的内存、句柄获取失败）：保留原始地址，
    // 至少还能在调试器里对着地址下断点；这里不 return，每一帧都要打出去。
    logf("[xbgtasix]   #%-2u <unknown>+0x%llX", index, static_cast<unsigned long long>(u));
}

// 主模块基址：事后用 objdump 把上面的 RVA 映射回代码时要用。
// 只打主模块，枚举全部模块会让日志长得没法看。
void log_main_module_base() {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) return;
    char name[kNameU8] = {0};
    if (!module_short_name_utf8(exe, name, sizeof(name))) strcpy(name, "<unknown>");
    logf("[xbgtasix]   模块基址: %s=0x%016llX", name,
         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(exe)));
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
        log_main_module_base();   // 先给基址，再把每帧换算成 模块+0xRVA
        for (USHORT i = 0; i < n; ++i) {
            log_frame(i, frames[i]);   // 逐帧 logf；解析失败走 <unknown> 兜底，不中断
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
