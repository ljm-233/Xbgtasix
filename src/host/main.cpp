// SPDX-License-Identifier: GPL-3.0-or-later
// xbgtasix.exe —— 转译层的"程序"入口
//
// 为什么需要它：三个 shim 是库，不是程序；tools/*.sh 要 bash、*.py 要 Python。
// 这个 exe 把"摆 DLL → 设环境 → 拉起 → 分类失败 → 给下一步"做成一个零依赖的命令行工具。
//
//   xbgtasix run    <exe|包目录>  拉起游戏并按退出码分类
//   xbgtasix check  <exe>         导入面 vs 我们的导出面（缺哪些名字）
//   xbgtasix verify <包目录>      按包自带 layout 清单核对完整性
//   xbgtasix doctor               环境自检（显卡/驱动/运行库/权限）
//   xbgtasix restore <包|exe>    撤销 run 的写入（还原备份 / 删掉我们放的 DLL）
//   xbgtasix firstrun <包|exe>   verify -> doctor -> check -> run 一次走完
//
// 设计取向：只做这几件事。不加 UI、不加注入、不加配置系统。
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <mmsystem.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pe.h"

namespace {

constexpr const char* kVersion = "0.1.0";
const char* g_self_dir_utf8 = "";

// ---------------------------------------------------------------- 小工具
void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void section(const char* title) {
    printf("\n== %s\n", title);
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    // 容量与长度分开：WideCharToMultiByte 会写 n 字节（含结尾 0），
    // 原来 resized 成 n-1 却让它写 n，等于写到 data()[size()] 上（标准上是越界）
    std::string out(static_cast<size_t>(n > 0 ? n : 1), 0);
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    out.resize(n > 0 ? static_cast<size_t>(n - 1) : 0);
    return out;
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n : 1), 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    out.resize(n > 0 ? static_cast<size_t>(n - 1) : 0);
    return out;
}

bool file_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring dir_of(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

std::wstring base_of(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring abs_path(const std::wstring& p) {
    wchar_t buf[MAX_PATH] = {0};
    if (GetFullPathNameW(p.c_str(), MAX_PATH, buf, nullptr) == 0) return p;
    return std::wstring(buf);
}

std::wstring self_dir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring d = dir_of(abs_path(buf));
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    return d;
}

// 两个路径是否指向同一个文件（自己拷自己会得 ERROR_SHARING_VIOLATION 32）
bool same_file(const std::wstring& a, const std::wstring& b) {
    wchar_t fa[MAX_PATH] = {0};
    wchar_t fb[MAX_PATH] = {0};
    if (!GetFullPathNameW(a.c_str(), MAX_PATH, fa, nullptr)) return false;
    if (!GetFullPathNameW(b.c_str(), MAX_PATH, fb, nullptr)) return false;
    return _wcsicmp(fa, fb) == 0;
}

std::string read_text(const std::wstring& path, size_t limit = 8u << 20) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    size_t n = static_cast<size_t>(sz.QuadPart);
    if (n > limit) n = limit;
    std::string out(n, 0);
    DWORD got = 0;
    if (n) ReadFile(h, out.data(), static_cast<DWORD>(n), &got, nullptr);
    CloseHandle(h);
    out.resize(got);
    return out;
}

// 读文件**尾部**：要看的是"日志末尾"，从开头读到 8MB 只能看到刚开始的那点东西。
std::string read_text_tail(const std::wstring& path, size_t limit = 2u << 20) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        return {};
    }
    const long long total = sz.QuadPart;
    const long long from =
        total > static_cast<long long>(limit) ? total - static_cast<long long>(limit) : 0;
    LARGE_INTEGER pos{};
    pos.QuadPart = from;
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) {
        CloseHandle(h);
        return {};
    }
    std::string out(static_cast<size_t>(total - from), 0);
    DWORD got = 0;
    if (!out.empty()) ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    CloseHandle(h);
    out.resize(got);
    // 起点大概率落在某一行中间：丢掉第一行残片，否则"日志末尾"会从半行开始
    if (from > 0) {
        const size_t nl = out.find('\n');
        out.erase(0, nl == std::string::npos ? out.size() : nl + 1);
    }
    return out;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

std::string exe_name_of_dir(const std::wstring& dir);   // 前向声明

// 三个 shim 的名字
// 我们要放进游戏目录的全部模块：3 个自己实现的 + 5 个补齐主机硬件抽象层的
// （后者游戏会导入，但游戏包里没有——真实主机上它们来自系统分区）
const wchar_t* kShims[] = {L"xgameruntime.dll",  L"xmem.dll",   L"d3d12_xs.dll",
                           L"AcpHal.dll",        L"xapu.dll",   L"chad.dll",
                           L"xg_xs.dll",         L"XCRDAPI.dll"};
// 上面的个数：写死 3 会在扩充 shim 后让 run/doctor 直接判"不齐"（实测 CI 就栽在这）
constexpr int kShimCount = static_cast<int>(sizeof(kShims) / sizeof(kShims[0]));

// 找 shim 所在目录：优先 --dll-dir，其次自身目录
std::wstring resolve_dll_dir(const std::wstring& explicit_dir) {
    if (!explicit_dir.empty()) return explicit_dir;
    return self_dir();
}

// ---------------------------------------------------------------- verify
struct VerifyResult {
    bool too_big = false;    // layout 文件过大，无法完整核对
    int listed = 0;
    int present = 0;
    int unexpanded = 0;                     // 含通配符、静态展开不了的条目数
    std::vector<std::string> missing;
    std::vector<std::string> empty_files;   // 存在但是 0 字节
    std::vector<std::string> invalid;       // 归一出包目录（../、绝对路径、盘符）的坏条目
    std::vector<std::string> unexpanded_list;
    std::string layout_file;
};

// 从一段 XML 文本里取某个属性值
// 解析 XML 属性：兼容 `name="v"`、`name = "v"`、`name='v'`，并还原常见实体。
// 原来的实现只认 `name="`，遇到 `name = "` 或单引号就把整条当不存在
// （layout 一旦换写法，verify 会静默少算条目 -> 假 PASS）。
std::string xml_unescape(std::string v) {
    struct { const char* ent; char ch; } kEnts[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    for (const auto& e : kEnts) {
        size_t pos = 0;
        while ((pos = v.find(e.ent, pos)) != std::string::npos) {
            v.replace(pos, strlen(e.ent), 1, e.ch);
            ++pos;
        }
    }
    return v;
}

std::string attr_of(const std::string& tag, const char* name) {
    const std::string key(name);
    size_t p = 0;
    while ((p = tag.find(key, p)) != std::string::npos) {
        // 必须是"独立属性名"：前面不能紧贴字母数字或下划线
        if (p > 0) {
            const char prev = tag[p - 1];
            if (isalnum(static_cast<unsigned char>(prev)) || prev == '_') {
                p += key.size();
                continue;
            }
        }
        size_t q = p + key.size();
        while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t')) ++q;
        if (q >= tag.size() || tag[q] != '=') { p += key.size(); continue; }
        ++q;
        while (q < tag.size() && (tag[q] == ' ' || tag[q] == '\t')) ++q;
        if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) { p += key.size(); continue; }
        const char quote = tag[q++];
        const size_t b = q;
        const size_t e = tag.find(quote, b);
        if (e == std::string::npos) return {};
        return xml_unescape(tag.substr(b, e - b));
    }
    return {};
}

// 一项目录信息对应的"包内相对路径"：DestinationPath + Include。
// verify 和"从包里找 exe"必须用同一条规则，否则会出现一个说存在、一个说缺失。
//
// 关键：文件落在 DestinationPath 指定的子目录里（如 \data\ttdb\graphics\），
// 不是包根目录。之前忽略这一项，把 68 个本来存在的文件误报成缺失。
std::string dest_rel_of(const std::string& tag, const std::string& include) {
    std::string dest = attr_of(tag, "DestinationPath");
    for (auto& c : dest) {
        if (c == '/') c = '\\';
    }
    while (!dest.empty() && (dest.front() == '\\' || dest.front() == '.')) {
        if (dest.front() == '.') break;
        dest.erase(dest.begin());
    }
    if (dest == ".") dest.clear();
    if (!dest.empty() && dest.back() != '\\') dest += '\\';
    return dest + include;
}

// 相对路径归一化后必须仍落在包目录之下。出现 .. / 绝对路径 / 盘符一律算非法条目：
// 不能让一份坏清单把核对的视线引到包外面去。
bool rel_inside(const std::wstring& dir, const std::string& rel, std::wstring* out_full) {
    if (rel.empty()) return false;
    if (rel.find(':') != std::string::npos) return false;   // C:\... 这类带盘符的绝对路径
    if (rel.front() == '\\' || rel.front() == '/') return false;
    const std::wstring wrel = to_wide(rel);
    for (size_t i = 0; i <= wrel.size();) {
        size_t e = wrel.find_first_of(L"\\/", i);
        if (e == std::wstring::npos) e = wrel.size();
        if (e - i == 2 && wrel.compare(i, 2, L"..") == 0) return false;
        i = e + 1;
    }
    std::wstring root = abs_path(dir);
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    const std::wstring full = abs_path(join(root, wrel));
    if (root.empty() || full.size() <= root.size()) return false;
    if (_wcsnicmp(full.c_str(), root.c_str(), root.size()) != 0) return false;
    if (full[root.size()] != L'\\' && full[root.size()] != L'/') return false;
    if (out_full) *out_full = full;
    return true;
}

VerifyResult do_verify(const std::wstring& dir) {
    VerifyResult r;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(join(dir, L"layout_*.xml").c_str(), &fd);
    std::wstring layout;
    if (h != INVALID_HANDLE_VALUE) {
        layout = join(dir, fd.cFileName);
        FindClose(h);
    }
    if (layout.empty()) {
        HANDLE h2 = FindFirstFileW(join(dir, L"*.xml").c_str(), &fd);
        if (h2 != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring cand = join(dir, fd.cFileName);
                const std::string text = read_text(cand, 4u << 20);
                if (text.find("<FileGroup") != std::string::npos) {
                    layout = cand;
                    break;
                }
            } while (FindNextFileW(h2, &fd));
            FindClose(h2);
        }
    }
    if (layout.empty()) return r;
    r.layout_file = to_utf8(base_of(layout));

    const std::string text = read_text(layout);
    std::vector<std::string> seen;   // 已计过的 rel（小写），用于去重
    size_t pos = 0;
    while ((pos = text.find("<FileGroup", pos)) != std::string::npos) {
        // 不能只找 "/>"：非自闭合写法的属性会被整段丢掉（少算条目 -> 假 PASS）
        const size_t close = text.find('>', pos);
        if (close == std::string::npos) break;
        const std::string tag = text.substr(pos, close - pos + 1);
        pos = close + 1;

        const std::string include = attr_of(tag, "Include");
        if (include.empty()) continue;
        // 通配符条目静态展开不了：单独统计，既不能当"存在"糊过去，也不能静默跳过
        if (include.find('*') != std::string::npos || include.find('?') != std::string::npos) {
            ++r.unexpanded;
            r.unexpanded_list.push_back(include);
            continue;
        }

        // 关键：文件落在 DestinationPath 指定的子目录里（如 \data\ttdb\graphics\），
        // 不是包根目录。拼接规则取自 dest_rel_of，与"从包里找 exe"共用同一套。
        const std::string rel = dest_rel_of(tag, include);

        // 同一个 rel 在清单里重复出现只算一次
        std::string low = rel;
        for (auto& c : low) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (std::find(seen.begin(), seen.end(), low) != seen.end()) continue;
        seen.push_back(low);

        ++r.listed;
        // 归一化后必须仍在包目录里：../ 与绝对路径是坏条目，点名计入失败
        std::wstring full;
        if (!rel_inside(dir, rel, &full)) {
            r.invalid.push_back(rel);
        } else if (!file_exists(full)) {
            r.missing.push_back(rel);
        } else {
            // 0 字节文件算"存在"会掩盖截断/下载失败（几十 GB 的包尤其危险）
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fad) &&
                fad.nFileSizeHigh == 0 && fad.nFileSizeLow == 0) {
                r.empty_files.push_back(rel);
            } else {
                ++r.present;
            }
        }
    }
    return r;
}

int cmd_verify(const std::wstring& dir) {
    if (!dir_exists(dir)) {
        say("错误：目录不存在：%s", to_utf8(dir).c_str());
        return 2;
    }
    section("包完整性");
    const VerifyResult r = do_verify(dir);
    if (r.listed == 0) {
        say("没找到 layout XML（含 <FileGroup> 的那种），无法核对");
        return 2;
    }
    say("清单文件：%s", r.layout_file.c_str());
    if (r.too_big) {
        say("FAIL：清单文件超过 64MB，超过本工具的单次核对上限（不给出半份结果）");
        return 1;
    }
    say("清单 %d 项，存在 %d，缺失 %d，0 字节 %d", r.listed, r.present,
        static_cast<int>(r.missing.size()), static_cast<int>(r.empty_files.size()));
    if (r.unexpanded > 0) {
        say("通配符条目 %d 条（未展开，未计入完整性）", r.unexpanded);
        for (size_t i = 0; i < r.unexpanded_list.size() && i < 10; ++i) {
            say("  ~ %s", r.unexpanded_list[i].c_str());
        }
        if (r.unexpanded_list.size() > 10) say("  ...（共 %zu 条）", r.unexpanded_list.size());
    }
    if (!r.invalid.empty()) {
        say("");
        say("非法条目 %d 条（含 .. 或绝对路径，归一化后跑出包目录）：",
            static_cast<int>(r.invalid.size()));
        for (size_t i = 0; i < r.invalid.size() && i < 30; ++i) {
            say("  ! %s", r.invalid[i].c_str());
        }
    }
    if (!r.empty_files.empty()) {
        say("");
        say("警告：%d 个文件存在但大小为 0（截断/下载失败）：", static_cast<int>(r.empty_files.size()));
        for (size_t i = 0; i < r.empty_files.size() && i < 10; ++i) {
            say("  ! %s", r.empty_files[i].c_str());
        }
    }
    if (!r.missing.empty()) {
        say("");
        say("缺失清单（最多列 30 个）：");
        for (size_t i = 0; i < r.missing.size() && i < 30; ++i) {
            say("  - %s", r.missing[i].c_str());
        }
        if (r.missing.size() > 30) say("  ...（共 %zu 个）", r.missing.size());
    }
    section("结论");
    // 只有"一项不缺、没有坏条目、也没有展开不了的通配符"才能报 PASS：
    // 不然"缺失 0"会变成一张空头保证。
    if (r.missing.empty() && r.invalid.empty() && r.unexpanded == 0 && r.empty_files.empty()) {
        say("PASS：清单里的 %d 项全部存在且非空", r.listed);
        return 0;
    }
    if (!r.empty_files.empty()) {
        say("FAIL：%d 个文件是 0 字节 —— 截断或下载失败，不能当完整",
            static_cast<int>(r.empty_files.size()));
    }
    if (!r.empty_files.empty()) {
        say("");
        say("警告：%d 个文件存在但大小为 0（截断/下载失败）：", static_cast<int>(r.empty_files.size()));
        for (size_t i = 0; i < r.empty_files.size() && i < 10; ++i) {
            say("  ! %s", r.empty_files[i].c_str());
        }
    }
    if (!r.missing.empty()) {
        say("FAIL：缺 %d 个文件（见上面的缺失清单）", static_cast<int>(r.missing.size()));
    }
    if (!r.invalid.empty()) {
        say("FAIL：%d 条清单条目非法（../ 或绝对路径），未计入完整性",
            static_cast<int>(r.invalid.size()));
    }
    if (r.unexpanded > 0) {
        say("FAIL：有 %d 条通配符条目未展开，未计入完整性——这批文件对不对得上要人工确认",
            r.unexpanded);
    }
    return 1;
}

// ---------------------------------------------------------------- check
// 探测一个导入模块能不能被解析（不是"我们提供的"，是"系统/包能不能找到"）。
// 为什么必须真去探测：run 在 MISSING_DLL 时让用户来 check 看，所以 check 只数个数就报
// PASS 等于把人送进死胡同。
// 顺序：先在 exe 同目录找（游戏包里的 DLL 不会在系统目录），再交给系统搜索链。
// LOAD_LIBRARY_AS_DATAFILE 只映射镜像、不跑 DllMain，不会把游戏 DLL 真的拉起来。
bool module_resolvable(const std::string& module, const std::wstring& near_dir) {
    const std::wstring name = to_wide(module);
    if (name.empty()) return false;
    wchar_t found[MAX_PATH] = {0};
    const DWORD n = SearchPathW(near_dir.empty() ? nullptr : near_dir.c_str(), name.c_str(),
                                nullptr, MAX_PATH, found, nullptr);
    HMODULE h = nullptr;
    if (n > 0 && n < MAX_PATH) {
        h = LoadLibraryExW(found, nullptr, LOAD_LIBRARY_AS_DATAFILE);
    } else {
        h = LoadLibraryExW(name.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    }
    if (!h) return false;
    FreeLibrary(h);
    return true;
}

int cmd_check(const std::wstring& exe, const std::wstring& dll_dir_in) {
    const std::wstring dll_dir = resolve_dll_dir(dll_dir_in);
    section("导入面核对");
    if (!file_exists(exe)) {
        say("错误：找不到 %s", to_utf8(exe).c_str());
        return 2;
    }
    const pe::Info target = pe::parse_file(exe);
    if (!target.ok) {
        say("错误：解析失败：%s", target.error.c_str());
        return 2;
    }
    say("目标：%s", to_utf8(base_of(exe)).c_str());
    say("      机器 0x%04X，%s，子系统 %u，导入模块 %zu 个", target.machine,
        target.is_64 ? "PE32+" : "PE32", target.subsystem, target.imports.size());

    // 我们三个 shim 的导出集合
    int broken_shims = 0;
    std::map<std::string, std::vector<std::string>> our_exports;
    for (const wchar_t* s : kShims) {
        const std::wstring p = join(dll_dir, s);
        if (!file_exists(p)) {
            say("  ! 缺少 shim：%s（在 %s 里找）", to_utf8(s).c_str(), to_utf8(dll_dir).c_str());
            continue;
        }
        // 先判 shim 自身是否可解析：文件被截断/拷坏时 read_exports 会返回空，
        // 那样后面会把它报成"缺符号"，让人去实现一个其实已经存在的符号。
        const pe::Info info = pe::parse_file(p);
        if (!info.ok) {
            say("  ! shim 无法解析：%s（%s）—— 先重新构建或重新拷贝", to_utf8(s).c_str(),
                info.error.c_str());
            ++broken_shims;
            continue;
        }
        if (info.exports.empty()) {
            say("  ! shim 没有任何导出：%s —— 文件可能被截断", to_utf8(s).c_str());
            ++broken_shims;
            continue;
        }
        // key 必须小写：PE 导入表里的模块名被统一转成小写（pe.cpp），
        // 用原始大小写做 key 会让 AcpHal.dll / XCRDAPI.dll 这类混合大小写的模块
        // 根本没被核对（表现为"check 里看不到它"，不是"覆盖完整"）。
        std::string key = to_utf8(s);
        for (auto& c : key) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        our_exports[key] = info.exports;
    }
    if (broken_shims > 0) {
        say("");
        say("错误：%d 个 shim 损坏（见上面 ! 行），对表结果不可信 —— 先修好它们", broken_shims);
        return 2;
    }
    if (our_exports.empty()) {
        say("错误：一个 shim 都没找到，先用 build.sh 构建，或用 --dll-dir 指定目录");
        return 2;
    }

    int total_missing = 0;
    int by_ordinal = 0;      // 按序号导入的符号总数（不按名字核对）
    section("按模块对表");
    for (const auto& imp : target.imports) {
        auto it = our_exports.find(imp.module);
        const bool ours = it != our_exports.end();
        if (!ours) continue;   // 系统 DLL 不归我们管
        std::vector<std::string> missing;
    std::vector<std::string> empty_files;   // 存在但是 0 字节
        for (const auto& n : imp.names) {
            // 按序号导入（#N）不能按名字核对，也**不该算缺失**：
            // 算成缺失会让人去 shim 里补一个根本不该按名字补的符号（还可能改乱 .def 序号）
            if (!n.empty() && n[0] == '#') {
                ++by_ordinal;
                continue;
            }
            if (n.empty()) continue;
            if (std::find(it->second.begin(), it->second.end(), n) == it->second.end()) {
                missing.push_back(n);
            }
        }
        say("%-20s 需要 %zu 个，缺失 %zu 个", imp.module.c_str(), imp.names.size(), missing.size());
        for (size_t i = 0; i < missing.size() && i < 25; ++i) {
            say("    - %s", missing[i].c_str());
        }
        if (missing.size() > 25) say("    ...（共 %zu 个）", missing.size());
        total_missing += static_cast<int>(missing.size());
    }

    // 非 shim 模块：一个个探能不能解析（按 exe 同目录、再系统）
    const std::wstring near_dir = dir_of(abs_path(exe));
    std::vector<std::string> unresolved;
    section("其它导入模块（不需要我们提供，但必须能找到）");
    for (const auto& imp : target.imports) {
        if (our_exports.find(imp.module) != our_exports.end()) continue;
        const bool ok = module_resolvable(imp.module, near_dir);
        if (!ok) unresolved.push_back(imp.module);
        say("  %-24s %zu 个符号  %s", imp.module.c_str(), imp.names.size(),
            ok ? "可解析" : "!! 找不到");
    }

    section("结论");
    if (total_missing == 0 && unresolved.empty()) {
        say("PASS：我们提供的三个模块覆盖了它要的全部符号，其它依赖模块也都找得到");
        return 0;
    }
    if (total_missing > 0) {
        say("FAIL：我们提供的三个模块共缺 %d 个符号。先补这些，再谈跑起来", total_missing);
    }
    if (!unresolved.empty()) {
        say("FAIL：我们提供的三个模块覆盖了 shim 部分，但另有 %d 个依赖模块找不到：",
            static_cast<int>(unresolved.size()));
        for (size_t i = 0; i < unresolved.size() && i < 30; ++i) {
            say("  - %s", unresolved[i].c_str());
        }
        if (unresolved.size() > 30) say("  ...（共 %zu 个）", unresolved.size());
        say("这些模块要跟 exe 放在同一个目录（或系统里装好），否则运行时会 MISSING_DLL。");
    }
    return 1;
}

// ---------------------------------------------------------------- doctor
int cmd_doctor(const std::wstring& dll_dir_in) {
    int critical = 0;
    section("环境自检");

    // 1) 系统版本
    {
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        RTL_OSVERSIONINFOW vi{};
        vi.dwOSVersionInfoSize = sizeof(vi);
        auto fn = reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        if (fn && fn(&vi) == 0) {
            say("  OS            %lu.%lu build %lu", vi.dwMajorVersion, vi.dwMinorVersion,
                vi.dwBuildNumber);
        } else {
            say("  OS            读不到版本");
        }
    }

    // d3d12.dll / dxgi.dll 用运行时解析：doctor 是"诊断工具"，
    // 不该在缺 d3d12.dll 的机器上（Win7/8、Server Core、精简镜像）自己都起不来——
    // 那种机器恰恰最需要它来报告"D3D12 不可用"。
    using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    using PFN_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, int, REFIID, void**);
    HMODULE dxgi_mod = LoadLibraryW(L"dxgi.dll");
    HMODULE d3d12_mod = LoadLibraryW(L"d3d12.dll");
    auto pfn_factory = dxgi_mod ? reinterpret_cast<PFN_CreateDXGIFactory1>(
                                      GetProcAddress(dxgi_mod, "CreateDXGIFactory1"))
                                : nullptr;
    auto pfn_d3d12 = d3d12_mod ? reinterpret_cast<PFN_D3D12CreateDevice>(
                                     GetProcAddress(d3d12_mod, "D3D12CreateDevice"))
                               : nullptr;

    // 2) 显卡
    {
        say("  dxgi.dll      %s", dxgi_mod ? "有" : "没有");
        say("  d3d12.dll     %s", d3d12_mod ? "有" : "没有（Win7/8、Server Core、精简镜像常见）");
        IDXGIFactory1* f = nullptr;
        if (pfn_factory &&
            SUCCEEDED(pfn_factory(IID_IDXGIFactory1, reinterpret_cast<void**>(&f))) && f) {
            int idx = 0;
            int hardware = 0;
            int software = 0;
            // 同一颗芯片会被枚举多次（不同 LUID），用 厂商:设备:子系统 去重才等于"物理显卡数"
            std::vector<unsigned long long> hw_keys;
            int shown = 0;   // 实际打印了几行（软件适配器不打印）
            IDXGIAdapter1* a = nullptr;
            while (f->EnumAdapters1(static_cast<UINT>(idx), &a) != DXGI_ERROR_NOT_FOUND && a) {
                DXGI_ADAPTER_DESC1 d{};
                a->GetDesc1(&d);
                // 输出（显示器）数量：0 说明是"只渲染不出图"的适配器。
                // 混合显卡笔记本上独显通常 0 输出，画面经核显输出 —— 这不是错配。
                UINT outs = 0;
                for (UINT o = 0;; ++o) {
                    IDXGIOutput* out = nullptr;
                    if (a->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
                    out->Release();
                    ++outs;
                }
                const bool is_sw = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
                if (is_sw) {
                    // 软件渲染器（Microsoft Basic Render Driver）不列出来：它不是"卡"，
                    // 列出来只会让人以为机器上多插了一张。数量在结论行里交代。
                    ++software;
                    a->Release();
                    a = nullptr;
                    ++idx;
                    continue;
                }
                ++hardware;
                {
                    const unsigned long long key =
                        (static_cast<unsigned long long>(d.VendorId) << 48) |
                        (static_cast<unsigned long long>(d.DeviceId) << 32) |
                        d.SubSysId;
                    if (std::find(hw_keys.begin(), hw_keys.end(), key) == hw_keys.end()) {
                        hw_keys.push_back(key);
                    }
                }
                say("  GPU[%d]        %s（%s %04X:%04X，显存 %.1f GB，%u 个输出）", shown++,
                    to_utf8(d.Description).c_str(),
                    d.VendorId == 0x10DE ? "NVIDIA" : d.VendorId == 0x8086 ? "Intel"
                    : d.VendorId == 0x1002 ? "AMD"   : "其它",
                    d.VendorId, d.DeviceId, d.DedicatedVideoMemory / 1024.0 / 1024 / 1024, outs);
                a->Release();
                a = nullptr;
                ++idx;
            }
            if (idx == 0) {
                say("  GPU           没枚举到适配器");
                ++critical;
            } else {
                // 同一颗芯片可能被枚举多次（不同 LUID），所以"适配器数"不等于"物理显卡数"
                say("  适配器        物理显卡 %d 张（%d 个硬件适配器）%s", static_cast<int>(hw_keys.size()),
                    hardware,
                    software > 0 ? "；已忽略软件渲染器" : "");
                if (hardware > static_cast<int>(hw_keys.size())) {
                    say("                （有的卡被枚举了多次，属正常现象，不是多插了显卡）");
                }
                if (shown == 0) {
                    say("  GPU           只有软件渲染器（Microsoft Basic Render Driver）");
                }
                if (hardware == 0) {
                    say("  [!]           没有任何硬件适配器，只有软件渲染 —— 游戏基本跑不动");
                    ++critical;
                }
            }
            f->Release();
        } else {
            say("  GPU           创建 DXGI 工厂失败");
            ++critical;
        }
    }

    // 3) D3D12 能否建设备
    if (!pfn_d3d12) {
        say("  [!] D3D12     不可用：这台机器没有 d3d12.dll —— 游戏必然起不来");
        ++critical;
    } else {
        ID3D12Device* dev = nullptr;
        const HRESULT hr = pfn_d3d12(nullptr, 0xb000 /*11_0*/, IID_ID3D12Device,
                                     reinterpret_cast<void**>(&dev));
        if (SUCCEEDED(hr) && dev) {
            say("  D3D12         可用（feature level 11_0 以上）");
            dev->Release();
        } else {
            say("  [!] D3D12     不可用 hr=0x%08lx", static_cast<unsigned long>(hr));
            ++critical;
        }
    }

    // 4) VC 运行库
    {
        const wchar_t* dlls[] = {L"vcruntime140.dll", L"msvcp140.dll", L"vcruntime140_1.dll"};
        int have = 0;
        for (const wchar_t* d : dlls) {
            HMODULE m = LoadLibraryW(d);
            if (m) {
                ++have;
                FreeLibrary(m);   // 顺手释放：doctor 是一次性进程，但没必要留句柄
            }
        }
        say("  VC 运行库     %d/3（%s）", have,
            have == 3 ? "齐全" : "游戏可能自带，缺了也不一定致命");
    }

    // 5) 三个 shim（--dll-dir 优先，其次程序同目录）
    {
        const std::wstring d = resolve_dll_dir(dll_dir_in);
        int have = 0;
        for (const wchar_t* s : kShims) {
            if (file_exists(join(d, s))) ++have;
        }
        say("  shim          %d/%d 在 %s", have, kShimCount, to_utf8(d).c_str());
        if (have != kShimCount) {
            say("                -> 先 build.sh，或用 --dll-dir 指定");
            ++critical;
        }
    }

    // 6) 日志目录可写
    // 注意：run 的日志路径是"游戏目录 -> %LOCALAPPDATA%\Xbgtasix -> 临时目录"三级回落，
    // 只测临时目录并把它算成 critical 会造成假警报（本机就是：temp 不可写，但另外两级可写，
    // 日志照样落盘）。这里逐级探测，只要有一级可用就算通过。
    {
        auto writable = [](const std::wstring& dir) {
            const std::wstring probe =
                join(dir, L"xbgtasix_write_probe_" +
                              std::to_wstring(GetCurrentProcessId()) + L".tmp");
            HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (h == INVALID_HANDLE_VALUE) return false;
            CloseHandle(h);
            DeleteFileW(probe.c_str());
            return true;
        };
        wchar_t tmp[MAX_PATH] = {0};
        GetTempPathW(MAX_PATH, tmp);
        wchar_t local[MAX_PATH] = {0};
        const DWORD nlocal = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);

        const bool w_exe = writable(self_dir());
        const bool w_local = nlocal > 0 && writable(std::wstring(local));
        const bool w_tmp = writable(tmp);
        say("  可写         程序目录 %s / LOCALAPPDATA %s / 临时目录 %s（日志至少落一处即可）",
            w_exe ? "可写" : "不可写", w_local ? "可写" : "不可写", w_tmp ? "可写" : "不可写");
        if (!w_exe && !w_local && !w_tmp) {
            say("  [!]          三级都不可写 —— 日志无处落盘，排障会很困难");
            ++critical;
        }
    }

    // 6b) 音频：包内的 Bink2Scarlett.dll 只从 XAudio2_9.dll 导入一个函数
    //     （序号 1 = XAudio2Create），所以只要这个 DLL 在、且有音频端点，音频路径就成立。
    //     缺 XAudio2_9 时游戏会在加载 Bink 那一步直接 0xC0000135。
    {
        HMODULE xa = LoadLibraryW(L"XAudio2_9.dll");
        const bool have_xa = xa != nullptr;
        const bool have_create =
            have_xa && GetProcAddress(xa, reinterpret_cast<LPCSTR>(1)) != nullptr;  // XAudio2Create
        if (xa) FreeLibrary(xa);
        const UINT out_devs = waveOutGetNumDevs();
        say("  音频          XAudio2_9 %s，波形输出端点 %u 个%s", have_xa ? "有" : "没有", out_devs,
            have_create ? "（XAudio2Create 可解析）" : "");
        if (!have_xa) {
            say("  [!]           缺少 XAudio2_9.dll —— 包内 Bink2Scarlett.dll 导入它，"
                "游戏会在加载视频/音频模块时 0xC0000135");
            say("                解决：装 DirectX/XAudio2 可再分发组件（Win10 1809+ 一般自带）");
            ++critical;
        } else if (!have_create) {
            say("  [!]           XAudio2_9 存在但取不到 XAudio2Create（序号 1）—— 版本不对？");
            ++critical;
        }
        if (out_devs == 0) {
            say("  [!]           没有波形输出端点：游戏可能卡在初始化音频（或静音）");
            // 不算 critical：没声卡也能跑，只是没声音
        }
    }

    // 7) 交互桌面（系统侧 UI 需要）
    {
        const HWINSTA ws = GetProcessWindowStation();
        USEROBJECTFLAGS uof{};
        DWORD need = 0;
        bool visible = false;
        if (ws && GetUserObjectInformationA(ws, UOI_FLAGS, &uof, sizeof(uof), &need)) {
            visible = (uof.dwFlags & WSF_VISIBLE) != 0;
        }
        say("  交互桌面      %s", visible ? "有（系统侧 UI 可用）" : "无（面板会降级成立即完成）");
    }

    if (d3d12_mod) FreeLibrary(d3d12_mod);
    if (dxgi_mod) FreeLibrary(dxgi_mod);

    section("结论");
    if (critical == 0) {
        say("PASS：环境没有阻碍启动的问题");
        return 0;
    }
    say("FAIL：%d 项需要处理（见上面标 -> 的行）", critical);
    return 1;
}

// ---------------------------------------------------------------- run
struct RunOptions {
    bool dry_run = false;   // 只预检，不写任何文件、不启动进程
    bool stop_mode = false;
    int timeout_sec = 180;
    std::wstring dll_dir;
    std::wstring log_path;   // 为空则按默认链：游戏目录 -> %LOCALAPPDATA% -> 临时目录
};

std::wstring find_exe_in_dir(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(join(dir, L"layout_*.xml").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    const std::wstring layout = join(dir, fd.cFileName);
    FindClose(h);
    const std::string text = read_text(layout, 4u << 20);

    // 与 do_verify 同一条拼接规则（DestinationPath + Include）：包里的 exe 经常在子目录里，
    // 之前只看 Include、直接当包根相对路径用，就永远找不到。
    std::vector<std::wstring> cands;
    size_t pos = 0;
    while ((pos = text.find("<FileGroup", pos)) != std::string::npos) {
        // 不能只找 "/>"：非自闭合写法的属性会被整段丢掉（少算条目 -> 假 PASS）
        const size_t close = text.find('>', pos);
        if (close == std::string::npos) break;
        const std::string tag = text.substr(pos, close - pos + 1);
        pos = close + 1;

        const std::string include = attr_of(tag, "Include");
        if (include.empty()) continue;
        // 通配符条目不是文件名：别把 "*.exe" 当成一个叫 "*.exe" 的文件
        if (include.find('*') != std::string::npos || include.find('?') != std::string::npos) {
            continue;
        }

        const std::string rel = dest_rel_of(tag, include);
        std::string low = rel;
        for (auto& c : low) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if (low.size() < 4 || low.compare(low.size() - 4, 4, ".exe") != 0) continue;
        // 崩溃处理器不是游戏本体（包里有好几个 exe，crashpad handler 常排在最前）
        if (low.find("crash") != std::string::npos || low.find("handler") != std::string::npos) {
            continue;
        }
        const std::wstring cand = join(dir, to_wide(rel));
        if (std::find(cands.begin(), cands.end(), cand) == cands.end()) cands.push_back(cand);
    }
    if (cands.empty()) return {};
    if (cands.size() > 1) {
        say("layout 里声明了 %zu 个候选 exe：", cands.size());
        for (const auto& c : cands) say("  - %s", to_utf8(base_of(c)).c_str());
        say("取第一个；不是想要的那个，就直接把 exe 路径传给命令行");
    }
    return cands.front();
}

std::string classify(unsigned long code, int unimpl_hits = 0) {
    switch (code) {
        case 0: return "PASS";
        // 70 是"我们自己的桩在 stop 模式下退出"的约定码。
        // 游戏自己 exit(70) 也会走到这里，所以只有在日志里真的看到
        // UNIMPLEMENTED 时才认成 UNIMPL（否则会给出完全错误的待办清单）。
        case 70: return unimpl_hits > 0 ? "UNIMPL" : "OTHER";
        case 124: return "TIMEOUT";
        case 0xC0000135UL: return "MISSING_DLL";
        case 0xC0000139UL: return "MISSING_EXPORT";
        case 0xC0000005UL: return "ACCESS_VIOLATION";
        case 0xC000007BUL: return "BAD_IMAGE";
        default: return "OTHER";
    }
}

std::string advice_for(const std::string& kind, int unimplemented_count, const std::string& log_path) {
    if (kind == "PASS") return "程序正常退出。若没看到画面，按 --stop 重跑看它卡在哪一步。";
    if (kind == "UNIMPL")
        return "撞到未实现的 API（--stop 模式）。上面列的名字就是要补的桩。";
    if (kind == "TIMEOUT")
        return "超时。多半卡在等一个永不完成的异步——看日志最后几行在等什么。";
    if (kind == "MISSING_DLL")
        return "缺依赖 DLL。用 `xbgtasix check <exe>` 看哪个依赖模块解析不到，把同名 DLL 拷到 exe 同目录再跑。";
    if (kind == "MISSING_EXPORT")
        return "缺导出符号（入口点未找到）。用 `xbgtasix check <exe>` 看缺哪些名字。";
    if (kind == "ACCESS_VIOLATION")
        return "访问冲突。看日志最后一行：常见是未实现接口返回空指针后被解引用。";
    if (kind == "BAD_IMAGE") return "映像格式无效。用 `xbgtasix check <exe>` 确认它对不对。";
    if (unimplemented_count > 0)
        return "非零退出且日志里有 UNIMPLEMENTED —— 先补频次最高的那几个。";
    return "非零退出但日志没有 UNIMPLEMENTED。看上面日志里的 FAIL 行，或用 --stop 重跑定位。";
}

int cmd_run(const std::wstring& target_in, const RunOptions& opt) {
    std::wstring target = target_in;
    std::wstring game_dir;

    section("预检");
    if (dir_exists(target)) {
        game_dir = abs_path(target);
        target = find_exe_in_dir(game_dir);
        if (target.empty()) {
            say("错误：目录里没有 layout 声明的 .exe");
            return 2;
        }
        if (!file_exists(target)) {
            say("错误：layout 声明了 %s，但文件不存在", to_utf8(base_of(target)).c_str());
            say("      用 `xbgtasix verify <目录>` 看完整缺失清单");
            return 2;
        }
        say("目标目录      %s", to_utf8(game_dir).c_str());
    } else if (file_exists(target)) {
        target = abs_path(target);
        game_dir = dir_of(target);
    } else {
        say("错误：找不到 %s", to_utf8(target_in).c_str());
        return 2;
    }
    say("目标程序      %s", to_utf8(base_of(target)).c_str());

    // 包完整性（有 layout 才检查）
    const VerifyResult vr = do_verify(game_dir);
    if (vr.listed > 0) {
        say("包完整性      清单 %d 项，缺失 %d 项%s", vr.listed, static_cast<int>(vr.missing.size()),
            vr.missing.empty() ? "" : "（继续尝试）");
        for (size_t i = 0; i < vr.missing.size() && i < 5; ++i) {
            say("               缺：%s", vr.missing[i].c_str());
        }
    }

    // shim
    const std::wstring dll_dir = resolve_dll_dir(opt.dll_dir);
    int shims = 0;
    for (const wchar_t* s : kShims) {
        if (file_exists(join(dll_dir, s))) ++shims;
    }
    say("shim          %d/%d 在 %s", shims, kShimCount, to_utf8(dll_dir).c_str());
    if (shims != kShimCount) {
        say("错误：shim 不齐（需要 %d 个），先跑 build.sh", kShimCount);
        return 2;
    }

    // 放置方式
    std::wstring run_dir = abs_path(game_dir);
    std::wstring run_exe = abs_path(target);
    section("准备运行环境");
    // 曾经的 --staging 用 junction 指向原包：看上去是"临时目录"，实际上
    // 三个 DLL 与备份改名**都写进了原包**（实测发现），承诺的"原包不动"根本不成立。
    // 要跑就必须把 DLL 放在 exe 旁边；所以改成诚实的做法：
    //   --dry-run  只检查、不写、不启动；真正运行时自动备份同名 DLL。
    if (run_dir == game_dir) {
        say("放置方式      直接放入游戏目录（原目录会被写入 3 个 DLL）");
    }

    if (opt.dry_run) {
        // 只预检：不写任何文件、不启动进程。用来在动手前看清楚"会发生什么"。
        say("dry-run       只做预检，不写文件、不启动进程");
        for (const wchar_t* sh : kShims) {
            const std::wstring src = join(dll_dir, sh);
            const std::wstring dst = join(run_dir, sh);
            if (same_file(src, dst)) {
                say("  %s 已就位（同一文件）", to_utf8(sh).c_str());
            } else if (file_exists(dst)) {
                say("  将备份 %s -> %s.xbgtasix-backup 后覆盖", to_utf8(sh).c_str(),
                    to_utf8(sh).c_str());
            } else {
                say("  将写入 %s", to_utf8(dst).c_str());
            }
        }
        say("  将启动 %s", to_utf8(run_exe).c_str());
        say("");
        say("预检通过。去掉 --dry-run 即可真正运行。");
        return 0;
    }

    // 拷贝三个 shim。覆盖同名文件前必须备份：一次坏构建不至于把包里的原文件永久改坏。
    int copied = 0;
    for (const wchar_t* s : kShims) {
        const std::wstring src = join(dll_dir, s);
        const std::wstring dst = join(run_dir, s);
        if (same_file(src, dst)) {
            ++copied;   // 自己拷自己会得 ERROR_SHARING_VIOLATION，当成已就位
            continue;
        }
        if (file_exists(dst)) {
            const std::wstring bak = dst + L".xbgtasix-backup";
            bool ok = false;
            if (file_exists(bak)) {
                // 备份已存在：原文件在备份里，dst 是上次我们放的，删掉 dst 就好，
                // 绝不能拿 dst 覆盖备份，否则第一次的原文件就永久没了。
                ok = DeleteFileW(dst.c_str()) != 0;
                if (ok) say("  备份已在，原文件保留在 %s", to_utf8(bak).c_str());
            } else {
                ok = MoveFileW(dst.c_str(), bak.c_str()) != 0;
                if (ok) {
                    say("  已备份 %s -> %s", to_utf8(base_of(dst)).c_str(), to_utf8(bak).c_str());
                }
            }
            if (!ok) {
                say("  ! 备份失败 %s err=%lu —— 中止，包里的原文件没动", to_utf8(s).c_str(),
                    GetLastError());
                return 2;
            }
        }
        if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            ++copied;
        } else {
            say("  ! 拷贝失败 %s err=%lu", to_utf8(s).c_str(), GetLastError());
        }
    }
    say("已放置        %d/%d 个 DLL", copied, kShimCount);
    if (copied != kShimCount) {
        say("错误：DLL 没放全，后面必然起不来");
        return 2;
    }

    // 日志路径：--log 优先，否则游戏目录 -> %LOCALAPPDATA% -> 临时目录
    std::wstring log_path = opt.log_path.empty() ? join(run_dir, L"xbgtasix.log") : opt.log_path;
    {
        HANDLE h = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE && !opt.log_path.empty()) {
            // 用户明确指定了 --log：打不开就说清楚，别偷偷换成别的路径
            // （否则他会在约定的位置找不到日志，还以为是兼容层没写）
            say("错误          --log 指定的路径打不开：%s err=%lu", to_utf8(log_path).c_str(),
                GetLastError());
            return 2;
        }
        if (h == INVALID_HANDLE_VALUE) {
            // 游戏目录写不了时退到 %LOCALAPPDATA%\Xbgtasix，再不行才用临时目录
            wchar_t local[MAX_PATH] = {0};
            const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
            if (n > 0) {
                const std::wstring dir = join(local, L"Xbgtasix");
                CreateDirectoryW(dir.c_str(), nullptr);
                const std::wstring cand = join(dir, L"xbgtasix.log");
                h = CreateFileW(cand.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) log_path = cand;
            }
        }
        if (h == INVALID_HANDLE_VALUE) {
            wchar_t tmp[MAX_PATH] = {0};
            GetTempPathW(MAX_PATH, tmp);
            log_path = join(tmp, L"xbgtasix.log");
            h = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    // 环境块：当前环境（先剔除我们要覆盖的同名项）+ 覆盖项
    //
    // 关键：GetEnvironmentVariable 取**第一次**出现，不是最后一次。原来只会往后追加，
    // 继承来的 XBGTASIX_LOG 就压过了 --log（实测：--log 指定的文件 0 字节，真日志写在
    // 继承路径上）。所以先把同名项摘掉，再追加自己的。
    const std::vector<std::pair<std::wstring, std::wstring>> overrides = {
        {L"XBGTASIX_UNIMPL", opt.stop_mode ? L"stop" : L"log"},
        {L"XBGTASIX_LOG", log_path},
        {L"XBGTASIX_UI", L"immediate"},
        {L"XBGTASIX_PACKAGE_ROOT", run_dir},
        {L"XBGTASIX_CONFIG", join(run_dir, L"MicrosoftGame.Config")},
    };
    // 只认 "NAME=" 这个形状：XBGTASIX_LOGX 不能被 XBGTASIX_LOG 误伤
    auto is_overridden = [&overrides](const wchar_t* block) {
        for (const auto& kv : overrides) {
            const size_t n = kv.first.size();
            if (_wcsnicmp(block, kv.first.c_str(), n) != 0) continue;
            if (block[n] == L'=') return true;
        }
        return false;
    };
    std::vector<wchar_t> env;
    {
        wchar_t* cur = GetEnvironmentStringsW();
        if (cur) {
            for (wchar_t* p = cur; *p;) {
                const size_t len = wcslen(p);
                // 以 '=' 开头的伪项（如 "=C:=C:\path"）是盘符当前目录，不能摘
                if (p[0] != L'=' && is_overridden(p)) {
                    p += len + 1;
                    continue;
                }
                env.insert(env.end(), p, p + len + 1);
                p += len + 1;
            }
            FreeEnvironmentStringsW(cur);
        }
    }
    for (const auto& kv : overrides) {
        const std::wstring line = kv.first + L"=" + kv.second;
        env.insert(env.end(), line.begin(), line.end());
        env.push_back(0);
    }
    env.push_back(0);   // 环境块以双 0 结尾

    section("运行");
    say("模式          %s，超时 %ds", opt.stop_mode ? "stop（撞到未实现即停）" : "log（跑到底）",
        opt.timeout_sec);
    say("日志          %s", to_utf8(log_path).c_str());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmdline = L"\"" + run_exe + L"\"";
    // 配置路径走环境变量（XBGTASIX_CONFIG），不要塞进 argv：
    // 多给的参数会让按 argv 取"存档目录/安装模式"的游戏行为不同。
    // Job Object：游戏常会拉起子进程（崩溃处理器、启动器、平台服务）。
    // 只 TerminateProcess 直接子进程会留下孤儿，继续写日志、继续占显存。
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    const DWORD t0 = GetTickCount();
    const BOOL started = CreateProcessW(run_exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                                        CREATE_UNICODE_ENVIRONMENT, env.data(), run_dir.c_str(), &si,
                                        &pi);
    if (started && job) {
        // 分配失败不致命（只影响超时清理），但要记下来
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            say("提示          Job Object 绑定失败 err=%lu（超时可能留下游戏子进程）", GetLastError());
        }
    }
    if (!started) {
        // 光打 errno 等于没说：这三个错误码占了实际遇到的大半，各自下一步完全不同。
        const DWORD e = GetLastError();
        say("启动失败      CreateProcess err=%lu", e);
        switch (e) {
            case ERROR_BAD_EXE_FORMAT:
            case ERROR_EXE_MACHINE_TYPE_MISMATCH:   // 实测：非 PE 文件这里给 216 而不是 193
                say("              不是有效的可执行文件（BAD_IMAGE）：PE 头/位数不对，"
                    "用 `xbgtasix check <exe>` 看它的机器类型");
                break;
            case ERROR_FILE_NOT_FOUND:
                say("              找不到 exe 或它依赖的某个模块：先 `xbgtasix verify <目录>`，"
                    "再把缺的 DLL 补到 exe 同目录");
                break;
            case ERROR_ACCESS_DENIED:
                say("              拒绝访问：多半被杀软/权限拦了，或目标在受保护目录；"
                    "换个目录、或提权后重试");
                break;
            default:
                say("              先用 `xbgtasix doctor` 做环境自检，再带 --stop 重跑");
                break;
        }
        return 2;
    }
    const DWORD wait = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(opt.timeout_sec) * 1000);
    unsigned long code = 124;
    if (wait == WAIT_TIMEOUT) {
        if (job) TerminateJobObject(job, 124);   // 连游戏拉起的子进程一起收
        else TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, 5000);
        code = 124;
    } else {
        DWORD ec = 0;
        // 取不到退出码时不能默认 0：那会被 classify 判成 PASS，把人骗得很彻底
        if (!GetExitCodeProcess(pi.hProcess, &ec)) {
            const DWORD e = GetLastError();
            say("错误          读不到退出码 err=%lu", e);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return 2;
        }
        code = ec;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (job) CloseHandle(job);   // KILL_ON_JOB_CLOSE：残留子进程在这里被清掉
    const DWORD elapsed = GetTickCount() - t0;

    // 读日志：只取末尾 2MB。日志可能几百 MB，"日志末尾"和 UNIMPLEMENTED 频次都应该基于尾部。
    constexpr size_t kTailBytes = 2u << 20;
    const std::string log_text = read_text_tail(log_path, kTailBytes);
    std::vector<std::string> lines = split_lines(log_text);
    std::map<std::string, int> unimpl;
    for (const auto& line : lines) {
        const size_t p = line.find("UNIMPLEMENTED: ");
        if (p != std::string::npos) {
            std::string name = line.substr(p + 15);
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
            if (!name.empty()) ++unimpl[name];
        }
    }
    int unimpl_total = 0;
    for (const auto& kv : unimpl) unimpl_total += kv.second;
    int fail_lines = 0;
    for (const auto& line : lines) {
        if (line.find("FAIL") != std::string::npos) ++fail_lines;
    }

    section("结果");
    const std::string kind = classify(code, unimpl_total);
    say("退出码        0x%08lx", code);
    say("分类          %s", kind.c_str());
    say("耗时          %lu ms", elapsed);
    say("日志          %zu 行（仅统计日志末尾 2MB），UNIMPLEMENTED %d 条，FAIL %d 行", lines.size(),
        unimpl_total, fail_lines);

    if (!unimpl.empty()) {
        std::vector<std::pair<std::string, int>> sorted(unimpl.begin(), unimpl.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        say("");
        say("未实现频次（先补最多的）：");
        for (size_t i = 0; i < sorted.size() && i < 15; ++i) {
            say("  %3d  %s", sorted[i].second, sorted[i].first.c_str());
        }
    }

    if (!lines.empty()) {
        say("");
        say("日志末尾：");
        const size_t from = lines.size() > 20 ? lines.size() - 20 : 0;
        for (size_t i = from; i < lines.size(); ++i) {
            say("  %s", lines[i].c_str());
        }
    }

    section("下一步");
    say("%s", advice_for(kind, unimpl_total, to_utf8(log_path)).c_str());
    return (kind == "PASS") ? 0 : 1;
}

// ---------------------------------------------------------------- restore
// run 只会写三样东西：备份同名 DLL、覆盖、写日志。restore 就是它的撤销。
// 判定表（三个 shim 逐个走）：
//   备份在          -> 删掉当前 shim（我们放的那份），再把备份改回原名   "已还原"
//   备份不在、DLL 在 -> 这个位置原本是空的，现在这个文件是我们放的：直接删 "已移除"
//   两个都不在      -> 没事可做，跳过
// 顺序很重要：必须先删 dst 再 MoveFile，否则备份改不回原名。
// 不碰包里其它文件（尤其是 xbgtasix.log：它是排障证据，不是我们放的 DLL）。
// 两个文件内容是否完全相同（先比大小，再比内容）。
// restore 用它判断"这个 DLL 到底是不是我们放的那份"。
bool same_content(const std::wstring& a, const std::wstring& b) {
    HANDLE ha = CreateFileW(a.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                            nullptr);
    if (ha == INVALID_HANDLE_VALUE) return false;
    HANDLE hb = CreateFileW(b.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                            nullptr);
    if (hb == INVALID_HANDLE_VALUE) {
        CloseHandle(ha);
        return false;
    }
    LARGE_INTEGER sa{}, sb{};
    bool same = GetFileSizeEx(ha, &sa) && GetFileSizeEx(hb, &sb) && sa.QuadPart == sb.QuadPart;
    std::vector<char> ba(65536), bb(65536);
    while (same) {
        DWORD ra = 0, rb = 0;
        if (!ReadFile(ha, ba.data(), static_cast<DWORD>(ba.size()), &ra, nullptr)) { same = false; break; }
        if (!ReadFile(hb, bb.data(), static_cast<DWORD>(bb.size()), &rb, nullptr)) { same = false; break; }
        if (ra != rb || (ra && std::memcmp(ba.data(), bb.data(), ra) != 0)) { same = false; break; }
        if (ra == 0) break;   // 两边都读完
    }
    CloseHandle(ha);
    CloseHandle(hb);
    return same;
}

int cmd_restore(const std::wstring& target_in, std::wstring dll_dir, bool force = false) {
    std::wstring dir;
    if (dir_exists(target_in)) {
        dir = abs_path(target_in);
    } else if (file_exists(target_in)) {
        // 也允许直接给 exe：DLL 就在 exe 旁边（run 就是这么放的）
        dir = dir_of(abs_path(target_in));
    } else {
        say("错误：找不到 %s", to_utf8(target_in).c_str());
        say("      用法：xbgtasix restore <包目录|exe路径>");
        return 2;
    }

    section("还原");
    say("目标目录      %s", to_utf8(dir).c_str());

    int restored = 0;   // 备份改回原名
    int removed = 0;    // 没有备份，直接删掉我们放的 DLL
    int failed = 0;
    int skipped = 0;   // 无备份且内容不像我们的 -> 拒绝删除
    for (const wchar_t* s : kShims) {
        const std::wstring dst = join(dir, s);
        const std::wstring bak = dst + L".xbgtasix-backup";
        const bool have_dst = file_exists(dst);
        const bool have_bak = file_exists(bak);
        if (have_bak) {
            // 先把我们放的那份删掉：MoveFile 到已存在的目标会失败
            if (have_dst && !DeleteFileW(dst.c_str())) {
                say("  ! 删除 %s 失败 err=%lu —— 备份还在，排除占用后重试", to_utf8(s).c_str(),
                    GetLastError());
                ++failed;
                continue;
            }
            if (MoveFileW(bak.c_str(), dst.c_str())) {
                say("已还原        %s（原件已回到原位）", to_utf8(s).c_str());
                ++restored;
            } else {
                say("  ! 还原 %s 失败 err=%lu —— 备份仍在 %s，可手工改名", to_utf8(s).c_str(),
                    GetLastError(), to_utf8(bak).c_str());
                ++failed;
            }
        } else if (have_dst) {
            // 没有备份时，"这个 DLL 到底是不是我们放的"无法凭空判断：
            // 如果用户从没 run 过，目录里可能就是游戏自带的原版 DLL，直接删掉会造成损坏。
            // 所以先跟本工具目录里的同名 shim 比内容：一致才删，不一致就拒绝并提示 --force。
            const std::wstring ours = join(resolve_dll_dir(dll_dir), s);
            const bool is_ours = file_exists(ours) && same_content(dst, ours);
            if (!is_ours && !force) {
                say("  ? 跳过 %s：没有备份，且内容与本工具的 shim 不同 —— 可能是游戏自带原版",
                    to_utf8(s).c_str());
                say("             确认它确实是上次 run 放进来的，可加 --force 强制删除");
                ++skipped;
                continue;
            }
            if (DeleteFileW(dst.c_str())) {
                say("已移除        %s（%s）", to_utf8(s).c_str(),
                    is_ours ? "确认是我们放的副本" : "--force 强制删除");
                ++removed;
            } else {
                say("  ! 移除 %s 失败 err=%lu（可能被游戏或杀软占用）", to_utf8(s).c_str(),
                    GetLastError());
                ++failed;
            }
        } else {
            say("  %s 不在这个目录里，跳过", to_utf8(s).c_str());
        }
    }

    // 收尾：目录里还有没有别的 *.xbgtasix-backup（不是三个 shim 的，或刚才没还原成功的）。
    // 有就列出来——要不要动得人自己判断，不能静默当"全好了"。
    std::vector<std::string> leftover;
    {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(join(dir, L"*.xbgtasix-backup").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    leftover.push_back(to_utf8(fd.cFileName));
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    section("结论");
    say("已还原 %d 个，已移除 %d 个，跳过 %d 个，失败 %d 个", restored, removed, skipped, failed);
    if (!leftover.empty()) {
        say("");
        say("目录里还剩 %d 个备份文件（需要人工判断，可能里面才是原件）：",
            static_cast<int>(leftover.size()));
        for (size_t i = 0; i < leftover.size() && i < 20; ++i) {
            say("  ? %s", leftover[i].c_str());
        }
        if (leftover.size() > 20) say("  ...（共 %zu 个）", leftover.size());
    }
    if (failed > 0) {
        say("FAIL：%d 项没还原成功（见上面 ! 行）", failed);
        return 1;
    }
    if (restored + removed == 0 && skipped == 0) {
        say("PASS：这个目录里没有我们放的 DLL，也没有备份 —— 无需还原");
    } else if (skipped > 0 && restored + removed == 0) {
        say("PASS：跳过 %d 个（内容不像我们的 shim，未动）—— 如需强制删除加 --force", skipped);
    } else {
        say("PASS：%d 个原件已归位、%d 个多余 DLL 已清掉", restored, removed);
    }
    return 0;
}

// ---------------------------------------------------------------- firstrun
// 拿到 exe 时最顺的四步：verify -> doctor -> check -> run。
// 前三步只报告，不拦路（缺文件/缺符号恰恰是最常见的开局，先让人看全）；
// 唯一的硬失败是"目标 exe 根本不存在"——没有它，后面三步全是空谈，直接返回 2。
int cmd_firstrun(const std::wstring& target_in, const RunOptions& opt) {
    say("######## 首次运行：verify -> doctor -> check -> run ########");

    // 1) 包完整性：缺失/非法/0 字节只警告，不中断
    section("第 1/4 步：包完整性（verify）");
    int rc_verify = 0;
    if (dir_exists(target_in)) {
        rc_verify = cmd_verify(abs_path(target_in));
    } else if (file_exists(target_in)) {
        say("给的是 exe 而不是包目录：完整性核对需要包目录，这一步跳过");
    } else {
        say("目标还不存在，先定位它（下一步就是硬失败点）");
    }
    if (rc_verify != 0) {
        say("第 1 步没 PASS（见上）：先记下缺什么，继续往下走 —— 缺文件不等于不用试");
    }

    // 定位目标 exe。它不存在就直接停：exit 2 = 参数/自身问题，与 run 的 2 一致
    std::wstring exe;
    if (dir_exists(target_in)) {
        exe = find_exe_in_dir(abs_path(target_in));
        if (!exe.empty() && !file_exists(exe)) {
            say("错误：layout 声明了 %s，但文件不存在", to_utf8(base_of(exe)).c_str());
            say("      先按第 1 步的缺失清单把文件补齐，再重跑 firstrun");
            return 2;
        }
    } else if (file_exists(target_in)) {
        exe = abs_path(target_in);
    }
    if (exe.empty()) {
        say("错误：找不到 %s", to_utf8(target_in).c_str());
        say("      用法：xbgtasix firstrun <包目录|exe路径> [--dry-run] [--stop] [--timeout 秒]");
        return 2;
    }

    // 2) 环境自检：critical 只警告
    section("第 2/4 步：环境自检（doctor）");
    const int rc_doctor = cmd_doctor(opt.dll_dir);
    if (rc_doctor != 0) {
        say("环境有 critical 项（见上）：下面可能起不来，先按提示处理再跑");
    }

    // 3) 导入面：缺符号只警告
    section("第 3/4 步：导入面（check）");
    const int rc_check = cmd_check(exe, opt.dll_dir);
    if (rc_check != 0) {
        say("导入面有缺口（见上）：能启动也可能半路崩在缺符号上");
    }

    // 4) 真正运行。--dry-run/--stop/--timeout/--dll-dir/--log 原样传下去：
    //    dry-run 时 cmd_run 只做预检，不写任何文件、不启动进程
    section(opt.dry_run ? "第 4/4 步：运行（run，--dry-run 只预检）" : "第 4/4 步：运行（run）");
    return cmd_run(exe, opt);
}

// ---------------------------------------------------------------- 交互
// 双击启动时，控制台只有我们自己（就绪即退出会"一闪而过"）。
// 这个判断决定要不要在结束前等一次回车。
// 只在"双击启动、控制台归我们独占"时才等回车。
// 之前用 n <= 1：没有控制台（DETACHED / stdin 被重定向）时 GetConsoleProcessList 返回 0，
// 0 <= 1 也成立 → getchar() 永久阻塞（实测无控制台启动会挂死）。所以必须是 n == 1，
// 再加一道 GetConsoleMode 确认 stdin 真的挂在控制台上。
bool own_console() {
    DWORD pids[4] = {0};
    const DWORD n = GetConsoleProcessList(pids, 4);
    if (n != 1) return false;   // 0 = 没有控制台；>1 = 控制台里还有别的进程
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
}

void pause_if_interactive() {
    if (!own_console()) return;
    printf("\n按回车退出...");
    fflush(stdout);
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
    }
}

// 在 dir 及其上 3 层父目录里找含 layout_*.xml 的包目录
// 判断一个目录里是否有 layout_*.xml（= 像游戏包的根）
bool has_layout(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(join(dir, L"layout_*.xml").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return true;
}

std::wstring find_package_near(const std::wstring& start) {
    std::wstring dir = abs_path(start);
    // 第 1 轮：自己 + 上 3 层祖先（工具被放在包里时命中）
    std::vector<std::wstring> ancestors;
    for (int depth = 0; depth < 4 && !dir.empty(); ++depth) {
        if (has_layout(dir)) return dir;
        ancestors.push_back(dir);
        const std::wstring up = dir_of(dir);
        if (up == dir) break;
        dir = up;
    }
    // 第 2 轮：祖先的**直接子目录**（常见情形：工具在 D:\GTA6\Xbgtasix\build，
    // 游戏包是兄弟目录 D:\GTA6\GTA6....；只往上看永远找不到）
    for (const auto& a : ancestors) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(join(a, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == L'.') continue;   // . 与 ..
            const std::wstring cand = join(a, fd.cFileName);
            if (has_layout(cand)) {
                FindClose(h);
                return cand;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return {};
}

int interactive_menu() {
    printf("\n");
    printf("  1) 运行游戏        2) 检查导入面\n");
    printf("  3) 核对包完整性    4) 环境自检\n");
    printf("  5) 还原(撤销 run)  6) 首次运行(四步走)    0) 退出\n");
    printf("选择: ");
    fflush(stdout);
    char line[256] = {0};
    if (!fgets(line, sizeof(line), stdin)) return -1;   // EOF：调用方按"缺参数"处理
    switch (line[0]) {
        case '1': return 1;
        case '2': return 2;
        case '3': return 3;
        case '4': return 4;
        case '5': return 5;   // 还原
        case '6': return 6;   // 首次运行
        default: return 0;
    }
}

// ---------------------------------------------------------------- CLI
void usage() {
    printf("xbgtasix %s —— Xbox GDK 兼容层的启动器/诊断工具\n\n", kVersion);
    printf("用法：\n");
    printf("  xbgtasix run    <exe|包目录> [--dry-run] [--stop] [--timeout 秒]"
           " [--dll-dir 目录] [--log 文件]\n");
    printf("  xbgtasix check  <exe> [--dll-dir 目录]\n");
    printf("  xbgtasix verify <包目录>\n");
    printf("  xbgtasix restore  <exe|包目录> [--force] [--dll-dir 目录]\n");
    printf("  xbgtasix firstrun <exe|包目录> [--dry-run] [--stop] [--timeout 秒] [--dll-dir 目录]\n");
    printf("  xbgtasix doctor\n");
    printf("  xbgtasix --version\n\n");
    printf("说明：\n");
    printf("  run     把三个 DLL 放到游戏旁边，拉起进程，按退出码分类并给出下一步\n");
    printf("  check   解析游戏的导入表，跟我们的导出面逐名字对表\n");
    printf("  verify  按包自带 layout 清单核对文件完整性\n");
    printf("  doctor  显卡/驱动/VC 运行库/写权限/交互桌面自检\n");
    printf("  restore 撤销 run 的写入：有备份就改回原名，没有备份的我们放的 DLL 直接删\n");
    printf("          （不碰包里其它文件，也不删 xbgtasix.log；末尾会列出剩下的备份）\n");
    printf("  firstrun 四步走：verify -> doctor -> check -> run；前两步只警告，exe 不存在才停\n");
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        // 双击进来（或直接运行）：自动找包，给菜单，别一闪而过
        const std::wstring pkg = find_package_near(self_dir());
        printf("xbgtasix %s —— Xbox GDK 兼容层启动器\n", kVersion);
        if (!pkg.empty()) {
            const std::wstring exe = find_exe_in_dir(pkg);
            printf("\n找到游戏包: %s\n", to_utf8(pkg).c_str());
            if (exe.empty()) {
                printf("  !! 包内没有 layout 声明的 .exe —— 启动不了（先用 3 核对完整性）\n");
            } else if (!file_exists(exe)) {
                // 关键：layout 里写着名字，不代表文件真的在。之前这里直接报"可执行文件: xxx"，
                // 会让人以为能启动，实际一跑就失败。
                printf("  !! layout 声明了 %s，但文件**不存在** —— 这就是启动不了的原因\n",
                       to_utf8(base_of(exe)).c_str());
                printf("     用 3) 核对包完整性 可以看到完整缺失清单\n");
            } else {
                printf("  可执行文件: %s\n", to_utf8(base_of(exe)).c_str());
            }
        } else {
            printf("\n附近没找到游戏包（需要含 layout_*.xml 的目录）\n");
            printf("可以把这个程序和三个 DLL 放进游戏目录后再双击\n");
        }
        usage();
        const int pick = interactive_menu();
        if (pick < 0) {
            // stdin 是 EOF（管道/自动化）：没给命令也没给选择，按"缺参数"返回 2，
            // 原来返回 0 会让"忘了写命令"被当成成功。
            printf("\n没有可交互的输入（stdin 已是 EOF）。请用命令行参数指定命令。\n");
            return 2;
        }
        if (pick == 0) {
            pause_if_interactive();
            return 0;
        }
        if (pick == 1) {
            if (pkg.empty()) {
                printf("没有可运行的包\n");
                pause_if_interactive();
                return 2;
            }
            // 直接运行（会先备份同名 DLL）。想只看不写就选命令行 run --dry-run。
            RunOptions ro;
            const int rc = cmd_run(pkg, ro);
            pause_if_interactive();
            return rc;
        }
        if (pick == 2) {
            if (pkg.empty()) {
                printf("没有可检查的包\n");
                pause_if_interactive();
                return 2;
            }
            const std::wstring exe = find_exe_in_dir(pkg);
            if (exe.empty()) {
                printf("包里没有 layout 声明的 .exe\n");
                pause_if_interactive();
                return 2;
            }
            const int rc = cmd_check(exe, L"");
            pause_if_interactive();
            return rc;
        }
        if (pick == 3) {
            const int rc = pkg.empty() ? 2 : cmd_verify(pkg);
            pause_if_interactive();
            return rc;
        }
        if (pick == 4) {
            const int rc = cmd_doctor(L"");
            pause_if_interactive();
            return rc;
        }
        if (pick == 5) {
            // 还原：撤销 run 写进包里的三个 DLL（有备份就改回原名）
            if (pkg.empty()) {
                printf("没有可还原的包\n");
                pause_if_interactive();
                return 2;
            }
            const int rc = cmd_restore(pkg, L"", false);
            pause_if_interactive();
            return rc;
        }
        if (pick == 6) {
            // 首次运行：四步走，最后一步真跑（想只预检就用命令行 firstrun --dry-run）
            if (pkg.empty()) {
                printf("没有可首次运行的包\n");
                pause_if_interactive();
                return 2;
            }
            RunOptions fro;
            const int rc = cmd_firstrun(pkg, fro);
            pause_if_interactive();
            return rc;
        }
        pause_if_interactive();
        return 2;
    }
    std::string cmd = argv[1];
    for (auto& c : cmd) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (cmd == "--version" || cmd == "-v") {
        printf("xbgtasix %s\n", kVersion);
        return 0;
    }
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        usage();
        return 0;
    }

    std::wstring dll_dir;
    bool force_flag = false;
    std::vector<std::string> pos;
    RunOptions ro;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if ((a == "--dll-dir" || a == "--log" || a == "--timeout") && i + 1 >= argc) {
            printf("%s 后面需要一个值\n", a.c_str());
            return 2;
        }
        if (a == "--dll-dir" && i + 1 < argc) {
            dll_dir = to_wide(argv[++i]);
        } else if (a == "--dry-run" || a == "--dryrun") {
            ro.dry_run = true;
        } else if (a == "--staging") {
            printf("--staging 已移除：它用 junction 指向原包，DLL 实际仍写进包里，"
                   "承诺的'原包不动'做不到。\n改用 --dry-run 只做预检（不写文件）。\n");
            return 2;
        } else if (a == "--log" && i + 1 < argc) {
            ro.log_path = to_wide(argv[++i]);
        } else if (a == "--force") {
            force_flag = true;
        } else if (a == "--stop") {
            ro.stop_mode = true;
        } else if (a == "--timeout" && i + 1 < argc) {
            // atoi 碰上超长数字是未定义行为；用 strtoull（Windows 的 long 只有 32 位，
            // 所以不能拿 unsigned long 接）并把上限夹到一天
            const char* s = argv[++i];
            char* endp = nullptr;
            errno = 0;
            const unsigned long long v = strtoull(s, &endp, 10);
            if (endp == s || *endp != '\0' || s[0] == '-' || errno == ERANGE || v == 0) {
                ro.timeout_sec = 180;
            } else if (v > 86400ull) {
                printf("--timeout 上限 86400 秒，按 86400 处理\n");
                ro.timeout_sec = 86400;
            } else {
                ro.timeout_sec = static_cast<int>(v);
            }
        } else if (!a.empty() && a[0] != '-') {
            pos.push_back(a);
        }
    }
    ro.dll_dir = dll_dir;

    // 显式命令（run/check/verify/doctor）一律不 pause：它们是从命令行/脚本里调的，
    // 卡在 getchar 上等回车只会让批处理挂死。只有无参数交互菜单结束时才等。
    if (cmd == "doctor") {
        return cmd_doctor(dll_dir);
    }
    if (cmd == "verify") {
        if (pos.empty()) { printf("verify 需要一个包目录\n"); return 2; }
        return cmd_verify(to_wide(pos[0]));
    }
    if (cmd == "check") {
        if (pos.empty()) { printf("check 需要一个 exe 路径\n"); return 2; }
        return cmd_check(to_wide(pos[0]), dll_dir);
    }
    if (cmd == "run") {
        if (pos.empty()) { printf("run 需要一个 exe 或包目录\n"); return 2; }
        return cmd_run(to_wide(pos[0]), ro);
    }
    if (cmd == "restore") {
        if (pos.empty()) { printf("restore 需要一个包目录或 exe 路径\n"); return 2; }
        return cmd_restore(to_wide(pos[0]), dll_dir, force_flag);
    }
    if (cmd == "firstrun") {
        if (pos.empty()) { printf("firstrun 需要一个包目录或 exe 路径\n"); return 2; }
        // ro 里已经带着 --dry-run/--stop/--timeout/--dll-dir/--log：一路传到底
        return cmd_firstrun(to_wide(pos[0]), ro);
    }

    printf("未知命令：%s\n\n", cmd.c_str());
    usage();
    return 2;
}
