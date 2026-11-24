// SPDX-License-Identifier: GPL-3.0-or-later
// drives.cpp —— 给进程映射 Xbox 主机的盘符
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
// 解法：用 DefineDosDeviceW 做**进程内**盘符映射（不需要管理员、不落注册表、
// 进程退出即失效）。目录不存在就先建出来。
//
// 开关：XBGTASIX_DRIVES=0 可关闭；目录可用环境变量覆盖：
//   XBGTASIX_DRIVE_G / XBGTASIX_DRIVE_T / XBGTASIX_DRIVE_D
#include <windows.h>

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
    xbgtasix::logf("[drives] 映射 %c: -> %s 调用成功但 G: 仍打不开，换写法重试", letter,
                   to_utf8(target).c_str());

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

}  // namespace

namespace xbgtasix {

// 由 DllMain（fstrace.cpp）在进程早期调用：必须早于游戏的任何文件访问。
void install_console_drives() {
    static bool done = false;
    if (done) return;
    done = true;

    char flag[8] = {0};
    if (GetEnvironmentVariableA("XBGTASIX_DRIVES", flag, sizeof(flag)) > 0 && flag[0] == '0') {
        return;
    }

    const std::wstring game_dir = self_dir();
    const std::wstring local = env_or(L"LOCALAPPDATA", game_dir);

    // G: 持久本地存储（主机上是 2 GB 的可写区，游戏在里面放 appdata0.rpf 等）
    // G: 的实际含义（实测推导）：**游戏内容分区**。
    // 游戏依次探测 G:\appdata0.rpf、G:\common.rpf —— 这些都是包根目录里的文件，
    // 所以默认把 G: 指向游戏目录本身；若用户用 XBGTASIX_DRIVE_G 指定了别的目录，
    // 则把包里的种子文件拷过去（主机上这一步由平台完成）。
    const std::wstring g_dir = env_or(L"XBGTASIX_DRIVE_G", game_dir);
    map_drive(L'G', g_dir);

    // 种子文件：包目录里**自带** appdata0.rpf（实测 8880 字节，是合法的主机 RPF 容器）。
    // 主机上这份由平台在首次运行时放进持久存储；PC 上没有平台，所以这里替它播种。
    // 缺了它游戏会认为挂载定义不存在，崩在 game+0x2237178 的断言上（实测）。
    {
        const std::wstring seed = join_path(game_dir, L"appdata0.rpf");
        const std::wstring target = join_path(g_dir, L"appdata0.rpf");
        if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(seed.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (CopyFileW(seed.c_str(), target.c_str(), FALSE)) {
                xbgtasix::logf("[drives] 已从包目录播种 appdata0.rpf -> G:");
            } else {
                xbgtasix::logf("[drives] 播种 appdata0.rpf 失败 err=%lu", GetLastError());
            }
        }
    }

    // T: 临时存储（崩溃转储、解压缓存）
    map_drive(L'T', env_or(L"XBGTASIX_DRIVE_T", local + L"\\Xbgtasix\\T"));

    // D: 游戏内容根 —— **只有当 D: 尚未被真实磁盘占用、或用户显式指定时才映射**，
    // 否则会把 PC 的 D 盘整个遮掉（游戏包本身可能就在 D:\ 下）。
    if (GetEnvironmentVariableW(L"XBGTASIX_DRIVE_D", nullptr, 0) > 0) {
        map_drive(L'D', env_or(L"XBGTASIX_DRIVE_D", game_dir));
    }
}

}  // namespace xbgtasix
