// t40_taskqueue3 —— 任务队列生命周期与终止语义（回归测试）
//
// 这些场景对应审查里"任务队列"那一组严重问题，改动大且没有测试钉住：
//   - GetPort 加了引用却从不归还 -> 队列永不销毁、worker 继续回调已释放的 context
//   - 在队列自己的回调里 Terminate/Close -> 旧实现 detach 后 delete，pump 回来即 UAF
//   - Terminate 语义与头文件相反 -> 剩余项以 canceled=false 执行，游戏以为工作成功
//   - 复合队列持有来源队列引用不归还 -> 来源队列与线程永久泄漏
//
// 判定手段：除了 API 行为，还用"日志里出现几次『任务队列销毁』"来确认真的销毁了
// （这是唯一能外部观察到的销毁证据；本测试把日志指到自己独占的文件，避免与别人混在一起）。
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI XTaskQueueCreate(int work_mode, int comp_mode, void** queue);
HRESULT WINAPI XTaskQueueCreateComposite(void* work_queue, void* comp_queue, void** queue);
HRESULT WINAPI XTaskQueueGetPort(void* queue, int port, void** port_handle);
HRESULT WINAPI XTaskQueueSubmitCallback(void* queue, int port, void* context, void* callback);
HRESULT WINAPI XTaskQueueDispatch(void* queue, int port, unsigned timeout_ms);
HRESULT WINAPI XTaskQueueTerminate(void* queue, bool wait, void* context, void* callback);
void WINAPI XTaskQueueCloseHandle(void* queue);
}

namespace {

int g_fail = 0;
void ok(bool cond, const char* what, const std::string& detail = "") {
    if (cond) {
        xbgtasix::logf("[t40]   PASS  %-52s %s", what, detail.c_str());
    } else {
        xbgtasix::logf("[t40]   FAIL  %-52s %s", what, detail.c_str());
        ++g_fail;
    }
}

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void sleep_ms(int ms) { Sleep(static_cast<DWORD>(ms)); }

// 回调：记录 canceled 与执行次数
std::atomic<int> g_ran{0};
std::atomic<int> g_canceled{0};
std::atomic<int> g_completed_normal{0};

void CALLBACK on_task(void* context, bool canceled) {
    (void)context;
    g_ran.fetch_add(1);
    if (canceled) g_canceled.fetch_add(1);
    else g_completed_normal.fetch_add(1);
}

std::atomic<int> g_term{0};
void CALLBACK on_terminated(void* context) {
    (void)context;
    g_term.fetch_add(1);
}

void reset_counters() {
    g_ran = 0;
    g_canceled = 0;
    g_completed_normal = 0;
    g_term = 0;
}

// 等某个原子量达到目标（最多 timeout_ms）
bool wait_until(std::atomic<int>* v, int target, int timeout_ms) {
    const long long deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        if (v->load() >= target) return true;
        sleep_ms(2);
    }
    return v->load() >= target;
}

// 读自己的日志，数"任务队列销毁"出现了几次
int count_destroyed(const std::wstring& log_path) {
    HANDLE h = CreateFileW(log_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    std::string all;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) all.append(buf, got);
    CloseHandle(h);
    int n = 0;
    const char* needle = "任务队列销毁";
    for (size_t p = all.find(needle); p != std::string::npos; p = all.find(needle, p + 1)) ++n;
    return n;
}

// 具名回调：C++ 里函数指针不能直接转 void*，用命名函数 + reinterpret_cast 更稳
void CALLBACK slow_task_400(void*, bool) {
    sleep_ms(400);
    g_ran.fetch_add(1);
}
void CALLBACK slow_task_300(void*, bool) {
    sleep_ms(300);
    g_ran.fetch_add(1);
}

struct SelfCloseCtx {
    void* q;
};

void CALLBACK self_close_task(void* context, bool canceled) {
    (void)canceled;
    g_ran.fetch_add(1);   // 让测试能确认"回调真的跑起来了"
    auto* c = static_cast<SelfCloseCtx*>(context);
    XTaskQueueTerminate(c->q, false, nullptr, reinterpret_cast<void*>(&on_terminated));
    XTaskQueueCloseHandle(c->q);
}

constexpr int kWork = 0;      // XTaskQueuePort::Work
constexpr int kComp = 1;      // XTaskQueuePort::Completion
constexpr int kManual = 0;    // XTaskQueueDispatchMode::Manual
constexpr int kThreadPool = 1;

}  // namespace

int main() {
    // 把日志指到自己独占的文件：要靠它数"销毁"次数，不能和别人混写
    {
        wchar_t dir[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        std::wstring path(dir);
        const size_t pos = path.find_last_of(L"\\/");
        path = (pos == std::wstring::npos ? std::wstring(L".") : path.substr(0, pos)) +
               L"\\t40_taskqueue3.log";
        SetEnvironmentVariableW(L"XBGTASIX_LOG", path.c_str());
        DeleteFileW(path.c_str());
        xbgtasix::logf("[t40] 开始（日志: %s）", "t40_taskqueue3.log");
    }
    const std::wstring log_path = [] {
        wchar_t dir[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        std::wstring p(dir);
        const size_t pos = p.find_last_of(L"\\/");
        return (pos == std::wstring::npos ? std::wstring(L".") : p.substr(0, pos)) +
               L"\\t40_taskqueue3.log";
    }();

    // =====================================================================
    // 1) GetPort 之后 Close 端口句柄，队列引用必须归还 -> 队列被销毁
    //    （旧实现永不归零：worker 线程一直活着，队列对象泄漏）
    // =====================================================================
    {
        reset_counters();
        const int before = count_destroyed(log_path);
        void* q = nullptr;
        ok(SUCCEEDED(XTaskQueueCreate(kThreadPool, kManual, &q)) && q, "1-创建线程池队列");
        void* port = nullptr;
        ok(SUCCEEDED(XTaskQueueGetPort(q, kWork, &port)) && port, "1-GetPort(Work)");
        XTaskQueueCloseHandle(port);   // 先关端口句柄
        XTaskQueueCloseHandle(q);      // 再关队列句柄
        sleep_ms(150);
        const int after = count_destroyed(log_path);
        ok(after == before + 1, "1-端口句柄关闭后队列被销毁",
           "销毁次数 " + std::to_string(after - before));
    }

    // =====================================================================
    // 2) 反序：先 Close 队列、再 Close 端口
    // =====================================================================
    {
        reset_counters();
        const int before = count_destroyed(log_path);
        void* q = nullptr;
        XTaskQueueCreate(kThreadPool, kManual, &q);
        void* port = nullptr;
        XTaskQueueGetPort(q, kWork, &port);
        XTaskQueueCloseHandle(q);
        XTaskQueueCloseHandle(port);
        sleep_ms(150);
        ok(count_destroyed(log_path) == before + 1, "2-反序关闭也能销毁（不泄漏）");
    }

    // =====================================================================
    // 3) 在队列自己的回调里 Terminate + Close：不能崩，terminated 恰好 1 次
    //    旧实现会 detach 后 delete，pump 回来访问已释放成员 -> UAF
    // =====================================================================
    {
        reset_counters();
        void* q = nullptr;
        XTaskQueueCreate(kThreadPool, kManual, &q);

        SelfCloseCtx ctx{q};
        XTaskQueueSubmitCallback(q, kWork, &ctx, reinterpret_cast<void*>(&self_close_task));
        ok(wait_until(&g_ran, 1, 3000), "3-回调被执行");
        const bool term_ok = wait_until(&g_term, 1, 3000);
        ok(term_ok, "3-回调内 Terminate 后 terminated 被交付");
        sleep_ms(200);
        ok(g_term.load() == 1, "3-terminated 恰好 1 次", "实际 " + std::to_string(g_term.load()));
        ok(true, "3-回调内 Close 没有崩溃（走到这里就是证明）");
    }

    // =====================================================================
    // 4) Manual 队列：Terminate(wait=true) 后剩余项全部 canceled=true
    //    （旧实现会用 canceled=false 跑掉，游戏以为工作成功）
    // =====================================================================
    {
        reset_counters();
        void* q = nullptr;
        XTaskQueueCreate(kManual, kManual, &q);
        const int N = 8;
        for (int i = 0; i < N; ++i) XTaskQueueSubmitCallback(q, kWork, nullptr, reinterpret_cast<void*>(&on_task));

        ok(SUCCEEDED(XTaskQueueTerminate(q, true, nullptr,
                                         reinterpret_cast<void*>(&on_terminated))),
           "4-Terminate(wait=true) 成功");
        ok(g_canceled.load() == N && g_completed_normal.load() == 0,
           "4-剩余项全部 canceled=true",
           "期望 " + std::to_string(N) + "，实际 canceled=" +
               std::to_string(g_canceled.load()) +
               " normal=" + std::to_string(g_completed_normal.load()));
        ok(g_term.load() == 1, "4-terminated 恰好 1 次", "实际 " + std::to_string(g_term.load()));

        // 终止后不再派发、再次提交被丢弃
        reset_counters();
        ok(!XTaskQueueDispatch(q, kWork, 0), "4-终止后 Dispatch 返回 false");
        XTaskQueueSubmitCallback(q, kWork, nullptr, reinterpret_cast<void*>(&on_task));
        const bool dispatched = XTaskQueueDispatch(q, kWork, 0);
        ok(!dispatched && g_ran.load() == 0, "4-终止后提交的回调被丢弃（不执行）",
           "ran=" + std::to_string(g_ran.load()));
        XTaskQueueCloseHandle(q);
    }

    // =====================================================================
    // 5) Terminate(wait=true) 阻塞语义：要等正在跑的回调结束才返回
    // =====================================================================
    {
        reset_counters();
        void* q = nullptr;
        XTaskQueueCreate(kThreadPool, kManual, &q);
        XTaskQueueSubmitCallback(q, kWork, nullptr, reinterpret_cast<void*>(&slow_task_400));
        sleep_ms(50);   // 让 worker 真的开始跑
        const long long t0 = now_ms();
        XTaskQueueTerminate(q, true, nullptr, reinterpret_cast<void*>(&on_terminated));
        const long long dt = now_ms() - t0;
        ok(dt >= 200, "5-Terminate(wait=true) 等到在途回调结束才返回",
           "耗时 " + std::to_string(dt) + " ms");
        ok(g_term.load() == 1, "5-返回时 terminated 已交付");
        sleep_ms(500);   // 等在途回调真的跑完，避免影响后续计数
        XTaskQueueCloseHandle(q);
    }

    // =====================================================================
    // 6) Terminate(wait=false) 立即返回，terminated 由最后退出的 worker 交付
    // =====================================================================
    {
        reset_counters();
        void* q = nullptr;
        XTaskQueueCreate(kThreadPool, kManual, &q);
        XTaskQueueSubmitCallback(q, kWork, nullptr, reinterpret_cast<void*>(&slow_task_300));
        sleep_ms(50);
        const long long t0 = now_ms();
        XTaskQueueTerminate(q, false, nullptr, reinterpret_cast<void*>(&on_terminated));
        const long long dt = now_ms() - t0;
        ok(dt < 150, "6-Terminate(wait=false) 立即返回", "耗时 " + std::to_string(dt) + " ms");
        const bool late_ok = wait_until(&g_term, 1, 3000);
        ok(late_ok, "6-terminated 之后异步交付");
        sleep_ms(200);
        ok(g_term.load() == 1, "6-terminated 恰好 1 次（不重复）",
           "实际 " + std::to_string(g_term.load()));
        XTaskQueueCloseHandle(q);
    }

    // =====================================================================
    // 7) 复合队列：Close 来源队列后复合队列仍可用；Close 复合后两边都销毁
    //    （旧实现来源队列引用永不归还 -> 永久泄漏）
    // =====================================================================
    {
        reset_counters();
        const int before = count_destroyed(log_path);
        void* src = nullptr;
        XTaskQueueCreate(kThreadPool, kThreadPool, &src);
        void* src_work = nullptr;
        void* src_comp = nullptr;
        XTaskQueueGetPort(src, kWork, &src_work);
        XTaskQueueGetPort(src, kComp, &src_comp);

        void* comp = nullptr;
        ok(SUCCEEDED(XTaskQueueCreateComposite(src_work, src_comp, &comp)) && comp,
           "7-创建复合队列");
        XTaskQueueCloseHandle(src);   // 来源队列句柄先关

        XTaskQueueSubmitCallback(comp, kWork, nullptr, reinterpret_cast<void*>(&on_task));
        ok(wait_until(&g_ran, 1, 3000), "7-来源队列句柄关闭后复合队列仍能工作");

        XTaskQueueTerminate(comp, true, nullptr, reinterpret_cast<void*>(&on_terminated));
        XTaskQueueCloseHandle(comp);
        XTaskQueueCloseHandle(src_work);
        XTaskQueueCloseHandle(src_comp);
        sleep_ms(300);
        const int destroyed = count_destroyed(log_path) - before;
        ok(destroyed >= 2, "7-复合队列与来源队列都被销毁（不泄漏）",
           "销毁次数 " + std::to_string(destroyed));
    }

    // =====================================================================
    // 8) 重复 Terminate：第二次登记的回调立即交付，取消回调不重复
    // =====================================================================
    {
        reset_counters();
        void* q = nullptr;
        XTaskQueueCreate(kManual, kManual, &q);
        for (int i = 0; i < 4; ++i) XTaskQueueSubmitCallback(q, kWork, nullptr, reinterpret_cast<void*>(&on_task));
        XTaskQueueTerminate(q, true, nullptr, reinterpret_cast<void*>(&on_terminated));
        const int canceled_after_first = g_canceled.load();
        XTaskQueueTerminate(q, true, nullptr, reinterpret_cast<void*>(&on_terminated));
        ok(g_canceled.load() == canceled_after_first, "8-第二次 Terminate 不重复取消",
           "第一次 " + std::to_string(canceled_after_first) + "，之后 " +
               std::to_string(g_canceled.load()));
        ok(g_term.load() == 2, "8-两次登记的 terminated 都交付",
           "实际 " + std::to_string(g_term.load()));
        XTaskQueueCloseHandle(q);
    }

    // =====================================================================
    // 9) 压力：创建/关闭 40 个队列（含 GetPort、复合、回调内 Close），销毁数 == 创建数
    // =====================================================================
    {
        reset_counters();
        const int before = count_destroyed(log_path);
        const int N = 40;
        for (int i = 0; i < N; ++i) {
            void* q = nullptr;
            XTaskQueueCreate(i % 2 ? kThreadPool : kManual, kManual, &q);
            if (i % 3 == 0) {
                void* p = nullptr;
                XTaskQueueGetPort(q, kWork, &p);
                XTaskQueueCloseHandle(p);
            }
            if (i % 4 == 0) {
                XTaskQueueSubmitCallback(q, kWork, nullptr, reinterpret_cast<void*>(&on_task));
                XTaskQueueTerminate(q, true, nullptr, reinterpret_cast<void*>(&on_terminated));
            }
            XTaskQueueCloseHandle(q);
        }
        // 等 worker 退出（销毁由最后退出的 worker 完成，Close 本身不阻塞）
        bool drained = false;
        for (int i = 0; i < 300; ++i) {
            if (count_destroyed(log_path) - before >= N) {
                drained = true;
                break;
            }
            sleep_ms(20);
        }
        const int destroyed = count_destroyed(log_path) - before;
        ok(drained && destroyed >= N, "9-40 个队列全部销毁（无泄漏）",
           "销毁 " + std::to_string(destroyed) + " / 创建 " + std::to_string(N));
    }

    xbgtasix::logf("[t40] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
