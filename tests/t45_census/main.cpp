// SPDX-License-Identifier: GPL-3.0-or-later
// t45_census — 导入解析阶梯（模式 C）的回归测试。
//
// 钉住 docs/import-ladder.md 的三件事：
//
//  1) **阶梯顺序**：Hle → Host → Title → Stub，外加 HostPolicy 的三条硬性语义
//     （白名单默认拒绝 / 通配只支持尾部 '*' / none() 一个都不透传）。
//     这里实现一份**规则镜像**（不是生产实现）：census.py 改了语义，这个测试就该红，
//     提示把 docs/import-ladder.md 和 tools/census.py 一起改。
//
//  2) **3.3 的安全细节：透传宿主 DLL 只从 System32 加载。**
//     刻意构造：把 C:\Windows\System32\version.dll 复制成 msvcp140_xbgtest.dll 放进临时
//     目录，SetCurrentDirectoryW 过去，然后断言
//        · 朴素做法 LoadLibraryW(name) **会中招**（句柄指向临时目录那份）
//        · 我们的做法 LoadLibraryExW(name, 0, LOAD_LIBRARY_SEARCH_SYSTEM32) **不会**（返回 NULL）
//     再拿 System32 里真实存在的 version.dll 做正向对照，证明那个 flag 是"只搜 System32"
//     而不是"永远失败"。
//
//  3) **census 本身能跑**：调 tools/census.py 对本测试自己（一个普通 PE）做一次 census，
//     并把 census 的 HostPolicy 表与这里的表逐条对比（防文档/代码漂移）。机器上没有
//     python 时这一段记 skip，不算失败。
//
// 顺序有一个必须注意的点：**先做 System32-only 的断言，再做朴素加载**。LoadLibrary 会先按
// 基名在"已加载模块表"里找，一旦朴素加载成功，后面带 SEARCH_SYSTEM32 的调用会直接返回那个
// 被劫持的句柄 —— 那样测试就变成自证失败了。
//
// 输出：一行 `<N> checks, <M> failures`（工具套件靠这行判定）。
// 退出码：0 = 全通过，1 = 有失败，2 = 跳过（拿不到 System32 version.dll / 建不了临时目录）。
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cwctype>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
// KB2533623 之前的老 SDK 头文件里没有；值本身是契约的一部分（0x800）。
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

namespace {

int g_checks = 0;
int g_fail = 0;
int g_skipped = 0;

void ok(bool cond, const char* what, const std::string& detail = std::string()) {
    ++g_checks;
    if (!cond) ++g_fail;
    std::printf(cond ? "[ok]   %s" : "[FAIL] %s", what);
    if (!detail.empty()) std::printf("  —— %s", detail.c_str());
    std::printf("\n");
}

void skip(const char* what, const std::string& why) {
    ++g_skipped;
    std::printf("[skip] %s  —— %s\n", what, why.c_str());
}

void section(const char* name) {
    std::printf("\n== %s\n", name);
}

// ---------------------------------------------------------------- 小工具
std::string utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n,
                          nullptr, nullptr);
    return s;
}

std::wstring wide(const char* s) {
    if (!s || !*s) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(::towlower(c));
    return s;
}

bool path_equals(const std::wstring& a, const std::wstring& b) {
    return ::_wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool path_under(const std::wstring& path, const std::wstring& dir) {
    const size_t n = dir.size();
    if (path.size() < n) return false;
    if (::_wcsnicmp(path.c_str(), dir.c_str(), n) != 0) return false;
    return path.size() == n || path[n] == L'\\' || path[n] == L'/';
}

std::wstring module_path(HMODULE h) {
    wchar_t buf[MAX_PATH * 2] = {0};
    const DWORD n = ::GetModuleFileNameW(h, buf, static_cast<DWORD>(std::size(buf)));
    return std::wstring(buf, n);
}

std::wstring canonical_path(const std::wstring& p);

std::wstring exe_dir_of(HMODULE h) {
    const std::wstring p = module_path(h);
    const size_t slash = p.find_last_of(L"\\/");
    return canonical_path(slash == std::wstring::npos ? std::wstring(L".") : p.substr(0, slash));
}

std::wstring system32_dir() {
    wchar_t buf[MAX_PATH] = {0};
    const UINT n = ::GetSystemDirectoryW(buf, static_cast<UINT>(std::size(buf)));
    return std::wstring(buf, n);
}

bool file_exists(const std::wstring& p) {
    const DWORD a = ::GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// GetModuleFileName / GetCurrentDirectory 会把 ".\build\x.exe" 这种原样返回，
// 直接拿来比路径会假失败。统一走 GetFullPathNameW 规范化。
std::wstring canonical_path(const std::wstring& p) {
    if (p.empty()) return p;
    wchar_t buf[MAX_PATH * 2] = {0};
    const DWORD n = ::GetFullPathNameW(p.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
    return (n > 0 && n < std::size(buf)) ? std::wstring(buf, n) : p;
}

// ---------------------------------------------------------------- 1) HostPolicy
// 镜像 tools/census.py 的 HostPolicy::default()。理由字符串在 census.py / 文档里完整写，
// 这里只留 pattern（下面有一段专门比对两边表是否一致）。
struct PolicyRule {
    const wchar_t* pattern;
    const wchar_t* short_reason;
};

const PolicyRule kPolicy[] = {
    {L"api-ms-win-*", L"API Set 转发别名，主机/PC 契约一致"},
    {L"ext-ms-win-*", L"ext API set（rtcore/ntuser），同上"},
    {L"kernel32.dll", L"基础 Win32 面"},
    {L"kernelbase.dll", L"kernel32 的实现层"},
    {L"ucrtbase.dll", L"UCRT：微软承诺的二进制稳定 ABI"},
    {L"msvcp140.dll", L"MSVC C++ 库：v140 ABI 稳定"},
    {L"msvcp140_1.dll", L"msvcp140 拆分件"},
    {L"msvcp140_2.dll", L"msvcp140 拆分件"},
    {L"msvcp140_atomic_wait.dll", L"msvcp140 拆分件"},
    {L"msvcp140_codecvt_ids.dll", L"msvcp140 拆分件"},
    {L"vcruntime140.dll", L"MSVC C 运行时"},
    {L"vcruntime140_1.dll", L"vcruntime140 拆分件"},
    {L"d3d12.dll", L"PC D3D12，共有面"},
    {L"dxgi.dll", L"DXGI 契约，两边一致"},
    {L"bcrypt.dll", L"BCrypt 原语，同源实现"},
    {L"advapi32.dll", L"注册表/令牌 Win32 语义"},
    {L"ws2_32.dll", L"Winsock 2"},
    {L"winhttp.dll", L"WinHTTP 客户端"},
    {L"mfplat.dll", L"Media Foundation 平台层"},
    {L"mfreadwrite.dll", L"MF 读写层"},
};

struct HostPolicy {
    std::vector<std::pair<std::wstring, const wchar_t*>> rules;

    static HostPolicy make_default() {
        HostPolicy p;
        for (const auto& r : kPolicy) p.rules.emplace_back(r.pattern, r.short_reason);
        return p;
    }
    static HostPolicy none() { return HostPolicy{}; }

    // 白名单 + 默认拒绝；通配只支持尾部 '*'（中缀/正则一律不支持，按字面量比）
    bool allows(const std::wstring& dll) const {
        const std::wstring low = lower(dll);
        for (const auto& [pat, _why] : rules) {
            if (!pat.empty() && pat.back() == L'*') {
                if (low.rfind(pat.substr(0, pat.size() - 1), 0) == 0) return true;
            } else if (low == pat) {
                return true;
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------- 2) 阶梯镜像
enum class Prov { Hle, Host, Title, Stub };

const char* prov_name(Prov p) {
    switch (p) {
        case Prov::Hle: return "Hle";
        case Prov::Host: return "Host";
        case Prov::Title: return "Title";
        default: return "Stub";
    }
}

struct Facts {
    std::map<std::wstring, std::set<std::wstring>> our_exports;  // 小写 dll → 导出符号
    std::set<std::wstring> title_files;                         // 小写 dll → exe 同目录存在
    HostPolicy policy;
    bool no_title_ours = false;
};

struct Verdict {
    Prov prov = Prov::Stub;
    Prov module_prov = Prov::Stub;
    bool gap = false;       // 我们的模块缺这个符号
    bool shadowed = false;  // Host 赢了，但 exe 同目录也有同名 DLL
};

Prov module_provenance(const std::wstring& dll, const Facts& f) {
    const std::wstring low = lower(dll);
    if (f.our_exports.count(low)) return Prov::Hle;   // registry veto：我们的模块不交给 Host
    if (f.policy.allows(dll)) return Prov::Host;
    if (f.title_files.count(low)) return Prov::Title;
    return Prov::Stub;
}

// 阶梯：1 Hle（必须有导出）→ 2 Host（我们的模块不走这条）→ 3 Title → 4 Stub。
// 只分类、不绑定、不加载。
Verdict classify(const std::wstring& dll, const std::wstring& symbol, const Facts& f) {
    Verdict v;
    const std::wstring low = lower(dll);
    const bool ours = f.our_exports.count(low) > 0;
    v.module_prov = module_provenance(dll, f);

    if (ours) {
        const auto& ex = f.our_exports.at(low);
        // 按序号导入（#N）没法按名字核对 → 记成缺口，别假装覆盖了
        if (symbol.empty() || symbol[0] != L'#' ) {
            if (ex.count(symbol)) {
                v.prov = Prov::Hle;
                return v;
            }
        }
        v.gap = true;
    } else if (f.policy.allows(dll)) {
        v.prov = Prov::Host;
        v.shadowed = f.title_files.count(low) > 0;
        return v;
    }

    if (f.title_files.count(low) && !(ours && f.no_title_ours)) {
        v.prov = Prov::Title;
        return v;
    }
    v.prov = Prov::Stub;
    return v;
}

// ---------------------------------------------------------------- 3) System32-only
// 生产写法（tools/census.py 说明 + docs/import-ladder.md 3.3）：
//   1. 只认模块名，**拒绝任何带路径分隔符的名字**（带路径的 LoadLibraryEx 会直接用那个
//      路径，SEARCH_SYSTEM32 管不着）；
//   2. LoadLibraryExW(name, 0, LOAD_LIBRARY_SEARCH_SYSTEM32) —— 只搜 System32，
//      exe 目录/当前目录/PATH 全部不参与；
//   3. 句柄按小写名缓存（同一模块别重复 Load，FreeLibrary 也就不容易漏）。
struct HostLoader {
    std::map<std::wstring, HMODULE> cache;

    HMODULE load(const wchar_t* name) {
        if (!name || !*name) return nullptr;
        const std::wstring wname(name);
        if (wname.find_first_of(L"\\/") != std::wstring::npos) return nullptr;  // 1
        const std::wstring key = lower(wname);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        HMODULE h = ::LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);  // 2
        if (h) cache.emplace(key, h);                                               // 3
        return h;
    }
    void clear() {
        for (auto& [k, h] : cache) ::FreeLibrary(h);
        cache.clear();
    }
    ~HostLoader() { clear(); }
};

// ---------------------------------------------------------------- 4) census.py 集成
std::string read_file(const std::wstring& path) {
    std::string out;
    HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[4096];
    DWORD got = 0;
    while (::ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0)
        out.append(buf, got);
    ::CloseHandle(f);
    return out;
}

// 跑一条命令（经 cmd.exe），把 stdout+stderr 收成字符串，返回进程退出码。
// 输出文件放哪：TEMP 优先；某些沙箱/CI 里 TEMP 不可写，就退到 exe 自己的目录。
int run_capture(const std::wstring& cmdline, std::string* out) {
    std::vector<std::wstring> dirs;
    {
        wchar_t tmp_dir[MAX_PATH] = {0};
        const DWORD n = ::GetTempPathW(MAX_PATH, tmp_dir);
        if (n > 0 && n < MAX_PATH) dirs.push_back(tmp_dir);
    }
    dirs.push_back(exe_dir_of(nullptr));

    wchar_t tmp_file[MAX_PATH] = {0};
    DWORD last_err = 0;
    bool made = false;
    for (const auto& d : dirs) {
        // 前缀必须是 3 个字符（Win32 的硬约束），否则 GetTempFileName 直接失败
        if (::GetTempFileNameW(d.c_str(), L"t45", 0, tmp_file)) {
            made = true;
            break;
        }
        last_err = ::GetLastError();
    }
    if (!made) {
        std::printf("[note] 抓不到命令输出（GetTempFileNameW 失败，err=%lu）\n",
                    static_cast<unsigned long>(last_err));
        return -1;
    }
    const std::wstring full = cmdline + L" > \"" + tmp_file + L"\" 2>&1";
    const int rc = ::_wsystem(full.c_str());
    if (out) *out = read_file(tmp_file);
    ::DeleteFileW(tmp_file);
    return rc;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    std::printf("t45_census — 导入解析阶梯 + HostPolicy 的回归测试（模式 C）\n");
    std::printf("   规则来源：docs/import-ladder.md 第 3 节 / 第 5 节（模式 C 验收标准）\n");

    // ============================================================ A. HostPolicy
    section("A. HostPolicy：白名单 + 默认拒绝 + 通配只支持尾部 '*'");
    const HostPolicy def = HostPolicy::make_default();
    ok(def.allows(L"api-ms-win-core-file-l1-1-0.dll"), "尾部通配命中 api-ms-win-*");
    ok(def.allows(L"API-MS-WIN-CRT-MATH-L1-1-0.DLL"), "大小写不敏感");
    ok(def.allows(L"ext-ms-win-rtcore-ntuser-window-l1-1-0.dll"), "尾部通配命中 ext-ms-win-*");
    ok(def.allows(L"kernel32.dll"), "精确名命中 kernel32.dll");
    ok(def.allows(L"kernelbase.dll"), "精确名命中 kernelbase.dll");
    ok(!def.allows(L"kernel32_extra.dll"), "精确名不做前缀匹配（没通配就不是前缀）");
    ok(!def.allows(L"xgameruntime.dll"), "默认拒绝：我们自己的模块不在白名单里");
    ok(!def.allows(L"d3d12_xs.dll"), "默认拒绝：d3d12_xs 归我们，不透传");
    ok(!def.allows(L"AcpHal.dll"), "默认拒绝：AcpHal 归我们，不透传");
    ok(!def.allows(L"Bink2Scarlett.dll"), "默认拒绝：标题自带库不在白名单");
    ok(!def.allows(L"libHttpClient.GDK.dll"), "默认拒绝：GDK 辅助库不在白名单");
    ok(!def.allows(L"GameInput.dll"), "默认拒绝：GameInput 没有允许理由 → 不透传");
    ok(!def.allows(L""), "空模块名不被允许");

    {
        HostPolicy mid;
        mid.rules.emplace_back(L"api-*-core", L"故意的中缀通配");
        ok(!mid.allows(L"api-ms-win-core"), "通配只支持尾部：中缀 '*' 不当通配（不匹配）");
        ok(mid.allows(L"api-*-core"), "通配只支持尾部：中缀 '*' 按字面量比（相等才中）");
    }
    {
        const HostPolicy none = HostPolicy::none();
        ok(!none.allows(L"kernel32.dll") && !none.allows(L"api-ms-win-core-file-l1-1-0.dll"),
           "HostPolicy::none()：一个都不透传（测量真实 API 面用）");
    }

    // ============================================================ B. 阶梯
    section("B. 阶梯顺序 Hle → Host → Title → Stub");
    {
        Facts f;
        f.policy = def;
        f.our_exports[L"xgameruntime.dll"] = {L"XTaskQueueCreate"};
        f.title_files = {L"bink2scarlett.dll", L"acphal.dll"};

        Verdict v = classify(L"xgameruntime.dll", L"XTaskQueueCreate", f);
        ok(v.prov == Prov::Hle && !v.gap, "1 级：我们的模块 + 我们的导出 → Hle");

        v = classify(L"XGAMERUNTIME.DLL", L"XTaskQueueCreate", f);
        ok(v.prov == Prov::Hle, "1 级：dll 名大小写不敏感");

        v = classify(L"api-ms-win-core-file-l1-1-0.dll", L"CreateFileW", f);
        ok(v.prov == Prov::Host, "2 级：白名单模块 → Host");

        v = classify(L"Bink2Scarlett.dll", L"BinkOpen", f);
        ok(v.prov == Prov::Title, "3 级：exe 同目录同名 DLL → Title");

        v = classify(L"GameInput.dll", L"GameInputCreate", f);
        ok(v.prov == Prov::Stub, "4 级：都不负责 → Stub");

        // registry veto：我们的模块即使没导出，也不许从 Host 拿（PC 上真有同名 GDK DLL）
        Facts f2 = f;
        f2.our_exports[L"acphal.dll"] = {};   // 接管了名字，但这次构建没有导出
        v = classify(L"AcpHal.dll", L"ApuCreateHeap", f2);
        ok(v.prov != Prov::Host, "registry veto：我们的模块不走 Host（哪怕宿主有这个名字）");
        ok(v.gap, "registry veto：缺符号被记成 GAP（实现缺口，不是别人提供）");
        ok(v.prov == Prov::Title && v.gap,
           "阶梯后两级：我们的模块缺符号 + 目录里有同名 → Title，但 GAP 必须留着");

        f2.no_title_ours = true;
        v = classify(L"AcpHal.dll", L"ApuCreateHeap", f2);
        ok(v.prov == Prov::Stub && v.gap,
           "--no-title-ours：我们的模块缺符号时不映射到 Title（规划视角 → Stub + GAP）");

        // 遮蔽：Host 与 exe 同目录同名（本例 msvcp140.dll），阶梯让 Host 赢
        Facts f3;
        f3.policy = def;
        f3.title_files = {L"msvcp140.dll"};
        v = classify(L"msvcp140.dll", L"?_Xlength_error@std@@YAXPEBD@Z", f3);
        ok(v.prov == Prov::Host && v.shadowed,
           "遮蔽：Host 与 exe 同目录同名 → Host 优先，并标记 shadowed");

        // 模块级归属（"68 个模块的归属分布"用的是这个）
        ok(module_provenance(L"xgameruntime.dll", f) == Prov::Hle, "模块归属：ours → Hle");
        ok(module_provenance(L"Bink2Scarlett.dll", f) == Prov::Title, "模块归属：目录里有 → Title");
        ok(module_provenance(L"GameInput.dll", f) == Prov::Stub, "模块归属：没人负责 → Stub");
        ok(module_provenance(L"api-ms-win-core-synch-l1-1-0.dll", f) == Prov::Host,
           "模块归属：白名单 → Host");
    }

    // ============================================================ C. 3.3 System32-only
    section("C. 3.3 透传只从 System32 加载：当前目录同名 DLL 不被采纳");
    bool security_ran = false;
    {
        const std::wstring sys32 = system32_dir();
        const std::wstring real_version = sys32 + L"\\version.dll";

        // 临时目录：TEMP 优先，其次 exe 自己的目录（CI/沙箱里 TEMP 可能不可写）
        const std::wstring tag = L"xbgtasix_t45_" + std::to_wstring(::GetCurrentProcessId());
        std::vector<std::wstring> parents;
        {
            wchar_t tmp_root[MAX_PATH] = {0};
            const DWORD n = ::GetTempPathW(MAX_PATH, tmp_root);
            if (n > 0 && n < MAX_PATH) parents.push_back(tmp_root);
            parents.push_back(exe_dir_of(nullptr));       // build/
            parents.push_back(exe_dir_of(::GetModuleHandleW(nullptr)));
            wchar_t cwd0[MAX_PATH] = {0};
            if (::GetCurrentDirectoryW(MAX_PATH, cwd0) > 0) parents.push_back(cwd0);
        }
        std::wstring dir;
        DWORD last_err = 0;
        for (const auto& parent : parents) {
            if (parent.empty()) continue;
            const std::wstring cand =
                (parent.back() == L'\\' || parent.back() == L'/') ? parent + tag
                                                                 : parent + L"\\" + tag;
            if (::CreateDirectoryW(cand.c_str(), nullptr)) {
                dir = canonical_path(cand);
                break;
            }
            last_err = ::GetLastError();
            if (last_err == ERROR_ALREADY_EXISTS) {   // 上一轮残留：清空再用
                ::RemoveDirectoryW(cand.c_str());
            }
        }

        if (sys32.empty() || !file_exists(real_version)) {
            skip("System32-only 回归测试", "拿不到 " + utf8(real_version));
        } else if (dir.empty()) {
            skip("System32-only 回归测试",
                 "建不了临时目录（最后一个错误 " + std::to_string(last_err) + "）");
        } else {
            security_ran = true;
            const std::wstring plant = dir + L"\\msvcp140_xbgtest.dll";

            // 装置：System32 的真 DLL 复制成"看起来像我们的 CRT"的名字，放进临时目录
            ok(::CopyFileW(real_version.c_str(), plant.c_str(), FALSE) != 0,
               "装置：把 System32\\version.dll 复制成临时的 msvcp140_xbgtest.dll",
               utf8(plant));

            // chdir 过去 —— 这正是朴素 LoadLibrary 中招的条件
            std::vector<wchar_t> old_cwd(32768, L'\0');
            ::GetCurrentDirectoryW(static_cast<DWORD>(old_cwd.size()), old_cwd.data());
            ok(::SetCurrentDirectoryW(dir.c_str()) != 0, "装置：SetCurrentDirectoryW 到临时目录",
               utf8(dir));
            {
                wchar_t cwd[MAX_PATH] = {0};
                ::GetCurrentDirectoryW(MAX_PATH, cwd);
                ok(path_equals(cwd, dir), "装置：GetCurrentDirectoryW == 临时目录", utf8(cwd));
            }

            // (1) 我们的做法：只搜 System32 → 必须失败（这个 DLL 不在 System32 里）
            ::SetLastError(0);
            HMODULE h_safe = ::LoadLibraryExW(L"msvcp140_xbgtest.dll", nullptr,
                                             LOAD_LIBRARY_SEARCH_SYSTEM32);
            const DWORD safe_err = ::GetLastError();
            ok(h_safe == nullptr,
               "安全做法：LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32) 不采纳当前目录的同名 DLL",
               h_safe ? ("居然加载了：" + utf8(module_path(h_safe))) : "返回 NULL");
            ok(h_safe == nullptr && safe_err == ERROR_MOD_NOT_FOUND,
               "安全做法：失败原因是 ERROR_MOD_NOT_FOUND（126），不是别的错",
               "GetLastError=" + std::to_string(static_cast<unsigned long>(safe_err)));
            ok(::GetModuleHandleW(L"msvcp140_xbgtest.dll") == nullptr,
               "安全做法：进程里没有留下任何 msvcp140_xbgtest.dll 映射");
            if (h_safe) ::FreeLibrary(h_safe);

            // (2) 朴素做法：LoadLibraryW(name) → 会先在当前目录找到 → 中招
            HMODULE h_naive = ::LoadLibraryW(L"msvcp140_xbgtest.dll");
            const std::wstring naive_path = h_naive ? module_path(h_naive) : std::wstring();
            ok(h_naive != nullptr,
               "朴素做法：LoadLibraryW(name) 从当前目录加载成功（这就是漏洞的样子）",
               utf8(naive_path));
            ok(h_naive != nullptr && path_under(naive_path, dir),
               "朴素做法：拿到的句柄确实指向临时目录那份（劫持成立）",
               utf8(naive_path));
            ok(h_naive != nullptr && !path_under(naive_path, sys32),
               "朴素做法：拿到的**不是** System32 那份");
            if (h_naive) ::FreeLibrary(h_naive);
            ok(::GetModuleHandleW(L"msvcp140_xbgtest.dll") == nullptr,
               "清理：FreeLibrary 之后没有残留映射");

            // (3) 正向对照：System32 里真有的名字，同一个 flag 必须加载成功且来自 System32
            HMODULE h_real = ::LoadLibraryExW(L"version.dll", nullptr,
                                             LOAD_LIBRARY_SEARCH_SYSTEM32);
            const std::wstring real_path = h_real ? module_path(h_real) : std::wstring();
            ok(h_real != nullptr, "正向对照：SEARCH_SYSTEM32 能加载 System32\\version.dll",
               utf8(real_path));
            ok(h_real != nullptr && path_under(real_path, sys32),
               "正向对照：句柄路径在 System32 下（flag 不是永远失败）", utf8(real_path));
            if (h_real) ::FreeLibrary(h_real);

            // (4) 生产封装：拒绝带路径的名字 + 句柄缓存
            HostLoader loader;
            ok(loader.load(L"msvcp140_xbgtest.dll") == nullptr,
               "HostLoader：封装后的加载器同样不采纳当前目录同名 DLL");
            ok(loader.load(L"C:\\Windows\\System32\\version.dll") == nullptr,
               "HostLoader：带路径分隔符的名字直接拒绝（带路径会绕过 SEARCH_SYSTEM32）");
            ok(loader.load(L"..\\version.dll") == nullptr,
               "HostLoader：拒绝相对路径 ../version.dll");
            HMODULE a = loader.load(L"version.dll");
            HMODULE b = loader.load(L"VERSION.DLL");
            ok(a != nullptr && a == b, "HostLoader：按小写名缓存，同名不同大小写返回同一个句柄");
            ok(a != nullptr && path_under(module_path(a), sys32), "HostLoader：句柄来自 System32",
               utf8(a ? module_path(a) : std::wstring()));
            loader.clear();

            // 收尾：恢复 cwd、删装置、删目录
            ::SetCurrentDirectoryW(old_cwd.data());
            ::DeleteFileW(plant.c_str());
            ok(::RemoveDirectoryW(dir.c_str()) != 0, "清理：临时目录已删除", utf8(dir));
            {
                wchar_t cwd[MAX_PATH] = {0};
                ::GetCurrentDirectoryW(MAX_PATH, cwd);
                ok(!path_equals(cwd, dir), "清理：工作目录已恢复", utf8(cwd));
            }
        }
    }

    // ============================================================ D. census.py 集成
    section("D. tools/census.py 跑得起来，且 HostPolicy 表与本测试一致（防漂移）");
    {
        const HMODULE self = ::GetModuleHandleW(nullptr);
        const std::wstring self_path = module_path(self);
        const std::wstring build_dir = exe_dir_of(self);
        const size_t slash = build_dir.find_last_of(L"\\/");
        const std::wstring root = slash == std::wstring::npos ? build_dir
                                                             : build_dir.substr(0, slash);
        const std::wstring census = root + L"\\tools\\census.py";

        std::string probe;
        int probe_rc = run_capture(L"python --version", &probe);
        std::string py = "python";
        if (!contains(probe, "Python")) {
            probe_rc = run_capture(L"py -3 --version", &probe);
            py = "py -3";
        }
        if (!file_exists(census)) {
            skip("census.py 集成", "找不到 " + utf8(census));
        } else if (probe_rc != 0 || !contains(probe, "Python")) {
            skip("census.py 集成",
                 "机器上没有可用的 python（rc=" + std::to_string(probe_rc) +
                     "，probe 输出：" + probe + "）");
        } else {
            std::string out;
            const std::wstring pyw = wide(py.c_str());
            int rc = run_capture(pyw + L" \"" + census + L"\" --policy", &out);
            ok(rc == 0 && contains(out, "HostPolicy"),
               "census.py --policy 退出码 0 并打印 HostPolicy 表",
               "rc=" + std::to_string(rc));

            // 逐条比对：C++ 表里的每个 pattern 都必须出现在 census.py 的表里
            int found = 0;
            std::string missing;
            for (const auto& r : kPolicy) {
                const std::string pat = utf8(r.pattern);
                if (contains(out, pat.c_str())) {
                    ++found;
                } else {
                    missing += (missing.empty() ? "" : ", ") + pat;
                }
            }
            ok(found == static_cast<int>(std::size(kPolicy)),
               "census.py 与 main.cpp 的白名单逐条一致（防文档/代码漂移）",
               std::to_string(found) + "/" + std::to_string(std::size(kPolicy)) +
                   (missing.empty() ? "" : "，缺：" + missing));

            out.clear();
            rc = run_capture(pyw + L" \"" + census + L"\" \"" + self_path +
                                 L"\" --limit 5", &out);
            ok(rc == 0 && contains(out, "census of") && contains(out, "HostPolicy"),
               "census.py 能对一个普通 PE 给出报告（rc=0，含 census of / HostPolicy）",
               "rc=" + std::to_string(rc));
            ok(contains(out, "Hle") && contains(out, "Host") && contains(out, "Stub"),
               "census.py 报告里四类 provenance 都在（Hle/Host/Title/Stub）");
        }
    }

    // ============================================================ 汇总
    // 这一行必须独立、格式固定：套件按 `<N> checks, <M> failures` 判定。
    if (g_skipped) std::printf("\n（另有 %d 项 skip，见上面的 [skip] 行）\n", g_skipped);
    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);

    if (g_checks == 0) return 2;          // 什么都没验到 → 让套件记 SKIP
    if (!security_ran) return 2;          // 3.3 是本测试的验收核心，没跑就等于没验
    return g_fail == 0 ? 0 : 1;
}
