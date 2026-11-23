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
#include <cstdio>
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
    std::string out(n > 0 ? n - 1 : 0, 0);
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, 0);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
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
    std::vector<std::string> missing;
    std::string layout_file;
};

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
        // 退一步：任意 xml 里含 <FileGroup 的
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
    size_t pos = 0;
    while ((pos = text.find("Include=\"", pos)) != std::string::npos) {
        pos += 9;
        const size_t end = text.find('"', pos);
        if (end == std::string::npos) break;
        const std::string name = text.substr(pos, end - pos);
        pos = end;
        if (name.empty() || name.find('*') != std::string::npos) continue;
        ++r.listed;
        if (file_exists(join(dir, to_wide(name)))) {
            ++r.present;
        } else {
            r.missing.push_back(name);
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
    if (!r.missing.empty()) {
        say("");
        say("缺失清单（最多列 30 个）：");
        for (size_t i = 0; i < r.missing.size() && i < 30; ++i) {
            say("  - %s", r.missing[i].c_str());
        }
        if (r.missing.size() > 30) say("  ...（共 %zu 个）", r.missing.size());
    }
    return r.missing.empty() ? 0 : 1;
}

// ---------------------------------------------------------------- check
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
    std::map<std::string, std::vector<std::string>> our_exports;
    for (const wchar_t* s : kShims) {
        const std::wstring p = join(dll_dir, s);
        if (!file_exists(p)) {
            say("  ! 缺少 shim：%s（在 %s 里找）", to_utf8(s).c_str(), to_utf8(dll_dir).c_str());
            continue;
        }
        our_exports[to_utf8(s)] = pe::read_exports(p);
    }
    if (our_exports.empty()) {
        say("错误：一个 shim 都没找到，先用 build.sh 构建，或用 --dll-dir 指定目录");
        return 2;
    }

    int total_missing = 0;
    section("按模块对表");
    for (const auto& imp : target.imports) {
        auto it = our_exports.find(imp.module);
        const bool ours = it != our_exports.end();
        if (!ours) continue;   // 系统 DLL 不归我们管
        std::vector<std::string> missing;
        for (const auto& n : imp.names) {
            if (n.empty() || n[0] == '#') {   // 按序号导入：先记为缺失，人工看
                missing.push_back(n);
                continue;
            }
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

    section("其它导入模块（不需要我们提供）");
    for (const auto& imp : target.imports) {
        if (our_exports.find(imp.module) == our_exports.end()) {
            say("  %-24s %zu 个符号", imp.module.c_str(), imp.names.size());
        }
    }

    section("结论");
    if (total_missing == 0) {
        say("PASS：我们提供的三个模块覆盖了它要的全部符号");
        return 0;
    }
    say("FAIL：共缺 %d 个符号。先补这些，再谈跑起来", total_missing);
    return 1;
}

// ---------------------------------------------------------------- doctor
int cmd_doctor() {
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

    // 5) 三个 shim
    {
        const std::wstring d = self_dir();
        int have = 0;
        for (const wchar_t* s : kShims) {
            if (file_exists(join(d, s))) ++have;
        }
        say("  shim          %d/3 在本程序同目录（%s）", have, to_utf8(d).c_str());
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
    bool staging = false;
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
    size_t pos = 0;
    while ((pos = text.find("Include=\"", pos)) != std::string::npos) {
        pos += 9;
        const size_t end = text.find('"', pos);
        if (end == std::string::npos) break;
        std::string name = text.substr(pos, end - pos);
        pos = end;
        if (name.size() > 4) {
            const std::string low = [&] {
                std::string s = name;
                for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                return s;
            }();
            if (low.size() >= 4 && low.compare(low.size() - 4, 4, ".exe") == 0) {
                return join(dir, to_wide(name));
            }
        }
    }
    return {};
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
        return "缺依赖 DLL。把游戏包里同名 DLL 拷到 exe 同目录，再跑一次。";
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
            say("错误：目录里没有 layout 声明 .exe，也没找到可执行文件");
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
    if (opt.staging) {
        wchar_t tmp[MAX_PATH] = {0};
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring stage =
            join(tmp, L"xbgtasix_stage_" + std::to_wstring(GetCurrentProcessId()));
        // 用 junction 把整个游戏目录挂进来，保证资源相对路径成立
        std::wstring cmd = L"cmd.exe /c mklink /J \"" + stage + L"\" \"" + game_dir + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::wstring mut = cmd;
        const BOOL okj = CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &si, &pi);
        if (okj) {
            WaitForSingleObject(pi.hProcess, 15000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        if (dir_exists(stage)) {
            run_dir = stage;
            run_exe = join(stage, base_of(target));
            say("staging       %s（junction，原包未改动）", to_utf8(stage).c_str());
        } else {
            say("staging       失败（mklink 不可用），回退到直接放入");
        }
    }
    if (run_dir == game_dir) {
        say("放置方式      直接放入游戏目录（原目录会被写入 3 个 DLL）");
    }

    // 拷贝三个 shim
    int copied = 0;
    for (const wchar_t* s : kShims) {
        const std::wstring src = join(dll_dir, s);
        const std::wstring dst = join(run_dir, s);
        if (same_file(src, dst) || CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
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

    // 环境块：当前环境 + 覆盖项
    std::vector<wchar_t> env;
    {
        wchar_t* cur = GetEnvironmentStringsW();
        if (cur) {
            for (wchar_t* p = cur; *p;) {
                const size_t len = wcslen(p);
                env.insert(env.end(), p, p + len + 1);
                p += len + 1;
            }
            FreeEnvironmentStringsW(cur);
        }
        env.push_back(0);
    }
    auto set_env = [&](const std::wstring& kv) {
        // 已有同名项则跳过（简单起见不影响正确性：后面的覆盖前面的由系统取第一次出现？）
        // Windows 取最后一次出现，所以直接追加即可
        env.insert(env.end() - 1, kv.begin(), kv.end());
        env.push_back(0);
    };
    set_env(L"XBGTASIX_UNIMPL=" + std::wstring(opt.stop_mode ? L"stop" : L"log"));
    set_env(L"XBGTASIX_LOG=" + log_path);
    set_env(L"XBGTASIX_UI=immediate");
    set_env(L"XBGTASIX_PACKAGE_ROOT=" + run_dir);

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
        const DWORD e = GetLastError();
        say("启动失败      CreateProcess err=%lu", e);
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
        GetExitCodeProcess(pi.hProcess, &ec);
        code = ec;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    const DWORD elapsed = GetTickCount() - t0;

    // 读日志
    const std::string log_text = read_text(log_path);
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
    say("日志          %zu 行，UNIMPLEMENTED %d 条，FAIL %d 行", lines.size(), unimpl_total,
        fail_lines);

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

// ---------------------------------------------------------------- CLI
void usage() {
    printf("xbgtasix %s —— Xbox GDK 兼容层的启动器/诊断工具\n\n", kVersion);
    printf("用法：\n");
    printf("  xbgtasix run    <exe|包目录> [--staging] [--stop] [--timeout 秒]"
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
        usage();
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
        } else if (a == "--staging") {
            ro.staging = true;
        } else if (a == "--log" && i + 1 < argc) {
            ro.log_path = to_wide(argv[++i]);
        } else if (a == "--stop") {
            ro.stop_mode = true;
        } else if (a == "--timeout" && i + 1 < argc) {
            ro.timeout_sec = atoi(argv[++i]);
            if (ro.timeout_sec <= 0) ro.timeout_sec = 180;
        } else if (!a.empty() && a[0] != '-') {
            pos.push_back(a);
        }
    }
    ro.dll_dir = dll_dir;

    if (cmd == "doctor") return cmd_doctor();
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
