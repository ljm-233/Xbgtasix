// xbgtasix.exe —— 转译层的"程序"入口
//
// 为什么需要它：三个 shim 是库，不是程序；tools/*.sh 要 bash、*.py 要 Python。
// 这个 exe 把"摆 DLL → 设环境 → 拉起 → 分类失败 → 给下一步"做成一个零依赖的命令行工具。
//
//   xbgtasix run    <exe|包目录>  拉起游戏并按退出码分类
//   xbgtasix check  <exe>         导入面 vs 我们的导出面（缺哪些名字）
//   xbgtasix verify <包目录>      按包自带 layout 清单核对完整性
//   xbgtasix doctor               环境自检（显卡/驱动/运行库/权限）
//
// 设计取向：只做这四件事。不加 UI、不加注入、不加配置系统。
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

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
const wchar_t* kShims[] = {L"xgameruntime.dll", L"xmem.dll", L"d3d12_xs.dll"};

// 找 shim 所在目录：优先 --dll-dir，其次自身目录
std::wstring resolve_dll_dir(const std::wstring& explicit_dir) {
    if (!explicit_dir.empty()) return explicit_dir;
    return self_dir();
}

// ---------------------------------------------------------------- verify
struct VerifyResult {
    int listed = 0;
    int present = 0;
    int unexpanded = 0;                     // 含通配符、静态展开不了的条目数
    std::vector<std::string> missing;
    std::vector<std::string> invalid;       // 归一出包目录（../、绝对路径、盘符）的坏条目
    std::vector<std::string> unexpanded_list;
    std::string layout_file;
};

// 从一段 XML 文本里取某个属性值
std::string attr_of(const std::string& tag, const char* name) {
    const std::string key = std::string(name) + "=\"";
    const size_t p = tag.find(key);
    if (p == std::string::npos) return {};
    const size_t b = p + key.size();
    const size_t e = tag.find('"', b);
    if (e == std::string::npos) return {};
    return tag.substr(b, e - b);
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
        const size_t close = text.find("/>", pos);
        if (close == std::string::npos) break;
        const std::string tag = text.substr(pos, close - pos + 2);
        pos = close + 2;

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
        } else if (file_exists(full)) {
            ++r.present;
        } else {
            r.missing.push_back(rel);
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
    say("清单 %d 项，存在 %d，缺失 %d", r.listed, r.present,
        static_cast<int>(r.missing.size()));
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
    if (r.missing.empty() && r.invalid.empty() && r.unexpanded == 0) {
        say("PASS：清单里的 %d 项全部存在", r.listed);
        return 0;
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
        our_exports[to_utf8(s)] = info.exports;
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

    // 2) 显卡
    {
        IDXGIFactory1* f = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void**>(&f))) && f) {
            int idx = 0;
            IDXGIAdapter1* a = nullptr;
            while (f->EnumAdapters1(static_cast<UINT>(idx), &a) != DXGI_ERROR_NOT_FOUND && a) {
                DXGI_ADAPTER_DESC1 d{};
                a->GetDesc1(&d);
                say("  GPU[%d]        %s（专用显存 %.1f GB）", idx, to_utf8(d.Description).c_str(),
                    d.DedicatedVideoMemory / 1024.0 / 1024 / 1024);
                a->Release();
                a = nullptr;
                ++idx;
            }
            if (idx == 0) say("  GPU           没枚举到适配器");
            f->Release();
        } else {
            say("  GPU           创建 DXGI 工厂失败");
        }
    }

    // 3) D3D12 能否建设备
    {
        ID3D12Device* dev = nullptr;
        const HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                                             reinterpret_cast<void**>(&dev));
        if (SUCCEEDED(hr) && dev) {
            say("  D3D12         可用（feature level 11_0 以上）");
            dev->Release();
        } else {
            say("  D3D12         不可用 hr=0x%08lx", static_cast<unsigned long>(hr));
            ++critical;
        }
    }

    // 4) VC 运行库
    {
        const wchar_t* dlls[] = {L"vcruntime140.dll", L"msvcp140.dll", L"vcruntime140_1.dll"};
        int have = 0;
        for (const wchar_t* d : dlls) {
            if (LoadLibraryW(d)) ++have;
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
        say("  shim          %d/3 在 %s", have, to_utf8(d).c_str());
        if (have != 3) {
            say("                -> 先 build.sh，或用 --dll-dir 指定");
            ++critical;
        }
    }

    // 6) 日志目录可写
    {
        wchar_t tmp[MAX_PATH] = {0};
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring probe = join(tmp, L"xbgtasix_write_probe.tmp");
        HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            DeleteFileW(probe.c_str());
            say("  可写         临时目录可写（日志能落盘）");
        } else {
            say("  可写         临时目录不可写，err=%lu", GetLastError());
            ++critical;
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
        const size_t close = text.find("/>", pos);
        if (close == std::string::npos) break;
        const std::string tag = text.substr(pos, close - pos + 2);
        pos = close + 2;

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

std::string classify(unsigned long code) {
    switch (code) {
        case 0: return "PASS";
        case 70: return "UNIMPL";
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
    say("shim          %d/3 在 %s", shims, to_utf8(dll_dir).c_str());
    if (shims != 3) {
        say("错误：三个 shim 不齐，先跑 build.sh");
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
    say("已放置        %d/3 个 DLL", copied);
    if (copied != 3) {
        say("错误：DLL 没放全，后面必然起不来");
        return 2;
    }

    // 日志路径：--log 优先，否则游戏目录 -> %LOCALAPPDATA% -> 临时目录
    std::wstring log_path = opt.log_path.empty() ? join(run_dir, L"xbgtasix.log") : opt.log_path;
    {
        HANDLE h = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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
    // 有配置就作为第一个参数传（本项目测试与工具都按这个约定取配置）
    const std::wstring cfg = join(run_dir, L"MicrosoftGame.Config");
    if (file_exists(cfg)) {
        cmdline += L" \"" + cfg + L"\" \"" + run_dir + L"\"";
    }
    const DWORD t0 = GetTickCount();
    const BOOL started = CreateProcessW(run_exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                                        CREATE_UNICODE_ENVIRONMENT, env.data(), run_dir.c_str(), &si,
                                        &pi);
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
        TerminateProcess(pi.hProcess, 124);
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
    const std::string kind = classify(code);
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
std::wstring find_package_near(const std::wstring& start) {
    std::wstring dir = abs_path(start);
    for (int depth = 0; depth < 4 && !dir.empty(); ++depth) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(join(dir, L"layout_*.xml").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            return dir;
        }
        const std::wstring up = dir_of(dir);
        if (up == dir) break;
        dir = up;
    }
    return {};
}

int interactive_menu() {
    printf("\n");
    printf("  1) 运行游戏        2) 检查导入面\n");
    printf("  3) 核对包完整性    4) 环境自检    0) 退出\n");
    printf("选择: ");
    fflush(stdout);
    char line[256] = {0};
    if (!fgets(line, sizeof(line), stdin)) return 0;
    switch (line[0]) {
        case '1': return 1;
        case '2': return 2;
        case '3': return 3;
        case '4': return 4;
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
    printf("  xbgtasix doctor\n");
    printf("  xbgtasix --version\n\n");
    printf("说明：\n");
    printf("  run     把三个 DLL 放到游戏旁边，拉起进程，按退出码分类并给出下一步\n");
    printf("  check   解析游戏的导入表，跟我们的导出面逐名字对表\n");
    printf("  verify  按包自带 layout 清单核对文件完整性\n");
    printf("  doctor  显卡/驱动/VC 运行库/写权限/交互桌面自检\n");
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
        pause_if_interactive();
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "--version" || cmd == "-v") {
        printf("xbgtasix %s\n", kVersion);
        return 0;
    }
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        usage();
        return 0;
    }

    std::wstring dll_dir;
    std::vector<std::string> pos;
    RunOptions ro;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
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

    printf("未知命令：%s\n\n", cmd.c_str());
    usage();
    return 2;
}
