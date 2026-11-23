// SPDX-License-Identifier: GPL-3.0-or-later
// t33_memory — CPU 内存的两个真实风险：ABA 假释放、泄漏
//
// 主机上 XMem 是统一内存，游戏可能长时间运行；PC 上我们只有 VirtualAlloc。
// 这里验两件最容易出错、也最难在别处发现的事：
//   1) **ABA 假释放**：malloc A -> free A -> malloc B（复用同址）-> 再次 free A。
//      只按地址匹配的实现会把 B 释放掉，且不报错——之后 B 的写入就落进已释放内存。
//   2) **泄漏_**：分配 N 个后只释放 N-1 个，统计必须如实反映还活着的数量。
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "xbgtasix/trace.h"

extern "C" {
void* WINAPI XMemAlloc(uint64_t attributes, size_t size);
void WINAPI XMemFree(void* p);
bool WINAPI XMemGetWorkingSetStatistics(void* stats);
}

namespace {

int g_fail = 0;
void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t33]   PASS  %-40s %s", what, detail);
    } else {
        xbgtasix::logf("[t33]   FAIL  %-40s %s", what, detail);
        ++g_fail;
    }
}

struct Stats {
    uint64_t TotalBytes;
    uint64_t PeakBytes;
    uint64_t AllocationCount;
    uint64_t Reserved[4];
};

Stats stats() {
    Stats s{};
    XMemGetWorkingSetStatistics(&s);
    return s;
}

}  // namespace

int main() {
    const Stats base = stats();
    xbgtasix::logf("[t33] 起始: live=%llu 次 / %llu 字节",
                   static_cast<unsigned long long>(base.AllocationCount),
                   static_cast<unsigned long long>(base.TotalBytes));

    // ---- 1) ABA：延迟 double free 不能释放后续同址分配 ----
    {
        const size_t size = 4096;
        void* a = XMemAlloc(0, size);
        ok(a != nullptr, "A = XMemAlloc(4KB)");
        std::memset(a, 0xAA, size);
        XMemFree(a);

        // 立刻再要一块：若实现没有隔离区，很可能是同一个地址
        void* b = XMemAlloc(0, size);
        ok(b != nullptr, "B = XMemAlloc(4KB)");
        const bool same_addr = (a == b);
        xbgtasix::logf("[t33] B 与 A 同址? %s（%p vs %p）", same_addr ? "是" : "否", a, b);

        std::memset(b, 0xBB, size);   // 写入 B 的合法内容

        // 危险动作：再释放一次已经释放过的 A
        XMemFree(a);

        // B 必须仍然可用：前 64 字节应该还是 0xBB（没被 0xDD 毒化、也没被释放）
        unsigned char probe[64];
        std::memcpy(probe, b, sizeof(probe));
        bool intact = true;
        for (unsigned char c : probe) {
            if (c != 0xBB) {
                intact = false;
                break;
            }
        }
        // 以前这条把"同址"和"不同址"两种情况都当通过（detail 里补一句解释就完了），
        // 而不同址时内容检查恒真 —— 等于没验。现在拆成两条各自可证伪的断言。
        // (1) 无论实现是否复用地址，重复 free 都不许改写 B 的已有内容
        ok(intact, "重复 free 之后 B 的前 64 字节仍然是 0xBB",
           same_addr ? "（同址：真正的 ABA 场景）" : "（不同址：隔离区生效）");

        // (2) 统计口径也必须证明 B 还活着：存活计数 = 起始 + 1（只有 B），
        //     存活字节至少多出一次请求的 4096 字节。以前只写 >= base+1，
        //     同址时"double free 把 B 一起释放"也能过。
        const Stats mid = stats();
        ok(mid.AllocationCount == base.AllocationCount + 1 &&
               mid.TotalBytes >= base.TotalBytes + size,
           "重复 free 之后统计仍把 B 算作存活（count=base+1、字节不回落）",
           same_addr ? "同址" : "不同址");

        std::memset(b, 0xCC, size);   // 还能写
        // 以前是 ok(true)：写下去不代表真写进去了。读回来比对
        unsigned char back[64];
        std::memcpy(back, b, sizeof(back));
        bool wrote = true;
        for (unsigned char c : back) {
            if (c != 0xCC) {
                wrote = false;
                break;
            }
        }
        ok(wrote, "B 仍然可写（写入 0xCC 后读回一致）");
        XMemFree(b);
    }

    // ---- 2) 泄漏：分配 100 个只释放 99 个，统计要如实 ----
    {
        const int N = 100;
        std::vector<void*> v;
        for (int i = 0; i < N; ++i) v.push_back(XMemAlloc(0, 65536));
        int alloc_ok = 0;
        for (void* p : v) if (p) ++alloc_ok;
        ok(alloc_ok == N, "100 次 64KB 分配全部成功");

        const Stats after_alloc = stats();
        for (int i = 0; i < N - 1; ++i) XMemFree(v[i]);
        const Stats after_free = stats();

        const long long delta_count = static_cast<long long>(after_alloc.AllocationCount) -
                                      static_cast<long long>(after_free.AllocationCount);
        xbgtasix::logf("[t33] 释放 %d 个后，存活计数减少 %lld（应为 %d）", N - 1, delta_count, N - 1);
        ok(delta_count == N - 1, "存活计数精确减少 N-1（泄漏可见）");
        ok(after_free.AllocationCount >= base.AllocationCount + 1, "仍有 1 个存活");

        XMemFree(v[N - 1]);
        const Stats cleaned = stats();
        ok(cleaned.AllocationCount == base.AllocationCount, "全释放后回到起始存活数");
        ok(cleaned.TotalBytes == base.TotalBytes, "全释放后字节数回到起始值");
    }

    // ---- 2.5) 隔离区本身：最近释放的地址不得被立刻复用（这是防 ABA 的机制）----
    {
        const size_t size = 32768;
        std::vector<void*> old_ptrs;
        for (int i = 0; i < 8; ++i) {
            void* p = XMemAlloc(0, size);
            ok(p != nullptr, "隔离区: 预分配");
            old_ptrs.push_back(p);
        }
        for (void* p : old_ptrs) XMemFree(p);

        int reused = 0;
        for (int i = 0; i < 8; ++i) {
            void* p = XMemAlloc(0, size);
            for (void* q : old_ptrs) if (p == q) ++reused;
            XMemFree(p);
        }
        xbgtasix::logf("[t33] 刚释放的 8 个地址里被复用 %d 个", reused);
        ok(reused == 0, "最近释放的地址不被立刻复用（ABA 防护生效）");
    }

    // ---- 3) 边界：null / 0 字节 / 重复释放 ----
    {
        XMemFree(nullptr);
        ok(true, "XMemFree(nullptr) 不崩溃");
        void* z = XMemAlloc(0, 0);
        ok(z == nullptr, "XMemAlloc(0) 返回 null");
        void* p = XMemAlloc(0, 128);
        XMemFree(p);
        XMemFree(p);   // 重复释放
        ok(true, "重复释放不崩溃");
    }

    xbgtasix::logf("[t33] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
