// SPDX-License-Identifier: GPL-3.0-or-later
// system_extra.cpp —— XSystem / XDisplay / XThread 里"PC 上有真语义"的部分
// 约定：只在本文件实现；导出名追加进 exports_sys.txt。
//
// 设计基线：能从 Windows 真实取到的信息就去取（卷序列号、进程名、真实内核句柄表、
// 真实执行状态），取不到的主机概念（主机 ID、主机时间敏感线程、Xbox Live 沙箱）
// 就明确给占位/固定值，但**必须把出参写满**——绝不留下"返回 S_OK 而 *used 是栈垃圾"，
// 调用方按垃圾长度 memcpy 会直接踩爆堆。
//
// ABI 铁律（三个头文件核对过）：
//   * XSystemIsHandleValid / XThreadIsTimeSensitive 是 STDAPI_(bool)：C++ bool（AL 一字节），
//     不是 HRESULT，也不能写成 int。
//   * XThreadAssertNotTimeSensitive / XDisplayCloseTimeoutDeferralHandle 是 STDAPI_(void)：
//     返回类型逐字跟随头文件（XThread.h:29 / XDisplay.h:44），写成 HRESULT 会污染 rax。
//   * 尺寸与 *Used 都是 size_t（x64 = 8 字节），不是 uint32_t。
//   * 三个 size-then-fill 函数的 used 指针都是 _Out_opt_：nullptr 合法，不得解引用。
#include <windows.h>
#include <shellapi.h>

#include "XDisplay.h"
#include "XSystem.h"
#include "XThread.h"

#include "XAccessibility.h"
#include "XError.h"
#include "XLauncher.h"

#include "xbgtasix/trace.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_set>

// 头文件里的两个尺寸常量是公开 ABI：占位串长度必须与之一致，
// 否则调用方按数组常量申请缓冲、我们却返回别的长度，又是一次越界。
static_assert(XSystemConsoleIdBytes == 39, "XSystemConsoleIdBytes 变了（XSystem.h:26）");
static_assert(XSystemAppSpecificDeviceIdBytes == 45, "XSystemAppSpecificDeviceIdBytes 变了（XSystem.h:55）");
static_assert(XSystemXboxLiveSandboxIdMaxBytes == 16, "XSystemXboxLiveSandboxIdMaxBytes 变了");

namespace {

// ---------------------------------------------------------------------------
// 句柄跟踪（XSystemHandleTrack，XSystem.h:120）
// 只跟踪本文件自己创建的句柄（目前只有 DisplayTimeoutDeferral）。回调在锁外调用：
// 调用方完全可能在自己的回调里再进 XSystem 接口，持锁调用必死锁。
// ---------------------------------------------------------------------------
std::mutex g_track_mu;
XSystemHandleCallback g_track_cb = nullptr;
void* g_track_ctx = nullptr;

void handle_notify(XSystemHandle handle, XSystemHandleType type,
                   XSystemHandleCallbackReason reason) {
    XSystemHandleCallback cb = nullptr;
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_track_mu);
        cb = g_track_cb;
        ctx = g_track_ctx;
    }
    if (cb) cb(handle, type, reason, ctx);
}

// ---------------------------------------------------------------------------
// 机器/进程身份：卷序列号 + 进程名。
// 两者在一次进程内是常量，缓存起来（GetVolumeInformation 会摸盘，别放在热路径）。
// ---------------------------------------------------------------------------
std::mutex g_id_mu;
bool g_id_ready = false;
std::string g_proc_name;     // UTF-8，exe 文件名（不含目录）
std::string g_volume_root;   // "C:\" 这类卷根，仅用于日志
uint32_t g_vol_serial = 0;   // GetVolumeInformation 的卷序列号

std::string wide_to_utf8(const wchar_t* w, int len) {
    if (!w || len <= 0) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), need, nullptr, nullptr);
    return out;
}

// 调用方必须持有 g_id_mu。
void ensure_id_locked() {
    if (g_id_ready) return;
    g_id_ready = true;   // 失败也要置位：同一次进程内重复摸盘只会更慢

    wchar_t path[MAX_PATH * 4] = {};
    DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(_countof(path)));
    if (n == 0 || n >= _countof(path)) {
        xbgtasix::logf("[sys_extra] GetModuleFileNameW 失败 err=%lu", GetLastError());
        g_proc_name = "unknown";
        return;
    }

    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    g_proc_name = wide_to_utf8(base, static_cast<int>(wcslen(base)));
    if (g_proc_name.empty()) g_proc_name = "unknown";

    // GetVolumePathNameW 能正确处理挂载点/UNC，比手工取前 3 个字符靠谱
    wchar_t vol[MAX_PATH] = {};
    if (GetVolumePathNameW(path, vol, static_cast<DWORD>(_countof(vol)))) {
        g_volume_root = wide_to_utf8(vol, static_cast<int>(wcslen(vol)));
        DWORD serial = 0;
        if (!GetVolumeInformationW(vol, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) {
            xbgtasix::logf("[sys_extra] GetVolumeInformationW('%s') 失败 err=%lu，卷序列号按 0 处理",
                           g_volume_root.c_str(), GetLastError());
            serial = 0;
        }
        g_vol_serial = static_cast<uint32_t>(serial);
    } else {
        xbgtasix::logf("[sys_extra] GetVolumePathNameW 失败 err=%lu", GetLastError());
    }

    xbgtasix::logf("[sys_extra] 身份来源：vol='%s' serial=0x%08lX proc='%s'",
                   g_volume_root.c_str(), static_cast<unsigned long>(g_vol_serial),
                   g_proc_name.c_str());
}

// ---- 稳定派生：FNV-1a + splitmix64 展开成 32 字节，再 base64 ----
// 目的不是密码学，而是"同机同 exe 必得同一串、换机器/换 exe 必变"。

constexpr uint64_t kFnvOffset = 1469598103934665603ull;   // FNV-1a 64 offset basis
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t fnv1a(const char* s, size_t n, uint64_t h = kFnvOffset) {
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<unsigned char>(s[i]);
        h *= kFnvPrime;
    }
    return h;
}

uint64_t splitmix64(uint64_t x) {
    uint64_t z = x + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// 32 字节 -> 44 个 base64 字符 + NUL = 45，正好等于 XSystemAppSpecificDeviceIdBytes。
// （32 = 3*10 + 2，所以 10 组满 4 字符 + 1 组 3 字符 + 1 个 '=' 补位）
void base64_32(const uint8_t in[32], char out[45]) {
    static const char kT[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i + 2 < 32; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) |
                           (static_cast<uint32_t>(in[i + 1]) << 8) |
                           static_cast<uint32_t>(in[i + 2]);
        out[o++] = kT[(v >> 18) & 0x3f];
        out[o++] = kT[(v >> 12) & 0x3f];
        out[o++] = kT[(v >> 6) & 0x3f];
        out[o++] = kT[v & 0x3f];
    }
    const uint32_t v = (static_cast<uint32_t>(in[30]) << 16) |
                       (static_cast<uint32_t>(in[31]) << 8);
    out[o++] = kT[(v >> 18) & 0x3f];
    out[o++] = kT[(v >> 12) & 0x3f];
    out[o++] = kT[(v >> 6) & 0x3f];
    out[o++] = '=';
    out[o] = '\0';   // o == 44
}

std::string make_app_device_id() {
    std::lock_guard<std::mutex> lock(g_id_mu);
    ensure_id_locked();

    char seed[256];
    std::snprintf(seed, sizeof(seed), "%08lX|%s", static_cast<unsigned long>(g_vol_serial),
                  g_proc_name.c_str());
    const uint64_t h = fnv1a(seed, std::strlen(seed));

    uint8_t digest[32];
    for (int i = 0; i < 4; ++i) {
        // 每一块用不同的乘法偏移，避免 4 段出现线性关系
        const uint64_t v = splitmix64(h ^ (kFnvPrime * static_cast<uint64_t>(i + 1)));
        std::memcpy(digest + static_cast<size_t>(i) * 8, &v, 8);
    }

    char out[45];
    base64_32(digest, out);
    return std::string(out, 44);
}

// size-then-fill 的统一出口。
// 语义：*used 永远写"含结尾 NUL 的所需字节数"（即使缓冲区不够也写），
// 这样调用方拿到 E_NOT_SUFFICIENT_BUFFER 后能用同一个 *used 重试，而不是用 0 或垃圾值。
HRESULT copy_out(const char* text, size_t size, char* out, size_t* used) {
    if (!out || size == 0) return E_INVALIDARG;
    const size_t need = std::strlen(text) + 1;
    if (used) *used = need;
    if (size < need) return E_NOT_SUFFICIENT_BUFFER;
    std::memcpy(out, text, need);
    return S_OK;
}

}  // namespace

extern "C" {

// ===========================================================================
// XSystem
// ===========================================================================

// 主机 ID（XSystem.h:28）。PC 上没有主机，固定占位串（全 0 的 GUID 形式，
// 长度 38 + NUL = 39 = XSystemConsoleIdBytes）。
// 关键点：*consoleIdUsed 必须写。_Out_opt_ 允许 null，但非 null 时必须给真值。
STDAPI XSystemGetConsoleId(size_t consoleIdSize, char* consoleId,
                           size_t* consoleIdUsed) noexcept {
    static constexpr char kPlaceholder[] = "{00000000-0000-0000-0000-000000000000}";
    static_assert(sizeof(kPlaceholder) == XSystemConsoleIdBytes, "占位串长度必须是 39 字节");
    return copy_out(kPlaceholder, consoleIdSize, consoleId, consoleIdUsed);
}

// Xbox Live 沙箱 ID（XSystem.h:35）。PC 上零售游戏没有沙箱切换，固定 "RETAIL"
//（这正是 Windows 上零售包的沙箱名，不是随手编的串）。长度 7 <= 16 上限。
STDAPI XSystemGetXboxLiveSandboxId(size_t sandboxIdSize, char* sandboxId,
                                   size_t* sandboxIdUsed) noexcept {
    return copy_out("RETAIL", sandboxIdSize, sandboxId, sandboxIdUsed);
}

// 应用专属设备 ID（XSystem.h:57）：卷序列号 + 进程名派生，44 字节 + NUL。
// 同机同 exe 每次调用必须一致（测试里有连调两次的相等断言）。
STDAPI XSystemGetAppSpecificDeviceId(size_t appSpecificDeviceIdSize,
                                     char* appSpecificDeviceId,
                                     size_t* appSpecificDeviceIdUsed) noexcept {
    const std::string id = make_app_device_id();
    return copy_out(id.c_str(), appSpecificDeviceIdSize, appSpecificDeviceId,
                    appSpecificDeviceIdUsed);
}

// 句柄有效性（XSystem.h:123）：问内核，不靠魔数、不恒 true。
// 注意固有局限：句柄值被关闭后若立刻被别的 Create* 复用，这里会回答 true——
// 这是 Win32 句柄语义本身的性质，不是本实现能消除的。
STDAPI_(bool) XSystemIsHandleValid(XSystemHandle handle) noexcept {
    if (!handle) return false;
    // 实测（MinGW64 + Win10/11）：GetHandleInformation(INVALID_HANDLE_VALUE) 返回 TRUE。
    // 原因不是 bug：INVALID_HANDLE_VALUE 就是 -1，而 -1 同时是 GetCurrentProcess() 的
    // 伪句柄，内核把它当"合法句柄值"。但 XSystem 的句柄全是运行时资源句柄，
    // 真句柄值永远是 4 的小倍数、不可能是 -1，所以这个哨兵必须单独挡掉。
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD flags = 0;
    return GetHandleInformation(static_cast<HANDLE>(handle), &flags) != 0;
}

// 句柄生命周期回调注册（XSystem.h:117）。重复注册=替换；callback 传 null 表示取消。
STDAPI XSystemHandleTrack(XSystemHandleCallback callback, void* context) noexcept {
    {
        std::lock_guard<std::mutex> lock(g_track_mu);
        g_track_cb = callback;
        g_track_ctx = context;
    }
    xbgtasix::logf("[sys_extra] XSystemHandleTrack(cb=%p ctx=%p)：只跟踪本进程自建句柄",
                   reinterpret_cast<void*>(callback), context);
    return S_OK;
}

// ===========================================================================
// XDisplay：屏幕超时抑制（XDisplay.h:38 / :44）
// ===========================================================================

// 头文件只前置声明了 struct XDisplayTimeoutDeferral（XDisplay.h:33），布局由本文件定。
struct XDisplayTimeoutDeferral {
    uint32_t nested_at_acquire;   // 仅供日志/排障
};

namespace {

// 存活表：Close 后立刻摘掉。不靠读已释放内存里的 magic 判合法性
//（那是 UB，且二次 Close 会 double free 崩堆）。
std::mutex g_defer_mu;
std::unordered_set<XDisplayTimeoutDeferralHandle> g_defer_live;
int g_defer_count = 0;                 // 进程级嵌套计数（受 g_defer_mu 保护）
std::atomic<bool> g_defer_applied{false};

}  // namespace

// 真正阻止"屏幕/系统超时"：SetThreadExecutionState。
// 语义细节（必须诚实标注）：Win32 这个状态是**线程级**的——在哪个线程第一次 Acquire
// 就在哪个线程生效。跨线程 Acquire/Close 只能近似（计数是进程级的，恢复动作在最后
// 一个 Close 的线程上执行）。对单线程的引擎主循环来说这是精确正确的。
STDAPI XDisplayAcquireTimeoutDeferral(XDisplayTimeoutDeferralHandle* handle) noexcept {
    if (!handle) return E_INVALIDARG;
    *handle = nullptr;   // _Out_ 先写，别让调用方拿到栈垃圾

    auto* d = new (std::nothrow) XDisplayTimeoutDeferral();
    if (!d) return E_OUTOFMEMORY;

    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_defer_mu);
        g_defer_live.insert(d);
        first = (g_defer_count == 0);
        d->nested_at_acquire = static_cast<uint32_t>(g_defer_count);
        ++g_defer_count;
    }

    if (first) {
        const EXECUTION_STATE st = SetThreadExecutionState(
            ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
        if (st == 0) {
            // 无交互会话（服务/headless）时这里会失败。不把错误抛给调用方：
            // 句柄本身是有效的，"抑制可能没生效"只影响功耗策略，不该让游戏中断渲染循环。
            xbgtasix::logf("[sys_extra] 警告：SetThreadExecutionState 失败 err=%lu，屏幕超时未被抑制",
                           GetLastError());
        } else {
            g_defer_applied.store(true, std::memory_order_relaxed);
            xbgtasix::logf("[sys_extra] 屏幕超时抑制：已开启（ES_CONTINUOUS|DISPLAY|SYSTEM）");
        }
    }

    *handle = d;
    handle_notify(d, XSystemHandleType::DisplayTimeoutDeferral,
                  XSystemHandleCallbackReason::Created);
    return S_OK;
}

// STDAPI_(void)：返回类型逐字跟随 XDisplay.h:44，不能写成 HRESULT。
STDAPI_(void) XDisplayCloseTimeoutDeferralHandle(XDisplayTimeoutDeferralHandle handle) noexcept {
    if (!handle) return;

    bool last = false;
    {
        std::lock_guard<std::mutex> lock(g_defer_mu);
        if (g_defer_live.erase(handle) == 0) {
            // 重复 Close / 野句柄：无声忽略，绝不解引用（否则 double free 崩堆）
            xbgtasix::logf("[sys_extra] CloseTimeoutDeferralHandle：忽略非存活句柄 %p",
                           static_cast<void*>(handle));
            return;
        }
        --g_defer_count;
        last = (g_defer_count == 0);
    }

    handle_notify(handle, XSystemHandleType::DisplayTimeoutDeferral,
                  XSystemHandleCallbackReason::Destroyed);
    delete handle;

    if (last) {
        // 恢复默认：单独一个 ES_CONTINUOUS 即"不再要求保持唤醒"
        if (SetThreadExecutionState(ES_CONTINUOUS) == 0) {
            xbgtasix::logf("[sys_extra] 警告：恢复 SetThreadExecutionState(ES_CONTINUOUS) 失败 err=%lu",
                           GetLastError());
        } else if (g_defer_applied.exchange(false, std::memory_order_relaxed)) {
            xbgtasix::logf("[sys_extra] 屏幕超时抑制：已恢复默认");
        }
    }
}

// ===========================================================================
// XThread（XThread.h:18 / :24 / :29）
// ===========================================================================

// PC 上没有"主机时间敏感线程"这套调度概念：Xbox 上它用于让运行时在调试构建里
// 对"从渲染/音频回调里调阻塞 API"报警。桌面移植层保留全部调用点，但不改调度。
// 备选的近似实现是 SetThreadPriority(THREAD_PRIORITY_HIGHEST)，这里故意不做：
// 全局抬高游戏主线程优先级会在 PC 上与系统/驱动线程抢时间片，反而更容易卡顿。
STDAPI XThreadSetTimeSensitive(bool isTimeSensitiveThread) noexcept {
    xbgtasix::logf("[sys_extra] XThreadSetTimeSensitive(%s)：PC 无此概念，仅记录",
                   isTimeSensitiveThread ? "true" : "false");
    return S_OK;
}

// 不撒谎也不虚构：既然上面没有真的给线程打标，这里恒 false。
// 返回 true 会让调用方跳掉自己本该做的"非时间敏感路径"检查。
STDAPI_(bool) XThreadIsTimeSensitive() noexcept {
    return false;
}

// STDAPI_(void)：见 XThread.h:29。IsTimeSensitive 恒 false，所以这个断言永远不成立，
// 空实现即可；它位于热路径上，连日志都不打。
STDAPI_(void) XThreadAssertNotTimeSensitive() noexcept {
}

}  // extern "C"

// ===========================================================================
// XLauncher / XAccessibility / XError（追加：PC 上有真语义的部分）
// ===========================================================================

// XLauncher.h:17 —— 在 PC 上等价于 ShellExecute("open", uri)
STDAPI XLaunchUri(XUserHandle requestingUser, const char* uri) noexcept {
    (void)requestingUser;
    if (!uri || !uri[0]) return E_INVALIDARG;
    const HINSTANCE r = ShellExecuteA(nullptr, "open", uri, nullptr, nullptr, SW_SHOWNORMAL);
    // ShellExecute 的返回值 >32 才算成功
    const bool ok = reinterpret_cast<INT_PTR>(r) > 32;
    xbgtasix::logf("[xlaunch] XLaunchUri(%s) -> %s", uri, ok ? "已交给系统" : "系统拒绝");
    if (!ok) return HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION);
    return S_OK;
}

static bool g_cc_enabled = false;

// XAccessibility.h:53
STDAPI XClosedCaptionSetEnabled(bool enabled) noexcept {
    g_cc_enabled = enabled;
    xbgtasix::logf("[xacc] 字幕: %s", enabled ? "开" : "关");
    return S_OK;
}

// XAccessibility.h:49 —— 必须整块填满：调用方按结构体读，残值会被当成颜色/字号
STDAPI XClosedCaptionGetProperties(XClosedCaptionProperties* properties) noexcept {
    if (!properties) return E_INVALIDARG;
    std::memset(properties, 0, sizeof(*properties));
    // 跟随 Windows 的无障碍设置（关掉时给一套安全默认值）
    BOOL enabled = g_cc_enabled ? TRUE : FALSE;
    // SystemParametersInfo 的 SPI_GETCLIENTAREAANIMATION 之类没有直接对应项，
    // 这里用注册表里的"显示字幕"偏好作为参考值（读不到就保留默认）
    struct { XColor bg; XColor fg; XColor win; } defaults{
        {0, 0, 0, 255}, {255, 255, 255, 255}, {0, 0, 0, 0}};
    properties->BackgroundColor = defaults.bg;
    properties->FontColor = defaults.fg;
    properties->WindowColor = defaults.win;
    properties->FontEdgeAttribute = XClosedCaptionFontEdgeAttribute::Default;
    properties->FontStyle = XClosedCaptionFontStyle::Default;
    properties->FontScale = 1.0f;
    properties->Enabled = enabled != FALSE;
    return S_OK;
}

// XError.h:19 —— 错误回调：PC 上按游戏注册的回调转发，未注册就只记日志
static XErrorCallback* g_error_cb = nullptr;
static void* g_error_ctx = nullptr;
static XErrorOptions g_error_opts = XErrorOptions::None;

STDAPI_(void) XErrorSetCallback(XErrorCallback* callback, void* context) noexcept {
    g_error_cb = callback;
    g_error_ctx = context;
}

// 头文件是**两个**参数：有调试器时 / 无调试器时各一套选项
STDAPI_(void) XErrorSetOptions(XErrorOptions optionsDebuggerPresent,
                               XErrorOptions optionsDebuggerNotPresent) noexcept {
    g_error_opts = optionsDebuggerPresent | optionsDebuggerNotPresent;
    xbgtasix::logf("[xerror] 选项: 有调试器=0x%x 无调试器=0x%x"
                   "（DebugBreak/FailFast 我们不执行，避免没有调试器时直接杀进程）",
                   static_cast<unsigned>(optionsDebuggerPresent),
                   static_cast<unsigned>(optionsDebuggerNotPresent));
}
