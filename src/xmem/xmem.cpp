// SPDX-License-Identifier: GPL-3.0-or-later
// xmem.dll shim — 主机内存 API → VirtualAlloc
//
// 证据等级标记（每条断言后面标）：
//   [事实]  有公开资料原文支撑，见 docs/xmem-evidence.md
//   [推断]  由公开资料合理推导，但无逐字定义
//   [自定]  完全由本项目选定，仅由 tests/t3_xmem 锁住，拿到 xmem.h 后必须替换
#include "xbgtasix/trace.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

// [事实] XMem 系列以 64 KB 粒度分配/映射物理页；GDK 用 MEM_64K_PAGES / MEM_2MB_PAGES
//        表示页大小，2 MB 是更大的页选项。
// [推断] 所以默认对齐取 64 KB —— 它同时也正好是 Windows 的分配粒度，
//        因此 VirtualQuery 回来的 RegionSize 天然是 64 KB 的整数倍，与主机口径一致。
constexpr size_t kPageGranularity = 64 * 1024;
constexpr size_t kMaxAlignment = 256ull * 1024 * 1024;

// [推断] attributes 低 32 位若是一个 >= 64 KB 的 2 的幂，就当作"请求的对齐字节数"；
//        否则回落 64 KB。这样既能表达 MEM_2MB_PAGES 这类大页请求，也不会因为
//        位掩码含义未知而分配出非页对齐的地址。
size_t alignment_from(uint64_t attributes) {
    const uint64_t req = attributes & 0xFFFFFFFFull;
    if (req >= kPageGranularity && req <= kMaxAlignment && (req & (req - 1)) == 0) {
        return static_cast<size_t>(req);
    }
    return kPageGranularity;
}

// 记录放在旁路表里，不在内存里留 Header：
// 反过来做（header 前置于返回指针）会为了对齐多提交整整一页，
// 导致 1 字节分配实占 128 KB（审查发现的严重项）。
struct Header {
    void* raw;
    size_t size;          // 请求字节
    size_t committed;     // 实际提交字节
    uint64_t attributes;
};

std::mutex g_mu;

// 释放隔离区（quarantine）。
// 为什么需要：只按地址匹配的话，一次延迟的 double free 若期间同址被重新分配，
// 就会去释放**活的**分配。把刚释放的块先留着不还（最多 kQuarantineMax 个），
// 地址就不会被立刻复用，这类假释放会走"unknown pointer"分支被记下来。
constexpr size_t kQuarantineMax = 64;
struct Quarantined {
    void* raw;
    size_t committed;
};
std::vector<Quarantined> g_quarantine;
std::unordered_map<void*, Header> g_allocs;
uint64_t g_live_bytes = 0;
uint64_t g_live_count = 0;
uint64_t g_peak_bytes = 0;
uint64_t g_total_allocs = 0;

bool poison_enabled() {
    static const bool on = [] {
        char buf[8] = {0};
        return GetEnvironmentVariableA("XBGTASIX_XMEM_POISON", buf, sizeof(buf)) > 0 &&
               std::strcmp(buf, "1") == 0;
    }();
    return on;
}

}  // namespace

extern "C" {

// [自定] 结构体布局是猜的（没有 xmem.h）。字段顺序依据是"主机查询接口通常给出
//        基址/大小/状态/保护/类型"，与 Windows MEMORY_BASIC_INFORMATION 同构。
struct XMemVirtualQueryInfo {
    void* BaseAddress;
    size_t RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
    uint64_t CommittedBytes;
};

// [自定] 同上。TotalBytes / PeakBytes / AllocationCount 语义自明，Reserved[] 未定义。
struct XMemWorkingSetStatistics {
    uint64_t TotalBytes;
    uint64_t PeakBytes;
    uint64_t AllocationCount;
    uint64_t Reserved[5];
};

// [推断] 签名顺序 (attributes, size)。公开资料只提到"XMemAlloc 系列"，
//        没有给出逐字声明；顺序反了会静默破坏 ABI，是这块最大的风险点。
__declspec(dllexport) void* __cdecl XMemAlloc(uint64_t attributes, size_t size) {
    if (size == 0) return nullptr;  // [自定]

    const size_t align = alignment_from(attributes);
    // 溢出检查要覆盖"最终真正提交的字节数"：后面算 committed 时还会再加
    // align + 64KB-1 并向上取整，只检查 size 是不够的（接近 SIZE_MAX 时会回绕成小值，
    // VirtualAlloc 成功但返回远小于请求量的块）
    if (size > SIZE_MAX - align - sizeof(Header) - 2 * kPageGranularity) {
        xbgtasix::logf("[xmem] XMemAlloc size overflow: %zu", size);
        return nullptr;
    }
    // [事实] 主机上一个分配就是"整数个 64 KB 物理页"，所以把请求向上取整到 64 KB 并全部提交，
    //        与主机口径一致。VirtualAlloc 本身保证 64 KB 粒度对齐，所以 align<=64KB 时不需要额外余量；
    //        只有请求更大对齐（如 2 MB 大页）时才多要一段用于手动对齐。
    const size_t need = (size + kPageGranularity - 1) / kPageGranularity * kPageGranularity;
    const size_t committed = align > kPageGranularity
        ? (need + align + kPageGranularity - 1) / kPageGranularity * kPageGranularity
        : need;

    auto* raw = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, committed, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!raw) {
        xbgtasix::logf("[xmem] VirtualAlloc(%zu, align=%zu) failed: %lu", size, align, GetLastError());
        return nullptr;
    }
    uint8_t* aligned = raw;
    if (align > kPageGranularity) {
        const uintptr_t a = (reinterpret_cast<uintptr_t>(raw) + (align - 1)) &
                            ~static_cast<uintptr_t>(align - 1);
        aligned = reinterpret_cast<uint8_t*>(a);
    }

    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_allocs[aligned] = Header{raw, size, committed, attributes};
        // 统计口径是**提交字节**，不是请求字节：按请求字节算会让预算类断言系统性高估可用内存
        g_live_bytes += committed;
        ++g_live_count;
        ++g_total_allocs;
        if (g_live_bytes > g_peak_bytes) g_peak_bytes = g_live_bytes;
    }
    return aligned;
}

// [推断] 二次释放/野指针在主机上是 undefined；我们选择"忽略 + 记日志"，
//        因为让移植过程崩溃在这里毫无诊断价值。
__declspec(dllexport) void __cdecl XMemFree(void* ptr) {
    if (!ptr) return;
    Header h{};
    bool unknown = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_allocs.find(ptr);
        if (it == g_allocs.end()) {
            unknown = true;
        } else {
            h = it->second;
            g_allocs.erase(it);
            g_live_bytes -= h.committed;
            --g_live_count;
        }
    }
    // 日志与释放都在锁外：持锁写 stderr，若 stderr 是无人读的管道会整段卡死
    if (unknown) {
        xbgtasix::logf("[xmem] XMemFree of unknown pointer %p (ignored)", ptr);
        return;
    }
    if (poison_enabled()) std::memset(ptr, 0xDD, h.size);

    // 先进隔离区：地址保留（防止同址立刻复用），但把物理页退掉（真实释放内存压力）
    VirtualFree(h.raw, h.committed, MEM_DECOMMIT);

    // 超过上限再真正归还最老的那个
    void* to_free_raw = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_quarantine.push_back(Quarantined{h.raw, h.committed});
        if (g_quarantine.size() > kQuarantineMax) {
            to_free_raw = g_quarantine.front().raw;
            g_quarantine.erase(g_quarantine.begin());
        }
    }
    if (to_free_raw) VirtualFree(to_free_raw, 0, MEM_RELEASE);
}

// [推断] 字段口径直接映射 Windows MEMORY_BASIC_INFORMATION。主机上没有
//        PAGE_READWRITE 这类保护概念，Protect 只对调试有意义。
__declspec(dllexport) bool __cdecl XMemVirtualQuery(void* ptr, XMemVirtualQueryInfo* info) {
    if (!ptr || !info) return false;
    // 先查自己的表：主机语义里 BaseAddress 就是分配返回的指针，
    // RegionSize 是这次分配的大小（不是内核合并后的整段空闲区）。
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_allocs.find(ptr);
        if (it != g_allocs.end()) {
            info->BaseAddress = ptr;
            info->RegionSize = it->second.committed;
            info->State = MEM_COMMIT;
            info->Protect = PAGE_READWRITE;
            info->Type = MEM_PRIVATE;
            info->CommittedBytes = it->second.committed;
            return true;
        }
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) return false;
    info->BaseAddress = mbi.BaseAddress;
    info->RegionSize = mbi.RegionSize;
    info->State = mbi.State;
    info->Protect = mbi.Protect;
    info->Type = mbi.Type;
    info->CommittedBytes = (mbi.State == MEM_COMMIT) ? mbi.RegionSize : 0;
    return true;
}

// [自定] 主机上这个接口给的是"标题内存使用量"（相对预算），PC 上没有预算概念，
//        所以我们报自己的分配统计。Reserved[0] 额外塞累计分配次数，仅供测试用。
__declspec(dllexport) bool __cdecl XMemGetWorkingSetStatistics(XMemWorkingSetStatistics* stats) {
    if (!stats) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    std::memset(stats, 0, sizeof(*stats));
    stats->TotalBytes = g_live_bytes;
    stats->PeakBytes = g_peak_bytes;
    stats->AllocationCount = g_live_count;
    stats->Reserved[0] = g_total_allocs;
    return true;
}

}  // extern "C"

// ===========================================================================
// 第二组：给真实 exe 补齐的导出（源自 game_scarlett_master.exe 的导入表）
//
// 这些名字是我们**从目标 exe 的导入表**里读到的，不是从 GDK 头/库里来的
// （xmem.h 不在 NDA 之外的任何产物里）。签名只能按名字推断，所以策略保守：
//   - 有 PC 等价实现的真做（XMemVirtualAlloc / XMemAllocDefault / XMemFreeDefault）
//   - 主机物理页/APU 相关的诚实失败（返回 null / 0），让游戏走降级路径
// 每条都在日志里留痕，便于 exe 跑起来后核对调用序列。
// ===========================================================================
namespace {

// ---------------------------------------------------------------------------
// 按需提交（lazy commit）
//
// 主机是统一内存：游戏可以往任何"分配到的"地址写，不需要先提交流程。
// PC 上如果一次性 MEM_COMMIT 几十 GB，会立刻吃光页文件配额，后续分配只能
// 退化成 MEM_RESERVE —— 游戏一写就 AV（实测：第 6 次 0.58GB 分配退化成
// reserve only，紧接着写 0x...0010 就崩）。
//
// 所以策略改成：分配时只 MEM_RESERVE 地址段并登记；真正的提交推迟到第一次
// 访问该页时，由异常钩子补上 MEM_COMMIT，然后让指令继续执行。
// ---------------------------------------------------------------------------
struct Region {
    uint8_t* base;
    size_t size;
};
std::mutex g_res_mu;
std::vector<Region> g_reserved;
std::atomic<bool> g_veh_installed{false};

bool commit_page_covering(void* fault_addr) {
    auto p = reinterpret_cast<uint8_t*>(fault_addr);
    std::lock_guard<std::mutex> lock(g_res_mu);
    for (const auto& r : g_reserved) {
        if (p >= r.base && p < r.base + r.size) {
            // 按 64KB 对齐提交这一页（主机页大小就是 64KB）
            uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~uintptr_t(0xFFFF);
            void* got = VirtualAlloc(reinterpret_cast<void*>(a), 0x10000, MEM_COMMIT, PAGE_READWRITE);
            return got != nullptr;
        }
    }
    return false;
}

LONG WINAPI lazy_commit_handler(PEXCEPTION_POINTERS info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    void* addr = reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]);
    if (commit_page_covering(addr)) {
        return EXCEPTION_CONTINUE_EXECUTION;   // 页已提交，重试这条指令
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

extern "C" {

// 默认分配器：等价于 XMemAlloc(0, size)
__declspec(dllexport) void* __cdecl XMemAllocDefault(size_t size) {
    return XMemAlloc(0, size);
}

// 默认释放：等价于 XMemFree
__declspec(dllexport) void __cdecl XMemFreeDefault(void* p) {
    XMemFree(p);
}

// 虚拟内存分配：主机上是同一套地址空间，PC 上直接给 VirtualAlloc
__declspec(dllexport) void* __cdecl XMemVirtualAlloc(void* addr, size_t size, uint32_t alloc_type,
                                                    uint32_t protect) {
    // 实测：exe 传的 alloc_type=0x20402000、protect=0x21 —— 那是 **XMem 自己的属性位**
    // （对齐/物理页/无缓存之类），不是 VirtualAlloc 的 flAllocationType。
    // 第一版把它直接透给 VirtualAlloc，得到 err=87（参数无效），游戏随即断言退出。
    // 正确做法：忽略这两个参数的位语义，按"保留+提交、可读写"给内存。
    (void)alloc_type;
    (void)protect;
    static bool logged = false;

    // 只保留地址段，提交交给缺页钩子按需做（见文件上方的 lazy commit 说明）
    void* p = VirtualAlloc(addr, size, MEM_RESERVE, PAGE_READWRITE);
    const char* how = "reserve（按需提交）";
    if (!p) {
        // 极少数情况保留也失败（地址空间碎片），再试一次保留+提交
        p = VirtualAlloc(addr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        how = "commit+reserve";
    }
    if (p) {
        {
            std::lock_guard<std::mutex> lock(g_res_mu);
            g_reserved.push_back(Region{static_cast<uint8_t*>(p), size});
        }
        if (!g_veh_installed.exchange(true)) {
            AddVectoredExceptionHandler(1, lazy_commit_handler);
            xbgtasix::logf("[xmem] 已启用按需提交（缺页时自动 MEM_COMMIT）");
        }
    }
    // 全量记录（上限 64 行）：内存布局是这类移植最先崩的地方，
    // 只记第一次不够——后面某次返回 null 才是真凶。
    static int calls = 0;
    if (calls < 64) {
        xbgtasix::logf("[xmem] XMemVirtualAlloc #%d  addr=%p size=%.2f GB type=0x%x prot=0x%x -> %p（%s）",
                       ++calls, addr, size / (1024.0 * 1024 * 1024), alloc_type, protect, p,
                       p ? how : "失败");
    }
    (void)logged;
    return p;
}

// 工具内存是否可用：PC 上"工具内存"概念不存在
__declspec(dllexport) bool __cdecl XMemIsToolsMemAvailable(void) { return false; }

// 以下四个是主机 APU/GPU 的物理页映射，PC 上没有对应硬件。
// 返回 null / 0 让游戏立刻知道"拿不到物理页"，而不是给它一个假地址。
__declspec(dllexport) void* __cdecl XMemAllocatePhysicalPages(size_t pages, size_t align) {
    xbgtasix::logf("[xmem] XMemAllocatePhysicalPages(%zu, %zu)：主机物理页分配，PC 无对应能力",
                   pages, align);
    return nullptr;
}
__declspec(dllexport) bool __cdecl XMemFreePhysicalPages(void* ptr, size_t pages) {
    (void)ptr; (void)pages;
    return false;
}
__declspec(dllexport) void* __cdecl XMemMapPhysicalPages(void* physical, size_t size) {
    (void)physical; (void)size;
    xbgtasix::logf("[xmem] XMemMapPhysicalPages：主机物理页映射，PC 无对应能力");
    return nullptr;
}
__declspec(dllexport) void* __cdecl XMemMapPhysicalPagesScatter(void* physical, size_t count) {
    (void)physical; (void)count;
    xbgtasix::logf("[xmem] XMemMapPhysicalPagesScatter：主机物理页映射，PC 无对应能力");
    return nullptr;
}
__declspec(dllexport) bool __cdecl XMemTransferMemory(void* dst, const void* src, size_t size) {
    if (!dst || !src || !size) return false;
    std::memcpy(dst, src, size);   // 主机上是 DMA，PC 上用内存拷贝等效
    return true;
}

}  // extern "C"
