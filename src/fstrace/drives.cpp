// SPDX-License-Identifier: GPL-3.0-or-later
// drives.cpp —— 主机盘符（G:/T:/D:）的两种落地方式：**路径重写（默认）** 与 DefineDosDevice（回退）
//
// 由来（实测）：给游戏加上文件 API 追踪后，第一屏就能看到它在探测主机盘符：
//     GetFileAttributesW "G:\appdata0.rpf"  -> 失败 err=3
//     CreateFileW        "T:\<guid>.dmp"    -> 失败 err=3
//
// 主机上这些是平台挂载的驱动器：
//   D:  游戏内容（安装包）
//   G:  持久本地存储（MicrosoftGame.Config 里 <PersistentLocalStorage><SizeMB>2048</SizeMB>）
//   T:  临时存储（崩溃转储、缓存）
// PC 上不存在，于是 RAGE 引擎的 pack 虚拟文件系统拿不到挂载定义，
// 表项全 0，最终落在 game+0x2237178 的裸断言上。
//
// 两种落地方式
// ------------
// 1) **路径重写（默认，XBGTASIX_PATH_REWRITE）**
//    盘符表只用来"查表换字符串"：文件 API 的 thunk 在转发之前把 "G:\x" / "G:/x" 换成
//    真实目录里的 "Z:\mnt\...\x"。不建盘符、不碰注册表、不碰 $WINEPREFIX/dosdevices，
//    Windows 与 Wine 走**同一条**代码路径。实测依据见 docs/wine-host.md 第 9 节：
//    DefineDosDeviceW 三种写法在 Wine 下都"返回成功但不生效"，盘符只能在起进程前由宿主建，
//    那是宿主耦合，不是游戏要求的东西。
// 2) **盘符映射（回退，XBGTASIX_NO_PATH_REWRITE=1）**
//    DefineDosDeviceW 做进程内映射。Windows 上有效；Wine 上必须配合宿主
//    dosdevices/<letter>: 符号链接，本文件只负责"映射 + 自检 + 大声报 404"。
//
// 环境变量
//   XBGTASIX_PATH_REWRITE=1        显式打开重写（默认已开）
//   XBGTASIX_NO_PATH_REWRITE=1     回退到盘符模式（两种模式都要能跑，方便对照）
//   XBGTASIX_DRIVES=0              盘符模式下完全不映射
//   XBGTASIX_DRIVE_G / _T / _D     覆盖落地目录（两种模式共用这张表）
#include <windows.h>

#include <atomic>
#include <mutex>
#include <set>
#include <string>

#include "xbgtasix/trace.h"

namespace {

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

std::wstring env_or(const wchar_t* name, const std::wstring& fallback) {
    wchar_t buf[MAX_PATH * 2] = {0};
    const DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    if (n > 0 && n < MAX_PATH * 2) return std::wstring(buf, n);
    return fallback;
}

// 递归建目录（游戏会在 G:/T: 下写文件，目录必须存在）
bool ensure_dir(const std::wstring& dir) {
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        const bool sep = (dir[i] == L'\\' || dir[i] == L'/');
        if (!sep && i + 1 != dir.size()) continue;
        if (cur.size() <= 3 && cur.size() >= 2 && cur[1] == L':') continue;   // 跳过 "D:\"
        CreateDirectoryW(cur.c_str(), nullptr);
    }
    return GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 映射是否真的生效：拿 "G:\" 去问文件系统。
// 为什么必须验：Windows 上 DefineDosDeviceW 成功就等于生效，但**Wine 上不一定** ——
// 实测（docs/wine-host.md 8.8）：映射调用返回 TRUE，`GetFileAttributesA("G:/appdata0.rpf")`
// 仍然是 ERROR_FILE_NOT_FOUND(2)，而同一个文件用 D:\GTA6\... 全路径能开。映射"假成功"。
bool drive_works(wchar_t letter) {
    const wchar_t root[4] = {letter, L':', L'\\', 0};
    return GetFileAttributesW(root) != INVALID_FILE_ATTRIBUTES;
}

// Wine 里把 DOS 路径翻成 unix 路径（kernel32 的内部导出，真 Windows 上没有）
std::string wine_unix_path(const std::wstring& dos) {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return {};
    using Fn = const char*(*)(LPCWSTR);
    auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
        GetProcAddress(k32, "wine_get_unix_file_name")));
    if (!fn) return {};
    const char* u = fn(dos.c_str());
    return u ? std::string(u) : std::string();
}

// 把盘符映射到目录：raw target 形式（\??\C:\path），进程内生效
bool map_drive(wchar_t letter, const std::wstring& target) {
    if (!ensure_dir(target)) {
        xbgtasix::logf("[drives] 目录建不出来，跳过 %c: -> %s", letter, to_utf8(target).c_str());
        return false;
    }
    const wchar_t drive[3] = {letter, L':', 0};
    wchar_t raw[MAX_PATH * 2] = {0};
    _snwprintf_s(raw, MAX_PATH * 2, _TRUNCATE, L"\\??\\%s", target.c_str());
    // 先清掉可能存在的旧映射，避免叠加
    DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_RAW_TARGET_PATH, drive, nullptr);
    const BOOL ok = DefineDosDeviceW(DDD_RAW_TARGET_PATH, drive, raw);
    if (!ok) {
        xbgtasix::logf("[drives] 映射 %c: -> %s 失败 err=%lu", letter, to_utf8(target).c_str(),
                       GetLastError());
        return false;
    }
    if (drive_works(letter)) {
        xbgtasix::logf("[drives] 已映射 %c: -> %s（已验证可访问）", letter,
                       to_utf8(target).c_str());
        return true;
    }

    // 到这里说明映射调用成功但**没生效** —— Wine 的常见表现。逐条换写法再验：
    //   1) raw + unix 路径（\??\unix\mnt\...）
    //   2) 非 raw（纯 DOS 目标）
    xbgtasix::logf("[drives] 映射 %c: -> %s 调用成功但 %c: 仍打不开，换写法重试", letter,
                   to_utf8(target).c_str(), letter);

    const std::string unix_path = wine_unix_path(target);
    if (!unix_path.empty()) {
        std::wstring uni(unix_path.begin(), unix_path.end());
        for (auto& ch : uni) {
            if (ch == L'/') ch = L'\\';
        }
        wchar_t raw2[MAX_PATH * 2] = {0};
        _snwprintf_s(raw2, MAX_PATH * 2, _TRUNCATE, L"\\??\\unix%s", uni.c_str());
        DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_RAW_TARGET_PATH, drive, nullptr);
        if (DefineDosDeviceW(DDD_RAW_TARGET_PATH, drive, raw2) && drive_works(letter)) {
            xbgtasix::logf("[drives] 已映射 %c: -> %s（unix 路径形式，已验证可访问）", letter,
                           unix_path.c_str());
            return true;
        }
        xbgtasix::logf("[drives] unix 路径形式也不行（unix=%s）", unix_path.c_str());
    } else {
        xbgtasix::logf("[drives] 拿不到 unix 路径（wine_get_unix_file_name 不可用）");
    }

    DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_RAW_TARGET_PATH, drive, nullptr);
    if (DefineDosDeviceW(0, drive, target.c_str()) && drive_works(letter)) {
        xbgtasix::logf("[drives] 已映射 %c: -> %s（非 raw 形式，已验证可访问）", letter,
                       to_utf8(target).c_str());
        return true;
    }

    xbgtasix::logf("[drives] !! %c: 三种写法都没能生效 —— 依赖这个盘符的路径会全部 404", letter);
    return false;
}

std::wstring join_path(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring self_dir() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    const size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : s.substr(0, p);
}

// ------------------------------------------------------------ 盘符表（重写用）
// 表就是 drives.cpp 里原来那张：盘符 -> 目录。区别只是"建盘符"变成"查表换字符串"。
constexpr int kMaxDrives = 4;
constexpr int kMaxRewriteLines = 200;   // 去重日志上限（游戏会成千上万次调文件 API）

wchar_t upper_letter(wchar_t c) {
    return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c;
}

// 目标目录规范化：统一反斜杠、去掉末尾分隔符（"Z:\" 这种根保留）。
// 不做大小写转换：Windows/Wine 的底层对已存在目录的大小写敏感度不一致，
// 把表里的路径改成大写反而会指向不存在的目录。大小写不敏感只做在**盘符字母**上。
void normalize_target(std::wstring& s) {
    for (auto& ch : s) {
        if (ch == L'/') ch = L'\\';
    }
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
}

struct DriveEntry {
    wchar_t letter = 0;
    std::wstring target;
};

struct DriveTable {
    DriveEntry entries[kMaxDrives];
    int n = 0;
};

void add_drive(DriveTable& t, wchar_t letter, std::wstring target) {
    normalize_target(target);
    if (target.empty() || t.n >= kMaxDrives) return;
    t.entries[t.n].letter = upper_letter(letter);
    t.entries[t.n].target = std::move(target);
    ++t.n;
}

void build_table(DriveTable& t) {
    const std::wstring game_dir = self_dir();
    const std::wstring local = env_or(L"LOCALAPPDATA", game_dir);

    // G: 持久本地存储 / 游戏内容分区（游戏依次探测 G:\appdata0.rpf、G:\common.rpf）。
    add_drive(t, L'G', env_or(L"XBGTASIX_DRIVE_G", game_dir));
    // T: 临时存储（崩溃转储、解压缓存）
    add_drive(t, L'T', env_or(L"XBGTASIX_DRIVE_T", join_path(local, L"Xbgtasix\\T")));
    // D: 游戏内容根 —— 只有用户显式指定时才进表，否则会把真实 D 盘整个遮掉
    if (GetEnvironmentVariableW(L"XBGTASIX_DRIVE_D", nullptr, 0) > 0) {
        add_drive(t, L'D', env_or(L"XBGTASIX_DRIVE_D", game_dir));
    }
}

// 惰性建表（magic static）：第一次查表时算一次，之后只读。
const DriveTable& table() {
    static const DriveTable* t = [] {
        auto* p = new DriveTable();   // 故意不释放：退出期不再跑析构，避免 loader 里出事
        build_table(*p);
        return p;
    }();
    return *t;
}

std::atomic<long long> g_rewrite_count{0};

struct RewriteLog {
    std::mutex mu;
    std::set<std::string> seen;
    int lines = 0;
};

RewriteLog& rewrite_log() {
    static RewriteLog* s = new RewriteLog();
    return *s;
}

// 记一行"谁被换成了什么"。去重 + 上限：命中可能上万次，日志只要能看到**哪些**路径被换。
// 先判重，**在锁外**调 logf（logf 有自己的锁，不构成锁序）。
void note_rewrite(const wchar_t* from, const std::wstring& to) {
    g_rewrite_count.fetch_add(1, std::memory_order_relaxed);

    std::string key = to_utf8(from);
    key += "  ->  ";
    key += to_utf8(to);

    bool claimed = false;
    int idx = 0;
    {
        RewriteLog& s = rewrite_log();
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.lines < kMaxRewriteLines && s.seen.insert(key).second) {
            idx = ++s.lines;
            claimed = true;
        }
    }
    if (claimed) xbgtasix::logf("[rewrite] #%d %s", idx, key.c_str());
}

}  // namespace

namespace xbgtasix {

bool path_rewrite_enabled() {
    static const bool on = [] {
        // 反向开关优先：NO_ 存在且不是 "0" 就回退到盘符模式
        char no[8] = {0};
        if (GetEnvironmentVariableA("XBGTASIX_NO_PATH_REWRITE", no, sizeof(no)) > 0 && no[0] != '0') {
            return false;
        }
        char yes[8] = {0};
        if (GetEnvironmentVariableA("XBGTASIX_PATH_REWRITE", yes, sizeof(yes)) > 0) {
            return yes[0] == '1';
        }
        return true;   // 默认：重写（目标状态，不依赖宿主盘符）
    }();
    return on;
}

bool rewrite_console_path(const wchar_t* in, std::wstring& out) {
    if (in == nullptr || !path_rewrite_enabled()) return false;

    // 只认 "X:" 开头。UNC（\\server\share）、设备路径（\\?\ / \\.\）、相对路径，
    // 以及开头不是字母的一律原样放行 —— 它们跟主机盘符无关。
    const wchar_t c0 = in[0];
    if (!((c0 >= L'A' && c0 <= L'Z') || (c0 >= L'a' && c0 <= L'z'))) return false;
    if (in[1] != L':') return false;

    const wchar_t letter = upper_letter(c0);
    const DriveTable& t = table();
    for (int i = 0; i < t.n; ++i) {
        if (t.entries[i].letter != letter) continue;
        // 跳过盘符后面的根分隔符：G:\x、G:/x、G:\ 三种写法等价
        const wchar_t* tail = in + 2;
        while (*tail == L'\\' || *tail == L'/') ++tail;
        out.assign(t.entries[i].target);
        if (*tail != 0) {
            out.push_back(L'\\');
            out.append(tail);   // 尾部原样保留（含其中的 '/'，两个平台都接受）
        }
        note_rewrite(in, out);
        return true;
    }
    return false;
}

long long path_rewrite_count() { return g_rewrite_count.load(std::memory_order_relaxed); }

// 由 DllMain（fstrace.cpp）在进程早期调用：必须早于游戏的任何文件访问。
void install_console_drives() {
    static bool done = false;
    if (done) return;
    done = true;

    char flag[8] = {0};
    const bool legacy_off =
        GetEnvironmentVariableA("XBGTASIX_DRIVES", flag, sizeof(flag)) > 0 && flag[0] == '0';

    const std::wstring game_dir = self_dir();

    // 两种模式都要先有落地目录；重写模式靠它把 G:/T: 落进去，盘符模式靠它挂载。
    const DriveTable& t = table();   // 注意：table() 只读，目录在这里建
    bool ok_all = true;
    for (int i = 0; i < t.n; ++i) {
        if (!ensure_dir(t.entries[i].target)) {
            xbgtasix::logf("[drives] 目录建不出来：%c: -> %s", t.entries[i].letter,
                           to_utf8(t.entries[i].target).c_str());
            ok_all = false;
        }
    }

    // 种子文件：包目录里**自带** appdata0.rpf（实测 8880 字节，是合法的主机 RPF 容器）。
    // 主机上这份由平台在首次运行时放进持久存储；PC 上没有平台，所以这里替它播种。
    // 缺了它游戏会认为挂载定义不存在，崩在 game+0x2237178 的断言上（实测）。
    {
        const std::wstring seed = join_path(game_dir, L"appdata0.rpf");
        std::wstring g_dir = game_dir;
        for (int i = 0; i < t.n; ++i) {
            if (t.entries[i].letter == L'G') g_dir = t.entries[i].target;
        }
        const std::wstring target = join_path(g_dir, L"appdata0.rpf");
        if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(seed.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (CopyFileW(seed.c_str(), target.c_str(), FALSE)) {
                xbgtasix::logf("[drives] 已从包目录播种 appdata0.rpf -> %s",
                               to_utf8(target).c_str());
            } else {
                xbgtasix::logf("[drives] 播种 appdata0.rpf 失败 err=%lu", GetLastError());
            }
        }
    }

    if (path_rewrite_enabled()) {
        // 重写模式：不建盘符、不碰注册表、不碰宿主 dosdevices。
        xbgtasix::logf("[rewrite] 路径重写模式：不建任何盘符，文件 API 层查表换路径");
        for (int i = 0; i < t.n; ++i) {
            xbgtasix::logf("[rewrite] 表 %c: -> %s", t.entries[i].letter,
                           to_utf8(t.entries[i].target).c_str());
        }
        if (!ok_all) {
            xbgtasix::logf("[rewrite] !! 有落地目录建不出来，落在这些盘符上的路径会 404");
        }

        // 自检：把"要处理的写法"和"必须放行的写法"都喂一遍，结果打进日志。
        // 这一段就是回归证据 —— 大小写不敏感、正/反斜杠都认、UNC/相对/表外盘符原样放行，
        // 不用指望游戏那一侧的调用恰好覆盖到每种形式。
        const wchar_t* kProbes[] = {
            L"G:\\appdata0.rpf",              // 反斜杠
            L"G:/appdata0.rpf",               // 正斜杠
            L"g:\\Common.RPF",                // 小写盘符
            L"G:",                            // 只有盘符
            L"\\\\server\\share\\x.rpf",      // UNC -> 必须原样
            L"appdata0.rpf",                  // 相对路径 -> 必须原样
            L"C:\\windows\\notepad.exe",      // 表外盘符 -> 必须原样
        };
        for (const wchar_t* probe : kProbes) {
            std::wstring rw;
            if (rewrite_console_path(probe, rw)) {
                xbgtasix::logf("[rewrite] 自检 \"%s\" 重写 -> \"%s\"", to_utf8(probe).c_str(),
                               to_utf8(rw).c_str());
            } else {
                xbgtasix::logf("[rewrite] 自检 \"%s\" 原样放行", to_utf8(probe).c_str());
            }
        }
        return;
    }

    if (legacy_off) return;   // XBGTASIX_DRIVES=0：什么都不映射（历史行为，保留）

    for (int i = 0; i < t.n; ++i) map_drive(t.entries[i].letter, t.entries[i].target);
}

}  // namespace xbgtasix
