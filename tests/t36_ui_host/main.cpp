// t36_ui_host — UI 两种模式：立即完成 vs 交给宿主
//
// 这个测试验证"UI 可以被外面接管"这件事到底成立不成立：
//
//   模式 immediate（默认）：ShowMessageDialogAsync 立刻完成，结果 = 默认按钮。
//                          保证没人接管时游戏不会卡住。
//   模式 callback        ：注册 XGameUiSetUiCallbacks 后，Async 保持挂起
//                          （XAsyncGetStatus(wait=false) == E_PENDING），
//                          宿主调 SetMessageDialogUiResponse 才完成，且结果是宿主选的。
//
// 关键断言是"挂起"那条：如果 Async 立刻完成，就说明回调根本没被用上，
// 所谓"接管 UI"只是形式主义。
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xbgtasix/trace.h"

namespace {

struct Blk {
    void* queue;
    void* context;
    void (*callback)(Blk*);
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};

int g_fail = 0;
void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t36]   PASS  %-44s %s", what, detail);
    } else {
        xbgtasix::logf("[t36]   FAIL  %-44s %s", what, detail);
        ++g_fail;
    }
}

// ---- 宿主侧：模拟一个"UI 窗口" ----
struct Host {
    void* pending_handle = nullptr;
    std::string last_title;
    std::string last_content;
    int show_message_calls = 0;
    int show_text_calls = 0;
    std::string text_default;
};

Host g_host;

// XGameUiShowMessageDialogUiCallback 的真实签名（9 个参数）
void CALLBACK on_message(void* callbackHandle, void* queue, const char* title, const char* content,
                        const char* b1, const char* b2, const char* b3, unsigned defaultButton,
                        unsigned cancelButton, void* context) {
    (void)queue; (void)b1; (void)b2; (void)b3; (void)defaultButton; (void)cancelButton;
    (void)context;
    auto* host = &g_host;
    host->pending_handle = callbackHandle;
    host->last_title = title ? title : "";
    host->last_content = content ? content : "";
    ++host->show_message_calls;
    xbgtasix::logf("[t36]   宿主收到消息框: \"%s\" / \"%s\"", host->last_title.c_str(),
                   host->last_content.c_str());
}

void CALLBACK on_text_entry(void* callbackHandle, void* queue, const char* title,
                            const char* description, const char* defaultText, unsigned scope,
                            unsigned maxLen, void* context) {
    (void)queue; (void)title; (void)description; (void)scope; (void)maxLen; (void)context;
    g_host.pending_handle = callbackHandle;
    g_host.text_default = defaultText ? defaultText : "";
    ++g_host.show_text_calls;
    xbgtasix::logf("[t36]   宿主收到输入框，默认文本 \"%s\"", g_host.text_default.c_str());
}

// XGameUiUiCallbacks：9 个指针（context 在前）
struct UiCallbacks {
    void* context;
    void* showPlayerProfileCardCallback;
    void* showPlayerPickerCallback;
    void* showSendGameInviteCallback;
    void* showAchievementsCallback;
    void* showMultiplayerActivityGameInviteCallback;
    void* showMessageDialogCallback;
    void* showErrorDialogCallback;
    void* showTextEntryCallback;
};

}  // namespace

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize(void);
HRESULT WINAPI XAsyncGetStatus(void* async_block, bool wait);
HRESULT WINAPI XGameUiSetUiCallbacks(const UiCallbacks* callbacks, bool use_system_ui);
HRESULT WINAPI XGameUiShowMessageDialogAsync(void* async, const char* title, const char* content,
                                             const char* b1, const char* b2, const char* b3,
                                             unsigned default_button, unsigned cancel_button);
HRESULT WINAPI XGameUiShowMessageDialogResult(void* async, unsigned* button);
HRESULT WINAPI XGameUiSetMessageDialogUiResponse(void* handle, unsigned response);
HRESULT WINAPI XGameUiShowTextEntryAsync(void* async, const char* title, const char* desc,
                                         const char* default_text, unsigned scope, unsigned max_len);
HRESULT WINAPI XGameUiShowTextEntryResultSize(void* async, unsigned* size);
HRESULT WINAPI XGameUiShowTextEntryResult(void* async, unsigned size, char* text, unsigned* used);
HRESULT WINAPI XGameUiSetTextEntryUiResponse(void* handle, const char* response);
}

int main() {
    XGameRuntimeInitialize();

    // =====================================================================
    // 一、immediate 模式：没人接管时必须立刻完成
    // =====================================================================
    {
        SetEnvironmentVariableA("XBGTASIX_UI", "immediate");
        Blk b{};
        ok(SUCCEEDED(XGameUiShowMessageDialogAsync(&b, "T", "C", "OK", "No", nullptr, 1, 1)),
           "immediate: 提交消息框");
        ok(XAsyncGetStatus(&b, false) == S_OK, "immediate: 立刻完成（不是 E_PENDING）");
        unsigned btn = 99;
        ok(SUCCEEDED(XGameUiShowMessageDialogResult(&b, &btn)) && btn == 1,
           "immediate: 结果是默认按钮 1");
    }

    // =====================================================================
    // 二、callback 模式：挂起 -> 宿主回填 -> 完成
    // =====================================================================
    {
        UiCallbacks cbs{};
        cbs.context = &g_host;
        cbs.showMessageDialogCallback = reinterpret_cast<void*>(&on_message);
        cbs.showTextEntryCallback = reinterpret_cast<void*>(&on_text_entry);
        ok(SUCCEEDED(XGameUiSetUiCallbacks(&cbs, true)), "注册宿主回调");

        SetEnvironmentVariableA("XBGTASIX_UI", "callback");

        // ---- 消息框 ----
        Blk b{};
        ok(SUCCEEDED(XGameUiShowMessageDialogAsync(&b, "退出游戏?", "进度会丢失", "继续", "退出",
                                                   nullptr, 0, 1)),
           "callback: 提交消息框");
        ok(g_host.show_message_calls == 1, "callback: 宿主回调被调用 1 次");
        ok(g_host.last_title == "退出游戏?", "callback: 宿主拿到了标题", g_host.last_title.c_str());
        ok(XAsyncGetStatus(&b, false) == E_PENDING, "callback: 异步**挂起**（这就是接管成立）");

        // 宿主"用户"点了第二个按钮
        ok(SUCCEEDED(XGameUiSetMessageDialogUiResponse(g_host.pending_handle, 2)),
           "callback: 宿主回填按钮 2");
        ok(XAsyncGetStatus(&b, false) == S_OK, "callback: 回填后异步完成");
        unsigned btn = 99;
        ok(SUCCEEDED(XGameUiShowMessageDialogResult(&b, &btn)) && btn == 2,
           "callback: 结果 = 宿主选的 2（不是默认值）", "");

        // ---- 文本输入 ----
        g_host.pending_handle = nullptr;
        Blk t{};
        ok(SUCCEEDED(XGameUiShowTextEntryAsync(&t, "起名", "请输入", "默认名", 0, 32)),
           "callback: 提交输入框");
        ok(g_host.show_text_calls == 1 && g_host.text_default == "默认名",
           "callback: 宿主拿到默认文本");
        ok(XAsyncGetStatus(&t, false) == E_PENDING, "callback: 输入框也挂起");

        ok(SUCCEEDED(XGameUiSetTextEntryUiResponse(g_host.pending_handle, "玩家自己输入的名字")),
           "callback: 宿主回填文本");
        unsigned size = 0;
        ok(SUCCEEDED(XGameUiShowTextEntryResultSize(&t, &size)) && size > 0, "callback: 能取结果大小");
        std::vector<char> buf(size ? size : 1, 0);
        unsigned used = 0;
        ok(SUCCEEDED(XGameUiShowTextEntryResult(&t, size, buf.data(), &used)) &&
               std::string(buf.data()) == "玩家自己输入的名字",
           "callback: 结果是宿主输入的文本", buf.data());
    }

    // =====================================================================
    // 三、注销回调后回到 immediate
    // =====================================================================
    {
        ok(SUCCEEDED(XGameUiSetUiCallbacks(nullptr, true)), "注销宿主回调");
        Blk b{};
        XGameUiShowMessageDialogAsync(&b, "T", "C", "OK", nullptr, nullptr, 0, 0);
        ok(XAsyncGetStatus(&b, false) == S_OK, "注销后回到立即完成");
    }

    xbgtasix::logf("[t36] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
