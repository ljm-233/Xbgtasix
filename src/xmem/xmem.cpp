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
#include <algorithm>
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

namespace {
// 入口追踪：XBGTASIX_TRACE=1 时记录每次调用（xmem 只有十来个导出，值得全记）。
// 游戏崩在"某个字段为 0"时，先确认 xmem 到底被调了哪几个、什么顺序。
bool xtrace_on() {
    static const bool on = [] {
        char b[8] = {0};
        return GetEnvironmentVariableA("XBGTASIX_TRACE", b, sizeof(b)) > 0 && b[0] == '1';
    }();
    return on;
}
int xtrace_budget() {
    static int left = xtrace_on() ? 200 : 0;
    return left-- > 0 ? 1 : 0;
}
}  // namespace

template <typename T>
inline unsigned long long xb_arg(T* p) {   // 任意指针（含非 void*）：走地址值
    return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p));
}
template <typename T>
inline unsigned long long xb_arg(T v) {    // 整数/枚举/布尔
    return static_cast<unsigned long long>(v);
}

#define XB_XMEM_TRACE(name, a1, a2)                                     \
    do {                                                                \
        if (xtrace_on() && xtrace_budget()) {                           \
            xbgtasix::logf("[trace] xmem::%s(%llx, %llx)", name,        \
                           xb_arg(a1), xb_arg(a2));                     \
        }                                                               \
    } while (0)

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
// 经验性探针：游戏的"瞬时块"拦截器检查"第二参数 & 0xC000 == 0x8000"，
// 那个机制对应的全局基址（game+0x75E6FF0）在我们的环境里恒为 0，
// 导致 game+0x247AF2D 的"跨度 <= 2 MiB-1"不变量恒假。
// 这里在 xmem 的每个入口记录一次命中，用来判断这条分配是否经过我们。
inline void probe_transient_flags(const char* api, uint64_t v) {
    if ((v & 0xC000ULL) != 0x8000ULL) return;
    static std::atomic<int> n{0};
    const int k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 24) xbgtasix::logf("[xmem] 探针 %s 命中 0x8000 类标志：值=0x%llX（第 %d 次）", api,
                               static_cast<unsigned long long>(v), k + 1);
}

__declspec(dllexport) void* __cdecl XMemAlloc(uint64_t attributes, size_t size) { XB_XMEM_TRACE("XMemAlloc", attributes, size); probe_transient_flags("XMemAlloc", attributes);
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
__declspec(dllexport) void __cdecl XMemFree(void* ptr) { XB_XMEM_TRACE("XMemFree", 0, 0);
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
// 结构布局移植自 omonkeyz/xbox-emulator（GPL-3.0）virt.rs 的 XMemBasicInformation
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

__declspec(dllexport) int __cdecl XMemVirtualQuery(HANDLE process, const void* address,
                                                   XMemBasicInformation* out) {
    XB_XMEM_TRACE("XMemVirtualQuery", address, out);
    if (!out) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    // 取证：游戏会不会拿主机 GPU VA（0x8000... 前缀）来问？如果问，说明它期望
    // 这些区间在"设备层"是已知的；我们按普通 Win32 查询必然答"不存在"。
    {
        const auto a = reinterpret_cast<unsigned long long>(address);
        if ((a >> 48) == 0x8000ULL) {
            static std::atomic<int> n{0};
            const int k = n.fetch_add(1, std::memory_order_relaxed);
            if (k < 32) xbgtasix::logf("[xmem] 查询的是主机 GPU VA：0x%llX（第 %d 次）", a, k + 1);
        }
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(process, address, &mbi, sizeof(mbi)) == 0) return 0;
    std::memset(out, 0, sizeof(*out));
    out->base = mbi.BaseAddress;
    out->alloc_base = mbi.AllocationBase;
    out->alloc_protect = mbi.AllocationProtect;
    out->region_size = mbi.RegionSize;
    out->state = mbi.State;
    out->protect = mbi.Protect;
    out->kind = mbi.Type;
    out->flags = 1;   // 非 partition 内存
    return 1;
}

namespace {

// "物理页"在 PC 上就是一块 64KB 的普通内存；PFN 用它的地址当不透明句柄。
// 游戏只会把它原样传回来，不会解析。
constexpr size_t kPhysPage = 0x10000;
std::mutex g_phys_mu;
std::vector<void*> g_phys_blocks;   // 已分配的物理页块（按块首地址记录）

// 描述符 arena：游戏用 MEM_RESERVE_PLACEHOLDER 预留一大段 VA，
// 随后要求 D3D12 的描述符 CPU 内存就落在这一段里（主机"标题内存描述符堆"语义）。
// 实测（Intel 日志 2391-2392 行）：预留 0.19 GB @ 0x196d7200000，
// 紧接着在该地址 commit 0.02 GB、映射 320 页，然后建 802,816 项的堆，
// 并断言 GetCPUDescriptorHandleForHeapStart() == 0x196d7200000。
std::mutex g_arena_mu;
void* g_descriptor_arena_base = nullptr;
size_t g_descriptor_arena_size = 0;
size_t g_descriptor_arena_used = 0;
std::vector<void*> g_phys_pages;   // 每页一项（按分配顺序），便于 map 时找到源数据

}  // namespace

__declspec(dllexport) bool __cdecl XMemGetWorkingSetStatistics(XMemWorkingSetStatistics* stats) { XB_XMEM_TRACE("XMemGetWorkingSetStatistics", stats, 0);
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
    {
        // 每次处理都记一条（前 12 次）：只有知道"哪一次没提交成功、为什么"，
        // 才能判断是区间没登记、还是分布在别的保留块里。
        static std::atomic<unsigned> n{0};
        if (n.fetch_add(1, std::memory_order_relaxed) < 12) {
            const bool ok = commit_page_covering(addr);
            xbgtasix::logf("[xmem] 懒提交尝试 addr=%p -> %s", addr, ok ? "已提交" : "不在已登记区间");
            if (ok) return EXCEPTION_CONTINUE_EXECUTION;
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    if (commit_page_covering(addr)) {
        return EXCEPTION_CONTINUE_EXECUTION;   // 页已提交，重试这条指令
    }
    // 没覆盖到：把故障地址和访问类型打出来，否则现场只有寄存器、看不出是哪个地址越界。
    {
        const ULONG_PTR rw = info->ExceptionRecord->NumberParameters > 0
                                 ? info->ExceptionRecord->ExceptionInformation[0]
                                 : 0;
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
            xbgtasix::logf("[xmem] AV 不在已登记区间内：addr=%p 访问类型=%llu（0=读 1=写 8=执行）", addr,
                           static_cast<unsigned long long>(rw));
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

extern "C" {

// 默认分配器：等价于 XMemAlloc(0, size)
__declspec(dllexport) void* __cdecl XMemAllocDefault(size_t size) { XB_XMEM_TRACE("XMemAllocDefault", size, 0); probe_transient_flags("XMemAllocDefault", size);
    return XMemAlloc(0, size);
}

// 默认释放：等价于 XMemFree
__declspec(dllexport) void __cdecl XMemFreeDefault(void* p) { XB_XMEM_TRACE("XMemFreeDefault", 0, 0);
    XMemFree(p);
}

// 虚拟内存分配：主机上是同一套地址空间，PC 上直接给 VirtualAlloc
// ===========================================================================
// XMemVirtualAlloc：**5 个参数**，不是 4 个
//
// 移植自 omonkeyz/xbox-emulator（GPL-3.0，与本项目同许可）的
// crates/compat/xmem/src/virt.rs 与 sys.rs。原始实现的关键事实：
//
//   XMemVirtualAlloc(address, size, alloc_type: u32, attributes: u64, protect: u32)
//   alloc_type 的高位编码页粒度：
//     XMEM_64K_PAGES   = 0x20400000  -> 64 KB 页，要求 64 KB 对齐
//     XMEM_LARGE_PAGES = 0x20000000  -> 2 MB 大页，要求 2 MB 对齐
//   低位按 Win32 语义解释（& (MEM_COMMIT|MEM_RESERVE|MEM_RESET|MEM_TOP_DOWN|
//   MEM_WRITE_WATCH|MEM_RESET_UNDO)），为 0 时默认 保留+提交。
//   MEM_RESERVE_PLACEHOLDER / MEM_REPLACE_PLACEHOLDER 由 VirtualAlloc2 处理。
//   protect 只取低 8 位，PAGE_GUARD 单独保留。
//
// 我们此前用的是 4 参数版本：游戏传的 attributes(u64) 落进了第 4 个形参位，
// 真正的 protect 在栈上被完全忽略 —— 这是必须修掉的 ABI 错误。
// ===========================================================================
namespace {

constexpr uint32_t XMEM_LARGE_PAGES = 0x20000000u;
constexpr uint32_t XMEM_64K_PAGES = 0x20400000u;
constexpr size_t XMEM_PAGE_SIZE = 0x10000;   // 主机页 64 KB

size_t page_granularity(uint32_t alloc_type) {
    if ((alloc_type & XMEM_64K_PAGES) == XMEM_64K_PAGES) return XMEM_PAGE_SIZE;
    if (alloc_type & XMEM_LARGE_PAGES) return 0x200000;   // 2 MB 大页
    return 0x1000;
}

uint32_t win_alloc_type(uint32_t alloc_type) {
    const uint32_t mask = MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN | MEM_WRITE_WATCH |
                          MEM_RESET_UNDO;
    const uint32_t t = alloc_type & mask;
    return t ? t : (MEM_RESERVE | MEM_COMMIT);
}

uint32_t win_protect(uint32_t protect) {
    const uint32_t base = protect & 0xff;
    switch (base) {
        case PAGE_NOACCESS:
        case PAGE_READONLY:
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return base | (protect & PAGE_GUARD);
        default:
            return PAGE_READWRITE | (protect & PAGE_GUARD);
    }
}

size_t round_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

// 大于 64 KB 的对齐必须先探测一段空闲地址再在其中对齐保留。
// 直接 VirtualAlloc 只保证 64 KB 粒度，拿不到 2 MB 对齐。
void* reserve_aligned(size_t size, size_t align, uint32_t wt, uint32_t wp) {
    if (align <= XMEM_PAGE_SIZE) {
        return VirtualAlloc(nullptr, size, wt, wp);
    }
    for (int attempt = 0; attempt < 32; ++attempt) {
        void* probe = VirtualAlloc(nullptr, size + align, MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) return nullptr;
        VirtualFree(probe, 0, MEM_RELEASE);
        void* want = reinterpret_cast<void*>(round_up(reinterpret_cast<size_t>(probe), align));
        void* p = VirtualAlloc(want, size, wt | MEM_RESERVE, wp);
        if (p) return p;
    }
    return nullptr;
}

void* allocate_placeholder(void* address, size_t size, DWORD type, DWORD protect) {
    using Alloc = void* (WINAPI*)(HANDLE, void*, SIZE_T, ULONG, ULONG,
                                  MEM_EXTENDED_PARAMETER*, ULONG);
    static const auto alloc = [] {
        const auto symbol = GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2");
        Alloc fn = nullptr;
        static_assert(sizeof(fn) == sizeof(symbol));
        std::memcpy(&fn, &symbol, sizeof(fn));
        return fn;
    }();
    if (!alloc) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return nullptr;
    }
    return alloc(GetCurrentProcess(), address, size, type, protect, nullptr, 0);
}

}  // namespace

__declspec(dllexport) void* __cdecl XMemVirtualAlloc(void* address, size_t size,
                                                    uint32_t alloc_type, uint64_t attributes,
                                                    uint32_t protect) { probe_transient_flags("XMemVirtualAlloc/type", alloc_type); probe_transient_flags("XMemVirtualAlloc/attr", attributes);
    XB_XMEM_TRACE("XMemVirtualAlloc", size, alloc_type);
    const size_t gran = page_granularity(alloc_type);
    const uint32_t placeholder = alloc_type & (MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER);
    const uint32_t wt = win_alloc_type(alloc_type) | placeholder;
    const uint32_t wp = win_protect(protect);

    size_t base = reinterpret_cast<size_t>(address);
    size_t len = round_up(size, gran);
    if (base) {
        base &= ~(gran - 1);
        len = round_up(reinterpret_cast<size_t>(address) + size, gran) - base;
    }

    // 已经在我们保留过的区域里做提交：直接提交这一段
    if (!placeholder && base && (wt & MEM_COMMIT)) {
        void* got = VirtualAlloc(reinterpret_cast<void*>(base), len, wt, wp);
        if (got) {
            static int n = 0;
            if (n++ < 32) {
                xbgtasix::logf("[xmem] 提交已有区域 %p + %.2f GB（gran=%zu）", got,
                               len / (1024.0 * 1024 * 1024), gran);
            }
            return got;
        }
    }

    void* p = nullptr;
    if (placeholder) {
        // The title splits these reservations with VirtualFree(MEM_PRESERVE_PLACEHOLDER).
        // Replacing a split range requires the exact address in that reservation.
        p = allocate_placeholder(address, round_up(size, gran), wt, wp);
        if (p) {
            std::lock_guard<std::mutex> lock(g_arena_mu);
            const size_t len_now = round_up(size, gran);
            // 取**最大**的占位区：游戏先预留几块小的，真正的描述符 arena 是那块 0.19 GB 的。
            // 先前的版本取了第一块（10 MB），导致"arena 不够：需要 32000000，剩余 10485760"。
            if (len_now > g_descriptor_arena_size) {
                g_descriptor_arena_base = p;
                g_descriptor_arena_size = len_now;
                g_descriptor_arena_used = 0;
                xbgtasix::logf("[xmem] 描述符 arena 候选：%p 大小 %.1f MB", p,
                               len_now / 1048576.0);
            }
        }
    } else if (wt & MEM_RESERVE) {
        const size_t align = (base == 0 && gran > XMEM_PAGE_SIZE) ? gran : 0;
        p = reserve_aligned(len, align, wt, wp);
    } else {
        p = VirtualAlloc(reinterpret_cast<void*>(base), len, wt, wp);
    }

    if (!p) {
        const DWORD e = GetLastError();
        xbgtasix::logf("[xmem] XMemVirtualAlloc(addr=%p size=%.2f GB type=0x%x->0x%x prot=0x%x->0x%x "
                       "gran=%zu) 失败 err=%lu",
                       address, size / (1024.0 * 1024 * 1024), alloc_type, wt, protect, wp, gran, e);
        return nullptr;
    }

    // A placeholder becomes eligible for lazy commit after replacement.
    if ((wt & (MEM_COMMIT | MEM_RESERVE_PLACEHOLDER)) == 0) {
        std::lock_guard<std::mutex> lock(g_res_mu);
        g_reserved.push_back(Region{static_cast<uint8_t*>(p), len});
        if (!g_veh_installed.exchange(true)) {
            AddVectoredExceptionHandler(1, lazy_commit_handler);
        }
    }

    static std::atomic<unsigned> logged{0};
    if (placeholder || logged.fetch_add(1, std::memory_order_relaxed) < 24) {
        xbgtasix::logf("[xmem] XMemVirtualAlloc(addr=%p size=%.2f GB type=0x%x->0x%x prot=0x%x->0x%x "
                       "gran=%zu) -> %p",
                       address, size / (1024.0 * 1024 * 1024), alloc_type, wt, protect, wp, gran, p);
    }
    return p;
}

// 工具内存是否可用：PC 上"工具内存"概念不存在
__declspec(dllexport) bool __cdecl XMemIsToolsMemAvailable(void) { XB_XMEM_TRACE("XMemIsToolsMemAvailable", 0, 0); return false; }

// ===========================================================================
// 物理页：签名与语义移植自 omonkeyz/xbox-emulator（GPL-3.0，同许可）virt.rs。
// 此前这五个函数是**猜的签名**（2 参数、返回 null），实测游戏会真的调用
// XMemAllocatePhysicalPages(640 页)，拿到 null 就崩。
// PC 上"物理页"就是普通内存；PFN 用其地址当不透明句柄（游戏只原样回传）。
// ===========================================================================
// 复用上方的 kPhysPage / g_phys_mu（kPhysPage 已在文件前部定义）

__declspec(dllexport) int __cdecl XMemAllocatePhysicalPages(uint64_t flags, size_t* num_pages,
                                                            uint64_t* page_array) {
    XB_XMEM_TRACE("XMemAllocatePhysicalPages", flags, num_pages ? *num_pages : 0);
    if (!num_pages) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const size_t n = *num_pages;
    // 大页请求（0x20000000）要求 32 页 = 2 MB 对齐，与移植来源一致
    if ((static_cast<uint32_t>(flags) & 0x20400000u) == 0x20000000u && (n & 0x1f) != 0) {
        *num_pages = 0;
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (n != 0 && !page_array) {
        *num_pages = 0;
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (n == 0) return 1;

    // 实验记录（2026-11-26）：改成"只预留 + 按需提交"能省下 710 MB，
    // 但**代价是确定性退步**——三次运行全部停在 7652 行、退出码 3（稳定复现），
    // 而一次性提交能跑到 1.27 万行。可见标题对这块物理页有"已落地"的假设
    // （或对提交时机敏感）。所以退回一次性提交；内存问题另想办法。
    // 想再试：XBGTASIX_PHYS_LAZY=1
    static const bool lazy_phys = []() {
        char b[8] = {0};
        return GetEnvironmentVariableA("XBGTASIX_PHYS_LAZY", b, sizeof(b)) > 0 && b[0] == '1';
    }();
    void* block = lazy_phys
                      ? VirtualAlloc(nullptr, n * kPhysPage, MEM_RESERVE, PAGE_READWRITE)
                      : VirtualAlloc(nullptr, n * kPhysPage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (block && lazy_phys) {
        std::lock_guard<std::mutex> lock(g_res_mu);
        g_reserved.push_back(Region{static_cast<uint8_t*>(block), n * kPhysPage});
        if (!g_veh_installed.exchange(true)) {
            AddVectoredExceptionHandler(1, lazy_commit_handler);
        }
    }
    if (!block) {
        xbgtasix::logf("[xmem] XMemAllocatePhysicalPages(%zu 页 = %.1f MB) 失败 err=%lu", n,
                       n * kPhysPage / 1048576.0, GetLastError());
        *num_pages = 0;
        return 0;
    }
    {
        std::lock_guard<std::mutex> lock(g_phys_mu);
        g_phys_blocks.push_back(block);
    }
    for (size_t i = 0; i < n; ++i) {
        page_array[i] = reinterpret_cast<uint64_t>(static_cast<uint8_t*>(block) + i * kPhysPage);
    }
    static int logged = 0;
    if (logged++ < 24) {
        xbgtasix::logf("[xmem] 物理页 %zu 页（%.1f MB）-> %p", n, n * kPhysPage / 1048576.0, block);
    }
    return 1;
}

__declspec(dllexport) int __cdecl XMemFreePhysicalPages(size_t num_pages, uint64_t* page_array) {
    XB_XMEM_TRACE("XMemFreePhysicalPages", num_pages, page_array);
    if (num_pages == 0) return 1;
    if (!page_array) {
        SetLastError(ERROR_NOACCESS);
        return 0;
    }
    void* first = reinterpret_cast<void*>(static_cast<uintptr_t>(page_array[0]));
    if (!first) return 1;
    {
        std::lock_guard<std::mutex> lock(g_phys_mu);
        auto it = std::find(g_phys_blocks.begin(), g_phys_blocks.end(), first);
        if (it == g_phys_blocks.end()) return 1;   // 不是整块首地址：不重复释放
        g_phys_blocks.erase(it);
    }
    VirtualFree(first, 0, MEM_RELEASE);
    return 1;
}

__declspec(dllexport) void* __cdecl XMemMapPhysicalPages(void* va, size_t num_pages,
                                                         uint64_t* page_array) {
    XB_XMEM_TRACE("XMemMapPhysicalPages", va, num_pages);
    if (!va) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (num_pages == 0) return va;
    if (num_pages > SIZE_MAX / kPhysPage) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    auto* dst = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(va) &
                                           ~(static_cast<uintptr_t>(kPhysPage) - 1));
    // Mapping backs a reserved range with accessible memory. Commit before
    // copying so a write to each new page does not have to fault first.
    MEMORY_BASIC_INFORMATION region{};
    if (page_array && VirtualQuery(dst, &region, sizeof(region)) && region.State == MEM_RESERVE &&
        !VirtualAlloc(dst, num_pages * kPhysPage, MEM_COMMIT, PAGE_READWRITE)) {
        xbgtasix::logf("[xmem] XMemMapPhysicalPages commit failed addr=%p pages=%zu err=%lu",
                       dst, num_pages, GetLastError());
        return nullptr;
    }
    for (size_t i = 0; i < num_pages; ++i) {
        if (!page_array) continue;
        const uint64_t pfn = page_array[i];
        if (!pfn) continue;
        std::memcpy(dst + i * kPhysPage, reinterpret_cast<void*>(static_cast<uintptr_t>(pfn)),
                    kPhysPage);
    }
    static int logged = 0;
    if (logged++ < 16) {
        xbgtasix::logf("[xmem] XMemMapPhysicalPages %zu 页 -> %p（拷贝语义：PC 无物理页别名）",
                       num_pages, dst);
    }
    return va;
}

__declspec(dllexport) int __cdecl XMemMapPhysicalPagesScatter(void** addresses, size_t num_pages,
                                                              uint64_t* page_array) {
    XB_XMEM_TRACE("XMemMapPhysicalPagesScatter", addresses, num_pages);
    if (!addresses) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    // 主机语义：page_array 里是**物理页号（PFN）**，驱动按 PFN 把物理页映进 va。
    // 旧实现把 PFN 直接当指针去 memcpy —— 那是错的，也是 0xC0000005 的来源：
    // 实测 XMemMapPhysicalPagesScatter(va=0x94b070000, pfn=0x9f0000) 之后
    // ucrtbase memcpy 从 0xb21000 读 64 KB（MEM_FREE），崩在 xmem+0x3550。
    // PC 上没有"按 PFN 取页"这回事（主机物理页在桌面侧不存在），所以：
    //   * 不读 pfn 指向的任何地址；
    //   * 目标页按 64 KB 清零，保证内容是确定的（不是随机残值）；
    //   * 前若干次大声记日志，说明这是**语义缺口**，别把它当成实现完成。
    static std::atomic<unsigned> logged{0};
    for (size_t i = 0; i < num_pages; ++i) {
        auto* va = addresses[i];
        if (!va) continue;
        const uint64_t pfn = page_array ? page_array[i] : 0;
        if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
            xbgtasix::logf("[xmem] XMemMapPhysicalPagesScatter: va=%p pfn=0x%llx -> PC 上无物理页别名，"
                           "目标页清零（语义缺口，不是实现）",
                           va, static_cast<unsigned long long>(pfn));
        }
        std::memset(va, 0, kPhysPage);
    }
    return 1;
}

// 主机上 XMemTransferMemory 是 DMA：两端都是主机物理内存，永远"可访问"。
// PC 上两端可能是我们 MEM_RESERVE 出来、还没提交的页（24 GiB 那几块就是这样），
// 直接 memcpy 会在跨页时 0xC0000005（实测 addr=0xb21000、读、页状态 MEM_FREE）。
// 所以先按页把"落在我们自己保留区间里的页"提交，再判断两端是否整段可访问；
// 不满足就返回 false 并大声记日志，绝不盲拷。
namespace {

// 返回 [p, p+size) 里有多少页已经可访问（顺带提交落在 g_reserved 里的页）。
size_t make_pages_accessible(void* p, size_t size) {
    auto* base = static_cast<uint8_t*>(p);
    size_t ok = 0;
    for (size_t off = 0; off < size; off += 0x1000) {
        auto* a = base + off;
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(a, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0) {
            ++ok;
            continue;
        }
        if (commit_page_covering(a)) ++ok;
    }
    return ok;
}

}  // namespace

__declspec(dllexport) bool __cdecl XMemTransferMemory(void* dst, const void* src, size_t size) { XB_XMEM_TRACE("XMemTransferMemory", size, 0);
    if (!dst || !src || !size) return false;

    {
        static std::atomic<unsigned> seen{0};
        if (seen.fetch_add(1, std::memory_order_relaxed) < 32) {
            xbgtasix::logf("[xmem] XMemTransferMemory(dst=%p, src=%p, size=%zu)", dst, src, size);
        }
    }

    const size_t pages = (size + 0xFFF) / 0x1000;
    const size_t ok_dst = make_pages_accessible(dst, size);
    const size_t ok_src = make_pages_accessible(const_cast<void*>(src), size);

    if (ok_dst != pages || ok_src != pages) {
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
            xbgtasix::logf("[xmem] XMemTransferMemory 拒绝：dst=%p(%zu/%zu 页可访问) src=%p(%zu/%zu 页)"
                           " size=%zu（跨到未提交/未保留的页上）",
                           dst, ok_dst, pages, src, ok_src, pages, size);
        }
        return false;
    }

    std::memcpy(dst, src, size);   // 主机上是 DMA，PC 上用内存拷贝等效
    return true;
}


// 给 d3d12_xs 用：从描述符 arena 里切一块（若没有 arena 就返回 nullptr）。
// 返回的地址必须落在游戏预留的 VA 内，否则游戏会断言（见文件上方说明）。
__declspec(dllexport) void* __cdecl XMemReserveDescriptorArena(size_t bytes, size_t align) {
    std::lock_guard<std::mutex> lock(g_arena_mu);
    if (!g_descriptor_arena_base || bytes == 0) return nullptr;
    const size_t a = align ? align : 0x10000;
    size_t off = (g_descriptor_arena_used + a - 1) & ~(a - 1);
    if (off + bytes > g_descriptor_arena_size) {
        xbgtasix::logf("[xmem] 描述符 arena 不够：需要 %zu，剩余 %zu（arena %zu）", bytes,
                       g_descriptor_arena_size - g_descriptor_arena_used, g_descriptor_arena_size);
        return nullptr;
    }
    void* p = static_cast<uint8_t*>(g_descriptor_arena_base) + off;
    g_descriptor_arena_used = off + bytes;
    xbgtasix::logf("[xmem] 描述符 arena 切出 %zu 字节 @ %p（已用 %zu / %zu）", bytes, p,
                   g_descriptor_arena_used, g_descriptor_arena_size);
    return p;
}

}  // extern "C"
