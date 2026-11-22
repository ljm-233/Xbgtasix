// t3_xmem — XMem 行为回归 + 压力测试
//
// 目的：xmem.h 锁在 NDA 里，签名和语义都是推定值。既然无法对照官方，
// 就用测试把"我们选定的行为"钉死：以后任何改动只要破坏了这些断言，立刻红。
//
// 覆盖：对齐矩阵 / 越界 canary / 二次释放 / 野指针 / 10 万次碎片化 churn /
//       大块分配 / 多线程并发 / 释放后 VirtualQuery / 统计口径
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "xbgtasix/trace.h"

// 与 src/xmem/xmem.cpp 的定义逐字一致（没有公开头文件可用，只能手动对齐）
struct XMemVirtualQueryInfo {
    void* BaseAddress;
    size_t RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
    uint64_t CommittedBytes;
};

struct XMemWorkingSetStatistics {
    uint64_t TotalBytes;
    uint64_t PeakBytes;
    uint64_t AllocationCount;
    uint64_t Reserved[5];
};

extern "C" {
void* __cdecl XMemAlloc(uint64_t attributes, size_t size);
void __cdecl XMemFree(void* ptr);
bool __cdecl XMemVirtualQuery(void* ptr, XMemVirtualQueryInfo* info);
bool __cdecl XMemGetWorkingSetStatistics(XMemWorkingSetStatistics* stats);
}

namespace {

using Stats = XMemWorkingSetStatistics;

int g_fail = 0;
int g_checks = 0;

void ok(bool cond, const char* what) {
    ++g_checks;
    if (!cond) {
        ++g_fail;
        xbgtasix::logf("[t3] FAIL %s", what);
    }
}

Stats stats() {
    Stats s{};
    XMemGetWorkingSetStatistics(&s);
    return s;
}

// 与 src/xmem 的规则保持一致：
//   attributes 低 32 位是 >=64KB 的 2 的幂 -> 视为请求的对齐字节数；否则 64KB
size_t expect_align(uint64_t attributes) {
    const uint64_t req = attributes & 0xFFFFFFFFull;
    const uint64_t cap = 256ull * 1024 * 1024;
    if (req >= 65536 && req <= cap && (req & (req - 1)) == 0) return static_cast<size_t>(req);
    return 65536;
}

constexpr unsigned char kCanary = 0xA5;

void stamp(unsigned char* p, size_t n) {
    std::memset(p, kCanary, n);
}

bool verify(const unsigned char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != kCanary) return false;
    }
    return true;
}

// ---- 1. 对齐矩阵 ----
void test_align_matrix() {
    const size_t sizes[] = {1, 4096, 65537, 1024 * 1024};
    // 覆盖：0(默认) / 1(非 2 的幂) / 64KB / 2MB / 16MB / 一个奇怪的码
    const uint64_t codes[] = {0, 1, 65536, 2ull * 1024 * 1024, 16ull * 1024 * 1024, 12345};
    for (uint64_t code : codes) {
        const size_t want = expect_align(code);
        for (size_t sz : sizes) {
            void* p = XMemAlloc(code, sz);
            ok(p != nullptr, "align: alloc");
            if (!p) continue;
            ok((reinterpret_cast<uintptr_t>(p) % want) == 0, "align: pointer multiple");
            const size_t probe = sz < 64 ? sz : 64;
            stamp(static_cast<unsigned char*>(p), probe);
            ok(verify(static_cast<unsigned char*>(p), probe), "align: canary");
            XMemFree(p);
        }
    }
    xbgtasix::logf("[t3] align matrix done");
}

// ---- 2. 二次释放 / 野指针 / nullptr ----
void test_bad_free() {
    const Stats before = stats();
    void* p = XMemAlloc(1, 4096);
    ok(p != nullptr, "badfree: alloc");
    XMemFree(p);
    XMemFree(p);                 // 二次释放：不许崩、不许改统计
    XMemFree(nullptr);           // 必须 no-op
    XMemFree(reinterpret_cast<void*>(0x1234));  // 野指针：不许崩
    const Stats after = stats();
    ok(after.TotalBytes == before.TotalBytes && after.AllocationCount == before.AllocationCount,
       "badfree: stats unchanged");
    xbgtasix::logf("[t3] bad free done");
}

// ---- 3. 碎片化 churn ----
void test_churn(unsigned iterations) {
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> size_dist(16, 32768);
    std::vector<std::pair<void*, size_t>> live;
    unsigned long long bytes = 0;

    for (unsigned i = 0; i < iterations; ++i) {
        const bool alloc_step = live.empty() || (rng() & 1) || live.size() < 8;
        if (alloc_step) {
            const size_t sz = static_cast<size_t>(size_dist(rng));
            void* p = XMemAlloc(1, sz);
            if (!p) {
                ok(false, "churn: alloc failed");
                break;
            }
            auto* b = static_cast<unsigned char*>(p);
            b[0] = kCanary;
            b[sz - 1] = kCanary;
            live.emplace_back(p, sz);
            bytes += sz;
            if (live.size() > 128) {
                // 强制回收最老的
                auto& front = live.front();
                auto* q = static_cast<unsigned char*>(front.first);
                if (q[0] != kCanary || q[front.second - 1] != kCanary) ok(false, "churn: canary (evict)");
                XMemFree(front.first);
                live.erase(live.begin());
            }
        } else {
            const size_t idx = static_cast<size_t>(rng()) % live.size();
            auto& e = live[idx];
            auto* q = static_cast<unsigned char*>(e.first);
            if (q[0] != kCanary || q[e.second - 1] != kCanary) ok(false, "churn: canary");
            XMemFree(e.first);
            live[idx] = live.back();
            live.pop_back();
        }
    }
    for (auto& e : live) XMemFree(e.first);
    const Stats s = stats();
    ok(s.TotalBytes == 0 && s.AllocationCount == 0, "churn: no leak after free-all");
    xbgtasix::logf("[t3] churn %u iters, %llu bytes touched, live stats: total=%llu count=%llu",
                 iterations, bytes, static_cast<unsigned long long>(s.TotalBytes),
                 static_cast<unsigned long long>(s.AllocationCount));
}

// ---- 4. 大块 ----
void test_large() {
    const size_t sz = 64ull * 1024 * 1024;
    const Stats before = stats();
    auto* p = static_cast<unsigned char*>(XMemAlloc(2ull * 1024 * 1024, sz));  // 请求 2MB 对齐
    ok(p != nullptr, "large: alloc");
    if (!p) return;
    ok((reinterpret_cast<uintptr_t>(p) % (2ull * 1024 * 1024)) == 0, "large: 2MB alignment");
    p[0] = kCanary;
    p[sz / 2] = kCanary;
    p[sz - 1] = kCanary;
    ok(p[0] == kCanary && p[sz / 2] == kCanary && p[sz - 1] == kCanary, "large: writable");

    const Stats during = stats();
    ok(during.TotalBytes >= before.TotalBytes + sz, "large: stats counted");

    XMemVirtualQueryInfo info{};
    ok(XMemVirtualQuery(p, &info), "large: query ok");
    ok(info.State == MEM_COMMIT, "large: query committed");
    ok(reinterpret_cast<uintptr_t>(p) >= reinterpret_cast<uintptr_t>(info.BaseAddress) &&
           reinterpret_cast<uintptr_t>(p) < reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize,
       "large: query covers pointer");

    XMemFree(p);
    xbgtasix::logf("[t3] large 64MB done");
}

// ---- 5. 多线程 ----
void test_threads(int n_threads, int per_thread) {
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < n_threads; ++t) {
        ts.emplace_back([t, per_thread, &errors] {
            std::mt19937 rng(1000 + t);
            std::uniform_int_distribution<int> d(128, 16384);
            for (int i = 0; i < per_thread; ++i) {
                const size_t sz = static_cast<size_t>(d(rng));
                void* p = XMemAlloc(1, sz);
                if (!p) {
                    ++errors;
                    continue;
                }
                auto* b = static_cast<unsigned char*>(p);
                b[0] = static_cast<unsigned char>(t & 0xFF);
                b[sz - 1] = static_cast<unsigned char>((t + 1) & 0xFF);
                if (b[0] != static_cast<unsigned char>(t & 0xFF) ||
                    b[sz - 1] != static_cast<unsigned char>((t + 1) & 0xFF))
                    ++errors;
                XMemFree(p);
            }
        });
    }
    for (auto& th : ts) th.join();
    ok(errors.load() == 0, "threads: no errors");
    const Stats s = stats();
    ok(s.TotalBytes == 0 && s.AllocationCount == 0, "threads: no leak");
    xbgtasix::logf("[t3] %d threads x %d allocs done", n_threads, per_thread);
}

// ---- 6. 区域粒度（主机 64KB 页） ----
void test_region_granularity() {
    auto* p = static_cast<unsigned char*>(XMemAlloc(0, 1));
    ok(p != nullptr, "gran: alloc");
    if (!p) return;
    ok((reinterpret_cast<uintptr_t>(p) % 65536) == 0, "gran: 64KB aligned");
    XMemVirtualQueryInfo info{};
    ok(XMemVirtualQuery(p, &info), "gran: query");
    ok((info.RegionSize % 65536) == 0, "gran: RegionSize multiple of 64KB");
    ok((reinterpret_cast<uintptr_t>(info.BaseAddress) % 65536) == 0, "gran: BaseAddress 64KB aligned");
    xbgtasix::logf("[t3] gran: base=%p region=%zu state=0x%lx", info.BaseAddress, info.RegionSize,
                 static_cast<unsigned long>(info.State));
    XMemFree(p);
}

// ---- 7. 释放后查询 ----
void test_query_after_free() {
    void* p = XMemAlloc(1, 4096);
    ok(p != nullptr, "qaf: alloc");
    if (!p) return;
    XMemFree(p);
    XMemVirtualQueryInfo info{};
    const bool got = XMemVirtualQuery(p, &info);
    ok(!got || info.State == MEM_FREE, "qaf: freed region is MEM_FREE or query fails");
    xbgtasix::logf("[t3] query-after-free: got=%d state=0x%lx", got ? 1 : 0,
                 static_cast<unsigned long>(info.State));
}

double now_ms() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * double(c.QuadPart) / double(f.QuadPart);
}

}  // namespace

int main(int argc, char** argv) {
    unsigned churn = 100000;
    if (argc > 1) churn = static_cast<unsigned>(std::atoi(argv[1]));
    xbgtasix::logf("[t3] start, churn=%u, poison=%s", churn,
                 GetEnvironmentVariableA("XBGTASIX_XMEM_POISON", nullptr, 0) ? "on" : "off");

    const double t0 = now_ms();
    const Stats begin = stats();
    ok(begin.TotalBytes == 0 && begin.AllocationCount == 0, "start: stats clean");

    test_align_matrix();
    test_bad_free();
    test_churn(churn);
    test_large();
    test_threads(8, 2000);
    test_region_granularity();
    test_query_after_free();

    const Stats end = stats();
    ok(end.TotalBytes == 0 && end.AllocationCount == 0, "end: no leak");
    const double dt = now_ms() - t0;

    xbgtasix::logf("[t3] peak=%llu B, total allocations=%llu, elapsed=%.0f ms",
                 static_cast<unsigned long long>(end.PeakBytes),
                 static_cast<unsigned long long>(end.Reserved[0]), dt);
    xbgtasix::logf("[t3] %d checks, %d failures -> %s", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
