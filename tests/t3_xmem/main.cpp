// SPDX-License-Identifier: GPL-3.0-or-later
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
// 真实布局（XMem 的查询结构），与小写字段名对应：
// 我们最早按"BaseAddress/RegionSize/State/Protect/Type"猜过一版，签名也是错的
// （少了 process 参数）。这里以移植来源为准。
struct XMemBasicInformation {
    void* base;
    void* alloc_base;
    uint32_t alloc_protect;
    uint32_t reserved0;
    size_t region_size;
    uint32_t state;
    uint32_t protect;
    uint32_t kind;
    uint32_t reserved1;
    uint64_t flags;
    uint32_t tag;
    char tag_text[4];
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
void* __cdecl XMemVirtualAlloc(void* address, size_t size, uint32_t type,
                              uint64_t attributes, uint32_t protect);
int __cdecl XMemAllocatePhysicalPages(uint64_t flags, size_t* count, uint64_t* pages);
int __cdecl XMemFreePhysicalPages(size_t count, uint64_t* pages);
void* __cdecl XMemMapPhysicalPages(void* address, size_t count, uint64_t* pages);
int __cdecl XMemVirtualQuery(void* process, const void* address, XMemBasicInformation* out);
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
// 审查意见：这里的规则不能照抄实现，否则用被测代码算期望值 = 恒真。
// 期望值写成字面量表（见下面的 cases），本函数只用于打印。
// 期望值写成字面量，不从实现推导（否则实现错了测试也跟着错）。
// 规则：attributes 低 32 位若是 >=64KB 且 <=256MB 的 2 的幂，就按它对齐；否则 64KB。
struct AlignCase { uint64_t code; size_t want; const char* why; };
// 0 / 1 / 12345 都不是"合法对齐码" -> 64KB
constexpr AlignCase kAlignCases[] = {
    {0ull,                  65536, "默认"},
    {1ull,                  65536, "非 2 的幂"},
    {12345ull,              65536, "奇怪的码"},
    {65536ull,              65536, "64KB 请求"},
    {2ull * 1024 * 1024,    2ull * 1024 * 1024, "2MB 大页"},
    {16ull * 1024 * 1024,   16ull * 1024 * 1024, "16MB"},
};

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
    for (const AlignCase& c : kAlignCases) {
        const uint64_t code = c.code;
        const size_t want = c.want;
        for (size_t sz : sizes) {
            void* p = XMemAlloc(code, sz);
            ok(p != nullptr, "align: alloc");
            if (!p) continue;
            ok((reinterpret_cast<uintptr_t>(p) % want) == 0, c.why);
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

    XMemBasicInformation info{};
    ok(XMemVirtualQuery(GetCurrentProcess(), p, &info), "large: query ok");
    ok(info.state == MEM_COMMIT, "large: query committed");
    ok(reinterpret_cast<uintptr_t>(p) >= reinterpret_cast<uintptr_t>(info.base) &&
           reinterpret_cast<uintptr_t>(p) < reinterpret_cast<uintptr_t>(info.base) + info.region_size,
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
    XMemBasicInformation info{};
    ok(XMemVirtualQuery(GetCurrentProcess(), p, &info), "gran: query");
    ok((info.region_size % 65536) == 0, "gran: RegionSize multiple of 64KB");
    ok((reinterpret_cast<uintptr_t>(info.base) % 65536) == 0, "gran: BaseAddress 64KB aligned");
    xbgtasix::logf("[t3] gran: base=%p region=%zu state=0x%lx", info.base, info.region_size,
                 static_cast<unsigned long>(info.state));
    XMemFree(p);
}

// ---- 7. 释放后查询 ----
void test_query_after_free() {
    void* p = XMemAlloc(1, 4096);
    ok(p != nullptr, "qaf: alloc");
    if (!p) return;
    XMemFree(p);
    XMemBasicInformation info{};
    const bool got = XMemVirtualQuery(GetCurrentProcess(), p, &info);
    // 以前写 `!got || State == MEM_FREE || State == MEM_RESERVE` —— !got 这一支
    // 让"查不到"也算过，"释放了但还查得到"更是完全不管。现在钉死一种语义：
    // 释放后实现把内存退到隔离区（MEM_DECOMMIT），所以必须查得到且状态是 MEM_RESERVE。
    ok(got && info.state == MEM_RESERVE, "qaf: 释放后查得到且状态为 MEM_RESERVE（隔离区已 decommit）");
    xbgtasix::logf("[t3] query-after-free: got=%d state=0x%lx", got ? 1 : 0,
                 static_cast<unsigned long>(info.state));
}

void test_placeholder() {
    if (!GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2")) {
        xbgtasix::logf("[t3] skip placeholder checks on Windows without VirtualAlloc2");
        return;
    }
    constexpr size_t page = 0x10000;
    constexpr DWORD xbox_64k_pages = 0x20400000;
    auto* base = static_cast<unsigned char*>(XMemVirtualAlloc(
        nullptr, 2 * page, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, 7, PAGE_NOACCESS));
    ok(base != nullptr, "placeholder: reserve two pages");
    if (!base) return;

    const bool split = VirtualFree(base, page, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
    ok(split, "placeholder: split with the native VirtualFree");
    if (split) {
        void* replaced = XMemVirtualAlloc(base, page,
            xbox_64k_pages | MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, 0x27, 0x1004);
        ok(replaced == base, "placeholder: replace exactly the requested page");
        if (replaced == base) {
            uint64_t physical_page = 0;
            size_t count = 1;
            const bool allocated = XMemAllocatePhysicalPages(0, &count, &physical_page) && count == 1;
            ok(allocated, "placeholder: allocate backing page");
            if (allocated) {
                std::memset(reinterpret_cast<void*>(physical_page), 0x5A, page);
                void* mapped = XMemMapPhysicalPages(base, count, &physical_page);
                ok(mapped == base && base[0] == 0x5A && base[page - 1] == 0x5A,
                   "placeholder: map page data at the original address");
                XMemFreePhysicalPages(count, &physical_page);
            }
        }
        ok(VirtualFree(base + page, 0, MEM_RELEASE), "placeholder: release the other page");
    }
    ok(VirtualFree(base, 0, MEM_RELEASE), "placeholder: release the first range");
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
    test_placeholder();

    const Stats end = stats();
    ok(end.TotalBytes == 0 && end.AllocationCount == 0, "end: no leak");
    const double dt = now_ms() - t0;

    xbgtasix::logf("[t3] peak=%llu B, total allocations=%llu, elapsed=%.0f ms",
                 static_cast<unsigned long long>(end.PeakBytes),
                 static_cast<unsigned long long>(end.Reserved[0]), dt);
    xbgtasix::logf("[t3] %d checks, %d failures -> %s", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
