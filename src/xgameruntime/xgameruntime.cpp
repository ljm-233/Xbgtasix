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
    g_rt.store_id = find_tag(xml, "StoreId");
    g_rt.msa_app_id = find_tag(xml, "MSAAppId");
    const std::string title = find_tag(xml, "TitleId");
    if (!title.empty()) {
        g_rt.title_id = static_cast<uint32_t>(std::stoul(title, nullptr, 16));
    }
    g_rt.config_loaded = true;
    xbgtasix::logf("[xgamert] 配置已读: %s", path);
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
bool ensure_pls_path() {
    std::lock_guard<std::mutex> lock(g_pls_mu);
    if (g_pls_ready) return true;
    // 允许用 XBGTASIX_PLS_ROOT 覆盖根目录（某些受限环境写不了 %LOCALAPPDATA%）
    char base[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("XBGTASIX_PLS_ROOT", base, MAX_PATH) == 0) {
        if (GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH) == 0) return false;
        _snprintf(base + strlen(base), MAX_PATH - strlen(base), "\\Xbgtasix");
    }
    char buf[MAX_PATH] = {0};
    _snprintf(buf, MAX_PATH, "%s\\%08lX", base, static_cast<unsigned long>(g_rt.title_id));
    // 逐级创建
    for (char* p = buf + strlen(base) + 1; *p; ++p) {
        if (*p == '\\') {
            *p = 0;
            CreateDirectoryA(buf, nullptr);
            *p = '\\';
        }
    }
    CreateDirectoryA(buf, nullptr);
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

struct TaskQueueState {
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
    const XTaskQueueDispatchMode mode = (port == XTaskQueuePort::Completion) ? q->comp_mode : q->work_mode;
    if (mode == XTaskQueueDispatchMode::Immediate) {
        callback(context, false);
        return S_OK;
    }
    {
        std::lock_guard<std::mutex> lock(q->mu);
        q->queue_for(port).push_back(TaskEntry{port, context, callback});
    }
    q->cv.notify_all();
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
    q->shutdown(true);
    delete q;
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

STDAPI_(void) XUserCloseHandle(XUserHandle user) noexcept {
    if (!user) return;
    delete reinterpret_cast<UserState*>(user);
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

// ---- 商店 / 授权（仍然桩） ----

STDAPI XStoreQueryProductForCurrentGameAsync(const XStoreContextHandle storeContextHandle,
                                             XAsyncBlock* async) noexcept {
    (void)storeContextHandle;
    (void)async;
    XB_UNIMPL("XStoreQueryProductForCurrentGameAsync");
    return E_NOTIMPL;
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
