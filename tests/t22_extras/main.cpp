// t22_extras — 两个子代理新实现的冒烟测试
//
// 重点验证三件容易"写完了但没用"的事：
//   1) chunk 枚举真的回调了（不是返回 S_OK 却一条不给）
//   2) 安装监控的句柄真的能建、进度结构体真的被填（void 函数没有错误通道）
//   3) UI 的 *Async 用 XAsyncGetStatus(wait=true) 能立刻拿到 E_NOTIMPL，
//      而不是空转 5 秒后回 E_PENDING（那样引擎轮询就是永久挂死）
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "xbgtasix/trace.h"

struct Blk {
    void* queue;
    void* context;
    void (*callback)(Blk*);
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize(void);
HRESULT WINAPI XAsyncGetStatus(void* async_block, bool wait);
HRESULT WINAPI XPackageGetUserLocale(size_t size, char* locale);
HRESULT WINAPI XPackageGetWriteStats(void* stats);
// 签名逐个对照 XPackage.h:170 / :123（第一次我按记忆写错了参数顺序，
// 结果是 E_INVALIDARG + 回调一次都没触发）
HRESULT WINAPI XPackageEnumerateChunkAvailability(const char* package_id, int selector_type,
                                                  void* context, void* callback);
HRESULT WINAPI XPackageCreateInstallationMonitor(const char* package_id, unsigned selector_count,
                                                 void* selectors, unsigned interval_ms,
                                                 void* queue, void** monitor);
void WINAPI XPackageGetInstallationProgress(void* monitor, void* progress);
bool WINAPI XPackageUpdateInstallationMonitor(void* monitor);
void WINAPI XPackageCloseInstallationMonitorHandle(void* monitor);
HRESULT WINAPI XGameUiTextEntryOpen(const void* options, unsigned max_len, const char* initial, unsigned cursor,
                                    void** handle);
void WINAPI XGameUiTextEntryClose(void* handle);
// XGameUI.h:418 —— 7 个参数，中间还有 cursorIndex / imeClause*
HRESULT WINAPI XGameUiTextEntryGetState(void* handle, unsigned* change_type, unsigned* cursor,
                                        unsigned* ime_start, unsigned* ime_end, unsigned buffer_size,
                                        char* buffer);
HRESULT WINAPI XGameUiShowAchievementsAsync(void* async_block, void* user, unsigned title_id);
}

namespace {

int g_fail = 0;
int g_chunks = 0;

void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t22]   PASS  %-34s %s", what, detail);
    } else {
        xbgtasix::logf("[t22]   FAIL  %-34s %s", what, detail);
        ++g_fail;
    }
}

// XPackageChunkAvailabilityCallback：返回 bool，false 表示停止枚举
bool CALLBACK on_chunk(void* context, const void* chunk_id, int availability) {
    (void)context;
    (void)chunk_id;
    (void)availability;
    ++g_chunks;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) SetEnvironmentVariableA("XBGTASIX_CONFIG", argv[1]);
    if (argc > 2) SetEnvironmentVariableA("XBGTASIX_PACKAGE_ROOT", argv[2]);
    ok(SUCCEEDED(XGameRuntimeInitialize()), "XGameRuntimeInitialize");

    // ---- 1) 本地化与写统计 ----
    char locale[64] = {0};
    ok(SUCCEEDED(XPackageGetUserLocale(sizeof(locale), locale)) && locale[0] != 0,
       "GetUserLocale 填了非空值", locale);

    // XPackageWriteStats 的真实布局（XPackage.h:98-104）：4 个 uint64。
    // 以前传 unsigned char[64] 灌 0xFF 之后**从不读**，等于没验"结构体真的被填"。
    struct WriteStats {
        unsigned long long interval;
        unsigned long long budget;
        unsigned long long elapsed;
        unsigned long long bytesWritten;
    };
    WriteStats ws{};
    std::memset(&ws, 0xFF, sizeof(ws));
    ok(SUCCEEDED(XPackageGetWriteStats(&ws)), "GetWriteStats 返回 S_OK");
    // 期望值都是字面量（约定见实现：限速窗口 1000ms、不设上限 0、不做限速所以计数为 0）
    ok(ws.interval == 1000, "结构体被真正填写：interval == 1000ms（不是 0xFF 残值）");
    ok(ws.budget == 0 && ws.elapsed == 0 && ws.bytesWritten == 0,
       "结构体被真正填写：budget / elapsed / bytesWritten 都是 0");

    // ---- 2) chunk 枚举：必须真的回调 ----
    const HRESULT hr = XPackageEnumerateChunkAvailability("*", 0 /*selectorType*/,
                                                          nullptr,
                                                          reinterpret_cast<void*>(&on_chunk));
    xbgtasix::logf("[t22] chunk 枚举 hr=0x%08lx 回调 %d 次",
                   static_cast<unsigned long>(hr), g_chunks);
    ok(SUCCEEDED(hr), "EnumerateChunkAvailability 成功");
    ok(g_chunks >= 1, "至少枚举到 1 个 chunk（回调真的被调用）");

    // ---- 3) 安装监控 ----
    void* monitor = nullptr;
    ok(SUCCEEDED(XPackageCreateInstallationMonitor("*", 0, nullptr, 1000, nullptr, &monitor)) && monitor,
       "CreateInstallationMonitor 给出非空句柄");
    if (monitor) {
        // 结构体布局逐字对照 XPackage.h:63-70：三个 uint64 之后才是 launchable/completed。
        // 旧写法往 unsigned char[64] 里塞 0xCC 再断言 progress[0] == 1 || == 0，
        // 读到的其实是 totalBytes 的低字节，跟 completed 一点关系都没有（所以恒真）。
        struct InstallationProgress {
            unsigned long long totalBytes;
            unsigned long long installedBytes;
            unsigned long long launchBytes;
            bool launchable;
            bool completed;
        };
        InstallationProgress progress;
        std::memset(&progress, 0xCC, sizeof(progress));
        XPackageGetInstallationProgress(monitor, &progress);   // void，无错误通道
        // 注释一直说 completed 必须是 true，就断言 true（以前还允许 0）
        ok(progress.completed == true, "GetInstallationProgress 把 completed 填成 true（非 0xCC 残值）");
        ok(progress.totalBytes == 0 && progress.installedBytes == 0 && progress.launchBytes == 0,
           "壳实现不虚报安装字节数（三项都是 0）");
        ok(XPackageUpdateInstallationMonitor(monitor), "UpdateInstallationMonitor 返回 true");
        XPackageCloseInstallationMonitorHandle(monitor);
        ok(true, "CloseInstallationMonitorHandle 未崩溃");
    }

    // ---- 4) 非模态文本输入（壳） ----
    void* entry = nullptr;
    ok(SUCCEEDED(XGameUiTextEntryOpen(nullptr, 32, "abc", 0, &entry)) && entry,
       "TextEntryOpen 给出句柄");
    if (entry) {
        unsigned change = 0xFFFF, cursor = 0, ime_start = 0, ime_end = 0;
        char buf[8] = {0};
        const HRESULT sh = XGameUiTextEntryGetState(entry, &change, &cursor, &ime_start, &ime_end,
                                                    sizeof(buf), buf);
        xbgtasix::logf("[t22] TextEntryGetState hr=0x%08lx changeType=%u buf='%s'",
                       static_cast<unsigned long>(sh), change, buf);
        ok(SUCCEEDED(sh), "TextEntryGetState 成功");
        ok(change == 0, "changeType == None(0)");
        // 初值必须能读回来：Open 时传的 initialText 是 "abc"（字面量），
        // 以前只断言 sh 成功，缓冲区里是空串也照样 PASS。
        ok(std::strcmp(buf, "abc") == 0, "GetState 读回初值 \"abc\"", buf);
        XGameUiTextEntryClose(entry);
        ok(true, "TextEntryClose 未崩溃");
    }

    // ---- 5) UI 异步：必须立刻完成，不是 5 秒后 E_PENDING ----
    Blk b{};
    const ULONGLONG t0 = GetTickCount64();
    ok(SUCCEEDED(XGameUiShowAchievementsAsync(&b, nullptr, 0)), "ShowAchievementsAsync 提交");
    const HRESULT st = XAsyncGetStatus(&b, true);
    const ULONGLONG dt = GetTickCount64() - t0;
    xbgtasix::logf("[t22] Achievements 状态 hr=0x%08lx，耗时 %llu ms",
                   static_cast<unsigned long>(st), static_cast<unsigned long long>(dt));
    ok(st != E_PENDING, "异步已完成（不是 E_PENDING）");
    ok(dt < 1000, "完成耗时 < 1 秒（没有空转 5 秒）");

    xbgtasix::logf("[t22] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
