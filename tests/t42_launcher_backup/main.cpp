// SPDX-License-Identifier: GPL-3.0-or-later
// t42_launcher_backup — xbgtasix.exe 的备份 / 还原决策（黑盒）
//
// 为什么要有它：run 覆盖游戏目录里的同名 DLL 之前必须备份，否则一次坏构建会永久改坏包。
// 但"备份"这件事有两个历史事故：
//   1) 如果目标位置本来就是我们自己的产物（例如按 HANDOFF 用 bash cp 手工部署过），
//      旧实现会把我们的 DLL 存成 *.xbgtasix-backup，**真正的原版就从世界上消失了**；
//   2) 此后 restore 把那个 shim 搬回原位，还打印"原件已回到原位"——报告和事实相反。
//
// 本测试用 8 个假 DLL（6 个"原版" + 2 个带我们构建标记的"旧产物"）和一个假游戏驱动
// 启动器，断言：
//   A. 原版会被备份，且备份内容 == 原版内容
//   B. 带标记的目标**不会**产生备份（不能伪造原版）
//   C. restore 遇到带标记的备份时，明确报告"原件已丢失"，而不是"已还原"
//
// 不碰真实游戏包；所有文件都在 build/launcher_backup_test 下。
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int g_checks = 0;
int g_fail = 0;

void ok(bool cond, const char* what) {
    ++g_checks;
    if (!cond) ++g_fail;
    printf(cond ? "[ok]   %s\n" : "[FAIL] %s\n", what);
}

std::string read_all(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return {};
    std::string s;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

bool write_all(const std::string& p, const std::string& s) {
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return false;
    const bool ok_w = fwrite(s.data(), 1, s.size(), f) == s.size();
    fclose(f);
    return ok_w;
}

bool exists(const std::string& p) {
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// 跑一条命令并把 stdout+stderr 收成字符串（启动器是控制台程序，输出就是它的报告）
std::string run_capture(const std::string& cmd, const std::string& out_file) {
    const std::string full = cmd + " > \"" + out_file + "\" 2>&1";
    const int rc = system(full.c_str());
    (void)rc;
    return read_all(out_file);
}

const char* kShims[] = {"xgameruntime.dll", "xmem.dll", "d3d12_xs.dll", "AcpHal.dll",
                        "xapu.dll", "chad.dll", "xg_xs.dll", "XCRDAPI.dll"};
constexpr int kShimCount = 8;

}  // namespace

int main(int argc, char** argv) {
    // 被启动器当成"假游戏"拉起来时：立刻、安静地成功退出。
    // （退出码 0 是给启动器的输入，启动器会把它当作"游戏正常结束"）
    if (getenv("XBGTASIX_TEST_CHILD")) return 0;

    (void)argc;
    const std::string work = "launcher_backup_test";
    const std::string launcher = ".\\xbgtasix.exe";
    if (!exists(launcher)) {
        printf("[skip] 缺 %s —— 先跑 build.sh\n", launcher.c_str());
        return 2;
    }

    // 清场：上一轮的残留会让"备份是否存在"这类断言失去意义
    std::string cmd = "cmd /c rmdir /s /q \"" + work + "\" > nul 2>&1";
    if (system(cmd.c_str()) != 0) { /* 目录不存在时非 0，正常 */ }
    cmd = "cmd /c mkdir \"" + work + "\"";
    if (system(cmd.c_str()) != 0) {
        printf("[FAIL] 建不出工作目录 %s\n", work.c_str());
        return 1;
    }
    if (!CopyFileA(argv[0], (work + "\\fake_game.exe").c_str(), FALSE)) {
        printf("[FAIL] 复制假游戏失败 err=%lu\n", GetLastError());
        return 1;
    }

    // 两个位置模拟"我们自己的旧产物"（带构建标记），其余六个模拟"游戏自带原版"
    const bool ours_idx[kShimCount] = {false, true, true, false, false, false, false, false};
    for (int i = 0; i < kShimCount; ++i) {
        const std::string name = kShims[i];
        const std::string path = work + "\\" + name;
        const std::string body = ours_idx[i] ? ("xbgtasix old build " + name) : ("CONSOLE ORIGINAL " + name);
        if (!write_all(path, body)) {
            printf("[FAIL] 写不出 %s\n", path.c_str());
            return 1;
        }
    }

    // 启动器的子进程会继承这个变量 -> 假游戏直接退出
    SetEnvironmentVariableA("XBGTASIX_TEST_CHILD", "1");

    printf("== A/B: run 的备份决策 ==\n");
    const std::string out_run =
        run_capture(launcher + " run \"" + work + "\\fake_game.exe\"", work + "\\run.out");
    ok(has(out_run, "已放置"), "run 完成后报告了 DLL 放置结果");

    for (int i = 0; i < kShimCount; ++i) {
        const std::string name = kShims[i];
        const std::string bak = work + "\\" + name + ".xbgtasix-backup";
        if (ours_idx[i]) {
            ok(!exists(bak), (name + "：目标是我们自己的产物 -> 不产生备份").c_str());
        } else {
            const bool have = exists(bak);
            ok(have, (name + "：原版 -> 产生了备份").c_str());
            if (have) {
                ok(read_all(bak) == ("CONSOLE ORIGINAL " + name),
                   (name + "：备份内容 == 原版内容").c_str());
            }
        }
    }
    ok(has(out_run, "已经是我们自己的产物"), "报告里说明了'原版不在这里'");

    printf("== C: restore 面对其实是我们自己的备份 ==\n");
    // 把 xmem 的备份换成带标记的内容，模拟历史事故留下的状态
    ok(write_all(work + "\\xmem.dll.xbgtasix-backup", "xbgtasix older shim"),
       "把一个备份改写成'其实是我们自己的'状态");
    const std::string out_restore = run_capture(launcher + " restore \"" + work + "\"", work + "\\restore.out");
    ok(has(out_restore, "PASS"), "restore 正常结束（输出里有 PASS）");
    ok(has(out_restore, "原件已丢失"), "restore 明说'原件已丢失'，不再谎报已还原");
    // 原版应当回到原位（内容一致）；被污染的 xmem 位置现在是我们那份标记内容
    ok(read_all(work + "\\xgameruntime.dll") == "CONSOLE ORIGINAL xgameruntime.dll",
       "原版 xgameruntime.dll 已回到原位");
    ok(read_all(work + "\\xmem.dll") == "xbgtasix older shim",
       "被污染的 xmem 位置搬回的是那份非原件（事实如此，报告已说明）");

    printf("== 结论 ==\n");
    printf("%d checks, %d failures\n", g_checks, g_fail);
    if (g_fail == 0) {
        printf("PASS: 备份不伪造原版、restore 不谎报已还原\n");
        return 0;
    }
    printf("FAIL: %d 项断言不成立\n", g_fail);
    return 1;
}
