// xgameruntime.dll shim — GDK 运行时入口
//
// 签名一律来自 GDK 真头文件（gdk/include_clean），所以 ABI 由编译器保证。
// 本文件实现三块：
//   1) 生命周期        XGameRuntimeInitialize / Uninitialize
//   2) 任务队列        XTaskQueue*：真的线程池 + 回调派发（替代 482 个桩里的同名项）
//   3) 用户            XUser*：本地模拟用户，跑通 AddAsync -> AddResult -> GetId -> Close
// 其余导出仍是生成桩：一被调用就打印名字并退出。
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "XGame.h"
#include "XGameRuntime.h"
#include "XStore.h"
#include "XSystem.h"
#include "XTaskQueue.h"
#include "XUser.h"

#include "xbgtasix/trace.h"

namespace {

void notify_port(XTaskQueuePort p);

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
                cv.wait(lock, [&] {
                    return stopped.load() || !queue_for(port).empty();
                });
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
        const bool work_threaded = work_mode != XTaskQueueDispatchMode::Manual &&
                                   work_mode != XTaskQueueDispatchMode::Immediate;
        const bool comp_threaded = comp_mode != XTaskQueueDispatchMode::Manual &&
                                   comp_mode != XTaskQueueDispatchMode::Immediate;
        if (work_threaded) worker_work = std::thread([this] { pump(XTaskQueuePort::Work); });
        if (comp_threaded) worker_comp = std::thread([this] { pump(XTaskQueuePort::Completion); });
        if (!work_threaded) worker_work = std::thread([this] { pump(XTaskQueuePort::Work); });
        if (!comp_threaded) worker_comp = std::thread([this] { pump(XTaskQueuePort::Completion); });
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
    TaskEntry e{port, context, callback};

    const XTaskQueueDispatchMode mode = (port == XTaskQueuePort::Completion) ? q->comp_mode : q->work_mode;
    if (mode == XTaskQueueDispatchMode::Immediate) {
        callback(context, false);
        return S_OK;
    }
    {
        std::lock_guard<std::mutex> lock(q->mu);
        q->queue_for(port).push_back(e);
    }
    q->cv.notify_all();
    return S_OK;
}

STDAPI XTaskQueueTerminate(XTaskQueueHandle queue, bool wait, void* context,
                           XTaskQueueTerminatedCallback* callback) noexcept {
    if (!queue) return E_INVALIDARG;
    auto* q = reinterpret_cast<TaskQueueState*>(queue);
    q->shutdown(wait);
    if (callback) callback(context);   // 注意：终止回调只有一个参数，不带 canceled
    xbgtasix::logf("[xgamert] XTaskQueueTerminate(wait=%d)", wait ? 1 : 0);
    return S_OK;
}

STDAPI_(void) XTaskQueueCloseHandle(XTaskQueueHandle queue) noexcept {
    if (!queue) return;
    auto* q = reinterpret_cast<TaskQueueState*>(queue);
    q->shutdown(true);
    delete q;
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
    // 同步完成：GDK 真实实现是异步的，这里为了可测先同步回调
    if (async->callback) async->callback(async);
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
    auto* u = reinterpret_cast<UserState*>(user);
    *userId = u->id;
    return S_OK;
}

STDAPI_(void) XUserCloseHandle(XUserHandle user) noexcept {
    if (!user) return;
    delete reinterpret_cast<UserState*>(user);
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

STDAPI XGameGetXboxTitleId(uint32_t* titleId) noexcept {
    if (titleId) *titleId = 0;
    XB_UNIMPL("XGameGetXboxTitleId");
    return E_NOTIMPL;
}

}  // extern "C"
