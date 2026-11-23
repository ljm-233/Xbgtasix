// SPDX-License-Identifier: GPL-3.0-or-later
// t25_sys — XSystem / XDisplay / XThread 的"PC 真实语义"验证
//
// 与 t22_extras 的写法差异（有意为之）：这里**直接 include GDK 真头文件**，不手抄声明。
// 本模块要验的核心就是"签名逐字一致"——bool / enum class / size_t 任一处抄错，
// 导出表依然能对上（名字对了），但调用当场崩或读到垃圾，手抄声明恰好会掩盖这类错误。
//
// 重点验证四件容易"写完了但没用"的事：
//   1) XSystemGetConsoleId 的 *used 真的被写（否则调用方按栈垃圾长度 memcpy）
//   2) 设备 ID 同机连调两次完全一致（每次重算都掺随机数就会漂）
//   3) XSystemIsHandleValid 是**双向**判断：合法 true、关闭后 false（恒 true 是假绿）
//   4) 屏幕超时抑制的 Acquire/Close 是真实资源操作：嵌套计数、二次 Close 不许崩、
//      句柄跟踪回调真的被触发
#include <windows.h>

#include "XDisplay.h"
#include "XSystem.h"
#include "XAccessibility.h"
#include "XError.h"
#include "XLauncher.h"
#include "XThread.h"

#include "xbgtasix/trace.h"

#include <cstdio>
#include <cstring>
#include <string>

// 只用到这两个运行时入口；手写声明只为减少头文件依赖，签名与 XGameRuntime.h /
// XAsync.h 一致（HRESULT WINAPI，bool 走 C++ bool）。
extern "C" HRESULT WINAPI XGameRuntimeInitialize(void);

// XAsyncBlock 的逐字镜像（XAsync.h:39）：queue, context, callback, internal。
// 本模块目前 0 个 *Async，但下面的导出面探针要在"以后有人加了异步却不让它完成"时炸出来。
struct Blk {
    void* queue;
    void* context;
    void (*callback)(Blk*);
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};

namespace {

int g_fail = 0;

void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t25]   PASS  %-40s %s", what, detail);
    } else {
        xbgtasix::logf("[t25]   FAIL  %-40s %s", what, detail);
        ++g_fail;
    }
}

bool is_base64_44(const char* s) {
    if (!s || std::strlen(s) != 44) return false;
    static const char kT[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";
    for (size_t i = 0; i < 44; ++i) {
        // 32 字节 base64 = 43 个数据字符 + 1 个 '=' 补位
        if (i == 43) {
            if (s[i] != '=') return false;
            continue;
        }
        if (!std::strchr(kT, s[i]) || s[i] == '=') return false;
    }
    return true;
}

// XSystemHandleTrack 的回调（XSystem.h:113 的 typedef）。
// 注意签名是 void CALLBACK(...)，且第一个参数是 void*（XSystemHandle）。
struct TrackStats {
    int created = 0;
    int destroyed = 0;
    int wrong_type = 0;
};

void CALLBACK on_handle(XSystemHandle handle, XSystemHandleType type,
                        XSystemHandleCallbackReason reason, void* context) {
    auto* st = static_cast<TrackStats*>(context);
    if (!st) return;
    if (type != XSystemHandleType::DisplayTimeoutDeferral) ++st->wrong_type;
    if (!handle) ++st->wrong_type;
    if (reason == XSystemHandleCallbackReason::Created) ++st->created;
    if (reason == XSystemHandleCallbackReason::Destroyed) ++st->destroyed;
}

// 本模块三个头文件里没有任何 *Async（XSystem.h / XDisplay.h / XThread.h 逐行核对）。
// 为了不把"以后有人加了异步却没让它完成"漏掉，这里探一遍导出面：真出现就按统一契约
// 断言 XAsyncGetStatus(wait=true) != E_PENDING 且 < 1000ms（签名未知，故只报不调）。
int probe_async_exports() {
    static const char* kCandidates[] = {
        "XSystemGetConsoleIdAsync",
        "XSystemGetAppSpecificDeviceIdAsync",
        "XSystemGetXboxLiveSandboxIdAsync",
        "XDisplayAcquireTimeoutDeferralAsync",
        "XThreadSetTimeSensitiveAsync",
    };
    HMODULE mod = GetModuleHandleA("xgameruntime.dll");
    if (!mod) {
        xbgtasix::logf("[t25] 无法探测 *Async（测试未通过 xgameruntime.dll 加载，直接静态链接）");
        return -1;
    }
    int found = 0;
    for (const char* name : kCandidates) {
        if (GetProcAddress(mod, name)) {
            xbgtasix::logf("[t25] 发现异步导出 %s：新加的 *Async 必须让 block 完成", name);
            ++found;
        }
    }
    return found;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) SetEnvironmentVariableA("XBGTASIX_CONFIG", argv[1]);
    ok(SUCCEEDED(XGameRuntimeInitialize()), "XGameRuntimeInitialize");

    // ---- 1) 主机 ID：PC 上固定占位串，但 *used 必须真写 ----
    {
        char id[64];
        std::memset(id, 0xCC, sizeof(id));
        size_t used = 0xDEADBEEF;
        const HRESULT hr = XSystemGetConsoleId(sizeof(id), id, &used);
        char detail[96];
        std::snprintf(detail, sizeof(detail), "used=%llu id='%s'",
                      static_cast<unsigned long long>(used), SUCCEEDED(hr) ? id : "?");
        ok(SUCCEEDED(hr), "GetConsoleId 成功");
        ok(used == XSystemConsoleIdBytes, "consoleIdUsed == 39（含结尾 NUL）", detail);
        ok(used <= sizeof(id) && id[used - 1] == '\0', "缓冲区 used-1 处是 NUL");
        ok(id[0] != '\0', "占位串非空");

        // 缓冲区不足：必须报 E_NOT_SUFFICIENT_BUFFER，且 *used 仍是正确长度
        //（原实现若漏写 *used，调用方会按 0xDEADBEEF 去 memcpy）
        char small[8];
        std::memset(small, 0xCC, sizeof(small));
        size_t used2 = 0;
        const HRESULT hr2 = XSystemGetConsoleId(sizeof(small), small, &used2);
        std::snprintf(detail, sizeof(detail), "hr=0x%08lx used2=%llu",
                      static_cast<unsigned long>(hr2),
                      static_cast<unsigned long long>(used2));
        ok(hr2 == E_NOT_SUFFICIENT_BUFFER, "小缓冲 -> E_NOT_SUFFICIENT_BUFFER", detail);
        ok(used2 == XSystemConsoleIdBytes, "小缓冲时 used 仍写 39", detail);
    }

    // ---- 2) 应用专属设备 ID：卷序列号 + 进程名，连调两次必须完全相同 ----
    {
        char a[64] = {0};
        char b[64] = {0};
        size_t ua = 0;
        size_t ub = 0;
        const HRESULT h1 = XSystemGetAppSpecificDeviceId(sizeof(a), a, &ua);
        const HRESULT h2 = XSystemGetAppSpecificDeviceId(sizeof(b), b, &ub);
        ok(SUCCEEDED(h1) && SUCCEEDED(h2), "GetAppSpecificDeviceId 两次都成功");
        ok(ua == XSystemAppSpecificDeviceIdBytes && ub == XSystemAppSpecificDeviceIdBytes,
           "两次 used 都是 45");
        ok(std::strcmp(a, b) == 0, "同一 device id 连调两次完全相同", a);
        ok(is_base64_44(a), "是 44 字符 base64 + NUL（长度常量对得上）", a);
    }

    // ---- 3) Xbox Live 沙箱 ID：PC 零售包固定 RETAIL，长度在上限内 ----
    {
        char sandbox[32];
        std::memset(sandbox, 0xCC, sizeof(sandbox));
        size_t used = 0;
        const HRESULT hr = XSystemGetXboxLiveSandboxId(sizeof(sandbox), sandbox, &used);
        ok(SUCCEEDED(hr), "GetXboxLiveSandboxId 成功");
        ok(used >= 2 && used <= XSystemXboxLiveSandboxIdMaxBytes,
           "sandboxIdUsed 在 (1, 16] 之间", sandbox);
        ok(sandbox[used - 1] == '\0' && sandbox[0] != '\0', "非空且 NUL 结尾", sandbox);
    }

    // ---- 4) 句柄有效性：双向断言（恒 true / 恒 false 都过不了） ----
    // 取值顺序很关键：ok() 会写日志，而第一次 logf 会 CreateFile，可能复用刚关掉的
    // 句柄值；所以 before/after 都在任何日志之前取好，中间不创建任何句柄。
    {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool before = ev ? XSystemIsHandleValid(ev) : false;
        if (ev) CloseHandle(ev);
        const bool after = ev ? XSystemIsHandleValid(ev) : true;
        const bool null_h = XSystemIsHandleValid(nullptr);
        const bool bad_h = XSystemIsHandleValid(INVALID_HANDLE_VALUE);

        ok(ev != nullptr, "CreateEventW 造出一个真实内核句柄");
        ok(before, "合法句柄 -> IsHandleValid == true");
        ok(!after, "CloseHandle 之后同一句柄 -> false（双向断言）");
        ok(!null_h, "nullptr -> false");
        ok(!bad_h, "INVALID_HANDLE_VALUE -> false");
    }

    // ---- 5) 屏幕超时抑制：真实 SetThreadExecutionState + 嵌套计数 ----
    {
        TrackStats stats;
        ok(SUCCEEDED(XSystemHandleTrack(&on_handle, &stats)), "HandleTrack 注册回调");

        XDisplayTimeoutDeferralHandle d1 = nullptr;
        XDisplayTimeoutDeferralHandle d2 = nullptr;
        const HRESULT ha = XDisplayAcquireTimeoutDeferral(&d1);
        const HRESULT hb = XDisplayAcquireTimeoutDeferral(&d2);
        ok(SUCCEEDED(ha) && d1 != nullptr, "Acquire 给出非空句柄");
        ok(SUCCEEDED(hb) && d2 != nullptr && d2 != d1, "嵌套 Acquire 给出另一个句柄");
        ok(stats.created == 2, "Created 回调触发 2 次（跟踪是真的）");
        ok(stats.wrong_type == 0, "回调里的 type 是 DisplayTimeoutDeferral");

        XDisplayCloseTimeoutDeferralHandle(d1);
        ok(stats.destroyed == 1, "内层 Close -> Destroyed 1 次");
        XDisplayCloseTimeoutDeferralHandle(d2);
        ok(stats.destroyed == 2, "全部 Close -> Destroyed 2 次（计数归零才恢复超时）");

        XDisplayCloseTimeoutDeferralHandle(d2);   // 二次 Close：不许崩、不许多发回调
        ok(stats.destroyed == 2, "二次 Close 被忽略（不 double free）");
        XDisplayCloseTimeoutDeferralHandle(nullptr);
        ok(true, "Close(nullptr) 不崩溃");

        XDisplayTimeoutDeferralHandle bad = nullptr;
        ok(XDisplayAcquireTimeoutDeferral(nullptr) == E_INVALIDARG,
           "Acquire(nullptr) -> E_INVALIDARG");
        ok(XDisplayAcquireTimeoutDeferral(&bad) == S_OK && bad, "再 Acquire 仍然可用");
        XDisplayCloseTimeoutDeferralHandle(bad);
    }

    // ---- 6) XThread：PC 没有主机时间敏感线程概念 ----
    {
        ok(SUCCEEDED(XThreadSetTimeSensitive(true)), "SetTimeSensitive(true) -> S_OK");
        ok(!XThreadIsTimeSensitive(), "IsTimeSensitive() == false（不撒谎）");
        ok(SUCCEEDED(XThreadSetTimeSensitive(false)), "SetTimeSensitive(false) -> S_OK");
        XThreadAssertNotTimeSensitive();   // STDAPI_(void)：无返回值，调一次验不崩
        ok(true, "AssertNotTimeSensitive() 不崩溃");
    }

    // ---- 7) 异步面：本模块应为 0 个 *Async ----
    // 探针拿不到 DLL 导出表（静态链接）时"这一条什么都没验"，
    // 以前写 ok(true, "跳过…") —— 那是把未验证当成通过。现在记 FAIL 行，
    // 并让整个测试以 2（SKIP=有项未验证）退出，而不是 0（PASS）。
    bool async_unverified = false;
    {
        const int n = probe_async_exports();
        if (n >= 0) {
            ok(n == 0, "本模块导出 0 个 *Async（有则必须让 block 完成）");
        } else {
            ok(false, "异步面探针可用（静态链接时拿不到导出表）",
               "本条未验证 -> 整体按 SKIP 退出");
            async_unverified = true;
        }
    }


    // ---- XLaunch / XClosedCaption / XError（追加）----
    {
        // 空 URI 必须拒绝，而不是把空串交给 ShellExecute
        ok(XLaunchUri(nullptr, "") == E_INVALIDARG, "XLaunchUri(空串) = E_INVALIDARG");
        ok(XLaunchUri(nullptr, nullptr) == E_INVALIDARG, "XLaunchUri(nullptr) = E_INVALIDARG");

        ok(SUCCEEDED(XClosedCaptionSetEnabled(true)), "ClosedCaptionSetEnabled(true)");
        XClosedCaptionProperties props;
        std::memset(&props, 0xCC, sizeof(props));
        const HRESULT ccr = XClosedCaptionGetProperties(&props);
        ok(SUCCEEDED(ccr), "ClosedCaptionGetProperties");
        ok(props.Enabled == true, "属性里的 Enabled 被填写（非 0xCC 残值）");

        XErrorSetCallback(nullptr, nullptr);   // 不应崩溃
        XErrorSetOptions(XErrorOptions::None, XErrorOptions::None);
        ok(true, "XError 设置回调/选项不崩溃");
    }

    xbgtasix::logf("[t25] done failures=%d async_unverified=%d -> %s", g_fail,
                   async_unverified ? 1 : 0,
                   g_fail ? "FAIL" : (async_unverified ? "SKIP（异步面未验证）" : "PASS"));
    // 真正的逻辑失败优先（返回 1）；"探针不可用"那一条算 2（SKIP=有项未验证），
    // 不能把它混进 FAIL，也不能让 0 退出——那正是以前的假绿。
    if (g_fail - (async_unverified ? 1 : 0) > 0) return 1;
    if (async_unverified) return 2;
    return 0;
}
