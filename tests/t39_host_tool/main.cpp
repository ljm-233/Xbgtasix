// SPDX-License-Identifier: GPL-3.0-or-later
// t39_host_tool —— 验证 xbgtasix.exe（启动器）的命令
//
// 用 C++ 写测试的理由（和用户要求一致）：项目里不引入 Python 依赖。
// 测试本身就是个 exe：起子进程、抓输出、对断言。
//
// 覆盖：
//   1. --version
//   2. check  ：对表（对 t0_hello，它只导入我们自己的符号）
//   3. verify ：按 layout 清单报告缺失
//   4. run    ：正常程序 -> PASS
//   5. run    ：未实现 API + --stop -> UNIMPL
//   6. run    ：目标不存在 -> 退出码 2
//   7. doctor ：输出各检查项
//   8. restore：有备份 -> 原件改回原名；无备份 -> 删掉我们放的 DLL
//   9. firstrun：目标不存在 -> 退出码 2
//  10. --help：列出 restore / firstrun
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xbgtasix/trace.h"

namespace {

int g_fail = 0;
void ok(bool cond, const char* what, const std::string& detail = "") {
    if (cond) {
        xbgtasix::logf("[t39]   PASS  %-46s %s", what, detail.c_str());
    } else {
        xbgtasix::logf("[t39]   FAIL  %-46s %s", what, detail.c_str());
        ++g_fail;
    }
}

std::string dir_of(const std::string& p) {
    const size_t pos = p.find_last_of("\\/");
    return pos == std::string::npos ? std::string(".") : p.substr(0, pos);
}

std::string self_dir() {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return dir_of(buf);
}

bool file_exists(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + "\\" + b;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// restore 的用例要造"原版 DLL / 我们放的 DLL / 备份"三种文件，内容必须能读回来核对，
// 所以这里给几个最小的小工具（只在 build 目录里用，不碰别处）
bool write_file(const std::string& p, const char* text) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return false;
    const size_t n = strlen(text);
    const bool ok = (n == 0) || (fwrite(text, 1, n, f) == n);
    fclose(f);
    return ok;
}

std::string read_file(const std::string& p) {
    std::string out;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

// 只删本用例造的那几个名字，再做一次非递归的 RemoveDirectory：
// 写错路径时最多"少删一个"，不会误伤 build 目录里的别的东西
void cleanup_restore_dir(const std::string& d) {
    const char* names[] = {"xmem.dll",
                           "xmem.dll.xbgtasix-backup",
                           "xgameruntime.dll",
                           "xgameruntime.dll.xbgtasix-backup",
                           "d3d12_xs.dll",
                           "d3d12_xs.dll.xbgtasix-backup"};
    for (const char* n : names) DeleteFileA(join(d, n).c_str());
    RemoveDirectoryA(d.c_str());
}

struct RunResult {
    unsigned long code = 0;
    std::string output;
};

// 起子进程并抓 stdout+stderr（重定向到同一个管道）
RunResult run_capture(const std::string& exe, const std::vector<std::string>& args,
                      const std::string& cwd) {
    RunResult r;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return r;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::string cmd = "\"" + exe + "\"";
    for (const auto& a : args) cmd += " \"" + a + "\"";

    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessA(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    if (!started) {
        CloseHandle(rd);
        CloseHandle(wr);
        return r;
    }
    CloseHandle(wr);

    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
        buf[got] = 0;
        r.output.append(buf, got);
    }
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    r.code = ec;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return r;
}

}  // namespace

int main() {
    const std::string dir = self_dir();
    const std::string host = join(dir, "xbgtasix.exe");
    if (!file_exists(host)) {
        xbgtasix::logf("[t39] 找不到 %s -> SKIP（先 build.sh）", host.c_str());
        return 2;
    }
    const std::string t0 = join(dir, "t0_hello.exe");
    const std::string probe_ok = join(dir, "probe_ok.exe");
    const std::string probe_un = join(dir, "probe_unimpl.exe");
    // 依赖的探针/被检查 exe 缺失 = 对应用例"什么都没验"。
    // 以前这些分支只打一行日志继续跑，最后照样 PASS；现在整体按 SKIP 退出。
    bool probe_missing = false;

    // ---- 1) --version ----
    {
        RunResult r = run_capture(host, {"--version"}, dir);
        ok(r.code == 0 && contains(r.output, "xbgtasix"), "--version 正常",
           "rc=" + std::to_string(r.code));
    }

    // ---- 2) check ----
    if (file_exists(t0)) {
        RunResult r = run_capture(host, {"check", t0, "--dll-dir", dir}, dir);
        ok(r.code == 0 && contains(r.output, "PASS"), "check 报告覆盖完整",
           "rc=" + std::to_string(r.code));
        ok(contains(r.output, "导入模块"), "check 输出了模块统计");
    } else {
        xbgtasix::logf("[t39] 缺 t0_hello.exe -> check 用例未验证（整体按 SKIP 退出）");
        probe_missing = true;
    }

    // ---- 3) verify（造一个只有 1/2 文件的假包）----
    {
        // 不用 %TEMP%：本项目进程在某些环境下对临时目录没有写权限（实测 err=5），
        // 用自己所在的 build 目录更可靠
        const std::string pkg = join(dir, "t39_pkg");
        CreateDirectoryA(pkg.c_str(), nullptr);
        const std::string layout = join(pkg, "layout_test.xml");
        FILE* f = fopen(layout.c_str(), "wb");
        if (f) {
            fprintf(f,
                    "<?xml version=\"1.0\"?>\n<Package>\n  <Chunk Id=\"1\">\n"
                    "    <FileGroup SourcePath=\".\\\" DestinationPath=\"\\\" Include=\"present.dll\" />\n"
                    "    <FileGroup SourcePath=\".\\\" DestinationPath=\"\\\" "
                    "Include=\"missing_file.exe\" />\n"
                    // 带 DestinationPath 的条目：文件在子目录里（真实包就是这样，
                    // 忽略它会把大量存在的文件误报成缺失）
                    "    <FileGroup SourcePath=\". \" DestinationPath=\"\\sub\\dir\\\" "
                    "Include=\"nested.dat\" />\n"
                    "  </Chunk>\n</Package>\n");
            fclose(f);
        }
        const std::string present = join(pkg, "present.dll");
        FILE* g = fopen(present.c_str(), "wb");
        if (g) { fputc('x', g); fclose(g); }
        // 子目录里的那个文件要真实存在
        CreateDirectoryA(join(pkg, "sub").c_str(), nullptr);
        CreateDirectoryA(join(pkg, "sub\\dir").c_str(), nullptr);
        const std::string nested = join(pkg, "sub\\dir\\nested.dat");
        FILE* nf = fopen(nested.c_str(), "wb");
        if (nf) { fputc('y', nf); fclose(nf); }

        RunResult r = run_capture(host, {"verify", pkg}, dir);
        ok(r.code == 1, "verify 发现缺失时退出码为 1", "rc=" + std::to_string(r.code));
        ok(contains(r.output, "missing_file.exe"), "verify 列出了缺失文件名");
        if (!contains(r.output, "清单 3 项")) {
            // 失败时把工具输出打出来，便于定位（也便于以后回归时一眼看懂）
            xbgtasix::logf("[t39] verify 实际输出：%s", r.output.c_str());
            FILE* dbg = fopen(layout.c_str(), "rb");
            if (dbg) {
                char buf[1024] = {0};
                const size_t n = fread(buf, 1, sizeof(buf) - 1, dbg);
                fclose(dbg);
                xbgtasix::logf("[t39] layout XML 内容（前 %zu 字节）：%s", n, buf);
            }
        }
        ok(contains(r.output, "清单 3 项") && contains(r.output, "缺失 1"),
           "verify 统计正确（3 项 / 缺 1，其中 1 项在子目录里）");
        ok(!contains(r.output, "nested.dat"), "子目录里的文件不被误报为缺失");
        DeleteFileA(layout.c_str());
        DeleteFileA(present.c_str());
        DeleteFileA(nested.c_str());
        RemoveDirectoryA(join(pkg, "sub\\dir").c_str());
        RemoveDirectoryA(join(pkg, "sub").c_str());
        RemoveDirectoryA(pkg.c_str());
    }

    // ---- 4) run 正常程序 ----
    if (file_exists(probe_ok)) {
        RunResult r = run_capture(host,
                                  {"run", probe_ok, "--timeout", "30", "--log",
                                   join(dir, "t39_tool_ok.log")},
                                  dir);
        ok(r.code == 0, "run 正常程序退出码 0", "rc=" + std::to_string(r.code));
        ok(contains(r.output, "分类          PASS"), "分类为 PASS");
        // shim 数量会随需求增长（3 -> 8），断言不能写死具体数字：
        // 只要求 n/n 形式，且 n >= 3
        ok(contains(r.output, "/"), "run 报告了放置数量");
        {
            const size_t p = r.output.find("已放置");
            bool okn = false;
            if (p != std::string::npos) {
                const size_t slash = r.output.find('/', p);
                if (slash != std::string::npos) {
                    const int n1 = std::atoi(r.output.c_str() + p + 9);
                    const int n2 = std::atoi(r.output.c_str() + slash + 1);
                    okn = (n1 == n2 && n1 >= 3);
                }
            }
            ok(okn, "所有 shim 都放到位（n/n 且 n>=3）");
        }
    } else {
        // 以前只打一行日志就继续，于是"探针缺失"的测试照样 PASS
        xbgtasix::logf("[t39] 缺 probe_ok.exe -> 该用例未验证（整体按 SKIP 退出）");
        probe_missing = true;
    }

    // ---- 5) run 未实现 + --stop ----
    if (file_exists(probe_un)) {
        // 指定独立日志：测试进程自己也占着 build\xbgtasix.log，
        // 不分开的话工具的 CREATE_ALWAYS 会撞共享冲突，读不到子进程日志
        RunResult r = run_capture(host,
                                  {"run", probe_un, "--stop", "--timeout", "30", "--log",
                                   join(dir, "t39_tool.log")},
                                  dir);
        ok(r.code == 1, "run 未实现时退出码 1", "rc=" + std::to_string(r.code));
        ok(contains(r.output, "分类          UNIMPL"), "分类为 UNIMPL");
        ok(contains(r.output, "XLaunchRestartOnCrash"), "列出撞到的 API 名字");
        ok(contains(r.output, "频次"), "给出未实现频次表");
    } else {
        xbgtasix::logf("[t39] 缺 probe_unimpl.exe -> 该用例未验证（整体按 SKIP 退出）");
        probe_missing = true;
    }

    // ---- 6) run 目标不存在 ----
    {
        RunResult r = run_capture(host, {"run", "Z:\\definitely_not_here.exe"}, dir);
        ok(r.code == 2, "目标不存在时退出码 2", "rc=" + std::to_string(r.code));
        ok(contains(r.output, "找不到"), "给出了明确错误信息");
    }

    // ---- 7) doctor ----
    {
        RunResult r = run_capture(host, {"doctor"}, dir);
        // 以前是 `r.code == 0 || r.code == 1` —— 只要不崩就算过，等于没验结论。
        // 现在要求退出码与输出自相印证：
        //   0 -> 结论行是 "PASS："，且没有失败项
        //   1 -> 结论行是 "FAIL："，且带失败项标记（"->"/"[!]"）
        // 本机因 %TEMP% 不可写返回 1，属于后者。
        const bool has_fail_text = contains(r.output, "FAIL：") && contains(r.output, "项需要处理");
        const bool has_marker = contains(r.output, "->") || contains(r.output, "[!]");
        const bool has_pass_text = contains(r.output, "PASS：");
        ok((r.code == 0 && has_pass_text && !has_fail_text) ||
               (r.code == 1 && has_fail_text && has_marker),
           "doctor 退出码与结论标记一致（0=PASS / 1=有失败项）", "rc=" + std::to_string(r.code));
        const char* keys[] = {"OS", "GPU", "D3D12", "VC 运行库", "shim", "交互桌面"};
        for (const char* k : keys) {
            ok(contains(r.output, k), (std::string("doctor 输出了「") + k + "」").c_str());
        }
    }

    // ---- 8) restore：有备份 -> 把原件改回原名 ----
    {
        // 同样不用 %TEMP%：本项目进程对临时目录没有写权限（实测 err=5）
        const std::string g = join(dir, "t39_restore_bak");
        CreateDirectoryA(g.c_str(), nullptr);
        write_file(join(g, "xmem.dll"), "OURS");
        write_file(join(g, "xmem.dll.xbgtasix-backup"), "ORIGINAL-DLL");

        RunResult r = run_capture(host, {"restore", g}, dir);
        const std::string got = read_file(join(g, "xmem.dll"));
        ok(r.code == 0, "restore 还原备份时退出码 0", "rc=" + std::to_string(r.code));
        ok(got == "ORIGINAL-DLL", "restore 把原件改回了原名", "xmem.dll=" + got);
        ok(!file_exists(join(g, "xmem.dll.xbgtasix-backup")), "restore 用掉了备份文件");
        ok(contains(r.output, "已还原"), "restore 报告「已还原」");
        if (!contains(r.output, "已还原")) {
            xbgtasix::logf("[t39] restore 实际输出：%s", r.output.c_str());
        }
        cleanup_restore_dir(g);
    }

    // ---- 9) restore：只有我们放的 DLL、没有备份 -> 直接删 ----
    {
        const std::string g = join(dir, "t39_restore_rm");
        CreateDirectoryA(g.c_str(), nullptr);
        write_file(join(g, "xmem.dll"), "OURS");

        RunResult r = run_capture(host, {"restore", g}, dir);
        ok(r.code == 0, "restore 移除无备份 DLL 时退出码 0", "rc=" + std::to_string(r.code));
        // 新语义：没有备份、内容又不像我们的 shim 时**不能删**（可能是游戏自带原版）
        ok(file_exists(join(g, "xmem.dll")), "restore 不删内容不明的 DLL（防误删原版）");
        ok(contains(r.output, "跳过"), "restore 报告「跳过」并说明原因");
        ok(contains(r.output, "--force"), "restore 提示可用 --force 强制");
        cleanup_restore_dir(g);
    }

    // ---- 9b) 无备份、但内容确实是我们的 shim -> 应当移除 ----
    {
        const std::string g = join(dir, "t39_restore_ours");
        cleanup_restore_dir(g);
        CreateDirectoryA(g.c_str(), nullptr);
        ok(CopyFileA(join(dir, "xmem.dll").c_str(), join(g, "xmem.dll").c_str(), FALSE) != 0,
           "准备：拷贝真实 shim 作为「我们放的那份」");
        RunResult r = run_capture(host, {"restore", g}, dir);
        ok(r.code == 0, "restore(内容一致) 退出码 0", "rc=" + std::to_string(r.code));
        ok(!file_exists(join(g, "xmem.dll")), "restore 删掉了内容一致的 DLL");
        ok(contains(r.output, "已移除"), "restore 报告「已移除」");
        ok(contains(r.output, "确认是我们放的副本"), "restore 说明了判定依据");
        cleanup_restore_dir(g);
    }

    // ---- 10) firstrun --dry-run 对不存在的 exe -> 硬失败，退出码 2 ----
    {
        RunResult r =
            run_capture(host, {"firstrun", "Z:\\definitely_not_here.exe", "--dry-run"}, dir);
        ok(r.code == 2, "firstrun 目标不存在时退出码 2", "rc=" + std::to_string(r.code));
        ok(contains(r.output, "找不到"), "firstrun 给出了明确的「找不到」");
    }

    // ---- 11) --help 里必须能看见这两个新命令 ----
    {
        RunResult r = run_capture(host, {"--help"}, dir);
        ok(r.code == 0, "--help 退出码 0", "rc=" + std::to_string(r.code));
        ok(contains(r.output, "restore"), "--help 列出了 restore");
        ok(contains(r.output, "firstrun"), "--help 列出了 firstrun");
    }

    xbgtasix::logf("[t39] done failures=%d probe_missing=%d -> %s", g_fail, probe_missing ? 1 : 0,
                   g_fail ? "FAIL" : (probe_missing ? "SKIP（探针缺失，部分用例未验证）" : "PASS"));
    if (g_fail) return 1;
    if (probe_missing) return 2;
    return 0;
}
