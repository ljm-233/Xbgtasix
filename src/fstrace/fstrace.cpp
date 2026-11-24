// SPDX-License-Identifier: GPL-3.0-or-later
// fstrace.cpp —— 主 exe 文件 API 的 IAT 挂钩层：**路径重写（常开）** + 追踪（可选）
//
// 这一层现在干两件事，共用同一批 thunk：
//   A) 路径重写 —— 把 "G:\x" / "G:/x" 换成包目录里的真实路径再转发。
//      开关 XBGTASIX_PATH_REWRITE（默认开）/ XBGTASIX_NO_PATH_REWRITE=1（回退盘符模式）。
//      为什么要在这一层做：盘符是宿主耦合（Wine 只认启动前的 $WINEPREFIX/dosdevices，
//      运行时 DefineDosDeviceW 三种写法和注册表都无效）。重写之后 Windows 和 Wine
//      走同一条代码路径。细节与表的来源见 drives.cpp。
//   B) 追踪 —— XBGTASIX_FSTRACE=1 时把每次文件 API 调用记一行（去重、上限 500 行）。
//      只有它开着才写日志；只开重写时一条日志都不写，开销是"查表 + 可能的替换"。
//
// 为什么需要它
// ------------
// 目标游戏 game_scarlett_master.exe（无符号，114 MB）初始化约 1.4 秒后崩在自己的
// 裸断言上（int3，RVA 0x2237178）。交叉引用分析显示它在解析自己的虚拟文件系统
// 挂载表 —— packroot:/ 、packcache:/config 是 RAGE 引擎的伪路径前缀 —— 拿不到
// 配置就带着全 0 结构继续跑，最后撞断言。要回答"到底读了哪些文件、去哪找挂载点"，
// 最直接的办法就是把主 exe 的文件 API 入口全部记下来。
//
// 做法
// ----
// 只改**主模块**（GetModuleHandleW(nullptr)）的导入地址表（IAT）：把指向
// kernel32 / api-ms-win-core-file-* 的那几个槽换成我们的 thunk，原函数指针存进
// g_orig，thunk 记一行日志后原样转发。手写 PE 解析，不用 detours 等第三方库。
//
// 三条硬约束（一切都是为了"绝不能让追踪本身把游戏弄挂"）
//  1. 写槽前 VirtualProtect 改成可写，写完立刻恢复原属性；任何一步解析失败
//     （PE 头、导入目录、越界 RVA、内存不可读）就整体放弃并只记一行日志。
//  2. 只 patch 主 exe 的 IAT，绝不碰系统 DLL 的代码/数据；导入表里没有的 API
//     直接跳过（实测本作 exe 只导入了 7 个目标里的 5 个：CreateFileW /
//     FindFirstFileW / GetFileAttributesW / GetFileAttributesExW / ReadFile，
//     全在 api-ms-win-core-file-l1-1-0.dll 里；CreateFile2 与 CopyFileW 根本不在
//     导入表，属于正常跳过而不是失败）。
//  3. 两个开关都关时**一个字节都不改**：连 IAT 都不 patch（比"patch 了但只转发"更保守，
//     少一条可能影响游戏行为的路径）；thunk 里仍然保留"没开就纯转发"的判断，
//     作为第二道保险。
//
// 重入（很关键）
// -------------
// thunk 里要写日志，而日志实现 xbgtasix::logf 内部会 CreateFileA / WriteFile ——
// 这两个都**不在**我们的挂钩名单里，所以今天不构成递归。但为了不依赖这个巧合，
// 仍然加了 thread_local 重入闸：只要本线程已经在某个 thunk 里，内层调用一律只
// 转发、不写日志，杜绝"日志 -> 文件 API -> 日志"的无限递归。
// 另外去重锁与日志锁是两把独立的锁，且 logf 一律在**释放去重锁之后**调用（先
// claim_line 判断，再在锁外 emit），两者不构成锁序，不会死锁。
//
// 只被编进 xmem.dll：build.sh / tools/build_linux_cross.sh 都是
// "src/xmem/xmem.cpp + src/fstrace/{fstrace,drives}.cpp + host_stall + trace" 一起编。
// xmem.dll 是游戏**静态导入**的，所以它的 DllMain 比游戏第一行代码更早跑。
#include <windows.h>

#include "xbgtasix/host_stall.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "xbgtasix/trace.h"

namespace xbgtasix {
namespace fstrace {

// 初始化入口。目前由本文件末尾的 DllMain 调用；若将来 DllMain 需要挪到别处，
// 直接调用这两个函数即可（install 幂等，recheck 只在极端情况下才有实际动作）。
void install_if_enabled();
void recheck_if_needed();

namespace {

static_assert(sizeof(void*) == 8, "fstrace 只做 x64 的 IAT 解析（IMAGE_THUNK_DATA64）");

// ------------------------------------------------------------------ 开关
// 环境变量只读一次，缓存进 static const bool：thunk 处在游戏的每一次文件调用上，
// 热路径里不能再有 GetEnvironmentVariable 的成本。
bool read_env_switch() {
    char b[8] = {0};
    const DWORD n = GetEnvironmentVariableA("XBGTASIX_FSTRACE", b, sizeof(b));
    return n > 0 && b[0] == '1';
}

// 追踪（XBGTASIX_FSTRACE=1）：只有它开启时才写日志。
bool tracing() {
    static const bool on = read_env_switch();
    return on;
}

// 是否要接管 IAT：追踪**或**路径重写任一开启。
// 这两件事共用一个 thunk 层（都在文件 API 转发之前动手），所以装钩子的条件取并集：
// 只做追踪时行为与从前完全一致（不重写）；只做重写时不写那 500 行/500 条日志。
bool active() {
    static const bool on = tracing() || xbgtasix::path_rewrite_enabled();
    return on;
}

// 路径重写的小包装：命中就返回替换后的缓冲，否则原样返回入参指针（零拷贝放行）。
// 用 std::wstring 是因为重写结果长度不定；只在命中时才分配。
struct RwPath {
    std::wstring buf;
    bool hit = false;
    const wchar_t* apply(const wchar_t* in) {
        hit = xbgtasix::rewrite_console_path(in, buf);
        return hit ? buf.c_str() : in;
    }
};

// ------------------------------------------------------------ 上限与缓冲
constexpr int kMaxLogLines = 500;      // 去重后的日志行上限（超了静默丢弃）
constexpr size_t kPathU8Buf = 1024;    // 单条路径的 UTF-8 缓冲
constexpr size_t kMaxWidePath = 300;   // 超长路径截断成前 300 个宽字符（<=900 字节）
constexpr size_t kMaxNameLen = 512;    // 导入 DLL 名 / 导入函数名的长度上限
constexpr size_t kMaxImportDescs = 4096;
constexpr size_t kMaxThunksPerDesc = 8192;
constexpr int kMaxSlotsPerApi = 4;     // 同一个 API 最多接管几个 IAT 槽（正常只有 1 个）

// ------------------------------------------------------------ 重入闸
thread_local bool t_in_thunk = false;

class ReentryGuard {
  public:
    ReentryGuard() noexcept : outermost_(!t_in_thunk) { t_in_thunk = true; }
    ~ReentryGuard() {
        if (outermost_) t_in_thunk = false;   // 内层帧不负责复位，由最外层负责
    }
    ReentryGuard(const ReentryGuard&) = delete;
    ReentryGuard& operator=(const ReentryGuard&) = delete;
    bool outermost() const noexcept { return outermost_; }

  private:
    bool outermost_;
};

// ------------------------------------------------------------ 去重表
// 用 new 分配且永不释放：DLL 卸载（进程退出）时不再跑析构，避免在 loader 里
// 做 set/string 析构这种"退出期才炸"的事。
struct SeenStore {
    std::mutex mu;
    std::set<std::string> seen;
    int lines = 0;
};

SeenStore& seen_store() {
    static SeenStore* s = new SeenStore();
    return *s;
}

// 还允许再记几行？给 ReadFile 用：预算用完就不必再做"句柄 -> 路径"解析了。
// 不抛异常：thunk 在游戏调用路径上，任何异常都不许漏出去。
bool budget_left() noexcept {
    try {
        SeenStore& s = seen_store();
        std::lock_guard<std::mutex> lk(s.mu);
        return s.lines < kMaxLogLines;
    } catch (...) {
        return false;
    }
}

// 登记一行：第一次见到这个 key 才返回 true。**不持锁调用 logf**。
// try/catch：std::set 插入要分配节点，极端情况下（堆被占满）会抛 bad_alloc。
// 我们的 thunk 是插在游戏的调用路径中间的，异常绝不允许漏到游戏那一侧 ——
// 记不下这一行就丢掉，转发照旧。
bool claim_line(const char* key) noexcept {
    try {
        SeenStore& s = seen_store();
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.lines >= kMaxLogLines) return false;
        if (!s.seen.insert(key).second) return false;   // 同一个 (API, 路径) 只记第一次
        ++s.lines;
        return true;
    } catch (...) {
        return false;
    }
}

// ------------------------------------------------------------ 文本工具
// 宽字符 -> UTF-8。超长路径（最长 32767 宽字符）截断成前缀 + "..."：
// 日志里能看出挂载点前缀就够了，不必整条打出来。
bool path_to_utf8(const wchar_t* w, char* out, size_t out_size) {
    if (!out || out_size < 16) return false;
    out[0] = 0;
    if (!w) return false;
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, static_cast<int>(out_size),
                                      nullptr, nullptr);
    if (n > 0) return true;
    size_t wl = 0;
    while (wl < kMaxWidePath && w[wl] != 0) ++wl;
    if (wl == 0) return false;
    const int m = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(wl), out,
                                      static_cast<int>(out_size) - 4, nullptr, nullptr);
    if (m <= 0) {
        out[0] = 0;
        return false;
    }
    out[m] = 0;
    strncat(out, "...", out_size - strlen(out) - 1);
    return true;
}

// ANSI 路径 -> UTF-8（只为日志）。先按 ACP 转 wide，再走上面那条路。
bool path_to_utf8_ansi(const char* s, char* out, size_t out_size) {
    if (!s || !out || out_size < 16) return false;
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 0) return false;
    std::wstring w(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n) != n) return false;
    w.resize(static_cast<size_t>(n - 1));
    return path_to_utf8(w.c_str(), out, out_size);
}

// dwDesiredAccess 解码成可读文本，例如 "GENERIC_READ|FILE_READ_DATA+0x2"。
// 认不出的位追加 +0x..，保证信息不丢。
const char* access_text(DWORD access, char* buf, size_t n) {
    struct Bit {
        DWORD mask;
        const char* name;
    };
    static const Bit kBits[] = {
        {GENERIC_READ, "GENERIC_READ"},
        {GENERIC_WRITE, "GENERIC_WRITE"},
        {GENERIC_EXECUTE, "GENERIC_EXECUTE"},
        {GENERIC_ALL, "GENERIC_ALL"},
        {FILE_READ_DATA, "FILE_READ_DATA"},
        {FILE_WRITE_DATA, "FILE_WRITE_DATA"},
        {FILE_APPEND_DATA, "FILE_APPEND_DATA"},
        {DELETE, "DELETE"},
        {SYNCHRONIZE, "SYNCHRONIZE"},
    };
    if (!buf || n < 8) return "?";
    buf[0] = 0;
    DWORD left = access;
    for (const Bit& b : kBits) {
        if ((left & b.mask) == 0) continue;
        if (buf[0]) strncat(buf, "|", n - strlen(buf) - 1);
        strncat(buf, b.name, n - strlen(buf) - 1);
        left &= ~b.mask;
    }
    if (left != 0) {
        char hex[24];
        snprintf(hex, sizeof(hex), "%s0x%lX", buf[0] ? "+" : "", static_cast<unsigned long>(left));
        strncat(buf, hex, n - strlen(buf) - 1);
    }
    if (buf[0] == 0) snprintf(buf, n, "0(无访问位)");
    return buf;
}

// 统一的落盘格式（中文说明）。整行自己拼好再用 logf("%s")：路径里带 % 也不会
// 被当成格式串。
void emit(const char* api, const char* path, const char* access, const char* result, DWORD err) {
    char line[1536];
    snprintf(line, sizeof(line), "[fstrace] %s 路径=\"%s\" 期望=%s 结果=%s err=%lu", api,
             path && path[0] ? path : "?", access && access[0] ? access : "?", result ? result : "?",
             static_cast<unsigned long>(err));
    line[sizeof(line) - 1] = 0;
    xbgtasix::logf("%s", line);
}

// 句柄 -> 路径（给 ReadFile 用：它只拿得到 HANDLE，"读了哪个文件"必须还原出来）。
// 取不到（管道、权限不足、超长）就返回空串，调用方回落到句柄值。
void handle_to_path(HANDLE h, char* out, size_t out_size) {
    if (!out || out_size < 16) return;
    out[0] = 0;
    if (!h || h == INVALID_HANDLE_VALUE) return;
    wchar_t w[MAX_PATH] = {0};
    const DWORD len = GetFinalPathNameByHandleW(
        h, w, static_cast<DWORD>(sizeof(w) / sizeof(w[0])), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (len == 0 || len >= sizeof(w) / sizeof(w[0])) return;
    path_to_utf8(w, out, out_size);
}

// ------------------------------------------------------------ 原函数表
struct OrigFns {
    decltype(&::CreateFileW) CreateFileW = nullptr;
    decltype(&::CreateFile2) CreateFile2 = nullptr;
    decltype(&::ReadFile) ReadFile = nullptr;
    decltype(&::GetFileAttributesW) GetFileAttributesW = nullptr;
    decltype(&::GetFileAttributesExW) GetFileAttributesExW = nullptr;
    decltype(&::FindFirstFileW) FindFirstFileW = nullptr;
    decltype(&::FindFirstFileExW) FindFirstFileExW = nullptr;
    decltype(&::CopyFileW) CopyFileW = nullptr;
    decltype(&::GetFileAttributesA) GetFileAttributesA = nullptr;
    decltype(&::CreateDirectoryW) CreateDirectoryW = nullptr;
    decltype(&::RemoveDirectoryW) RemoveDirectoryW = nullptr;
    decltype(&::DeleteFileW) DeleteFileW = nullptr;
    decltype(&::MoveFileExW) MoveFileExW = nullptr;
    decltype(&::SetFileAttributesW) SetFileAttributesW = nullptr;
};

OrigFns g_orig;

// 把原函数指针写进 g_orig 的对应成员。用 memcpy 而不是 *(void**)&member = p：
// 函数指针与 void* 是不同对象类型，直接写属于类型双关（严格说是 UB，-O2 下
// 理论上可被优化掉），memcpy 是"按位拷贝"的唯一无争议写法。
static_assert(sizeof(g_orig.CreateFileW) == sizeof(void*), "x64 下函数指针与 void* 同宽");
void store_orig(void** field, void* value) { memcpy(field, &value, sizeof(void*)); }

// ============================================================ thunk
// 每个 thunk 的结构都一样：
//   1) 两个开关都关 -> 直接转发（正常情况下这些槽根本没被改过，走不到这里）
//   2) 原指针为空 -> 返回失败而不是空指针调用（正常不可能发生：install 保证
//      "先拿到原函数指针，再改写槽"）
//   3) **先重写路径**（RwPath::apply），再把重写后的指针转发下去。命中与否都会
//      走同一次原函数调用，游戏那一侧的入参缓冲一字不改（我们只读）。
//   4) 先转发，再取 GetLastError，再记日志，最后 SetLastError 还原 ——
//      日志内部的 CreateFileA/WriteFile/GetFinalPathNameByHandleW 会覆盖
//      LastError，不还原就等于我们偷偷改了游戏看得见的状态。
// 重写不算"多一条路径"：没命中（UNC/相对路径/表外盘符）时 apply 返回原指针，
// 和从前完全一致。
HANDLE WINAPI hook_CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                               LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                               DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
                               HANDLE hTemplateFile) {
    if (!active()) {
        return g_orig.CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                  dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    }
    if (!g_orig.CreateFileW) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return INVALID_HANDLE_VALUE;
    }
    ReentryGuard guard;
    // 路径重写：G:\x / G:/x -> 真实目录下的 x（见 drives.cpp）。不命中就原样转发。
    RwPath rw;
    const wchar_t* const name = rw.apply(lpFileName);
    const HANDLE h = g_orig.CreateFileW(name, dwDesiredAccess, dwShareMode,
                                        lpSecurityAttributes, dwCreationDisposition,
                                        dwFlagsAndAttributes, hTemplateFile);
    const DWORD err = GetLastError();
    if (tracing() && guard.outermost()) {
        char path[kPathU8Buf];
        if (!path_to_utf8(lpFileName, path, sizeof(path))) {
            snprintf(path, sizeof(path), "(路径转 UTF-8 失败)");
        }
        char acc[256];
        access_text(dwDesiredAccess, acc, sizeof(acc));
        char key[kPathU8Buf + 32];
        snprintf(key, sizeof(key), "CreateFileW|%s", path);
        if (claim_line(key)) {
            char res[64];
            if (h == INVALID_HANDLE_VALUE || h == nullptr) {
                snprintf(res, sizeof(res), "失败");
            } else {
                snprintf(res, sizeof(res), "HANDLE=0x%llX",
                         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
            }
            emit("CreateFileW", path, acc, res, err);
        }
    }
    SetLastError(err);
    return h;
}

HANDLE WINAPI hook_CreateFile2(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                               DWORD dwCreationDisposition,
                               LPCREATEFILE2_EXTENDED_PARAMETERS pCreateExParams) {
    if (!active()) {
        return g_orig.CreateFile2(lpFileName, dwDesiredAccess, dwShareMode, dwCreationDisposition,
                                  pCreateExParams);
    }
    if (!g_orig.CreateFile2) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return INVALID_HANDLE_VALUE;
    }
    ReentryGuard guard;
    RwPath rw;
    const wchar_t* const name = rw.apply(lpFileName);
    const HANDLE h = g_orig.CreateFile2(name, dwDesiredAccess, dwShareMode,
                                        dwCreationDisposition, pCreateExParams);
    const DWORD err = GetLastError();
    if (tracing() && guard.outermost()) {
        char path[kPathU8Buf];
        if (!path_to_utf8(lpFileName, path, sizeof(path))) {
            snprintf(path, sizeof(path), "(路径转 UTF-8 失败)");
        }
        char acc[256];
        access_text(dwDesiredAccess, acc, sizeof(acc));
        char key[kPathU8Buf + 32];
        snprintf(key, sizeof(key), "CreateFile2|%s", path);
        if (claim_line(key)) {
            char res[64];
            if (h == INVALID_HANDLE_VALUE || h == nullptr) {
                snprintf(res, sizeof(res), "失败");
            } else {
                snprintf(res, sizeof(res), "HANDLE=0x%llX",
                         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
            }
            emit("CreateFile2", path, acc, res, err);
        }
    }
    SetLastError(err);
    return h;
}

BOOL WINAPI hook_ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
                          LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped) {
    // ReadFile 只拿 HANDLE，没有路径可重写 —— 这个钩子纯粹为了追踪。
    // 重写模式（不追踪）下直接转发，不做任何额外动作。
    if (!tracing()) {
        return g_orig.ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead,
                               lpOverlapped);
    }
    if (!g_orig.ReadFile) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    ReentryGuard guard;
    const BOOL ok =
        g_orig.ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
    const DWORD err = GetLastError();
    // budget_left() 先把"日志预算已用完"挡掉：此时不必再为每条 ReadFile 做一次
    // 句柄 -> 路径解析（那是热路径上最贵的部分）。
    if (guard.outermost() && budget_left()) {
        char path[kPathU8Buf];
        char disp[kPathU8Buf];
        handle_to_path(hFile, path, sizeof(path));
        if (path[0]) {
            snprintf(disp, sizeof(disp), "%s", path);
        } else {
            snprintf(disp, sizeof(disp), "(句柄 0x%llX)",
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hFile)));
        }
        char key[kPathU8Buf + 32];
        snprintf(key, sizeof(key), "ReadFile|%s", disp);
        if (claim_line(key)) {
            char res[96];
            if (ok) {
                // 同步读才会回填字节数；异步（overlapped）时读它没有意义
                const DWORD got = (lpOverlapped == nullptr && lpNumberOfBytesRead)
                                      ? *lpNumberOfBytesRead
                                      : 0;
                snprintf(res, sizeof(res), "成功(读 %lu 字节)", static_cast<unsigned long>(got));
            } else {
                snprintf(res, sizeof(res), "失败");
            }
            emit("ReadFile", disp, "读", res, err);
        }
    }
    SetLastError(err);
    return ok;
}

DWORD WINAPI hook_GetFileAttributesW(LPCWSTR lpFileName) {
    if (!active()) return g_orig.GetFileAttributesW(lpFileName);
    if (!g_orig.GetFileAttributesW) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return INVALID_FILE_ATTRIBUTES;
    }
    ReentryGuard guard;
    RwPath rw;
    const DWORD attrs = g_orig.GetFileAttributesW(rw.apply(lpFileName));
    const DWORD err = GetLastError();
    if (tracing() && guard.outermost()) {
        char path[kPathU8Buf];
        if (!path_to_utf8(lpFileName, path, sizeof(path))) {
            snprintf(path, sizeof(path), "(路径转 UTF-8 失败)");
        }
        char key[kPathU8Buf + 32];
        snprintf(key, sizeof(key), "GetFileAttributesW|%s", path);
        if (claim_line(key)) {
            char res[64];
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                snprintf(res, sizeof(res), "失败(INVALID_FILE_ATTRIBUTES)");
            } else {
                snprintf(res, sizeof(res), "属性=0x%lX", static_cast<unsigned long>(attrs));
            }
            emit("GetFileAttributesW", path, "查询属性", res, err);
        }
    }
    SetLastError(err);
    return attrs;
}

BOOL WINAPI hook_GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId,
                                      LPVOID lpFileInformation) {
    if (!active()) return g_orig.GetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);
    if (!g_orig.GetFileAttributesExW) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    ReentryGuard guard;
    RwPath rw;
    const BOOL ok = g_orig.GetFileAttributesExW(rw.apply(lpFileName), fInfoLevelId, lpFileInformation);
    const DWORD err = GetLastError();
    if (tracing() && guard.outermost()) {
        char path[kPathU8Buf];
        if (!path_to_utf8(lpFileName, path, sizeof(path))) {
            snprintf(path, sizeof(path), "(路径转 UTF-8 失败)");
        }
        char key[kPathU8Buf + 32];
        snprintf(key, sizeof(key), "GetFileAttributesExW|%s", path);
        if (claim_line(key)) {
            char res[64];
            snprintf(res, sizeof(res), ok ? "成功" : "失败");
            emit("GetFileAttributesExW", path, "查询属性(含大小/时间)", res, err);
        }
    }
    SetLastError(err);
    return ok;
}

HANDLE WINAPI hook_FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData) {
    if (!active()) return g_orig.FindFirstFileW(lpFileName, lpFindFileData);
    if (!g_orig.FindFirstFileW) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return INVALID_HANDLE_VALUE;
    }
    ReentryGuard guard;
    RwPath rw;
    const HANDLE h = g_orig.FindFirstFileW(rw.apply(lpFileName), lpFindFileData);
    const DWORD err = GetLastError();
    if (tracing() && guard.outermost()) {
        char path[kPathU8Buf];
        if (!path_to_utf8(lpFileName, path, sizeof(path))) {
            snprintf(path, sizeof(path), "(路径转 UTF-8 失败)");
        }
        char key[kPathU8Buf + 32];
        snprintf(key, sizeof(key), "FindFirstFileW|%s", path);
        if (claim_line(key)) {
            char res[64];
            if (h == INVALID_HANDLE_VALUE || h == nullptr) {
                snprintf(res, sizeof(res), "失败(无匹配)");
            } else {
                snprintf(res, sizeof(res), "HANDLE=0x%llX",
                         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
            }
            emit("FindFirstFileW", path, "枚举目录", res, err);
        }
    }
    SetLastError(err);
    return h;
}

// FindFirstFileExW：枚举的另一种入口（游戏可能用它按大小/名字过滤目录）。
// 和 FindFirstFileW 一样只重写路径，转发原函数。
HANDLE WINAPI hook_FindFirstFileExW(LPCWSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId,
                                    LPVOID lpFindFileData, FINDEX_SEARCH_OPS fSearchOp,
                                    LPVOID lpSearchFilter, DWORD dwAdditionalFlags) {
    if (!active()) {
        return g_orig.FindFirstFileExW(lpFileName, fInfoLevelId, lpFindFileData, fSearchOp,
                                       lpSearchFilter, dwAdditionalFlags);
    }
    if (!g_orig.FindFirstFileExW) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return INVALID_HANDLE_VALUE;
    }
    ReentryGuard guard;
    RwPath rw;
    const HANDLE h = g_orig.FindFirstFileExW(rw.apply(lpFileName), fInfoLevelId, lpFindFileData,
                                             fSearchOp, lpSearchFilter, dwAdditionalFlags);
    const DWORD err = GetLastError();
    if (tracing() && guard.outermost()) {
        char path[kPathU8Buf];
        if (!path_to_utf8(lpFileName, path, sizeof(path))) {
            snprintf(path, sizeof(path), "(路径转 UTF-8 失败)");
        }
        char key[kPathU8Buf + 32];
        snprintf(key, sizeof(key), "FindFirstFileExW|%s", path);
        if (claim_line(key)) {
            char res[64];
            if (h == INVALID_HANDLE_VALUE || h == nullptr) {
                snprintf(res, sizeof(res), "失败(无匹配)");
            } else {
                snprintf(res, sizeof(res), "HANDLE=0x%llX",
                         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(h)));
            }
            emit("FindFirstFileExW", path, "枚举目录(Ex)", res, err);
        }
    }
    SetLastError(err);
    return h;
}

BOOL WINAPI hook_CopyFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName,
                           BOOL bFailIfExists) {
    if (!active()) return g_orig.CopyFileW(lpExistingFileName, lpNewFileName, bFailIfExists);
    if (!g_orig.CopyFileW) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    ReentryGuard guard;
    // 两个入参都是路径，都要重写（主机盘符上的复制：包目录 -> G:，或反过来）
    RwPath rw_from, rw_to;
    const wchar_t* const src_path = rw_from.apply(lpExistingFileName);
    const wchar_t* const dst_path = rw_to.apply(lpNewFileName);
    const BOOL ok = g_orig.CopyFileW(src_path, dst_path, bFailIfExists);
    const DWORD err = GetLastError();
    if (tracing() && guard.outermost()) {
        char from[kPathU8Buf];
        char to[kPathU8Buf];
        if (!path_to_utf8(lpExistingFileName, from, sizeof(from))) {
            snprintf(from, sizeof(from), "(路径转 UTF-8 失败)");
        }
        if (!path_to_utf8(lpNewFileName, to, sizeof(to))) snprintf(to, sizeof(to), "(?)");
        char path[2 * kPathU8Buf + 8];
        snprintf(path, sizeof(path), "%s -> %s", from, to);
        char key[2 * kPathU8Buf + 40];
        snprintf(key, sizeof(key), "CopyFileW|%s", path);
        if (claim_line(key)) {
            char res[64];
            snprintf(res, sizeof(res), ok ? "成功" : "失败");
            emit("CopyFileW", path, "复制", res, err);
        }
    }
    SetLastError(err);
    return ok;
}

// ------------------------------------------------ 编码转换（给 ANSI 变体的钩子用）
// ACP -> wide / wide -> ACP。用 CP_ACP 是因为 GetFileAttributesA 的入参就是 ACP；
// 任何一步失败都让调用方"原样转发"，绝不用猜出来的路径去开文件。
bool acp_to_wide(const char* s, std::wstring& out) {
    if (s == nullptr) return false;
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 0) return false;
    out.assign(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, s, -1, out.data(), n) != n) return false;
    out.resize(static_cast<size_t>(n - 1));
    return true;
}

bool wide_to_acp(const std::wstring& s, std::string& out) {
    const int n = WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return false;
    out.assign(static_cast<size_t>(n), '\0');
    if (WideCharToMultiByte(CP_ACP, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr) != n) {
        return false;
    }
    out.resize(static_cast<size_t>(n - 1));
    return true;
}

// 写侧 / 属性 / ANSI 变体的统一日志（只在 XBGTASIX_FSTRACE=1 时写，且逐路径去重）
void emit_simple(const char* api, const wchar_t* path, const char* what, BOOL ok, DWORD err) {
    char p[kPathU8Buf];
    if (!path_to_utf8(path, p, sizeof(p))) snprintf(p, sizeof(p), "(路径转 UTF-8 失败)");
    char key[kPathU8Buf + 32];
    snprintf(key, sizeof(key), "%s|%s", api, p);
    if (claim_line(key)) emit(api, p, what, ok ? "成功" : "失败", err);
}

// ============================================================ 写侧与 ANSI 变体
// 任务书第一步只拦了读（CreateFileW / GetFileAttributes* / FindFirstFile*），那一轮已经
// 跑到 game+0x247AF2D。这里补第二步：游戏导入表里真实存在的写侧与 ANSI 入口。
// 全部只做"重写路径 -> 转发"，表外路径（C:/D:/相对/UNC）一个字节都不动。
// 不拦 CreateFileMappingW：它收的是 HANDLE，不是路径，天然不需要重写。
DWORD WINAPI hook_GetFileAttributesA(LPCSTR lpFileName) {
    if (!active()) return g_orig.GetFileAttributesA(lpFileName);
    if (!g_orig.GetFileAttributesA) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return INVALID_FILE_ATTRIBUTES;
    }
    ReentryGuard guard;
    // ACP -> wide -> 重写 -> ACP。任何一步失败就原样转发（宁可 404 也不猜路径）。
    std::wstring wide, rewritten;
    std::string narrow;
    const char* use = lpFileName;
    if (acp_to_wide(lpFileName, wide) && xbgtasix::rewrite_console_path(wide.c_str(), rewritten) &&
        wide_to_acp(rewritten, narrow)) {
        use = narrow.c_str();
    }
    const DWORD attrs = g_orig.GetFileAttributesA(use);
    const DWORD err = GetLastError();
    if (tracing() && guard.outermost()) {
        char p[kPathU8Buf];
        if (lpFileName == nullptr || !path_to_utf8_ansi(lpFileName, p, sizeof(p))) {
            snprintf(p, sizeof(p), "(ANSI 路径)");
        }
        char key[kPathU8Buf + 32];
        snprintf(key, sizeof(key), "GetFileAttributesA|%s", p);
        if (claim_line(key)) {
            char res[64];
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                snprintf(res, sizeof(res), "失败(INVALID_FILE_ATTRIBUTES)");
            } else {
                snprintf(res, sizeof(res), "属性=0x%lX", static_cast<unsigned long>(attrs));
            }
            emit("GetFileAttributesA", p, "查询属性(ANSI)", res, err);
        }
    }
    SetLastError(err);
    return attrs;
}

BOOL WINAPI hook_CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
    if (!active()) return g_orig.CreateDirectoryW(lpPathName, lpSecurityAttributes);
    if (!g_orig.CreateDirectoryW) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    ReentryGuard guard;
    RwPath rw;
    const BOOL ok = g_orig.CreateDirectoryW(rw.apply(lpPathName), lpSecurityAttributes);
    const DWORD err = GetLastError();
    if (tracing() && guard.outermost()) emit_simple("CreateDirectoryW", lpPathName, "建目录", ok, err);
    SetLastError(err);
    return ok;
}

BOOL WINAPI hook_RemoveDirectoryW(LPCWSTR lpPathName) {
    if (!active()) return g_orig.RemoveDirectoryW(lpPathName);
    if (!g_orig.RemoveDirectoryW) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    ReentryGuard guard;
    RwPath rw;
    const BOOL ok = g_orig.RemoveDirectoryW(rw.apply(lpPathName));
    const DWORD err = GetLastError();
    if (tracing() && guard.outermost()) emit_simple("RemoveDirectoryW", lpPathName, "删目录", ok, err);
    SetLastError(err);
    return ok;
}

BOOL WINAPI hook_DeleteFileW(LPCWSTR lpFileName) {
    if (!active()) return g_orig.DeleteFileW(lpFileName);
    if (!g_orig.DeleteFileW) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    ReentryGuard guard;
    RwPath rw;
    const BOOL ok = g_orig.DeleteFileW(rw.apply(lpFileName));
    const DWORD err = GetLastError();
    if (tracing() && guard.outermost()) emit_simple("DeleteFileW", lpFileName, "删文件", ok, err);
    SetLastError(err);
    return ok;
}

BOOL WINAPI hook_SetFileAttributesW(LPCWSTR lpFileName, DWORD dwFileAttributes) {
    if (!active()) return g_orig.SetFileAttributesW(lpFileName, dwFileAttributes);
    if (!g_orig.SetFileAttributesW) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    ReentryGuard guard;
    RwPath rw;
    const BOOL ok = g_orig.SetFileAttributesW(rw.apply(lpFileName), dwFileAttributes);
    const DWORD err = GetLastError();
    if (tracing() && guard.outermost()) {
        emit_simple("SetFileAttributesW", lpFileName, "设属性", ok, err);
    }
    SetLastError(err);
    return ok;
}

BOOL WINAPI hook_MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags) {
    if (!active()) return g_orig.MoveFileExW(lpExistingFileName, lpNewFileName, dwFlags);
    if (!g_orig.MoveFileExW) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    ReentryGuard guard;
    RwPath rw_from, rw_to;   // 两个入参都是路径
    const BOOL ok = g_orig.MoveFileExW(rw_from.apply(lpExistingFileName),
                                       rw_to.apply(lpNewFileName), dwFlags);
    const DWORD err = GetLastError();
    if (tracing() && guard.outermost()) {
        char from[kPathU8Buf];
        char to[kPathU8Buf];
        if (!path_to_utf8(lpExistingFileName, from, sizeof(from))) snprintf(from, sizeof(from), "(?)");
        if (!path_to_utf8(lpNewFileName, to, sizeof(to))) snprintf(to, sizeof(to), "(?)");
        char path[2 * kPathU8Buf + 8];
        snprintf(path, sizeof(path), "%s -> %s", from, to);
        char key[2 * kPathU8Buf + 40];
        snprintf(key, sizeof(key), "MoveFileExW|%s", path);
        if (claim_line(key)) emit("MoveFileExW", path, "移动/改名", ok ? "成功" : "失败", err);
    }
    SetLastError(err);
    return ok;
}

// ------------------------------------------------------------ 挂钩点表
struct Target {
    const char* name;   // PE 导入名
    void* thunk;        // 我们的替换函数
    void** orig;        // 指向 g_orig 里对应的成员
    void* slots[kMaxSlotsPerApi];   // 被改写过的 IAT 槽地址（复检用）
    int nslots;
};

// 注意每个条目都显式写全 {slots={}, nslots=0}：少写会被 -Wmissing-field-initializers
// 报警（build.sh 带 -Wall -Wextra），而且显式写出来能让"槽表初始为空"这件事一眼可见。
Target g_targets[] = {
    {"CreateFileW", reinterpret_cast<void*>(&hook_CreateFileW),
     reinterpret_cast<void**>(&g_orig.CreateFileW), {}, 0},
    {"CreateFile2", reinterpret_cast<void*>(&hook_CreateFile2),
     reinterpret_cast<void**>(&g_orig.CreateFile2), {}, 0},
    {"ReadFile", reinterpret_cast<void*>(&hook_ReadFile),
     reinterpret_cast<void**>(&g_orig.ReadFile), {}, 0},
    {"GetFileAttributesW", reinterpret_cast<void*>(&hook_GetFileAttributesW),
     reinterpret_cast<void**>(&g_orig.GetFileAttributesW), {}, 0},
    {"GetFileAttributesExW", reinterpret_cast<void*>(&hook_GetFileAttributesExW),
     reinterpret_cast<void**>(&g_orig.GetFileAttributesExW), {}, 0},
    {"FindFirstFileW", reinterpret_cast<void*>(&hook_FindFirstFileW),
     reinterpret_cast<void**>(&g_orig.FindFirstFileW), {}, 0},
    {"FindFirstFileExW", reinterpret_cast<void*>(&hook_FindFirstFileExW),
     reinterpret_cast<void**>(&g_orig.FindFirstFileExW), {}, 0},
    {"CopyFileW", reinterpret_cast<void*>(&hook_CopyFileW),
     reinterpret_cast<void**>(&g_orig.CopyFileW), {}, 0},
    // 写侧 / 属性 / ANSI 变体（第二步）。导入表里没有的会被 install 静默跳过，
    // 所以这里多写几条不影响没用到它们的程序。
    {"GetFileAttributesA", reinterpret_cast<void*>(&hook_GetFileAttributesA),
     reinterpret_cast<void**>(&g_orig.GetFileAttributesA), {}, 0},
    {"CreateDirectoryW", reinterpret_cast<void*>(&hook_CreateDirectoryW),
     reinterpret_cast<void**>(&g_orig.CreateDirectoryW), {}, 0},
    {"RemoveDirectoryW", reinterpret_cast<void*>(&hook_RemoveDirectoryW),
     reinterpret_cast<void**>(&g_orig.RemoveDirectoryW), {}, 0},
    {"DeleteFileW", reinterpret_cast<void*>(&hook_DeleteFileW),
     reinterpret_cast<void**>(&g_orig.DeleteFileW), {}, 0},
    {"MoveFileExW", reinterpret_cast<void*>(&hook_MoveFileExW),
     reinterpret_cast<void**>(&g_orig.MoveFileExW), {}, 0},
    {"SetFileAttributesW", reinterpret_cast<void*>(&hook_SetFileAttributesW),
     reinterpret_cast<void**>(&g_orig.SetFileAttributesW), {}, 0},
};

constexpr size_t kTargetCount = sizeof(g_targets) / sizeof(g_targets[0]);

// 复检标志：只有在"改写时 IAT 还没被 loader 绑定"这种极端情况下才置位。
// 正常部署（游戏 LoadLibraryW("xgameruntime.dll") 加载我们，见文件末尾说明）
// 永远是 false，也就不会有多余动作。
bool g_recheck_needed = false;

// ------------------------------------------------------------ 崩溃安全的地址校验
// 只解析主 exe 自己的 PE，但即使数据异常也必须保证不触碰未提交页：
// 每次解引用前用 VirtualQuery 确认已提交、可读、没有 PAGE_GUARD。
bool mem_readable(const void* p, size_t n) {
    uintptr_t cur = reinterpret_cast<uintptr_t>(p);
    if (cur == 0 || n == 0) return false;
    const uintptr_t end = cur + n;
    if (end < cur) return false;   // 地址回绕
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cur), &mbi, sizeof(mbi)) != sizeof(mbi)) {
            return false;
        }
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD) != 0) return false;
        const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= cur) return false;
        cur = region_end;
    }
    return true;
}

// 在 s 起、最多 max 字节内找 '\0'。分窗做可读性校验（不逐字节 VirtualQuery，
// 也绝不越过未提交页去 memchr）。
bool find_nul(const char* s, size_t max, size_t* out) {
    size_t i = 0;
    while (i < max) {
        const size_t page_off = reinterpret_cast<uintptr_t>(s + i) & 0xFFF;
        size_t win = 0x1000 - page_off;
        if (win > max - i) win = max - i;
        if (mem_readable(s + i, win)) {
            const void* q = memchr(s + i, 0, win);
            if (q) {
                *out = i + static_cast<size_t>(static_cast<const char*>(q) - (s + i));
                return true;
            }
            i += win;
        } else {
            // 窗口尾巴跨到不可读页：降到逐字节，走到第一个不可读字节为止
            for (size_t k = 0; k < win; ++k) {
                if (!mem_readable(s + i + k, 1)) return false;
                if (s[i + k] == 0) {
                    *out = i + k;
                    return true;
                }
            }
            i += win;
        }
    }
    return false;
}

// 只认"核心文件 API 的宿主模块"：实测本作 exe 的 5 个目标函数全在
// api-ms-win-core-file-l1-1-0.dll（API set，运行时解析到 KERNELBASE），
// 另外也接受 KERNEL32 / KERNELBASE 直连与 api-ms-win-core-file-* 的其它变体。
bool is_core_file_module(const char* dll, size_t len) {
    static const char kApiSetPrefix[] = "api-ms-win-core-file";
    if (len == 0) return false;
    auto ieq = [](const char* a, size_t an, const char* b) {
        return _strnicmp(a, b, an) == 0 && b[an] == 0;
    };
    if (ieq(dll, len, "KERNEL32.dll")) return true;
    if (ieq(dll, len, "KERNELBASE.dll")) return true;
    const size_t plen = sizeof(kApiSetPrefix) - 1;
    return len > plen && _strnicmp(dll, kApiSetPrefix, plen) == 0;
}

// 在挂钩点表里按导入名精确查找
Target* find_target(const char* name, size_t len) {
    for (size_t i = 0; i < kTargetCount; ++i) {
        Target& t = g_targets[i];
        if (strlen(t.name) == len && memcmp(t.name, name, len) == 0) return &t;
    }
    return nullptr;
}

// 改写一个 IAT 槽：改页权限 -> 写指针 -> 恢复原页属性。
// 恢复失败不影响正确性（槽已写好），所以不当作失败处理。
bool patch_slot(void** slot, void* replacement) {
    DWORD old_prot = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) return false;
    *slot = replacement;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old_prot, &ignored);
    return true;
}

// ============================================================ 安装
// 解析主 exe 的导入表并改写目标槽。任何异常都只记一行日志然后返回，
// 绝不 longjmp / 抛异常 / 终止进程。
void install_iat_hooks() {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) {
        logf("[fstrace] 放弃：取不到主模块句柄，本次不改写任何内存");
        return;
    }
    BYTE* const base = reinterpret_cast<BYTE*>(exe);

    auto give_up = [](const char* why) {
        logf("[fstrace] 放弃 IAT 挂钩（%s）：本次不做任何改写，游戏照常运行", why);
    };

    // --- DOS 头 ---
    if (!mem_readable(base, sizeof(IMAGE_DOS_HEADER))) {
        give_up("DOS 头不可读");
        return;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        give_up("主模块不是 PE 映像");
        return;
    }
    const LONG lfanew = dos->e_lfanew;
    if (lfanew <= 0 || !mem_readable(base + lfanew, sizeof(IMAGE_NT_HEADERS64))) {
        give_up("NT 头不可读");
        return;
    }
    // 先用 32 位视图读 Magic（两个结构里 Magic 的偏移相同），再决定按 64 位解析
    const auto* nt32 = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + lfanew);
    if (nt32->Signature != IMAGE_NT_SIGNATURE) {
        give_up("NT 签名不对");
        return;
    }
    if (nt32->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        give_up("主模块不是 PE32+");
        return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + lfanew);
    const size_t image_size = nt->OptionalHeader.SizeOfImage;
    if (image_size < sizeof(IMAGE_NT_HEADERS64)) {
        give_up("SizeOfImage 非法");
        return;
    }
    auto in_image = [image_size](size_t off, size_t len) {
        return off <= image_size && len <= image_size - off;
    };

    // --- 导入目录 ---
    const IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dd.VirtualAddress == 0) {
        give_up("主模块没有导入目录");
        return;
    }
    if (!in_image(dd.VirtualAddress, sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
        give_up("导入目录 RVA 越界");
        return;
    }
    size_t dir_bytes = dd.Size;
    if (dir_bytes < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        dir_bytes = sizeof(IMAGE_IMPORT_DESCRIPTOR) * 64;   // 少数链接器把 Size 写成 0
    }
    if (!in_image(dd.VirtualAddress, dir_bytes)) dir_bytes = image_size - dd.VirtualAddress;

    int patched = 0;
    int skipped = 0;      // 导入表里没有 / 取不到原函数
    int patch_failed = 0; // VirtualProtect 被拒
    bool unbound_fallback = false;

    const size_t desc_bytes = sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (size_t di = 0; di < kMaxImportDescs; ++di) {
        const size_t off = di * desc_bytes;
        if (off + desc_bytes > dir_bytes) break;
        const size_t desc_rva = static_cast<size_t>(dd.VirtualAddress) + off;
        if (!in_image(desc_rva, desc_bytes)) break;
        const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + desc_rva);
        if (!mem_readable(desc, desc_bytes)) break;
        if (desc->Name == 0 && desc->FirstThunk == 0) break;   // 全 0：描述符数组结束
        if (desc->FirstThunk == 0 || desc->Name == 0) continue;
        if (!in_image(desc->Name, 2)) continue;

        const char* dll = reinterpret_cast<const char*>(base + desc->Name);
        size_t dll_len = 0;
        if (!find_nul(dll, kMaxNameLen, &dll_len)) continue;
        if (!is_core_file_module(dll, dll_len)) continue;

        // 按序号导入的模块没有名字表，无法按名字匹配 -> 跳过（系统 DLL 都是按名字导入的）
        if (desc->OriginalFirstThunk == 0) continue;
        if (!in_image(desc->OriginalFirstThunk, sizeof(IMAGE_THUNK_DATA64)) ||
            !in_image(desc->FirstThunk, sizeof(IMAGE_THUNK_DATA64))) {
            continue;
        }

        for (size_t i = 0; i < kMaxThunksPerDesc; ++i) {
            const size_t oft_rva =
                static_cast<size_t>(desc->OriginalFirstThunk) + i * sizeof(IMAGE_THUNK_DATA64);
            const size_t iat_rva =
                static_cast<size_t>(desc->FirstThunk) + i * sizeof(IMAGE_THUNK_DATA64);
            if (!in_image(oft_rva, sizeof(IMAGE_THUNK_DATA64)) ||
                !in_image(iat_rva, sizeof(IMAGE_THUNK_DATA64))) {
                break;
            }
            const auto* oft = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + oft_rva);
            if (!mem_readable(oft, sizeof(IMAGE_THUNK_DATA64))) break;
            const ULONGLONG addr = oft->u1.AddressOfData;
            if (addr == 0) break;                              // 名字表结束
            if ((addr & IMAGE_ORDINAL_FLAG64) != 0) continue;   // 按序号导入，无名字
            if (addr > 0xFFFFFFFFull) continue;
            const size_t ibn_rva = static_cast<size_t>(addr);
            if (!in_image(ibn_rva, 4)) continue;
            const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + ibn_rva);
            if (!mem_readable(ibn, 4)) continue;
            const char* nm = reinterpret_cast<const char*>(ibn->Name);
            size_t nm_len = 0;
            if (!find_nul(nm, 256, &nm_len)) continue;
            Target* t = find_target(nm, nm_len);
            if (!t) continue;

            void** const slot = reinterpret_cast<void**>(base + iat_rva);
            if (!mem_readable(slot, sizeof(void*))) continue;
            if (*slot == t->thunk) continue;   // 已经是我们了（重复条目/重复调用）

            void* orig = *slot;
            if (orig == nullptr) {
                // 该槽还没被 loader 绑定（只会在"DllMain 早于导入绑定"的极端时序下
                // 出现）：直接从 kernel32 取真实地址兜底，并置复检标志。
                HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
                orig = k32 ? reinterpret_cast<void*>(
                                 reinterpret_cast<uintptr_t>(GetProcAddress(k32, t->name)))
                           : nullptr;
                if (orig == nullptr) {
                    ++skipped;
                    continue;
                }
                unbound_fallback = true;
            }
            store_orig(t->orig, orig);   // 先把原函数指针存好，再改写槽（顺序不能反）
            if (!patch_slot(slot, t->thunk)) {
                ++patch_failed;
                continue;
            }
            if (t->nslots < kMaxSlotsPerApi) {
                t->slots[t->nslots++] = reinterpret_cast<void*>(slot);
            }
            ++patched;
        }
    }

    if (patched == 0) {
        give_up("主模块导入表里没有可接管的文件 API");
        if (skipped != 0) logf("[fstrace] 跳过 %d 个候选（取不到原函数）", skipped);
        return;
    }

    char names[256] = {0};
    for (size_t i = 0; i < kTargetCount; ++i) {
        if (g_targets[i].nslots == 0) continue;
        strncat(names, g_targets[i].name, sizeof(names) - strlen(names) - 1);
        strncat(names, " ", sizeof(names) - strlen(names) - 1);
    }
    logf("[fstrace] 已接管主 exe 的 %d 个 IAT 槽：%s（去重上限 %d 行）", patched, names,
         kMaxLogLines);
    if (patch_failed != 0) {
        logf("[fstrace] 有 %d 个槽改写失败（VirtualProtect 被拒），已跳过", patch_failed);
    }
    if (skipped != 0) {
        logf("[fstrace] %d 个候选取不到原函数，已跳过", skipped);
    }
    if (unbound_fallback) {
        g_recheck_needed = true;
        logf("[fstrace] 注意：改写时 IAT 尚未被 loader 绑定，已用 GetProcAddress 兜底，"
             "会在后续线程创建时复检");
    }
}

// 复检：只有在 unbound_fallback 这种极端时序下才可能被 loader 覆盖，正常不会
// 有任何动作（第一次进来就会把标志清掉）。
void recheck_slots() {
    if (!g_recheck_needed) return;
    g_recheck_needed = false;
    int redone = 0;
    for (size_t ti = 0; ti < kTargetCount; ++ti) {
        Target& t = g_targets[ti];
        for (int i = 0; i < t.nslots; ++i) {
            void** slot = reinterpret_cast<void**>(t.slots[i]);
            if (!mem_readable(slot, sizeof(void*))) continue;
            if (*slot == t.thunk) continue;   // 还在，说明没被覆盖
            if (patch_slot(slot, t.thunk)) ++redone;
        }
    }
    if (redone != 0) logf("[fstrace] 复检：重建了 %d 个被覆盖的槽", redone);
}

}  // namespace

void install_if_enabled() {
    // 追踪和路径重写都没开就什么都不做：不读 PE、不改内存、不写日志。
    if (!active()) return;
    static bool done = false;   // 幂等：DllMain 理论上只来一次，复检路径也走这里
    if (done) return;
    done = true;
    install_iat_hooks();
}

void recheck_if_needed() {
    if (!active()) return;
    recheck_slots();
}

}  // namespace fstrace
}  // namespace xbgtasix

// ============================================================================
// DllMain
// ----------------------------------------------------------------------------
// 为什么由本文件定义：已确认 src/xgameruntime/*.cpp 里**没有**别的 DllMain
// （grep DllMain / DLL_PROCESS_ATTACH 只命中注释），所以这里定义全仓库唯一的
// 一个，不需要 #pragma section / .CRT$XLB 那类 TLS 回调花招。
// 若将来别处新增了 DllMain，请删掉这一个，并在那个 DllMain 的
// DLL_PROCESS_ATTACH 分支里调用 ::xbgtasix::fstrace::install_if_enabled()，
// 在 DLL_THREAD_ATTACH 分支里调用 recheck_if_needed()。
//
// 时序：本 DLL 是被游戏在运行期用 LoadLibrary 拉起来的（exe 的导入表里没有
// xgameruntime.dll，但二进制里有 UTF-16 的 "xgameruntime.dll" 字符串），所以在
// 我们执行到这里时主 exe 的导入表早已被 loader 绑定完毕，改写 IAT 是安全的。
// 即使换成"被主 exe 静态导入"的部署方式也没问题：用 mingw 构造同形态 exe+DLL
// 实测过，依赖 DLL 的 DllMain 执行时主 exe 的 kernel32 槽已绑定，且 DllMain 里
// 写进去的补丁在 loader 收尾后依然有效。
// 万一遇到相反时序（槽还是 0），install 会走 GetProcAddress 兜底并置复检标志，
// 由 DLL_THREAD_ATTACH 补一次。
//
// DLL_PROCESS_DETACH 故意什么都不做：还原 IAT 只会在进程退出期制造竞态，而
// xgameruntime.dll 一旦加载就活到进程结束，没有"卸载后要还原"的场景。
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD fdwReason, LPVOID) {
    // 主机盘符映射必须在任何文件访问之前完成（游戏启动期就会探测 G:/T:）
    if (fdwReason == DLL_PROCESS_ATTACH) {
        xbgtasix::install_console_drives();
        // 主机侧停顿的 Sleep 钩子：标题的开机 hang check 会因为我们的驱动编译
        // 误判"主线程卡死"并在 30 s 后自杀，这里必须无条件装上（不看开关）。
        xbgtasix::host_stall::install();
    }

    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            ::xbgtasix::fstrace::install_if_enabled();
            break;
        case DLL_THREAD_ATTACH:
            ::xbgtasix::fstrace::recheck_if_needed();
            break;
        default:
            break;
    }
    return TRUE;
}
