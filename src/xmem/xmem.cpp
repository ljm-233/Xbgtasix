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
#include <mutex>
#include <unordered_map>

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

struct Header {
    void* raw;
    size_t size;
    uint64_t attributes;
};

std::mutex g_mu;
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
    if (size > SIZE_MAX - align - sizeof(Header)) {
        xbgtasix::logf("[xmem] XMemAlloc size overflow: %zu", size);
        return nullptr;
    }
    // 多要 align + header 用于手动对齐。
    // [事实] 主机上一个分配就是"整数个 64 KB 物理页"，所以这里把整段向上取整到 64 KB
    //        并**全部提交**。否则 VirtualQuery 会只看得到 ptr 之后那几页（实测 4096），
    //        与主机口径不符。代价是 1 字节的分配也要占 64 KB —— 这也是主机上的真实代价。
    const size_t total = size + align + sizeof(Header);
    const size_t committed = (total + kPageGranularity - 1) / kPageGranularity * kPageGranularity;

    auto* raw = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, committed, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!raw) {
        xbgtasix::logf("[xmem] VirtualAlloc(%zu, align=%zu) failed: %lu", size, align, GetLastError());
        return nullptr;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(raw) + sizeof(Header);
    const uintptr_t aligned = (base + (align - 1)) & ~static_cast<uintptr_t>(align - 1);
    auto* slot = reinterpret_cast<Header*>(aligned - sizeof(Header));
    slot->raw = raw;
    slot->size = size;
    slot->attributes = attributes;

    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_allocs[reinterpret_cast<void*>(aligned)] = *slot;
        g_live_bytes += size;
        ++g_live_count;
        ++g_total_allocs;
        if (g_live_bytes > g_peak_bytes) g_peak_bytes = g_live_bytes;
    }
    return reinterpret_cast<void*>(aligned);
}

// [推断] 二次释放/野指针在主机上是 undefined；我们选择"忽略 + 记日志"，
//        因为让移植过程崩溃在这里毫无诊断价值。
__declspec(dllexport) void __cdecl XMemFree(void* ptr) {
    if (!ptr) return;
    Header h{};
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_allocs.find(ptr);
        if (it == g_allocs.end()) {
            xbgtasix::logf("[xmem] XMemFree of unknown pointer %p (ignored)", ptr);
            return;
        }
        h = it->second;
        g_allocs.erase(it);
        g_live_bytes -= h.size;
        --g_live_count;
    }
    if (poison_enabled()) std::memset(ptr, 0xDD, h.size);
    VirtualFree(h.raw, 0, MEM_RELEASE);
}

// [推断] 字段口径直接映射 Windows MEMORY_BASIC_INFORMATION。主机上没有
//        PAGE_READWRITE 这类保护概念，Protect 只对调试有意义。
__declspec(dllexport) bool __cdecl XMemVirtualQuery(void* ptr, XMemVirtualQueryInfo* info) {
    if (!ptr || !info) return false;
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
