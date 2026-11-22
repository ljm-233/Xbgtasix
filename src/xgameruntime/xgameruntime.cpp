// xgameruntime.dll shim — GDK 运行时入口
//
// 签名一律来自 GDK 真头文件（gdk/include_clean），ABI 由编译器保证。
// 已实现：
//   1) 生命周期  —— 真的去读 MicrosoftGame.Config（TitleId / StoreId / MSAAppId）
//   2) 任务队列  —— 线程池 + 手动态 + 延迟提交 + 终止
//   3) 用户      —— 本地模拟用户
//   4) XAsync    —— GetStatus / Cancel 的最小闭环
// 其余导出是生成桩：一被调用就打印名字并退出。
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "XAsync.h"
#include "XGame.h"
#include "XGameRuntime.h"
#include "XStore.h"
#include "XSystem.h"
#include "XGameSaveFiles.h"
#include "XGameActivation.h"
#include "XGameRuntimeFeature.h"
#include "XGameUI.h"
#include "XPackage.h"
#include "XPersistentLocalStorage.h"
#include "XTaskQueue.h"
#include "XUser.h"

#include "xbgtasix/trace.h"

namespace {

// ---------------- 运行时状态 / 游戏配置 ----------------

struct RuntimeState {
    uint32_t title_id = 0;
    std::string store_id;
    std::string msa_app_id;
    std::string config_path;
    std::string package_id;        // MicrosoftGame.Config 的 <Identity Name="...">
    std::string package_root;      // 包内容所在目录（XBGTASIX_PACKAGE_ROOT 可覆盖）
    bool config_loaded = false;
};
RuntimeState g_rt;

std::string read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string find_tag(const std::string& xml, const char* tag) {
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const size_t a = xml.find(open);
    if (a == std::string::npos) return {};
    const size_t b = xml.find(close, a + open.size());
    if (b == std::string::npos) return {};
    std::string v = xml.substr(a + open.size(), b - a - open.size());
    // 去空白
    const size_t s = v.find_first_not_of(" \t\r\n");
    const size_t e = v.find_last_not_of(" \t\r\n");
    if (s == std::string::npos) return {};
    return v.substr(s, e - s + 1);
}

// 优先用 XBGTASIX_CONFIG 指定的路径，否则找 exe 同目录的 MicrosoftGame.Config
void load_game_config() {
    char path[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("XBGTASIX_CONFIG", path, MAX_PATH) == 0) {
        char exe[MAX_PATH] = {0};
        GetModuleFileNameA(GetModuleHandleA(nullptr), exe, MAX_PATH);
        if (char* slash = strrchr(exe, '\\')) *(slash + 1) = 0;
        _snprintf(path, MAX_PATH, "%sMicrosoftGame.Config", exe);
    }
    g_rt.config_path = path;
    const std::string xml = read_file(path);
    if (xml.empty()) {
        xbgtasix::logf("[xgamert] 未找到游戏配置: %s", path);
        return;
    }
    // <Identity Name="..." Publisher="..." Version="..." /> 是自闭合标签，单独取属性
    {
        const size_t idpos = xml.find("<Identity");
        if (idpos != std::string::npos) {
            const size_t endpos = xml.find('>', idpos);
            const std::string tag = xml.substr(idpos, endpos == std::string::npos ? std::string::npos
                                                                                  : endpos - idpos);
            const size_t namepos = tag.find("Name=\"");
            if (namepos != std::string::npos) {
                const size_t vstart = namepos + 6;
                const size_t vend = tag.find('"', vstart);
                if (vend != std::string::npos) g_rt.package_id = tag.substr(vstart, vend - vstart);
            }
        }
    }
    g_rt.store_id = find_tag(xml, "StoreId");
    g_rt.msa_app_id = find_tag(xml, "MSAAppId");
    const std::string title = find_tag(xml, "TitleId");
    if (!title.empty()) {
        g_rt.title_id = static_cast<uint32_t>(std::stoul(title, nullptr, 16));
    }
    g_rt.config_loaded = true;
    // 包内容目录：优先环境变量，否则用 config 所在目录
    {
        char root[MAX_PATH] = {0};
        if (GetEnvironmentVariableA("XBGTASIX_PACKAGE_ROOT", root, MAX_PATH) > 0) {
            g_rt.package_root = root;
        } else {
            std::string p = path;
            const size_t slash = p.find_last_of("\\/");
            g_rt.package_root = slash == std::string::npos ? "." : p.substr(0, slash);
        }
    }
    xbgtasix::logf("[xgamert] 配置已读: %s", path);
    xbgtasix::logf("[xgamert]   包身份=%s", g_rt.package_id.empty() ? "?" : g_rt.package_id.c_str());
    xbgtasix::logf("[xgamert]   包目录=%s", g_rt.package_root.c_str());
    xbgtasix::logf("[xgamert]   TitleId=0x%08lX StoreId=%s MSAAppId=%s",
                   static_cast<unsigned long>(g_rt.title_id),
                   g_rt.store_id.empty() ? "?" : g_rt.store_id.c_str(),
                   g_rt.msa_app_id.empty() ? "?" : g_rt.msa_app_id.c_str());
}

// ---------------- 持久化本地存储 (PLS) ----------------

std::mutex g_pls_mu;
std::string g_pls_path;
bool g_pls_ready = false;

uint64_t pls_total_bytes() {
    // MicrosoftGame.Config 里的 <PersistentLocalStorage><SizeMB>2048</SizeMB>
    const std::string xml = read_file(g_rt.config_path.c_str());
    const std::string mb = find_tag(xml, "SizeMB");
    if (!mb.empty()) return std::stoull(mb) * 1024ull * 1024ull;
    return 256ull * 1024ull * 1024ull;   // 没配置时的保守默认
}

uint64_t dir_used_bytes(const std::string& dir) {
    uint64_t total = 0;
    WIN32_FIND_DATAA fd{};
    const std::string pattern = dir + "\\*";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0) continue;
        const std::string child = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += dir_used_bytes(child);
        } else {
            LARGE_INTEGER sz{};
            sz.LowPart = fd.nFileSizeLow;
            sz.HighPart = static_cast<LONG>(fd.nFileSizeHigh);
            total += static_cast<uint64_t>(sz.QuadPart);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return total;
}

// 目录形如 %LOCALAPPDATA%\Xbgtasix\<TitleId>
// 逐级创建目录，最后验证。
// 注意：CreateDirectoryA 不建中间层，必须从盘符起每一级都建。
// 曾经写成从 base 之后开始扫，把 "\\Xbgtasix" 这一级整个跳过 -> ERROR_PATH_NOT_FOUND(3)；
// 同时又没检查返回值，日志照打"成功"，把问题误判成"沙箱写不了"（见 docs/env-gotchas.md 第 1、2 条）
bool ensure_dir_tree(const std::string& path) {
    if (path.size() < 4) return false;
    std::string cur;
    size_t i = 0;
    if (path.size() > 1 && path[1] == ':') {   // 盘符前缀 "C:"
        cur = path.substr(0, 2);
        i = 2;
    }
    for (; i < path.size(); ++i) {
        const char c = path[i];
        if (c == '\\' || c == '/') {
            if (cur.size() > (cur.size() > 1 && cur[1] == ':' ? 2u : 0u)) {
                CreateDirectoryA(cur.c_str(), nullptr);
            }
        }
        cur.push_back(c);
    }
    if (!cur.empty()) CreateDirectoryA(cur.c_str(), nullptr);

    const DWORD attr = GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        xbgtasix::logf("[xgamert] 目录创建失败（%lu）: %s", GetLastError(), path.c_str());
        return false;
    }
    return true;
}

bool ensure_pls_path() {
    std::lock_guard<std::mutex> lock(g_pls_mu);
    if (g_pls_ready) return true;
    // XBGTASIX_PLS_ROOT 用于便携安装，也用于受限环境（沙箱只放行编译产物写工作区内）
    // 和测试时隔离存档目录，避免污染真实 %LOCALAPPDATA%
    char base[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("XBGTASIX_PLS_ROOT", base, MAX_PATH) == 0) {
        if (GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH) == 0) return false;
        _snprintf(base + strlen(base), MAX_PATH - strlen(base), "\\Xbgtasix");
    }
    char buf[MAX_PATH] = {0};
    _snprintf(buf, MAX_PATH, "%s\\%08lX", base, static_cast<unsigned long>(g_rt.title_id));
    if (!ensure_dir_tree(buf)) return false;
    g_pls_path = buf;
    g_pls_ready = true;
    xbgtasix::logf("[xgamert] PLS 目录: %s", buf);
    return true;
}

// ---------------- 异步完成表 ----------------

struct AsyncState {
    bool done = false;
    HRESULT status = S_OK;
    size_t result_size = 0;
    std::vector<unsigned char> result;
};

std::mutex g_async_mu;
std::unordered_map<XAsyncBlock*, AsyncState> g_async;

// 完成通知：标记完成 + 调回调（GDK 语义：回调在 completion 端口执行）
void complete_async(XAsyncBlock* a, HRESULT status = S_OK) {
    {
        std::lock_guard<std::mutex> lock(g_async_mu);
        AsyncState& st = g_async[a];
        st.done = true;
        st.status = status;
    }
    if (a->callback) a->callback(a);
}

bool is_async_done(XAsyncBlock* a) {
    std::lock_guard<std::mutex> lock(g_async_mu);
    auto it = g_async.find(a);
    return it != g_async.end() && it->second.done;
}

HRESULT async_status(XAsyncBlock* a) {
    std::lock_guard<std::mutex> lock(g_async_mu);
    auto it = g_async.find(a);
    return it == g_async.end() ? E_PENDING : it->second.status;
}

// ---------------- 任务队列 ----------------

struct TaskEntry {
    XTaskQueuePort port;
    void* context;
    XTaskQueueCallback* callback;
};

struct WaiterReg {
    XTaskQueuePort port;
    HANDLE wait_handle;
};

struct MonitorReg {
    XTaskQueuePort port;
    void* context;
    XTaskQueueMonitorCallback* callback;
};

struct TaskQueueState {
    std::atomic<int> refcount{1};
    std::vector<WaiterReg> waiters;
    std::vector<MonitorReg> monitors;
    XTaskQueueDispatchMode work_mode = XTaskQueueDispatchMode::ThreadPool;
    XTaskQueueDispatchMode comp_mode = XTaskQueueDispatchMode::ThreadPool;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<TaskEntry> work;
    std::deque<TaskEntry> comp;
    std::atomic<bool> stopped{false};
    std::thread worker_work;
    std::thread worker_comp;

    std::deque<TaskEntry>& queue_for(XTaskQueuePort p) {
        return p == XTaskQueuePort::Completion ? comp : work;
    }

    void pump(XTaskQueuePort port) {
        for (;;) {
            TaskEntry e{};
            {
                std::unique_lock<std::mutex> lock(mu);
                cv.wait(lock, [&] { return stopped.load() || !queue_for(port).empty(); });
                if (queue_for(port).empty()) {
                    if (stopped.load()) return;
                    continue;
                }
                e = queue_for(port).front();
                queue_for(port).pop_front();
            }
            if (e.callback) e.callback(e.context, false);
        }
    }

    void start_workers() {
        // 只有线程池模式才起工作线程；Manual 必须由 XTaskQueueDispatch 驱动，
        // Immediate 在提交时同步执行。之前给所有模式都起了线程，
        // 导致 Manual 队列的回调被后台线程"偷偷"跑掉（t10 抓到）。
        const bool work_threaded = work_mode == XTaskQueueDispatchMode::ThreadPool ||
                                   work_mode == XTaskQueueDispatchMode::SerializedThreadPool;
        const bool comp_threaded = comp_mode == XTaskQueueDispatchMode::ThreadPool ||
                                   comp_mode == XTaskQueueDispatchMode::SerializedThreadPool;
        if (work_threaded) worker_work = std::thread([this] { pump(XTaskQueuePort::Work); });
        if (comp_threaded) worker_comp = std::thread([this] { pump(XTaskQueuePort::Completion); });
    }

    void shutdown(bool wait) {
        stopped.store(true);
        cv.notify_all();
        if (!wait) return;
        if (worker_work.joinable()) worker_work.join();
        if (worker_comp.joinable()) worker_comp.join();
    }
};

// ---------------- 用户 ----------------

struct UserState {
    uint64_t id = 0;
    std::atomic<int> refs{1};
};

std::mutex g_user_mu;
std::unordered_map<XAsyncBlock*, XUserHandle> g_pending_user;
std::atomic<uint64_t> g_next_user_id{1};

}  // namespace

extern "C" {

// ---- 生命周期 ----

STDAPI XGameRuntimeInitialize() {
    load_game_config();
    xbgtasix::logf("[xgamert] XGameRuntimeInitialize()");
    return S_OK;
}

STDAPI XGameRuntimeInitializeWithOptions(const XGameRuntimeOptions* options) {
    xbgtasix::logf("[xgamert] XGameRuntimeInitializeWithOptions(source=%d)",
                   options ? static_cast<int>(options->gameConfigSource) : -1);
    return XGameRuntimeInitialize();
}

STDAPI_(void) XGameRuntimeUninitialize() {
    xbgtasix::logf("[xgamert] XGameRuntimeUninitialize()");
}

// ---- 任务队列 ----

STDAPI XTaskQueueCreate(XTaskQueueDispatchMode workDispatchMode,
                        XTaskQueueDispatchMode completionDispatchMode,
                        XTaskQueueHandle* queue) noexcept {
    if (!queue) return E_INVALIDARG;
    auto* q = new (std::nothrow) TaskQueueState();
    if (!q) return E_OUTOFMEMORY;
    q->work_mode = workDispatchMode;
    q->comp_mode = completionDispatchMode;
    q->start_workers();
    *queue = reinterpret_cast<XTaskQueueHandle>(q);
    xbgtasix::logf("[xgamert] XTaskQueueCreate(work=%u, completion=%u) -> %p",
                   static_cast<unsigned>(workDispatchMode), static_cast<unsigned>(completionDispatchMode),
                   static_cast<void*>(q));
    return S_OK;
}

STDAPI XTaskQueueSubmitCallback(XTaskQueueHandle queue, XTaskQueuePort port, void* context,
                               XTaskQueueCallback* callback) noexcept {
    if (!queue || !callback) return E_INVALIDARG;
    auto* q = reinterpret_cast<TaskQueueState*>(queue);
    std::vector<MonitorReg> pending_monitors;
    const XTaskQueueDispatchMode mode = (port == XTaskQueuePort::Completion) ? q->comp_mode : q->work_mode;
    if (mode == XTaskQueueDispatchMode::Immediate) {
        callback(context, false);
        return S_OK;
    }
    {
        std::lock_guard<std::mutex> lock(q->mu);
        q->queue_for(port).push_back(TaskEntry{port, context, callback});
        // 有活干了：唤醒所有登记在該端口上的 waiter，并通知 monitor
        for (const auto& w : q->waiters) {
            if (w.port == port && w.wait_handle) SetEvent(w.wait_handle);
        }
        pending_monitors = q->monitors;
    }
    q->cv.notify_all();
    for (const auto& m : pending_monitors) {
        if (m.callback) m.callback(m.context, queue, port);
    }
    return S_OK;
}

STDAPI XTaskQueueSubmitDelayedCallback(XTaskQueueHandle queue, XTaskQueuePort port, uint32_t delayMs,
                                      void* context, XTaskQueueCallback* callback) noexcept {
    if (!queue || !callback) return E_INVALIDARG;
    // 简化实现：起一个一次性线程，到点再投递（真实 GDK 是队列内定时器）
    std::thread([queue, port, delayMs, context, callback] {
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        XTaskQueueSubmitCallback(queue, port, context, callback);
    }).detach();
    return S_OK;
}

STDAPI_(bool) XTaskQueueDispatch(XTaskQueueHandle queue, XTaskQueuePort port,
                                uint32_t timeoutInMs) noexcept {
    if (!queue) return false;
    auto* q = reinterpret_cast<TaskQueueState*>(queue);
    TaskEntry e{};
    {
        std::unique_lock<std::mutex> lock(q->mu);
        auto ready = [&] { return q->stopped.load() || !q->queue_for(port).empty(); };
        if (timeoutInMs == 0 ? !ready() : !q->cv.wait_for(lock, std::chrono::milliseconds(timeoutInMs), ready))
            return false;
        if (q->queue_for(port).empty()) return false;
        e = q->queue_for(port).front();
        q->queue_for(port).pop_front();
    }
    if (e.callback) e.callback(e.context, false);
    return true;
}

STDAPI XTaskQueueTerminate(XTaskQueueHandle queue, bool wait, void* context,
                           XTaskQueueTerminatedCallback* callback) noexcept {
    if (!queue) return E_INVALIDARG;
    auto* q = reinterpret_cast<TaskQueueState*>(queue);
    q->shutdown(wait);
    if (callback) callback(context);
    xbgtasix::logf("[xgamert] XTaskQueueTerminate(wait=%d)", wait ? 1 : 0);
    return S_OK;
}

STDAPI_(void) XTaskQueueCloseHandle(XTaskQueueHandle queue) noexcept {
    if (!queue) return;
    auto* q = reinterpret_cast<TaskQueueState*>(queue);
    if (q->refcount.fetch_sub(1) > 1) return;   // DuplicateHandle 后要等引用计数归零
    q->shutdown(true);
    delete q;
}

STDAPI XTaskQueueDuplicateHandle(XTaskQueueHandle queueHandle, XTaskQueueHandle* duplicated) noexcept {
    if (!queueHandle || !duplicated) return E_INVALIDARG;
    auto* q = reinterpret_cast<TaskQueueState*>(queueHandle);
    q->refcount.fetch_add(1);
    *duplicated = queueHandle;   // 同一实例 + 引用计数，语义等价于"多一个句柄"
    return S_OK;
}

// ---- waiter / monitor ----

STDAPI XTaskQueueRegisterWaiter(XTaskQueueHandle queue, XTaskQueuePort port, HANDLE waitHandle,
                                void* callbackContext, XTaskQueueCallback* callback,
                                XTaskQueueRegistrationToken* token) noexcept {
    if (!queue || !waitHandle || !token) return E_INVALIDARG;
    (void)callbackContext;   // 真 GDK 里 waiter 的回调由调用方线程执行，我们只负责 signal
    (void)callback;
    auto* q = reinterpret_cast<TaskQueueState*>(queue);
    {
        std::lock_guard<std::mutex> lock(q->mu);
        q->waiters.push_back(WaiterReg{port, waitHandle});
        token->token = q->waiters.size();
    }
    xbgtasix::logf("[xgamert] RegisterWaiter(port=%u, event=%p)", static_cast<unsigned>(port),
                   waitHandle);
    return S_OK;
}

STDAPI_(void) XTaskQueueUnregisterWaiter(XTaskQueueHandle queue,
                                         XTaskQueueRegistrationToken token) noexcept {
    if (!queue) return;
    auto* q = reinterpret_cast<TaskQueueState*>(queue);
    std::lock_guard<std::mutex> lock(q->mu);
    const size_t idx = static_cast<size_t>(token.token);
    if (idx >= 1 && idx <= q->waiters.size()) q->waiters.erase(q->waiters.begin() + (idx - 1));
}

STDAPI XTaskQueueRegisterMonitor(XTaskQueueHandle queue, void* callbackContext,
                                 XTaskQueueMonitorCallback* callback,
                                 XTaskQueueRegistrationToken* token) noexcept {
    if (!queue || !callback || !token) return E_INVALIDARG;
    auto* q = reinterpret_cast<TaskQueueState*>(queue);
    {
        std::lock_guard<std::mutex> lock(q->mu);
        q->monitors.push_back(MonitorReg{XTaskQueuePort::Work, callbackContext, callback});
        token->token = q->monitors.size();
    }
    xbgtasix::logf("[xgamert] RegisterMonitor（监听整个队列）");
    return S_OK;
}

STDAPI_(void) XTaskQueueUnregisterMonitor(XTaskQueueHandle queue,
                                          XTaskQueueRegistrationToken token) noexcept {
    if (!queue) return;
    auto* q = reinterpret_cast<TaskQueueState*>(queue);
    std::lock_guard<std::mutex> lock(q->mu);
    const size_t idx = static_cast<size_t>(token.token);
    if (idx >= 1 && idx <= q->monitors.size()) q->monitors.erase(q->monitors.begin() + (idx - 1));
}

// ---- 进程级默认队列 ----

std::mutex g_proc_queue_mu;
XTaskQueueHandle g_proc_queue = nullptr;

STDAPI_(void) XTaskQueueSetCurrentProcessTaskQueue(XTaskQueueHandle queue) noexcept {
    std::lock_guard<std::mutex> lock(g_proc_queue_mu);
    if (queue) reinterpret_cast<TaskQueueState*>(queue)->refcount.fetch_add(1);
    g_proc_queue = queue;
    xbgtasix::logf("[xgamert] SetCurrentProcessTaskQueue(%p)", static_cast<void*>(queue));
}

STDAPI_(bool) XTaskQueueGetCurrentProcessTaskQueue(XTaskQueueHandle* queue) noexcept {
    std::lock_guard<std::mutex> lock(g_proc_queue_mu);
    if (!g_proc_queue) return false;
    if (queue) *queue = g_proc_queue;
    return true;
}

// ---- XAsync ----

STDAPI XAsyncGetStatus(XAsyncBlock* asyncBlock, bool wait) noexcept {
    if (!asyncBlock) return E_INVALIDARG;
    if (is_async_done(asyncBlock)) return async_status(asyncBlock);
    if (!wait) return E_PENDING;
    for (int i = 0; i < 5000 && !is_async_done(asyncBlock); ++i) Sleep(1);
    return is_async_done(asyncBlock) ? async_status(asyncBlock) : E_PENDING;
}

STDAPI_(void) XAsyncCancel(XAsyncBlock* asyncBlock) noexcept {
    if (!asyncBlock) return;
    complete_async(asyncBlock, E_ABORT);
}

// 通用异步：把 work 丢到线程里跑，完成时按 GDK 语义回调
STDAPI XAsyncRun(XAsyncBlock* asyncBlock, XAsyncWork* work) noexcept {
    if (!asyncBlock || !work) return E_INVALIDARG;
    {
        std::lock_guard<std::mutex> lock(g_async_mu);
        g_async[asyncBlock] = AsyncState{};
    }
    std::thread([asyncBlock, work] {
        const HRESULT hr = work(asyncBlock);
        complete_async(asyncBlock, SUCCEEDED(hr) ? S_OK : hr);
    }).detach();
    return S_OK;
}

STDAPI XAsyncGetResultSize(XAsyncBlock* asyncBlock, size_t* bufferSize) noexcept {
    if (!asyncBlock || !bufferSize) return E_INVALIDARG;
    // 注意：只加一次锁。std::mutex 不可重入，持锁再调 async_status() 会自死锁（踩过，60 秒超时）
    std::lock_guard<std::mutex> lock(g_async_mu);
    auto it = g_async.find(asyncBlock);
    if (it == g_async.end() || !it->second.done) return E_PENDING;
    *bufferSize = it->second.result_size;
    return it->second.status;
}

// ---- 用户（本地模拟） ----

STDAPI XUserAddAsync(XUserAddOptions options, XAsyncBlock* async) noexcept {
    (void)options;
    if (!async) return E_INVALIDARG;
    auto* u = new (std::nothrow) UserState();
    if (!u) return E_OUTOFMEMORY;
    u->id = g_next_user_id.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(g_user_mu);
        g_pending_user[async] = reinterpret_cast<XUserHandle>(u);
    }
    xbgtasix::logf("[xgamert] XUserAddAsync -> 本地用户 id=%llu",
                   static_cast<unsigned long long>(u->id));
    complete_async(async);
    return S_OK;
}

STDAPI XUserAddResult(XAsyncBlock* async, XUserHandle* newUser) noexcept {
    if (!async || !newUser) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_user_mu);
    auto it = g_pending_user.find(async);
    if (it == g_pending_user.end()) return E_UNEXPECTED;
    *newUser = it->second;
    g_pending_user.erase(it);
    return S_OK;
}

STDAPI XUserGetId(XUserHandle user, uint64_t* userId) noexcept {
    if (!user || !userId) return E_INVALIDARG;
    *userId = reinterpret_cast<UserState*>(user)->id;
    return S_OK;
}

// 本地模拟用户：没有微软账号、没有网络，但查询类接口可以有稳定语义
STDAPI XUserGetGamertag(XUserHandle user, XUserGamertagComponent component, size_t gamertagSize,
                        char* gamertag, size_t* gamertagUsed) noexcept {
    if (!user || !gamertag) return E_INVALIDARG;
    (void)component;   // 只是显示名，不同组件给同一份
    const char* name = "Player";
    const size_t need = std::strlen(name) + 1;
    if (gamertagUsed) *gamertagUsed = need;
    if (gamertagSize < need) return E_NOT_SUFFICIENT_BUFFER;
    std::memcpy(gamertag, name, need);
    return S_OK;
}

STDAPI XUserGetLocalId(XUserHandle user, XUserLocalId* localId) noexcept {
    if (!user || !localId) return E_INVALIDARG;
    localId->value = reinterpret_cast<UserState*>(user)->id;
    return S_OK;
}

STDAPI XUserGetIsGuest(XUserHandle user, bool* isGuest) noexcept {
    if (!user || !isGuest) return E_INVALIDARG;
    *isGuest = false;
    return S_OK;
}

STDAPI XUserGetState(XUserHandle user, XUserState* state) noexcept {
    if (!user || !state) return E_INVALIDARG;
    *state = XUserState::SignedIn;   // 本地用户永远是"已登录"
    return S_OK;
}

STDAPI XUserFindUserById(uint64_t userId, XUserHandle* user) noexcept {
    if (!user) return E_INVALIDARG;
    if (userId == 0) return E_INVALIDARG;
    auto* u = new (std::nothrow) UserState();
    if (!u) return E_OUTOFMEMORY;
    u->id = userId;
    *user = reinterpret_cast<XUserHandle>(u);
    return S_OK;
}

STDAPI_(void) XUserCloseHandle(XUserHandle user) noexcept {
    if (!user) return;
    auto* u = reinterpret_cast<UserState*>(user);
    if (u->refs.fetch_sub(1) > 1) return;   // DuplicateHandle 过的不释放
    delete u;
}

// ---- XUser 本地可实现的查询/句柄管理（签名全部对照 XUser.h） ----

STDAPI XUserGetMaxUsers(uint32_t* maxUsers) noexcept {
    if (!maxUsers) return E_INVALIDARG;
    *maxUsers = 8;
    return S_OK;
}

STDAPI_(int32_t) XUserCompare(XUserHandle user1, XUserHandle user2) noexcept {
    if (user1 == user2) return 0;
    if (!user1) return -1;
    if (!user2) return 1;
    const uint64_t a = reinterpret_cast<UserState*>(user1)->id;
    const uint64_t b = reinterpret_cast<UserState*>(user2)->id;
    return a < b ? -1 : (a > b ? 1 : 0);
}

STDAPI XUserDuplicateHandle(XUserHandle user, XUserHandle* duplicatedUser) noexcept {
    if (!user || !duplicatedUser) return E_INVALIDARG;
    reinterpret_cast<UserState*>(user)->refs.fetch_add(1);
    *duplicatedUser = user;
    return S_OK;
}

STDAPI_(bool) XUserIsStoreUser(XUserHandle user) noexcept {
    return user != nullptr;
}

STDAPI_(bool) XUserIsSignOutPresent() noexcept {
    return false;   // 本地用户不会被系统登出
}

STDAPI XUserGetAgeGroup(XUserHandle user, XUserAgeGroup* ageGroup) noexcept {
    if (!user || !ageGroup) return E_INVALIDARG;
    *ageGroup = XUserAgeGroup::Adult;
    return S_OK;
}

STDAPI XUserCheckPrivilege(XUserHandle user, XUserPrivilegeOptions options, XUserPrivilege privilege,
                           bool* hasPrivilege, XUserPrivilegeDenyReason* reason) noexcept {
    (void)options;
    (void)privilege;
    if (!user || !hasPrivilege) return E_INVALIDARG;
    *hasPrivilege = true;   // 本地用户拥有全部特权
    if (reason) *reason = static_cast<XUserPrivilegeDenyReason>(0);
    return S_OK;
}

STDAPI XUserFindUserByLocalId(XUserLocalId localId, XUserHandle* user) noexcept {
    if (!user) return E_INVALIDARG;
    if (localId.value == 0) return E_INVALIDARG;
    auto* u = new (std::nothrow) UserState();
    if (!u) return E_OUTOFMEMORY;
    u->id = localId.value;
    *user = reinterpret_cast<XUserHandle>(u);
    return S_OK;
}

// 登录态变化事件：注册表保留，事件永不触发（PC 上没有系统级登录态）
std::mutex g_user_evt_mu;
uint64_t g_user_evt_next = 1;

STDAPI XUserRegisterForChangeEvent(XTaskQueueHandle queue, void* context,
                                   XUserChangeEventCallback* callback,
                                   XTaskQueueRegistrationToken* token) noexcept {
    (void)queue;
    (void)context;
    (void)callback;
    if (!token) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_user_evt_mu);
    token->token = g_user_evt_next++;
    return S_OK;
}

STDAPI_(bool) XUserUnregisterForChangeEvent(XTaskQueueRegistrationToken token,
                                            bool wait) noexcept {
    (void)token;
    (void)wait;
    return true;
}

STDAPI XUserSignOutAsync(XUserHandle user, XAsyncBlock* async) noexcept {
    if (!user || !async) return E_INVALIDARG;
    {
        std::lock_guard<std::mutex> lock(g_async_mu);
        g_async[async].done = true;
        g_async[async].status = S_OK;
    }
    if (async->callback) async->callback(async);
    return S_OK;
}

STDAPI XUserSignOutResult(XAsyncBlock* async) noexcept {
    if (!async) return E_INVALIDARG;
    return is_async_done(async) ? S_OK : E_PENDING;
}

// ---- 持久化本地存储 ----

STDAPI XPersistentLocalStorageGetPathSize(size_t* pathSize) noexcept {
    if (!pathSize) return E_INVALIDARG;
    if (!ensure_pls_path()) return E_FAIL;
    *pathSize = g_pls_path.size() + 1;
    return S_OK;
}

STDAPI XPersistentLocalStorageGetPath(size_t pathSize, char* path, size_t* pathUsed) noexcept {
    if (!path || pathSize == 0) return E_INVALIDARG;
    if (!ensure_pls_path()) return E_FAIL;
    const size_t need = g_pls_path.size() + 1;
    if (pathSize < need) return E_NOT_SUFFICIENT_BUFFER;
    std::memcpy(path, g_pls_path.c_str(), need);
    if (pathUsed) *pathUsed = need;
    return S_OK;
}

STDAPI XPersistentLocalStorageGetSpaceInfo(XPersistentLocalStorageSpaceInfo* info) noexcept {
    if (!info) return E_INVALIDARG;
    if (!ensure_pls_path()) return E_FAIL;
    const uint64_t total = pls_total_bytes();
    const uint64_t used = dir_used_bytes(g_pls_path);
    info->totalBytes = total;
    info->usedBytes = used;
    info->availableFreeBytes = total > used ? total - used : 0;
    info->totalFreeBytes = info->availableFreeBytes;
    return S_OK;
}

// ---- 包管理（XPackage） ----

struct MountInstance {
    std::string path;
};

// XAsyncBlock 里没有 result 字段（GDK 的原话是 internal-only），
// 所以挂载结果放旁路表，按 async 指针索引。
std::mutex g_mount_mu;
std::unordered_map<XAsyncBlock*, XPackageMountHandle> g_pending_mount;

STDAPI XPackageGetCurrentProcessPackageIdentifier(size_t bufferSize, char* buffer) noexcept {
    if (!buffer || bufferSize == 0) return E_INVALIDARG;
    const std::string& id = g_rt.package_id;
    if (id.empty()) return E_FAIL;
    if (bufferSize < id.size() + 1) return E_NOT_SUFFICIENT_BUFFER;
    std::memcpy(buffer, id.c_str(), id.size() + 1);
    return S_OK;
}

STDAPI_(bool) XPackageIsPackagedProcess() noexcept {
    // 只要读到了 MicrosoftGame.Config 就当成"打包进程"
    return g_rt.config_loaded;
}

STDAPI_(void) XPackageCloseMountHandle(XPackageMountHandle mount) noexcept {
    delete reinterpret_cast<MountInstance*>(mount);
}

STDAPI XPackageGetMountPathSize(XPackageMountHandle mount, size_t* pathSize) noexcept {
    if (!mount || !pathSize) return E_INVALIDARG;
    *pathSize = reinterpret_cast<MountInstance*>(mount)->path.size() + 1;
    return S_OK;
}

STDAPI XPackageGetMountPath(XPackageMountHandle mount, size_t pathSize, char* path) noexcept {
    if (!mount || !path || pathSize == 0) return E_INVALIDARG;
    const std::string& p = reinterpret_cast<MountInstance*>(mount)->path;
    if (pathSize < p.size() + 1) return E_NOT_SUFFICIENT_BUFFER;
    std::memcpy(path, p.c_str(), p.size() + 1);
    return S_OK;
}

// 挂载：把包标识符解析成真实目录。
// 主机上这是"挂载一个安装好的包"；PC 上等价于"这个包的根目录在哪"。
static HRESULT mount_impl(const char* packageId, XPackageMountHandle* mount) {
    if (!packageId || !mount) return E_INVALIDARG;
    std::string dir = g_rt.package_root;
    const std::string id(packageId);
    if (id != g_rt.package_id && id != "*" && !id.empty()) {
        // 允许挂载包根目录下的子目录（例如 DLC 包名 = 子目录名）
        const std::string candidate = g_rt.package_root + "\\" + id;
        const DWORD attr = GetFileAttributesA(candidate.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            xbgtasix::logf("[xgamert] XPackageMount(%s) 找不到 -> E_NOTFOUND", packageId);
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        dir = candidate;
    } else {
        dir = g_rt.package_root;
    }
    auto* inst = new (std::nothrow) MountInstance();
    if (!inst) return E_OUTOFMEMORY;
    inst->path = dir;
    *mount = reinterpret_cast<XPackageMountHandle>(inst);
    xbgtasix::logf("[xgamert] XPackageMount(%s) -> %s", packageId, dir.c_str());
    return S_OK;
}

STDAPI_(HRESULT STDAPICALLTYPE) XPackageMount(const char* packageIdentifier,
                                              XPackageMountHandle* mount) noexcept {
    return mount_impl(packageIdentifier, mount);
}

STDAPI XPackageMountWithUiAsync(const char* packageIdentifier, XAsyncBlock* async) noexcept {
    if (!async) return E_INVALIDARG;
    XPackageMountHandle handle = nullptr;
    const HRESULT hr = mount_impl(packageIdentifier, &handle);
    {
        std::lock_guard<std::mutex> lock(g_mount_mu);
        if (SUCCEEDED(hr)) g_pending_mount[async] = handle;
    }
    {
        std::lock_guard<std::mutex> lock(g_async_mu);
        g_async[async].done = true;
        g_async[async].status = hr;
    }
    if (async->callback) async->callback(async);
    return S_OK;
}

STDAPI XPackageMountWithUiResult(XAsyncBlock* async, XPackageMountHandle* mount) noexcept {
    if (!async || !mount) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_async_mu);
    auto it = g_async.find(async);
    if (it == g_async.end()) return E_UNEXPECTED;
    if (FAILED(it->second.status)) return it->second.status;
    std::lock_guard<std::mutex> mlock(g_mount_mu);
    auto mit = g_pending_mount.find(async);
    if (mit == g_pending_mount.end()) return E_UNEXPECTED;
    *mount = mit->second;
    g_pending_mount.erase(mit);
    return S_OK;
}

// ---- 存档（XGameSaveFiles：目录式，映射到 PLS 下的子目录） ----

std::mutex g_save_mu;
std::unordered_map<XAsyncBlock*, std::string> g_pending_save;

// configurationId 由游戏自定（例如 "slot1"），这里也当目录名用，需要过滤非法字符
static std::string sanitize(const std::string& in) {
    std::string out;
    for (char c : in) {
        const bool bad = c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
                         c == '"' || c == '<' || c == '>' || c == '|' || c < 0x20;
        out.push_back(bad ? '_' : c);
    }
    if (out.empty() || out == "." || out == "..") out = "default";
    return out;
}

static std::string save_folder_for(const char* configurationId) {
    if (!ensure_pls_path()) return {};
    const std::string dir = g_pls_path + "\\SaveData\\" +
                            sanitize(configurationId ? configurationId : "default");
    if (!ensure_dir_tree(dir)) return {};
    return dir;
}

STDAPI XGameSaveFilesGetFolderWithUiAsync(XUserHandle requestingUser, const char* configurationId,
                                          XAsyncBlock* async) noexcept {
    (void)requestingUser;   // 本地模拟用户，不校验
    if (!async || !configurationId) return E_INVALIDARG;
    const std::string dir = save_folder_for(configurationId);
    {
        std::lock_guard<std::mutex> lock(g_save_mu);
        if (!dir.empty()) g_pending_save[async] = dir;
    }
    {
        std::lock_guard<std::mutex> lock(g_async_mu);
        g_async[async].done = true;
        g_async[async].status = dir.empty() ? E_FAIL : S_OK;
    }
    xbgtasix::logf("[xgamert] XGameSaveFiles 文件夹(%s) -> %s", configurationId,
                   dir.empty() ? "(失败)" : dir.c_str());
    if (async->callback) async->callback(async);
    return S_OK;
}

STDAPI XGameSaveFilesGetFolderWithUiResult(XAsyncBlock* async, size_t folderSize,
                                           char* folderResult) noexcept {
    if (!async || !folderResult || folderSize == 0) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_save_mu);
    auto it = g_pending_save.find(async);
    if (it == g_pending_save.end()) return E_UNEXPECTED;
    if (folderSize < it->second.size() + 1) return E_NOT_SUFFICIENT_BUFFER;
    std::memcpy(folderResult, it->second.c_str(), it->second.size() + 1);
    g_pending_save.erase(it);
    return S_OK;
}

STDAPI XGameSaveFilesGetRemainingQuota(XUserHandle userContext, const char* configurationId,
                                       int64_t* remainingQuota) noexcept {
    (void)userContext;
    if (!configurationId || !remainingQuota) return E_INVALIDARG;
    const std::string dir = save_folder_for(configurationId);
    if (dir.empty()) return E_FAIL;
    const uint64_t total = pls_total_bytes();
    const uint64_t used = dir_used_bytes(g_pls_path);   // 整个 PLS 目录的占用
    *remainingQuota = static_cast<int64_t>(total > used ? total - used : 0);
    return S_OK;
}

// ---- 商店 / 授权（仍然桩） ----

STDAPI XStoreQueryProductForCurrentGameAsync(const XStoreContextHandle storeContextHandle,
                                             XAsyncBlock* async) noexcept {
    (void)storeContextHandle;
    (void)async;
    XB_UNIMPL("XStoreQueryProductForCurrentGameAsync");
    return E_NOTIMPL;
}

// ---- 能力门（返回值直接决定游戏走哪条分支，必须是真的） ----

STDAPI_(bool) XGameRuntimeIsFeatureAvailable(XGameRuntimeFeature feature) noexcept {
    // 我们确实实现了的：XAsync / XPackage / XPersistentLocalStorage / XTaskQueue / XUser / XError
    // 明确没实现的主机功能一律 false —— 宁可让游戏走降级路径，也不要让它以为能用
    switch (feature) {
        case XGameRuntimeFeature::XAsync:
        case XGameRuntimeFeature::XPackage:
        case XGameRuntimeFeature::XPersistentLocalStorage:
        case XGameRuntimeFeature::XTaskQueue:
        case XGameRuntimeFeature::XUser:
        case XGameRuntimeFeature::XGameSave:
        case XGameRuntimeFeature::XError:
        case XGameRuntimeFeature::XThread:
        case XGameRuntimeFeature::XGame:
            return true;
        default:
            return false;
    }
}

// ---- 事件订阅（注册表保留，事件永不触发；Unregister 必须返回 true） ----

std::mutex g_game_evt_mu;
std::vector<XTaskQueueRegistrationToken> g_game_evt_tokens;
uint64_t g_game_evt_next = 1;

STDAPI XGameActivationRegisterForEvent(XTaskQueueHandle queue, void* context,
                                       XGameActivationCallback* callback,
                                       XTaskQueueRegistrationToken* token) noexcept {
    (void)queue;
    (void)context;
    (void)callback;
    if (!token) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_game_evt_mu);
    token->token = g_game_evt_next++;
    g_game_evt_tokens.push_back(*token);
    xbgtasix::logf("[xgamert] RegisterForEvent(token=%llu)，PC 上事件不会触发",
                   static_cast<unsigned long long>(token->token));
    return S_OK;
}

STDAPI_(bool) XGameActivationUnregisterForEvent(XTaskQueueRegistrationToken token,
                                                bool wait) noexcept {
    (void)wait;
    std::lock_guard<std::mutex> lock(g_game_evt_mu);
    for (auto it = g_game_evt_tokens.begin(); it != g_game_evt_tokens.end(); ++it) {
        if (it->token == token.token) {
            g_game_evt_tokens.erase(it);
            return true;
        }
    }
    return true;   // 注销未注册的 token 也返回 true（GDK 语义：不报错）
}

// ---- UI（把主机弹窗降级成"立即完成"，绝不挂起） ----
//
// 铁律：*Async 必须让 XAsyncBlock 完成。返回 E_NOTIMPL 却不完成，
// 调用方的 XAsyncGetStatus(wait=true) 会重试 5000 次（我们 5 秒后返回 E_PENDING），
// 引擎若轮询或断言 S_OK 就等于永久卡死。

struct UiPending {
    XGameUiMessageDialogButton button = XGameUiMessageDialogButton::First;
    std::string text;
    std::string completion_uri;
};

std::mutex g_ui_mu;
std::unordered_map<XAsyncBlock*, UiPending> g_ui_pending;
XGameUiNotificationPositionHint g_ui_notif_pos = XGameUiNotificationPositionHint::TopRight;

STDAPI XGameUiSetUiCallbacks(const XGameUiUiCallbacks* callbacks,
                            bool useSystemUiIfAvailable) noexcept {
    (void)callbacks;
    (void)useSystemUiIfAvailable;
    // 不接管 UI：保持"立即完成"策略，避免注册回调后等一个永不到来的响应
    xbgtasix::logf("[xgamert] SetUiCallbacks：忽略（本实现不走系统 UI）");
    return S_OK;
}

STDAPI XGameUiSetNotificationPositionHint(XGameUiNotificationPositionHint position) noexcept {
    g_ui_notif_pos = position;
    return S_OK;
}

STDAPI XGameUiShowErrorDialogAsync(XAsyncBlock* async, HRESULT errorCode,
                                   const char* context) noexcept {
    if (!async) return E_INVALIDARG;
    xbgtasix::logf("[xgamert] UI 错误框: hr=0x%08lx %s", static_cast<unsigned long>(errorCode),
                   context ? context : "");
    complete_async(async, S_OK);
    return S_OK;
}

STDAPI XGameUiShowErrorDialogResult(XAsyncBlock* async) noexcept {
    if (!async) return E_INVALIDARG;
    return is_async_done(async) ? async_status(async) : E_PENDING;
}

STDAPI XGameUiShowMessageDialogAsync(XAsyncBlock* async, const char* titleText,
                                     const char* contentText, const char* firstButtonText,
                                     const char* secondButtonText, const char* thirdButtonText,
                                     XGameUiMessageDialogButton defaultButton,
                                     XGameUiMessageDialogButton cancelButton) noexcept {
    (void)thirdButtonText;
    (void)cancelButton;
    if (!async) return E_INVALIDARG;
    xbgtasix::logf("[xgamert] UI 消息框: %s / %s [%s][%s] -> 默认按钮 %u",
                   titleText ? titleText : "", contentText ? contentText : "",
                   firstButtonText ? firstButtonText : "", secondButtonText ? secondButtonText : "",
                   static_cast<unsigned>(defaultButton));
    {
        std::lock_guard<std::mutex> lock(g_ui_mu);
        g_ui_pending[async].button = defaultButton;
    }
    complete_async(async, S_OK);
    return S_OK;
}

STDAPI XGameUiShowMessageDialogResult(XAsyncBlock* async,
                                      XGameUiMessageDialogButton* resultButton) noexcept {
    if (!async || !resultButton) return E_INVALIDARG;
    if (!is_async_done(async)) return E_PENDING;
    std::lock_guard<std::mutex> lock(g_ui_mu);
    auto it = g_ui_pending.find(async);
    if (it == g_ui_pending.end()) return E_UNEXPECTED;
    *resultButton = it->second.button;
    g_ui_pending.erase(it);
    return S_OK;
}

STDAPI XGameUiShowTextEntryAsync(XAsyncBlock* async, const char* titleText,
                                 const char* descriptionText, const char* defaultText,
                                 XGameUiTextEntryInputScope inputScope,
                                 uint32_t maxTextLength) noexcept {
    (void)titleText;
    (void)descriptionText;
    (void)inputScope;
    (void)maxTextLength;
    if (!async) return E_INVALIDARG;
    {
        std::lock_guard<std::mutex> lock(g_ui_mu);
        g_ui_pending[async].text = defaultText ? defaultText : "";
    }
    complete_async(async, S_OK);
    return S_OK;
}

STDAPI XGameUiShowTextEntryResultSize(XAsyncBlock* async, uint32_t* resultSize) noexcept {
    if (!async || !resultSize) return E_INVALIDARG;
    if (!is_async_done(async)) return E_PENDING;
    std::lock_guard<std::mutex> lock(g_ui_mu);
    auto it = g_ui_pending.find(async);
    if (it == g_ui_pending.end()) return E_UNEXPECTED;
    *resultSize = static_cast<uint32_t>(it->second.text.size() + 1);
    return S_OK;
}

STDAPI XGameUiShowTextEntryResult(XAsyncBlock* async, uint32_t resultSize, char* resultText,
                                  uint32_t* resultUsed) noexcept {
    if (!async || !resultText) return E_INVALIDARG;
    if (!is_async_done(async)) return E_PENDING;
    std::lock_guard<std::mutex> lock(g_ui_mu);
    auto it = g_ui_pending.find(async);
    if (it == g_ui_pending.end()) return E_UNEXPECTED;
    const uint32_t need = static_cast<uint32_t>(it->second.text.size() + 1);
    if (resultSize < need) return E_NOT_SUFFICIENT_BUFFER;
    std::memcpy(resultText, it->second.text.c_str(), need);
    if (resultUsed) *resultUsed = need;
    g_ui_pending.erase(it);
    return S_OK;
}

static HRESULT web_auth_impl(XAsyncBlock* async, const char* completionUri) {
    if (!async) return E_INVALIDARG;
    xbgtasix::logf("[xgamert] UI Web 认证：视为已通过（离线无浏览器交互）");
    {
        std::lock_guard<std::mutex> lock(g_ui_mu);
        g_ui_pending[async].completion_uri = completionUri ? completionUri : "";
    }
    complete_async(async, S_OK);
    return S_OK;
}

STDAPI XGameUiShowWebAuthenticationAsync(XAsyncBlock* async, XUserHandle requestingUser,
                                         const char* requestUri,
                                         const char* completionUri) noexcept {
    (void)requestingUser;
    xbgtasix::logf("[xgamert] WebAuth 请求: %s", requestUri ? requestUri : "");
    return web_auth_impl(async, completionUri);
}

STDAPI XGameUiShowWebAuthenticationWithOptionsAsync(XAsyncBlock* async, XUserHandle requestingUser,
                                                    const char* requestUri,
                                                    const char* completionUri,
                                                    XGameUiWebAuthenticationOptions options) noexcept {
    (void)requestingUser;
    (void)options;
    xbgtasix::logf("[xgamert] WebAuth(带选项) 请求: %s", requestUri ? requestUri : "");
    return web_auth_impl(async, completionUri);
}

STDAPI XGameUiShowWebAuthenticationResultSize(XAsyncBlock* async, size_t* bufferSize) noexcept {
    if (!async || !bufferSize) return E_INVALIDARG;
    if (!is_async_done(async)) return E_PENDING;
    std::lock_guard<std::mutex> lock(g_ui_mu);
    auto it = g_ui_pending.find(async);
    if (it == g_ui_pending.end()) return E_UNEXPECTED;
    // 结构体 + 结尾的 completionUri + 结束符
    *bufferSize = sizeof(XGameUiWebAuthenticationResultData) + it->second.completion_uri.size() + 1;
    return S_OK;
}

STDAPI XGameUiShowWebAuthenticationResult(XAsyncBlock* async, size_t bufferSize, void* buffer,
                                          XGameUiWebAuthenticationResultData** ptrToBuffer,
                                          size_t* bufferUsed) noexcept {
    if (!async || !buffer) return E_INVALIDARG;
    if (!is_async_done(async)) return E_PENDING;
    std::lock_guard<std::mutex> lock(g_ui_mu);
    auto it = g_ui_pending.find(async);
    if (it == g_ui_pending.end()) return E_UNEXPECTED;
    const size_t uriBytes = it->second.completion_uri.size() + 1;
    const size_t need = sizeof(XGameUiWebAuthenticationResultData) + uriBytes;
    if (bufferSize < need) return E_NOT_SUFFICIENT_BUFFER;
    auto* data = static_cast<XGameUiWebAuthenticationResultData*>(buffer);
    char* uri = reinterpret_cast<char*>(data + 1);
    std::memcpy(uri, it->second.completion_uri.c_str(), uriBytes);
    data->responseStatus = S_OK;
    data->responseCompletionUriSize = uriBytes;
    data->responseCompletionUri = uri;          // 指向 buffer 内部
    if (ptrToBuffer) *ptrToBuffer = data;       // 同样指向 buffer 内部
    if (bufferUsed) *bufferUsed = need;
    g_ui_pending.erase(it);
    return S_OK;
}

// ---- 系统信息 ----

STDAPI_(XSystemDeviceType) XSystemGetDeviceType() noexcept {
    xbgtasix::logf("[xgamert] XSystemGetDeviceType() -> Pc");
    return XSystemDeviceType::Pc;
}

// 现在返回从 MicrosoftGame.Config 解析出的真值（而不是桩）
STDAPI XGameGetXboxTitleId(uint32_t* titleId) noexcept {
    if (!titleId) return E_INVALIDARG;
    if (!g_rt.config_loaded) {
        *titleId = 0;
        return E_FAIL;
    }
    *titleId = g_rt.title_id;
    return S_OK;
}

}  // extern "C"
