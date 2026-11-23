// SPDX-License-Identifier: GPL-3.0-or-later
// ui_extra.cpp —— XGameUI 剩余面板（由子代理填写）
// 约定：只在本文件实现，导出名写进 exports_ui.txt。
//
// 设计基线：这些面板在桌面上没有对应 UI，一律"立即完成 + 记日志"，
// 绝不留下一个永不完成的 XAsyncBlock（本项目 XAsyncGetStatus(wait=true) 会空转
// 5000ms 再返回 E_PENDING，引擎轮询就等于永久挂死）。
#include <windows.h>

#include "XGameUI.h"

#include "xbgtasix/trace.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------------------
// 非模态文本输入的假句柄
// 头文件只前置声明了 struct XGameUiTextEntry（XGameUI.h:374），真实布局由本文件定义。
// 句柄合法性不靠 magic（读已释放内存仍是 UB：二次 Close 会 double free 崩堆），
// 而是靠下面的存活句柄表 g_te_live —— 二次 Close / 用已关闭句柄都会被挡成无害返回。
// ---------------------------------------------------------------------------
struct XGameUiTextEntry {
    uint32_t max_length;
    uint32_t cursor_index;
    XGameUiTextEntryOptions options;
    std::string text;
};

namespace {

// 存活句柄表：句柄是关键资源，Close 后立刻摘掉，之后任何使用都只返回错误、不解引用。
std::mutex g_te_mu;
std::unordered_set<XGameUiTextEntryHandle> g_te_live;

// ---------------------------------------------------------------------------
// XAsyncBlock 的逐字镜像。
// 字段顺序是 queue, context, callback, internal（XAsync.h:39-60）——
// 不是 context 在前。下面四个 static_assert 把这条 ABI 事实钉死。
// ---------------------------------------------------------------------------
struct Blk {
    void* queue;
    void* context;
    void (*callback)(Blk*);
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};

static_assert(sizeof(Blk) == sizeof(XAsyncBlock), "XAsyncBlock 大小不匹配");
static_assert(offsetof(Blk, queue) == offsetof(XAsyncBlock, queue), "queue 必须是第 1 个字段");
static_assert(offsetof(Blk, context) == offsetof(XAsyncBlock, context), "context 必须是第 2 个字段");
static_assert(offsetof(Blk, callback) == offsetof(XAsyncBlock, callback), "callback 必须是第 3 个字段");
static_assert(offsetof(Blk, internal) == offsetof(XAsyncBlock, internal), "internal 必须在最后");

// 每个 async block 一次请求的状态。
// 光靠 block 指针当 key 不够：栈上的 XAsyncBlock 地址会被反复复用，旧条目会让
// "从没发起过本文件异步"的陌生 block 被误判成已完成。所以再往 XAsyncBlock 的
// internal[32]（头文件注明 Internal use only，本项目运行时从不碰它，XAsync.h:59）
// 里写一个每次调用都不同的 tag，Result 时必须对得上号才算数。
struct Req {
    long hr = S_OK;
    XAsyncCompletionRoutine* user_cb = nullptr;
    bool cb_finished = false;
    uint64_t tag = 0;
};

constexpr uint64_t kAsyncTagSeed = 0x5842545549415348ull; // 'XBTUIASH'

std::mutex g_mu;
std::unordered_map<void*, Req> g_req;
std::atomic<uint64_t> g_tag_seq{0};

uint64_t tag_of(XAsyncBlock* a) {
    uint64_t v = 0;
    std::memcpy(&v, a->internal, sizeof(v));
    return v;
}

bool tag_ok(XAsyncBlock* a, uint64_t expect) { return tag_of(a) == expect; }

// XAsyncWork：把这个 block 期望的 HRESULT 交回给运行时去完成。
HRESULT CALLBACK work_thunk(XAsyncBlock* a) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_req.find(a);
    return it == g_req.end() ? S_OK : static_cast<HRESULT>(it->second.hr);
}

// 包一层完成回调：先把 block 的 callback 还原成调用方原值（block 可能被复用），
// 再调调用方的回调，最后标记"回调已跑完"，好让 finish() 知道可以安全返回了。
void CALLBACK cb_tramp(XAsyncBlock* a) {
    XAsyncCompletionRoutine* cb = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_req.find(a);
        if (it != g_req.end()) cb = it->second.user_cb;
    }
    a->callback = cb; // 还原
    if (cb) cb(a);
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_req.find(a);
        if (it != g_req.end()) it->second.cb_finished = true;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// finish：让 XAsyncBlock 真正"完成"。
//
// 陷阱：本项目的完成状态登记在 xgameruntime.cpp 的 g_async（complete_async，
// xgameruntime.cpp:240；查询在 XAsyncGetStatus，xgameruntime.cpp:574），那个表在
// 匿名命名空间里，本文件拿不到。只调 b->callback(b) 不登记，wait=true 依旧空转 5 秒。
//
// 因此这里借用同一 DLL 里已导出的 XAsyncRun（exports.txt，xgameruntime.cpp:600）：
// 它先把 block 登记进 g_async，再在 worker 线程里跑 work_thunk 并用 complete_async
// 完成（= 登记状态 + 回调）。随后自旋等"回调已跑完"（无回调时等状态登记），
// 上限 2s、正常 <1ms —— 这样 Async 返回时 block 已经彻底完成，调用方即便在拿到
// E_NOTIMPL 后立刻丢弃栈上的 block 也不会被 worker 线程追着写（use-after-free）。
// ---------------------------------------------------------------------------
static void finish(void* async_block, long hr) {
    auto* a = static_cast<XAsyncBlock*>(async_block);
    if (!a) return;

    const bool has_cb = (a->callback != nullptr);
    {
        std::lock_guard<std::mutex> lock(g_mu);
        Req& r = g_req[a];
        r.hr = hr;
        r.user_cb = has_cb ? a->callback : nullptr;
        r.cb_finished = false;
        r.tag = kAsyncTagSeed + g_tag_seq.fetch_add(1, std::memory_order_relaxed) + 1;
        std::memcpy(a->internal, &r.tag, sizeof(r.tag)); // 见 Req 注释：标记"这次是本文件的异步"
    }
    if (has_cb) a->callback = &cb_tramp;

    const HRESULT hr_run = XAsyncRun(a, &work_thunk);
    if (FAILED(hr_run)) {
        // 兜底：登记失败也不能让引擎空等，直接按 XAsyncBlock 语义回调一次
        if (a->callback) a->callback(a);
        return;
    }

    for (int i = 0; i < 2000; ++i) {
        bool settled;
        if (has_cb) {
            std::lock_guard<std::mutex> lock(g_mu);
            auto it = g_req.find(a);
            settled = (it != g_req.end() && it->second.cb_finished);
        } else {
            settled = (XAsyncGetStatus(a, false) != E_PENDING);
        }
        if (settled) return;
        ::Sleep(1);
    }
    xbgtasix::logf("[ui_extra] 警告：异步完成登记超时 block=%p hr=0x%08lx",
                   static_cast<void*>(a), static_cast<unsigned long>(hr));
}

namespace {

// 本文件发起的异步：查询它记录的状态。
// 注意这里【不能】退回 XAsyncGetStatus 兜底：运行时 g_async 也是拿 block 指针当 key，
// 栈地址复用后会读出别人的陈旧状态，把陌生 block 误判成"已完成"。只认自己的登记 + tag。
HRESULT result_of(XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_req.find(async);
    if (it != g_req.end() && tag_ok(async, it->second.tag)) return static_cast<HRESULT>(it->second.hr);
    return E_PENDING;
}

bool completed(XAsyncBlock* async) {
    if (!async) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_req.find(async);
    return it != g_req.end() && tag_ok(async, it->second.tag);
}

// Tier1 通用：记日志 + 完成异步 + 回 E_NOTIMPL
HRESULT not_impl_async(const char* api, XAsyncBlock* async) {
    if (!async) return E_INVALIDARG;
    xbgtasix::logf("[ui_extra] %s：桌面无对应面板，异步以 E_NOTIMPL 完成", api);
    finish(async, E_NOTIMPL);
    // GDK 语义：*Async 的返回值表示"请求是否被受理"，操作结果走 XAsyncGetStatus。
    // 这里已完成异步，所以返回 S_OK；把 E_NOTIMPL 当受理失败会让调用方提前放弃，
    // 与"异步已结束"自相矛盾。
    return S_OK;
}

} // namespace

extern "C" {

// ===========================================================================
// Tier 1：返回 E_NOTIMPL，但异步必须完成（XGameUI.h:43-116, 451-461）
// ===========================================================================

// 成就面板（XGameUI.h:82）
STDAPI XGameUiShowAchievementsAsync(XAsyncBlock* async, XUserHandle requestingUser,
                                    uint32_t titleId) noexcept {
    (void)requestingUser;
    (void)titleId;
    return not_impl_async("XGameUiShowAchievementsAsync", async);
}

// 成就面板结果（XGameUI.h:88）：语义就是"回异步状态"
STDAPI XGameUiShowAchievementsResult(XAsyncBlock* async) noexcept {
    return result_of(async);
}

// State Share（XGameUI.h:453）：声明在头文件末尾的 Authentication 段之后
STDAPI XGameUiShowStateShareAsync(XAsyncBlock* async, XUserHandle requestingUser,
                                  const char* linkToken) noexcept {
    (void)requestingUser;
    (void)linkToken;
    return not_impl_async("XGameUiShowStateShareAsync", async);
}

// State Share 结果（XGameUI.h:459）
STDAPI XGameUiShowStateShareResult(XAsyncBlock* async) noexcept {
    return result_of(async);
}

// 发送游戏邀请（XGameUI.h:43）
STDAPI XGameUiShowSendGameInviteAsync(XAsyncBlock* async, XUserHandle requestingUser,
                                      const char* sessionConfigurationId,
                                      const char* sessionTemplateName, const char* sessionId,
                                      const char* invitationText,
                                      const char* customActivationContext) noexcept {
    (void)requestingUser;
    (void)sessionConfigurationId;
    (void)sessionTemplateName;
    (void)sessionId;
    (void)invitationText;
    (void)customActivationContext;
    return not_impl_async("XGameUiShowSendGameInviteAsync", async);
}

// 发送游戏邀请结果（XGameUI.h:53）
STDAPI XGameUiShowSendGameInviteResult(XAsyncBlock* async) noexcept {
    return result_of(async);
}

// MultiplayerActivity 邀请（XGameUI.h:59）
STDAPI XGameUiShowMultiplayerActivityGameInviteAsync(XAsyncBlock* async,
                                                     XUserHandle requestingUser) noexcept {
    (void)requestingUser;
    return not_impl_async("XGameUiShowMultiplayerActivityGameInviteAsync", async);
}

// MultiplayerActivity 邀请结果（XGameUI.h:64）
STDAPI XGameUiShowMultiplayerActivityGameInviteResult(XAsyncBlock* async) noexcept {
    return result_of(async);
}

// 玩家选择器（XGameUI.h:94）
STDAPI XGameUiShowPlayerPickerAsync(XAsyncBlock* async, XUserHandle requestingUser,
                                    const char* promptText, uint32_t selectFromPlayersCount,
                                    const uint64_t* selectFromPlayers,
                                    uint32_t preSelectedPlayersCount,
                                    const uint64_t* preSelectedPlayers,
                                    uint32_t minSelectionCount,
                                    uint32_t maxSelectionCount) noexcept {
    (void)requestingUser;
    (void)promptText;
    (void)selectFromPlayersCount;
    (void)selectFromPlayers;
    (void)preSelectedPlayersCount;
    (void)preSelectedPlayers;
    (void)minSelectionCount;
    (void)maxSelectionCount;
    return not_impl_async("XGameUiShowPlayerPickerAsync", async);
}

// 玩家选择器结果数量（XGameUI.h:106）：视为"用户没选人"，count=0 且 S_OK
STDAPI XGameUiShowPlayerPickerResultCount(XAsyncBlock* async,
                                          uint32_t* resultPlayersCount) noexcept {
    if (!async || !resultPlayersCount) return E_INVALIDARG;
    if (!completed(async)) return E_PENDING;
    *resultPlayersCount = 0;
    return S_OK;
}

// 玩家选择器结果（XGameUI.h:111）：count=0 语义，一个玩家都不写
STDAPI XGameUiShowPlayerPickerResult(XAsyncBlock* async, uint32_t resultPlayersCount,
                                     uint64_t* resultPlayers,
                                     uint32_t* resultPlayersUsed) noexcept {
    (void)resultPlayers;
    (void)resultPlayersCount;
    if (!async) return E_INVALIDARG;
    if (!completed(async)) return E_PENDING;
    if (resultPlayersUsed) *resultPlayersUsed = 0;
    return S_OK;
}

// ===========================================================================
// Tier 3：玩家名片（XGameUI.h:70），同样是"无面板 -> E_NOTIMPL 但完成"
// ===========================================================================

STDAPI XGameUiShowPlayerProfileCardAsync(XAsyncBlock* async, XUserHandle requestingUser,
                                         uint64_t targetPlayer) noexcept {
    (void)requestingUser;
    (void)targetPlayer;
    return not_impl_async("XGameUiShowPlayerProfileCardAsync", async);
}

STDAPI XGameUiShowPlayerProfileCardResult(XAsyncBlock* async) noexcept {
    return result_of(async);
}

// ===========================================================================
// Tier 2：非模态文本输入纯壳（XGameUI.h:406-449）
// ===========================================================================

STDAPI XGameUiTextEntryOpen(const XGameUiTextEntryOptions* options, uint32_t maxLength,
                            const char* initialText, uint32_t cursorIndex,
                            XGameUiTextEntryHandle* handle) noexcept {
    if (!handle) return E_INVALIDARG;
    *handle = nullptr; // _Out_ 先写，避免调用方拿到栈垃圾

    auto* h = new (std::nothrow) XGameUiTextEntry();
    if (!h) return E_OUTOFMEMORY;

    h->max_length = maxLength;
    if (initialText) {
        h->text.assign(initialText);                       // _In_opt_z_：到 NUL 为止
        if (h->text.size() > maxLength) h->text.resize(maxLength); // 超出上限就截断
    }
    h->cursor_index = cursorIndex > h->text.size() ? static_cast<uint32_t>(h->text.size())
                                                   : cursorIndex;
    if (options) {
        h->options = *options;
    } else {
        // 调用方传 null（_In_ 但桌面实现容忍）：用头文件里的 0 值默认
        h->options.inputScope = XGameUiTextEntryInputScope::Default;
        h->options.positionHint = XGameUiTextEntryPositionHint::Bottom;
        h->options.visibilityFlags = XGameUiTextEntryVisibilityFlags::Default;
    }

    xbgtasix::logf("[ui_extra] TextEntryOpen：假句柄 %p（maxLength=%u, 初值长度=%u）",
                   static_cast<void*>(h), static_cast<unsigned>(maxLength),
                   static_cast<unsigned>(h->text.size()));
    {
        std::lock_guard<std::mutex> lock(g_te_mu);
        g_te_live.insert(h);
    }
    *handle = h;
    return S_OK;
}

// STDAPI_(void)：返回类型必须逐字跟随头文件（XGameUI.h:414），否则 ABI/导出不匹配
STDAPI_(void) XGameUiTextEntryClose(XGameUiTextEntryHandle handle) noexcept {
    if (!handle) return;
    // 只有还在存活表里的句柄才 delete；重复 Close / 野句柄一律无声忽略（不解引用）
    {
        std::lock_guard<std::mutex> lock(g_te_mu);
        if (g_te_live.erase(handle) == 0) {
            xbgtasix::logf("[ui_extra] TextEntryClose：忽略非存活句柄 %p", static_cast<void*>(handle));
            return;
        }
    }
    delete handle;
}

STDAPI XGameUiTextEntryGetState(XGameUiTextEntryHandle handle,
                               XGameUiTextEntryChangeTypeFlags* changeType,
                               uint32_t* cursorIndex, uint32_t* imeClauseStartIndex,
                               uint32_t* imeClauseEndIndex, uint32_t bufferSize,
                               char* buffer) noexcept {
    if (!handle || !changeType) return E_INVALIDARG;

    std::lock_guard<std::mutex> lock(g_te_mu);
    if (g_te_live.find(handle) == g_te_live.end()) return E_INVALIDARG;

    // 句柄里存着当前文本与光标（Open 时按 initialText/cursorIndex 初始化）。
    // 以前这里只写空串，测试 t22 因此抓到：GetState 永远看不到初值。
    *changeType = XGameUiTextEntryChangeTypeFlags::None; // = 0x0（XGameUI.h:378）
    if (cursorIndex) *cursorIndex = handle->cursor_index;
    if (imeClauseStartIndex) *imeClauseStartIndex = 0;
    if (imeClauseEndIndex) *imeClauseEndIndex = 0;
    if (buffer && bufferSize) {
        // bufferSize 是字节数（含结尾 0）：最多拷 bufferSize-1 个字符
        size_t n = handle->text.size();
        if (n > bufferSize - 1) n = bufferSize - 1;
        std::memcpy(buffer, handle->text.data(), n);
        buffer[n] = '\0';
    }
    return S_OK;
}

STDAPI XGameUiTextEntryGetExtents(XGameUiTextEntryHandle handle,
                                 XGameUiTextEntryExtents* extents) noexcept {
    if (!extents) return E_INVALIDARG;
    // XGameUiTextEntryExtents 是 4 个 float，共 16 字节（全 0 的 bit 模式就是 0.0f）
    std::memset(extents, 0, sizeof(*extents));
    if (!handle) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_te_mu);
    if (g_te_live.find(handle) == g_te_live.end()) return E_INVALIDARG;
    return S_OK;
}

STDAPI XGameUiTextEntryUpdatePositionHint(XGameUiTextEntryHandle handle,
                                         XGameUiTextEntryPositionHint positionHint) noexcept {
    if (!handle) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_te_mu);
    if (g_te_live.find(handle) == g_te_live.end()) return E_INVALIDARG;
    handle->options.positionHint = positionHint;
    return S_OK;
}

STDAPI XGameUiTextEntryUpdateVisibility(XGameUiTextEntryHandle handle,
                                       XGameUiTextEntryVisibilityFlags visibilityFlags) noexcept {
    if (!handle) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_te_mu);
    if (g_te_live.find(handle) == g_te_live.end()) return E_INVALIDARG;
    handle->options.visibilityFlags = visibilityFlags;
    return S_OK;
}

} // extern "C"
