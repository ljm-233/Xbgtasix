// SPDX-License-Identifier: GPL-3.0-or-later
// t39_host_tool —— 验证 xbgtasix.exe（启动器）的四条命令
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
        ok(contains(r.output, "已放置        3/3"), "三个 DLL 都放到位");
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

    xbgtasix::logf("[t39] done failures=%d probe_missing=%d -> %s", g_fail, probe_missing ? 1 : 0,
                   g_fail ? "FAIL" : (probe_missing ? "SKIP（探针缺失，部分用例未验证）" : "PASS"));
    if (g_fail) return 1;
    if (probe_missing) return 2;
    return 0;
}
