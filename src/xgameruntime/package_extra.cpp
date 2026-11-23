// SPDX-License-Identifier: GPL-3.0-or-later
// package_extra.cpp —— XPackage 的 chunk / 安装 / 枚举
//
// 原则（与 xgameruntime.cpp 一致）：
//   * 签名逐字来自 gdk/include_clean/XPackage.h，ABI 交给编译器；
//   * 能真实现的真实现（用户区域、统计、chunk 枚举 = 包根目录下的第一层子目录）；
//   * 不能实现的（安装/卸载/下载量）返回 E_NOTIMPL，但 *Async 一律让 XAsyncBlock
//     走到"完成"，绝不让调用方的 XAsyncGetStatus(wait=true) 空转 5000ms 后拿到 E_PENDING。
//
// 关于 XAsyncBlock 的完成：
//   本项目的 XAsyncGetStatus 只认宿主的内部状态表（xgameruntime.cpp 里的 g_async），
//   而那张表只有 XAsyncRun 能写入。所以这里借 XAsyncRun 完成"置完成 + 回调一次"，
//   下面的 Blk/finish 是兜底路径（XAsyncRun 万一同步失败时至少把回调打出去）。
//   两个字段布局必须逐字节一致，故有 static_assert 兜底。
#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "XPackage.h"
#include "XTaskQueue.h"

#include "xbgtasix/trace.h"
#include "trace_entry.h"   // 统一入口追踪（XB_TRACE_ENTRY）

// GDK 的 E_NOTFOUND = HRESULT_FROM_WIN32(ERROR_NOT_FOUND)，但这套精简过的头文件没带
// 那个宏。按同一数值补一个，语义与真 GDK 一致（xgameruntime.cpp 里也是这个值）。
#ifndef E_NOTFOUND
#define E_NOTFOUND HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
#endif

// ---------------- XAsyncBlock 完成（ABI 陷阱集中处） ----------------
//
// 字段顺序是 queue, context, callback, internal —— 顺序错了回调就是野指针。
struct Blk
{
    void* queue;
    void* context;
    void (*callback)(Blk*);
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};

static_assert(sizeof(Blk) == sizeof(XAsyncBlock), "Blk 必须与 XAsyncBlock 同宽");
static_assert(offsetof(Blk, callback) == offsetof(XAsyncBlock, callback),
              "Blk.callback 的偏移必须与 XAsyncBlock.callback 一致");
static_assert(offsetof(Blk, internal) == offsetof(XAsyncBlock, internal),
              "Blk.internal 的偏移必须与 XAsyncBlock.internal 一致");

// 兜底完成：只负责把回调打出去（宿主状态表那条路由 XAsyncRun 负责）。
static void finish(Blk* b, HRESULT hr)
{
    if (b && b->callback) b->callback(b);
    (void)hr; // 状态由宿主表记录，这里只为不触发 -Wunused-parameter
}

// 旁路表：async -> 期望的完成结果。work 在宿主线程里读它并作为异步结果返回。
static std::mutex g_pkg_async_mu;
static std::unordered_map<XAsyncBlock*, HRESULT> g_pkg_async_hr;

static HRESULT CALLBACK pkg_async_work(XAsyncBlock* async) noexcept
{
    std::lock_guard<std::mutex> lock(g_pkg_async_mu);
    auto it = g_pkg_async_hr.find(async);
    if (it == g_pkg_async_hr.end()) return E_NOTIMPL;
    const HRESULT hr = it->second;
    g_pkg_async_hr.erase(it);
    return hr;
}

// 让 XAsyncBlock 真正完成：宿主 XAsyncGetStatus 能看到状态，且回调恰好一次。
static void complete_pkg_async(XAsyncBlock* async, HRESULT hr)
{
    if (!async) return;
    {
        std::lock_guard<std::mutex> lock(g_pkg_async_mu);
        g_pkg_async_hr[async] = hr;
    }
    if (FAILED(XAsyncRun(async, &pkg_async_work)))
        finish(reinterpret_cast<Blk*>(async), hr);
}

// ---------------- 包根目录 / chunk 目录枚举 ----------------

// 包目录来自环境变量 XBGTASIX_PACKAGE_ROOT（与 XPackageMount 的 package_root 同源）。
static bool package_root(std::string* out)
{
    char buf[32768] = {};
    const DWORD n = ::GetEnvironmentVariableA("XBGTASIX_PACKAGE_ROOT", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return false;
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    if (s.empty()) return false;
    *out = s;
    return true;
}

// 第一层子目录 = 已安装的 chunk
static bool list_chunk_dirs(const std::string& root, std::vector<std::string>* names)
{
    WIN32_FIND_DATAA fd{};
    const std::string pattern = root + "\\*";
    HANDLE h = ::FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0) continue;
        names->emplace_back(fd.cFileName);
    } while (::FindNextFileA(h, &fd));
    ::FindClose(h);
    return true;
}

static uint32_t fnv1a32(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

// 目录名 -> chunkId：纯数字名直接用数字（和游戏手写的 chunkId 对得上），
// 其余用稳定的 FNV-1a，保证"枚举出来的 id"和"查询时算出来的 id"一致。
static uint32_t chunk_id_for(const std::string& name)
{
    bool digits = !name.empty() && name.size() <= 10;
    for (char c : name) {
        if (c < '0' || c > '9') {
            digits = false;
            break;
        }
    }
    if (digits) {
        const unsigned long long v = std::strtoull(name.c_str(), nullptr, 10);
        // 超 uint32 的数字不回绕，退回哈希，保证 id 唯一可逆
        if (v <= 0xFFFFFFFFull) return static_cast<uint32_t>(v);
    }
    return fnv1a32(name);
}

static bool ieq(const char* a, const std::string& b)
{
    if (!a || std::strlen(a) != b.size()) return false;
    for (size_t i = 0; i < b.size(); ++i) {
        const int x = std::tolower(static_cast<unsigned char>(a[i]));
        const int y = std::tolower(static_cast<unsigned char>(b[i]));
        if (x != y) return false;
    }
    return true;
}

static bool selector_matches(const XPackageChunkSelector& sel, const std::string& name)
{
    switch (sel.type) {
    case XPackageChunkSelectorType::Language: return ieq(sel.language, name);
    case XPackageChunkSelectorType::Tag:      return ieq(sel.tag, name);
    case XPackageChunkSelectorType::Feature:  return ieq(sel.feature, name);
    case XPackageChunkSelectorType::Chunk:    return sel.chunkId == chunk_id_for(name);
    }
    return false;
}

// ---------------- 安装监控（壳，但字段/句柄生命周期是真的） ----------------

struct XPackageInstallationMonitor
{
    std::string package_id;
    std::string root;
    uint32_t interval_ms = 0;
    XTaskQueueHandle queue = nullptr;

    struct Reg
    {
        uint64_t token = 0;
        void* context = nullptr;
        XPackageInstallationProgressCallback* callback = nullptr;
    };
    std::mutex mu;
    std::vector<Reg> regs;
};

static std::atomic<uint64_t> g_next_token{1};

// ---------------- A. 用户区域与统计 ----------------

// 本进程写过的字节数。本层不接管文件写入，所以恒为 0；结构体四个字段仍要如实填满。
static std::atomic<uint64_t> g_bytes_written{0};

STDAPI XPackageGetWriteStats(XPackageWriteStats* writeStats) noexcept
{
    XB_TRACE_ENTRY("XPackageGetWriteStats", writeStats, 0ULL, 0ULL, 0ULL);
    if (!writeStats) return E_INVALIDARG;
    writeStats->interval = 1000; // ms
    writeStats->budget = 0;      // 0 = 不设上限
    writeStats->elapsed = 0;     // 本实现不做限速，无需累计窗口
    writeStats->bytesWritten = g_bytes_written.load(std::memory_order_relaxed);
    return S_OK;
}

STDAPI XPackageGetUserLocale(size_t localeSize, char* locale) noexcept
{
    XB_TRACE_ENTRY("XPackageGetUserLocale", localeSize, locale, 0ULL, 0ULL);
    if (!locale || localeSize == 0) return E_INVALIDARG;

    std::string loc;
    // 1) 系统区域（Windows Settings 里那一个）
    wchar_t wide[LOCALE_NAME_MAX_LENGTH] = {};
    const int wlen = ::GetUserDefaultLocaleName(wide, LOCALE_NAME_MAX_LENGTH);
    if (wlen > 0) {
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, wide, wlen, nullptr, 0, nullptr, nullptr);
        if (need > 0) {
            loc.resize(static_cast<size_t>(need));
            ::WideCharToMultiByte(CP_UTF8, 0, wide, wlen, loc.data(), need, nullptr, nullptr);
        }
    }

    // 2) 环境变量兜底（LANG / LC_ALL）："en_US.UTF-8" -> "en-US"
    if (loc.empty()) {
        const char* kKeys[] = {"LANG", "LC_ALL"};
        for (const char* key : kKeys) {
            char env[128] = {};
            const DWORD n = ::GetEnvironmentVariableA(key, env, sizeof(env));
            if (n == 0 || n >= sizeof(env)) continue;
            std::string s(env, n);
            const size_t cut = s.find_first_of(".[@");
            if (cut != std::string::npos) s.resize(cut);
            for (char& c : s) {
                if (c == '_') c = '-';
            }
            if (!s.empty()) {
                loc = s;
                break;
            }
        }
    }

    if (loc.empty()) return E_FAIL;
    if (localeSize < loc.size() + 1) return E_NOT_SUFFICIENT_BUFFER;
    std::memcpy(locale, loc.c_str(), loc.size() + 1);
    return S_OK;
}

// ---------------- B. chunk 枚举 ----------------

STDAPI XPackageEnumerateChunkAvailability(
    const char* packageIdentifier,
    XPackageChunkSelectorType type,
    void* context,
    XPackageChunkAvailabilityCallback* callback) noexcept
{
    XB_TRACE_ENTRY("XPackageEnumerateChunkAvailability", packageIdentifier, type, context, callback);
    (void)packageIdentifier; // 单包进程：包标识符不参与定位，目录才是唯一事实
    if (!callback) return E_INVALIDARG;

    std::string root;
    if (!package_root(&root)) {
        xbgtasix::logf("[xgamert] EnumerateChunkAvailability: 无 XBGTASIX_PACKAGE_ROOT -> E_NOTFOUND");
        return E_NOTFOUND;
    }
    std::vector<std::string> names;
    if (!list_chunk_dirs(root, &names)) {
        xbgtasix::logf("[xgamert] EnumerateChunkAvailability: 读不了目录 %s -> E_NOTFOUND",
                       root.c_str());
        return E_NOTFOUND;
    }

    // names 必须活过回调：selector 里的 const char* 直接指向它
    for (const std::string& name : names) {
        XPackageChunkSelector sel{};
        sel.type = type;
        switch (type) {
        case XPackageChunkSelectorType::Language: sel.language = name.c_str(); break;
        case XPackageChunkSelectorType::Tag:      sel.tag = name.c_str(); break;
        case XPackageChunkSelectorType::Feature:  sel.feature = name.c_str(); break;
        case XPackageChunkSelectorType::Chunk:    sel.chunkId = chunk_id_for(name); break;
        default: return E_INVALIDARG; // 头文件里没别的枚举值，非法值不猜
        }
        // 目录存在 = 已就绪（Ready）。回调返回 false 表示调用方不想要后续了。
        if (!callback(context, &sel, XPackageChunkAvailability::Ready)) break;
    }
    xbgtasix::logf("[xgamert] EnumerateChunkAvailability: %s 下 %zu 个 chunk 已就绪",
                   root.c_str(), names.size());
    return S_OK;
}

STDAPI XPackageFindChunkAvailability(
    const char* packageIdentifier,
    uint32_t selectorCount,
    XPackageChunkSelector* selectors,
    XPackageChunkAvailability* availability) noexcept
{
    XB_TRACE_ENTRY("XPackageFindChunkAvailability", packageIdentifier, selectorCount, selectors, availability);
    (void)packageIdentifier;
    if (!availability) return E_INVALIDARG;
    *availability = XPackageChunkAvailability::Unavailable;

    std::string root;
    if (!package_root(&root)) {
        xbgtasix::logf("[xgamert] FindChunkAvailability: 无 XBGTASIX_PACKAGE_ROOT -> E_NOTFOUND");
        return E_NOTFOUND;
    }
    std::vector<std::string> names;
    if (!list_chunk_dirs(root, &names)) {
        xbgtasix::logf("[xgamert] FindChunkAvailability: 读不了目录 %s -> E_NOTFOUND", root.c_str());
        return E_NOTFOUND;
    }

    // 空选择器 = 不要求任何 chunk，直接 Ready
    if (selectorCount == 0 || selectors == nullptr) {
        *availability = XPackageChunkAvailability::Ready;
        return S_OK;
    }

    // 整套选择器是一个整体：全部命中才 Ready，否则还没装 -> Installable
    bool all_ready = true;
    for (uint32_t i = 0; i < selectorCount; ++i) {
        bool hit = false;
        for (const std::string& name : names) {
            if (selector_matches(selectors[i], name)) {
                hit = true;
                break;
            }
        }
        if (!hit) {
            all_ready = false;
            break;
        }
    }
    *availability = all_ready ? XPackageChunkAvailability::Ready
                              : XPackageChunkAvailability::Installable;
    xbgtasix::logf("[xgamert] FindChunkAvailability: %u 个选择器 -> %s",
                   selectorCount,
                   all_ready ? "Ready" : "Installable");
    return S_OK;
}

STDAPI XPackageChangeChunkInstallOrder(
    const char* packageIdentifier,
    uint32_t selectorCount,
    XPackageChunkSelector* selectors) noexcept
{
    XB_TRACE_ENTRY("XPackageChangeChunkInstallOrder", packageIdentifier, selectorCount, selectors, 0ULL);
    (void)selectors;
    xbgtasix::logf("[xgamert] ChangeChunkInstallOrder(%s, %u)：本实现无安装队列，记下即返回 S_OK",
                   packageIdentifier ? packageIdentifier : "(null)", selectorCount);
    return S_OK;
}

// ---------------- C. 安装监控 ----------------

STDAPI XPackageCreateInstallationMonitor(
    const char* packageIdentifier,
    uint32_t selectorCount,
    XPackageChunkSelector* selectors,
    uint32_t minimumUpdateIntervalMs,
    XTaskQueueHandle queue,
    XPackageInstallationMonitorHandle* installationMonitor) noexcept
{
    XB_TRACE_ENTRY("XPackageCreateInstallationMonitor", packageIdentifier, selectorCount, selectors, minimumUpdateIntervalMs);
    if (!installationMonitor) return E_INVALIDARG;
    *installationMonitor = nullptr;
    if (!packageIdentifier) return E_INVALIDARG;

    auto* mon = new (std::nothrow) XPackageInstallationMonitor();
    if (!mon) return E_OUTOFMEMORY;
    mon->package_id = packageIdentifier;
    mon->interval_ms = minimumUpdateIntervalMs;
    mon->queue = queue;
    (void)package_root(&mon->root); // 拿不到就算了：监控壳不依赖它
    (void)selectorCount;
    (void)selectors;

    *installationMonitor = mon;
    xbgtasix::logf("[xgamert] CreateInstallationMonitor(%s, %u selectors, %u ms) -> %p",
                   packageIdentifier, selectorCount, minimumUpdateIntervalMs,
                   static_cast<void*>(mon));
    return S_OK;
}

STDAPI_(void) XPackageCloseInstallationMonitorHandle(
    XPackageInstallationMonitorHandle installationMonitor) noexcept
{
    XB_TRACE_ENTRY("XPackageCloseInstallationMonitorHandle", installationMonitor, 0ULL, 0ULL, 0ULL);
    if (!installationMonitor) return; // GDK 允许传空
    {
        std::lock_guard<std::mutex> lock(installationMonitor->mu);
        installationMonitor->regs.clear();
    }
    delete installationMonitor;
}

// STDAPI_(void)：没有返回值，也必须把整个结构体写干净，否则调用方读到的是栈垃圾。
STDAPI_(void) XPackageGetInstallationProgress(
    XPackageInstallationMonitorHandle installationMonitor,
    XPackageInstallationProgress* progress) noexcept
{
    XB_TRACE_ENTRY("XPackageGetInstallationProgress", installationMonitor, progress, 0ULL, 0ULL);
    (void)installationMonitor; // 壳：不跟踪真实安装
    if (!progress) return;
    std::memset(progress, 0, sizeof(*progress));
    // 头文件里的字段是 completed（不是某些文档写的 isComplete）
    progress->completed = true;
}

STDAPI_(bool) XPackageUpdateInstallationMonitor(
    XPackageInstallationMonitorHandle installationMonitor) noexcept
{
    XB_TRACE_ENTRY("XPackageUpdateInstallationMonitor", installationMonitor, 0ULL, 0ULL, 0ULL);
    (void)installationMonitor;
    return true; // "有变化"：让引擎立刻去读 GetInstallationProgress
}

STDAPI XPackageRegisterInstallationProgressChanged(
    XPackageInstallationMonitorHandle installationMonitor,
    void* context,
    XPackageInstallationProgressCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept
{
    XB_TRACE_ENTRY("XPackageRegisterInstallationProgressChanged", installationMonitor, context, callback, token);
    if (!installationMonitor || !callback || !token) return E_INVALIDARG;
    XPackageInstallationMonitor::Reg reg{};
    reg.token = g_next_token.fetch_add(1);
    reg.context = context;
    reg.callback = callback;
    {
        std::lock_guard<std::mutex> lock(installationMonitor->mu);
        installationMonitor->regs.push_back(reg);
    }
    token->token = reg.token;
    return S_OK;
}

STDAPI_(bool) XPackageUnregisterInstallationProgressChanged(
    XPackageInstallationMonitorHandle installationMonitor,
    XTaskQueueRegistrationToken token,
    bool wait) noexcept
{
    XB_TRACE_ENTRY("XPackageUnregisterInstallationProgressChanged", installationMonitor, token, wait, 0ULL);
    (void)wait; // 本实现回调从不排队，没有需要等待的在途调用
    if (!installationMonitor) return false;
    std::lock_guard<std::mutex> lock(installationMonitor->mu);
    for (auto it = installationMonitor->regs.begin(); it != installationMonitor->regs.end(); ++it) {
        if (it->token == token.token) {
            installationMonitor->regs.erase(it);
            return true;
        }
    }
    // 注销一个没注册过的 token 不视为错误（与 xgameruntime.cpp 的注销语义一致）
    return true;
}

// ---------------- D. 安装 / 卸载（完成异步，但明确 E_NOTIMPL） ----------------

STDAPI XPackageInstallChunksAsync(
    const char* packageIdentifier,
    uint32_t selectorCount,
    XPackageChunkSelector* selectors,
    uint32_t minimumUpdateIntervalMs,
    bool suppressUserConfirmation,
    XAsyncBlock* asyncBlock) noexcept
{
    XB_TRACE_ENTRY("XPackageInstallChunksAsync", packageIdentifier, selectorCount, selectors, minimumUpdateIntervalMs);
    if (!asyncBlock) return E_INVALIDARG;
    (void)selectors;
    xbgtasix::logf("[xgamert] InstallChunksAsync(%s, %u selectors, %u ms, suppressUi=%d)："
                   "本实现无安装器 -> 完成异步并返回 E_NOTIMPL",
                   packageIdentifier ? packageIdentifier : "(null)", selectorCount,
                   minimumUpdateIntervalMs, suppressUserConfirmation ? 1 : 0);
    complete_pkg_async(asyncBlock, E_NOTIMPL);
    return S_OK;
}

STDAPI XPackageInstallChunksResult(
    XAsyncBlock* asyncBlock,
    XPackageInstallationMonitorHandle* installationMonitor) noexcept
{
    XB_TRACE_ENTRY("XPackageInstallChunksResult", asyncBlock, installationMonitor, 0ULL, 0ULL);
    if (!asyncBlock) return E_INVALIDARG;
    if (installationMonitor) *installationMonitor = nullptr; // 失败时绝不留野句柄
    // 宿主状态表由 XAsyncRun 写入；未完成时这里是 E_PENDING。
    return XAsyncGetStatus(asyncBlock, false);
}

STDAPI XPackageUninstallChunks(
    const char* packageIdentifier,
    uint32_t selectorCount,
    XPackageChunkSelector* selectors) noexcept
{
    XB_TRACE_ENTRY("XPackageUninstallChunks", packageIdentifier, selectorCount, selectors, 0ULL);
    (void)selectors;
    xbgtasix::logf("[xgamert] UninstallChunks(%s, %u selectors) -> E_NOTIMPL（不删用户目录）",
                   packageIdentifier ? packageIdentifier : "(null)", selectorCount);
    return E_NOTIMPL;
}

STDAPI XPackageEstimateDownloadSize(
    const char* packageIdentifier,
    uint32_t selectorCount,
    XPackageChunkSelector* selectors,
    uint64_t* downloadSize,
    bool* shouldPresentUserConfirmation) noexcept
{
    XB_TRACE_ENTRY("XPackageEstimateDownloadSize", packageIdentifier, selectorCount, selectors, downloadSize);
    (void)selectors;
    // 输出参数先写干净：调用方可能在 FAILED 之后仍然读它们
    if (downloadSize) *downloadSize = 0;
    if (shouldPresentUserConfirmation) *shouldPresentUserConfirmation = false;
    if (!downloadSize) return E_INVALIDARG;
    xbgtasix::logf("[xgamert] EstimateDownloadSize(%s, %u selectors) -> E_NOTIMPL",
                   packageIdentifier ? packageIdentifier : "(null)", selectorCount);
    return E_NOTIMPL;
}

// ---------------- E. 包枚举（用包根目录合成一个条目） ----------------

STDAPI XPackageEnumeratePackages(
    XPackageKind kind,
    XPackageEnumerationScope scope,
    void* context,
    XPackageEnumerationCallback* callback) noexcept
{
    XB_TRACE_ENTRY("XPackageEnumeratePackages", kind, scope, context, callback);
    (void)scope; // 本实现只有一个包，ThisOnly / ThisAndRelated / ThisPublisher 结果相同
    if (!callback) return E_INVALIDARG;

    std::string root;
    if (!package_root(&root)) {
        xbgtasix::logf("[xgamert] EnumeratePackages: 无 XBGTASIX_PACKAGE_ROOT -> E_NOTFOUND");
        return E_NOTFOUND;
    }

    // 包标识符：优先用运行时读到的 MicrosoftGame.Config Identity（与 XPackageMount 可互认），
    // 读不到就退回 "*"（XPackageMount 对 "*" 直接映射包根目录）。
    char idbuf[XPACKAGE_IDENTIFIER_MAX_LENGTH] = {};
    std::string id = "*";
    if (SUCCEEDED(XPackageGetCurrentProcessPackageIdentifier(sizeof(idbuf), idbuf)) && idbuf[0])
        id = idbuf;

    // 显示名用包目录的最后一段（也是人最容易认的名字）
    const size_t slash = root.find_last_of("\\/");
    std::string leaf = slash == std::string::npos ? root : root.substr(slash + 1);
    if (leaf.empty()) leaf = id;

    // 结构体里全是 const char*（不是定长数组），字符串必须在回调期间保持有效。
    XVersion version{};
    version.Value = 0;

    XPackageDetails details{};
    details.packageIdentifier = id.c_str();
    details.version = version;
    details.kind = kind;
    details.displayName = leaf.c_str();
    details.description = "";
    details.publisher = "";
    details.storeId = "";
    details.installing = false;
    details.index = 0;
    details.count = 1; // 只有一个条目
    details.ageRestricted = false;
    details.titleId = "";

    // 回调返回 false = 调用方停止枚举
    callback(context, &details);
    xbgtasix::logf("[xgamert] EnumeratePackages: 合成条目 id=%s root=%s", id.c_str(), root.c_str());
    return S_OK;
}
