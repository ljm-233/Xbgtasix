// SPDX-License-Identifier: GPL-3.0-or-later
// t37_system_ui — 系统侧 UI：游戏没注册回调时，由我们真的开窗画面板
//
// 这是"第二个角色"（docs/ui-references.md）：
//   标题自绘（XGameUiSetUiCallbacks）  == t36 验的那条
//   系统侧绘制（我们开窗）             == 本测试
//
// 验证链：
//   1) XBGTASIX_UI=system 且没人注册回调 -> ShowMessageDialogAsync 不立刻完成（E_PENDING）
//   2) 窗口线程画一帧（D3D12 flip 交换链）+ 原生按钮
//   3) 用 XBGTASIX_UI_AUTORESPONSE 脚本化"用户点击"
//   4) 异步完成，结果 = 脚本化的那个按钮
//
// 没有交互桌面（CI 的服务会话）时降级为立即完成——测试按 SKIP 处理，不算失败。
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "xbgtasix/trace.h"

namespace {
int g_fail = 0;
void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t37]   PASS  %-46s %s", what, detail);
    } else {
        xbgtasix::logf("[t37]   FAIL  %-46s %s", what, detail);
        ++g_fail;
    }
}

struct Blk {
    void* queue;
    void* context;
    void (*callback)(Blk*);
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};

// 当前会话有没有"可见桌面"。这是区分"逻辑坏了"和"环境不支持"的唯一依据：
//   - 有桌面：系统侧 UI 必须真的开窗（异步必须挂起），不挂起就是 FAIL
//   - 无桌面（服务会话）：允许降级成立即完成，那是环境属性 -> SKIP
bool has_interactive_desktop() {
    const HWINSTA ws = GetProcessWindowStation();
    if (!ws) return false;
    USEROBJECTFLAGS uof{};
    DWORD need = 0;
    if (!GetUserObjectInformationA(ws, UOI_FLAGS, &uof, sizeof(uof), &need)) return false;
    return (uof.dwFlags & WSF_VISIBLE) != 0;
}

}  // namespace

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize(void);
HRESULT WINAPI XAsyncGetStatus(void* async_block, bool wait);
HRESULT WINAPI XGameUiShowMessageDialogAsync(void* async, const char* title, const char* content,
                                             const char* b1, const char* b2, const char* b3,
                                             unsigned default_button, unsigned cancel_button);
HRESULT WINAPI XGameUiShowMessageDialogResult(void* async, unsigned* button);
HRESULT WINAPI XGameUiShowTextEntryAsync(void* async, const char* title, const char* desc,
                                         const char* default_text, unsigned scope, unsigned max_len);
HRESULT WINAPI XGameUiShowTextEntryResultSize(void* async, unsigned* size);
HRESULT WINAPI XGameUiShowTextEntryResult(void* async, unsigned size, char* text, unsigned* used);
}

int main() {
    XGameRuntimeInitialize();

    // 环境：系统侧 UI + 脚本化回答（模拟"用户点了第二个按钮"）
    SetEnvironmentVariableA("XBGTASIX_UI", "system");
    SetEnvironmentVariableA("XBGTASIX_UI_AUTORESPONSE", "second");

    // ---- 1) 消息框 ----
    {
        Blk b{};
        const HRESULT hr = XGameUiShowMessageDialogAsync(
            &b, "系统侧 UI 测试", "这个面板是本项目自己开的窗口画的", "继续", "取消", nullptr, 0, 1);
        ok(SUCCEEDED(hr), "提交消息框（系统侧接管）");

        // 关键：不能立刻完成，否则说明根本没开窗，还是"假装画了"
        const HRESULT st0 = XAsyncGetStatus(&b, false);
        const bool desktop = has_interactive_desktop();
        if (st0 != E_PENDING && !desktop) {
            // 无交互桌面（CI 的服务会话）时会降级成立即完成 —— 那是环境属性，记 SKIP
            xbgtasix::logf("[t37] 系统侧 UI 在当前会话不可用（无可见桌面，降级成立即完成）-> SKIP");
            return 2;
        }
        // 以前这里是 ok(true)：只要前面没 return，就无条件记 PASS，核心断言被吞掉了。
        // 现在：有桌面却拿到非 E_PENDING，就是系统侧 UI 没接管 -> FAIL(1)
        ok(st0 == E_PENDING, "提交后异步挂起（证明真的交给窗口了）",
           st0 == E_PENDING ? "" : "有交互桌面却没有挂起 -> 系统侧 UI 没生效");
        if (st0 != E_PENDING) return 1;

        // 窗口在后台线程跑，等它画完一帧并"被点击"
        bool done = false;
        for (int i = 0; i < 5000; ++i) {
            if (XAsyncGetStatus(&b, false) != E_PENDING) { done = true; break; }
            Sleep(2);
        }
        ok(done, "窗口流程结束后异步完成");
        unsigned btn = 99;
        const HRESULT rr = XGameUiShowMessageDialogResult(&b, &btn);
        ok(SUCCEEDED(rr) && btn == 1, "结果是脚本化的'取消'(1)，不是默认值 0",
           rr == S_OK ? (btn == 1 ? "" : "结果等于默认值 -> 系统 UI 没生效") : "取结果失败");
    }

    // ---- 2) 文本输入 ----
    {
        // 中文必须用宽字符版：SetEnvironmentVariableA 对 UTF-8 中文直接失败（实测返回 0）
        const BOOL setok = SetEnvironmentVariableW(L"XBGTASIX_UI_AUTORESPONSE", L"玩家输入的内容");
        ok(setok != 0, "设置中文自动回答（宽字符版）");
        Blk t{};
        ok(SUCCEEDED(XGameUiShowTextEntryAsync(&t, "起名", "系统侧输入框", "默认", 0, 64)),
           "提交输入框（系统侧接管）");
        const HRESULT st0 = XAsyncGetStatus(&t, false);
        ok(st0 == E_PENDING, "输入框也挂起");

        bool done = false;
        for (int i = 0; i < 5000; ++i) {
            if (XAsyncGetStatus(&t, false) != E_PENDING) { done = true; break; }
            Sleep(2);
        }
        ok(done, "输入框流程结束后异步完成");
        unsigned size = 0;
        ok(SUCCEEDED(XGameUiShowTextEntryResultSize(&t, &size)) && size > 0, "能取结果大小");
        char buf[128] = {0};
        unsigned used = 0;
        ok(SUCCEEDED(XGameUiShowTextEntryResult(&t, size, buf, &used)) &&
               std::string(buf) == "玩家输入的内容",
           "结果 = 脚本化文本", buf);
    }

    // ---- 3) 切回 immediate，确认默认行为没被破坏 ----
    {
        SetEnvironmentVariableA("XBGTASIX_UI", "immediate");
        Blk b{};
        XGameUiShowMessageDialogAsync(&b, "T", "C", "A", "B", nullptr, 1, 1);
        ok(XAsyncGetStatus(&b, false) == S_OK, "immediate 模式仍然立刻完成");
        unsigned btn = 99;
        XGameUiShowMessageDialogResult(&b, &btn);
        ok(btn == 1, "immediate 模式结果仍是默认按钮 1");
    }

    xbgtasix::logf("[t37] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
