// SPDX-License-Identifier: GPL-3.0-or-later
// acphal.cpp —— AcpHal.dll：Xbox 主机 APU（音频处理器）HAL 的 PC 端移植
//
// ============================ 移植来源 ============================
// 仓库：omonkeyz/xbox-emulator       （GPL-3.0，与本项目同许可）
//   crates/compat/acphal/src/lib.rs       APU 堆分配器 / APU 地址空间 / IAcpHal 对象
//   crates/compat/acphal/src/audiorec.rs  导出壳（#[no_mangle] extern "system"）+ 录制层
//   crates/compat/acphal/src/shape.rs     SHAPE 引擎（XMA/SRC/DMA/flowgraph/fence）—— 本移植不实现
//   docs/LIBRARY_MAP.md  "AcpHal.dll: 9 functions; implemented 9" 一节 —— 恰好就是本文件的 9 个导出
//
// ============================ 签名依据（重点） ============================
// 主机头文件在 NDA 内，签名只能从参考实现的 extern "system" 定义反推。逐个列出出处行，
// 便于以后核对（行号对应 2026-11 的 main 分支）：
//
//   ApuCreateHeap(size_a: u32, size_b: u32) -> i32                       audiorec.rs:330
//   ApuAlloc(pp: *mut *mut c_void, p_apu: *mut u32,
//            size: u32, align: u32, heap: u32) -> i32                    audiorec.rs:335
//   ApuFree(p: *mut c_void) -> i32                                       audiorec.rs:347
//   AcpHalAllocateShapeContexts(ctx: *mut u8) -> i32                     audiorec.rs:368
//   AcpHalReleaseShapeContexts() -> i32                                  audiorec.rs:393
//   AcpHalCreate(out: *mut *mut c_void) -> i32                           audiorec.rs:410
//   ApuMapVirtualAddress(p: *const c_void) -> u32                        lib.rs:331
//   ApuMapApuAddress(a: u32) -> *mut c_void                              lib.rs:336
//   ApuIsVirtualAddressValid(p: *const c_void, alignment: u32) -> i32     lib.rs:341
//
// ★ 与旧桩的差异（旧 console_hw.cpp 全是 uintptr_t f(void*,void*,void*) 的猜法）：
//   AcpHalCreate 1 个参数（旧 3）、AcpHalAllocateShapeContexts 1 个（旧 2）、
//   AcpHalReleaseShapeContexts 0 个（旧 2）、ApuCreateHeap 2 个（旧 3）、
//   ApuAlloc 5 个（旧 4）、ApuFree 1 个（旧 2）、ApuMapApuAddress 1 个（旧 3）、
//   ApuMapVirtualAddress 1 个（旧 3）。参数个数错时，多写的返回寄存器/栈槽位会让游戏
//   读到上一个函数的残值——比直接返回失败更难查。
//
// ============================ 分层策略 ============================
// 【真实现】APU 堆 + APU 地址空间（本文件的主体）。这两套逻辑在参考实现里是纯软件模型，
//   与主机硬件无关，可以逐行移植：
//     · ApuCreateHeap 建两个堆（A=0x01000000 起，B 紧随 A）+ 一个内部堆，
//       并在 A 上预留三块控制台内部结构（0x1000@0x100、0x14@4、8@4）。
//     · ApuAlloc/ApuFree = best-fit 小分配器：按"APU 地址对齐"算 pad，
//       块切成 [pad|user]，free 时与左右空闲邻居合并。
//     · ApuMapVirtualAddress / ApuMapApuAddress / ApuIsVirtualAddressValid = 双向地址翻译，
//       **只有落在"已分配块的用户区间"内才翻译成功**，堆里未用的空洞一律返回 0。
// 【真实现】AcpHalAllocateShapeContexts / AcpHalReleaseShapeContexts：校验 6 个计数、
//   按控制台每类的元素大小/对齐从堆 B 分配 6 组数组，并把 VA 与 APU 地址回填进
//   游戏给的结构体（+0x18 起 6 个 u64 VA，+0x48 起 6 个 u32 APU 地址）。
//   ——这部分同样是纯内存管理，不需要引擎；但它背后没有 SHAPE 引擎来服务这些上下文。
// 【诚实失败】AcpHalCreate：返回 E_NOTIMPL（0x80004001），见函数处详细说明。
//
// ============================ 其它硬性约束 ============================
// · 用 __declspec(dllexport) 导出，**不用 .def**（build.sh 对这三个模块直接 -shared，
//   不过 .def；同一份源码只编成一个 DLL，所以不会像旧 console_hw.cpp 那样一次导出 22 个符号）。
// · 目标只有 x86_64（build.sh 断言三元组），x64 只有一种调用约定，
//   所以 extern "C" 与参考实现的 extern "system" 等价，导出名也不带修饰。
// · 日志只走 xbgtasix::logf；**不要**用 XB_UNIMPL —— 它默认会 TerminateProcess。

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

#include "xbgtasix/trace.h"

namespace {

// ============================== 返回码 ==============================
// 照抄参考实现的常量（lib.rs:21-36）。这些是主机 acphal.dll 自己的私有码段
// （0x8AC8_xxxx / 0x8ACA_xxxx），PC 上没有对应定义，别用 HRESULT_FROM_WIN32 反推。
constexpr int32_t kS_OK = 0;
constexpr int32_t kE_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t kE_OUTOFMEMORY = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t kE_NOTIMPL = static_cast<int32_t>(0x80004001u);
constexpr int32_t kAPU_E_HEAP_ALREADY_CREATED = static_cast<int32_t>(0x8ACA0001u);
constexpr int32_t kAPU_E_HEAP_NOT_CREATED = static_cast<int32_t>(0x8ACA0002u);
constexpr int32_t kSHAPE_E_ALREADY_ALLOCATED = static_cast<int32_t>(0x8AC80001u);
constexpr int32_t kSHAPE_E_NOT_ALLOCATED = static_cast<int32_t>(0x8AC80002u);

// ============================== 控制台布局常量 ==============================
constexpr uint32_t kDefaultSizeA = 0x04000000;  // 64 MiB，参考实现的默认堆 A 大小
constexpr uint32_t kDefaultSizeB = 0x00114000;  // 堆 B 默认大小
constexpr uint32_t kApuBaseA = 0x01000000;      // 堆 A 的 APU 起始地址（控制台地址空间）
constexpr uint32_t kPageMask = 0x1000;

// SHAPE 上下文：6 类，每类 (计数上限, 单个元素大小, 单个元素对齐)，lib.rs:366-373
struct ShapeSize {
    uint32_t max_count;
    uint32_t elem_size;
    uint32_t elem_align;
};
constexpr ShapeSize kShapeSizes[6] = {
    {0x1000, 0x20, 0x20},  // [0] SRC 上下文
    {0x1000, 0xA0, 0x10},  // [1] 均衡器(fv) 上下文
    {0x1000, 0x20, 0x20},  // [2] 第二组 SRC
    {0x1000, 0x10, 0x10},  // [3] DMA 上下文
    {0x200, 0x40, 0x40},   // [4] XMA 上下文（与 [5] 共用一块连续分配）
    {0xE00, 0x20, 0x40},   // [5] PCM 上下文
};
// 游戏给的结构体（descriptor）里的偏移，lib.rs:436-438 与 audiorec.rs:377-380
constexpr size_t kDescCounts = 0x00;   // 6 * u32 计数
constexpr size_t kDescVa = 0x18;       // 6 * u64 回填 VA
constexpr size_t kDescApu = 0x48;      // 6 * u32 回填 APU 地址

// ============================== 小工具 ==============================
constexpr uint32_t round_up(uint32_t v, uint32_t align) {
    return (v + (align - 1)) & ~(align - 1);
}
constexpr uint64_t round_up64(uint64_t v, uint64_t align) {
    return (v + (align - 1)) & ~(align - 1);
}

// 非对齐读写：游戏传进来的 descriptor 不保证 8 字节对齐，用 memcpy 避免 UB。
uint32_t rd32(const void* base, size_t off) {
    uint32_t v = 0;
    std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(v));
    return v;
}
void wr32(void* base, size_t off, uint32_t v) {
    std::memcpy(static_cast<uint8_t*>(base) + off, &v, sizeof(v));
}
void wr64(void* base, size_t off, uint64_t v) {
    std::memcpy(static_cast<uint8_t*>(base) + off, &v, sizeof(v));
}

// ============================== 只记一次 ==============================
// 游戏在一次会话里会成千上万次调 ApuAlloc/ApuFree（每个音频缓冲一次）。
// 每个 API 的首调记详细参数，之后静默，避免把日志刷爆。
struct OnceTable {
    CRITICAL_SECTION cs{};
    const char* seen[128]{};
    int n = 0;
    OnceTable() { InitializeCriticalSection(&cs); }
};
OnceTable& once_table() {
    // 故意 new 出来不析构：DLL 卸载/进程退出时若还有工作线程在调，析构会变成 use-after-free。
    static OnceTable* t = new OnceTable();
    return *t;
}
bool first_time(const char* api) {
    OnceTable& t = once_table();
    EnterCriticalSection(&t.cs);
    bool first = true;
    for (int i = 0; i < t.n; ++i) {
        if (std::strcmp(t.seen[i], api) == 0) {
            first = false;
            break;
        }
    }
    if (first && t.n < static_cast<int>(sizeof(t.seen) / sizeof(t.seen[0]))) t.seen[t.n++] = api;
    LeaveCriticalSection(&t.cs);
    return first;
}

bool trace_on() {
    static const bool on = [] {
        char b[8] = {0};
        return GetEnvironmentVariableA("XBGTASIX_TRACE", b, sizeof(b)) > 0 && b[0] == '1';
    }();
    return on;
}

// XBGTASIX_TRACE=1 时逐次记录调用（带 400 行/调用点 的上限），用于对齐游戏的真实调用序列。
#define XBTRACE(api, ...)                                                              \
    do {                                                                               \
        static std::atomic<int> n_{0};                                                 \
        if (trace_on() && n_.fetch_add(1) < 400)                                       \
            ::xbgtasix::logf("[acphal][trace] " api " " __VA_ARGS__);                   \
    } while (0)
#define XBTRACE0(api)                                                                  \
    do {                                                                               \
        static std::atomic<int> n_{0};                                                 \
        if (trace_on() && n_.fetch_add(1) < 400) ::xbgtasix::logf("[acphal][trace] " api); \
    } while (0)

// ============================== APU 堆 ==============================
// 结构体布局照抄 lib.rs:48-66。blocks 按 off 升序；每个块记 pad（为满足用户对齐而在块首
// 空出的字节数）与 req（用户请求的字节数），这样 free 时能精确还原记账：
//   free  = Σ空闲块 size        used = Σreq        overhead = Σpad        count = 活跃块数
struct Block {
    uint32_t off;
    uint32_t size;
    bool used;
    uint32_t pad;
    uint32_t req;
};

struct Heap {
    uint8_t* va = nullptr;   // 宿主虚拟地址（VirtualAlloc，天然 64 KiB 对齐）
    uint32_t apu = 0;        // 这块内存对应的 APU 地址基址
    uint32_t size = 0;       // 已按 4 KiB 向上取整
    std::vector<Block> blocks;
    uint32_t free_bytes = 0;
    uint32_t used_bytes = 0;
    uint32_t overhead = 0;
    uint32_t count = 0;

    Heap() = default;
    // 拷贝没有意义（会两个对象共用一个 VirtualAlloc），只允许"转移所有权"的移动。
    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;
    Heap(Heap&& o) noexcept { *this = std::move(o); }
    Heap& operator=(Heap&& o) noexcept {
        if (this != &o) {
            va = o.va;
            apu = o.apu;
            size = o.size;
            blocks = std::move(o.blocks);
            free_bytes = o.free_bytes;
            used_bytes = o.used_bytes;
            overhead = o.overhead;
            count = o.count;
            o.va = nullptr;  // 源对象不再持有这块内存
            o.apu = 0;
            o.size = 0;
        }
        return *this;
    }

    // 失败路径上要能显式放掉已经拿到的 VirtualAlloc（正常堆到进程退出都不释放，
    // 与参考实现"静态 Mutex 里的 State 永不 drop"一致）。
    void release_storage() {
        if (va) {
            VirtualFree(va, 0, MEM_RELEASE);
            va = nullptr;
        }
        blocks.clear();
        size = apu = free_bytes = used_bytes = overhead = count = 0;
    }

    bool create(uint32_t want, uint32_t apu_base) {
        const uint32_t sz = round_up(want, kPageMask);
        if (sz == 0) return false;
        void* p = VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p) return false;
        va = static_cast<uint8_t*>(p);
        // 参考实现用 0x10000 对齐的 alloc_zeroed（lib.rs:71）。VirtualAlloc 的保留粒度
        // 就是 64 KiB，所以这里天然满足；万一不满足，对齐假设（APU 地址 = 基址 + 偏移）
        // 就会错，必须报出来而不是继续跑。
        if ((reinterpret_cast<uintptr_t>(va) & 0xFFFFu) != 0) {
            xbgtasix::logf("[acphal] 警告：VirtualAlloc 返回的基址 %p 不是 64 KiB 对齐，APU 对齐模型可能失真",
                           static_cast<void*>(va));
        }
        apu = apu_base;
        size = sz;
        blocks.clear();
        blocks.push_back(Block{0, sz, false, 0, 0});
        free_bytes = sz;
        used_bytes = 0;
        overhead = 0;
        count = 0;
        return true;
    }

    uint32_t largest_free() const {
        uint32_t best = 0;
        for (const Block& b : blocks) {
            if (!b.used && b.size > best) best = b.size;
        }
        return best;
    }

    // best-fit：在能装下的空闲块里挑 size 最小的那个（lib.rs:92-120）。
    // pad 是按 **APU 地址** 算的，不是按宿主 VA 算的 —— 游戏断言的是 APU 地址对齐。
    bool alloc(uint32_t sz, uint32_t align, uintptr_t* out_va, uint32_t* out_apu) {
        if (!va || align == 0) return false;
        size_t best = blocks.size();
        for (size_t i = 0; i < blocks.size(); ++i) {
            const Block& b = blocks[i];
            if (b.used) continue;
            const uint64_t apu_at = static_cast<uint64_t>(apu) + b.off;
            const uint64_t pad = round_up64(apu_at, align) - apu_at;
            if (pad + sz > static_cast<uint64_t>(b.size)) continue;
            if (best == blocks.size() || b.size < blocks[best].size) best = i;
        }
        if (best == blocks.size()) return false;

        const Block b = blocks[best];  // 先拷一份：下面的 insert 会让引用失效
        const uint64_t apu_at = static_cast<uint64_t>(apu) + b.off;
        const uint32_t pad = static_cast<uint32_t>(round_up64(apu_at, align) - apu_at);
        const uint32_t total = pad + sz;
        if (b.size > total) {
            blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(best) + 1,
                          Block{b.off + total, b.size - total, false, 0, 0});
        }
        blocks[best] = Block{b.off, total, true, pad, sz};
        free_bytes -= total;
        used_bytes += sz;
        overhead += pad;
        ++count;

        const uint32_t user_off = b.off + pad;
        // 参考实现 alloc_zeroed 并把用户区清零（lib.rs:118）：游戏可能假设新分配的
        // 音频上下文是 0，不清零会让它拿到上一个块的残值。
        std::memset(va + user_off, 0, sz);
        *out_va = reinterpret_cast<uintptr_t>(va) + user_off;
        *out_apu = apu + user_off;
        return true;
    }

    // 找"用户指针正好等于 block 用户区起点"的那个已用块（lib.rs:122-130）。
    // 注意用的是 ==，不是区间包含：传块中间地址来 free 会失败，和主机一致。
    size_t find_used_by_user(uintptr_t p) const {
        if (!va || p < reinterpret_cast<uintptr_t>(va)) return blocks.size();
        const uint64_t delta = p - reinterpret_cast<uintptr_t>(va);
        if (delta >= size) return blocks.size();
        const uint32_t off = static_cast<uint32_t>(delta);
        size_t lo = 0, hi = blocks.size();
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (blocks[mid].off <= off) lo = mid + 1; else hi = mid;
        }
        if (lo == 0) return blocks.size();
        const size_t i = lo - 1;
        const Block& b = blocks[i];
        if (b.used && b.off + b.pad == off) return i;
        return blocks.size();
    }

    // 归还并与左右空闲邻居合并（lib.rs:132-151）。
    bool release(uintptr_t p) {
        const size_t found = find_used_by_user(p);
        if (found == blocks.size()) return false;
        size_t i = found;
        const Block b = blocks[i];
        free_bytes += b.size;
        used_bytes -= b.req;
        overhead -= b.pad;
        --count;
        blocks[i] = Block{b.off, b.size, false, 0, 0};
        if (i + 1 < blocks.size() && !blocks[i + 1].used) {
            blocks[i].size += blocks[i + 1].size;
            blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        }
        if (i > 0 && !blocks[i - 1].used) {
            blocks[i - 1].size += blocks[i].size;
            blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
        }
        return true;
    }

    // off 落在哪个已用块的"用户区间"里（pad 段不算，lib.rs:153-157）。
    bool used_block_containing(uint32_t off, Block* out) const {
        size_t lo = 0, hi = blocks.size();
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (blocks[mid].off <= off) lo = mid + 1; else hi = mid;
        }
        if (lo == 0) return false;
        const Block& b = blocks[lo - 1];
        if (b.used && off >= b.off + b.pad && off < b.off + b.size) {
            if (out) *out = b;
            return true;
        }
        return false;
    }

    uint32_t map_va(uintptr_t p) const {
        if (!va || p < reinterpret_cast<uintptr_t>(va)) return 0;
        const uint64_t delta = p - reinterpret_cast<uintptr_t>(va);
        if (delta >= size) return 0;
        const uint32_t off = static_cast<uint32_t>(delta);
        if (!used_block_containing(off, nullptr)) return 0;
        return apu + off;
    }

    uintptr_t map_apu(uint32_t a) const {
        if (!va || a < apu) return 0;
        const uint32_t off = a - apu;
        if (off >= size) return 0;
        if (!used_block_containing(off, nullptr)) return 0;
        return reinterpret_cast<uintptr_t>(va) + off;
    }

    uint32_t user_size(uintptr_t p) const {
        const size_t i = find_used_by_user(p);
        if (i == blocks.size()) return 0;
        return blocks[i].size - blocks[i].pad;
    }
};

// ============================== 全局状态 ==============================
// 布局照抄 lib.rs:194-206：两个公开堆 + 一个内部堆 + SHAPE 上下文分配记录。
struct State {
    uint32_t cfg[2] = {kDefaultSizeA, kDefaultSizeB};
    std::optional<Heap> heaps[2];
    std::optional<Heap> internal_heap;
    bool shape_allocated = false;
    uintptr_t shape_ptrs[6] = {};
};

// 同样故意不析构（见 OnceTable 注释）；堆内存随进程退出由系统回收。
State& state() {
    static State* s = new State();
    return *s;
}
std::mutex& state_mutex() {
    static std::mutex* m = new std::mutex();
    return *m;
}

bool heap_created(const State& s) { return s.heaps[0].has_value(); }

// 建堆（lib.rs:217-236）。堆 B 的 APU 基址紧接 A 的"64 KiB 向上取整"之后，
// 再往后是内部堆——这个排列是控制台地址空间的一部分，游戏侧可能有断言依赖它。
int32_t ensure_created(State& s) {
    if (heap_created(s)) return kS_OK;

    const uint32_t size_a = round_up(s.cfg[0], kPageMask);
    if (size_a == 0) return kE_OUTOFMEMORY;

    Heap a;
    if (!a.create(size_a, kApuBaseA)) return kE_OUTOFMEMORY;

    const uint32_t apu_b = kApuBaseA + static_cast<uint32_t>(round_up64(size_a, 0x10000));
    Heap b;
    if (!b.create(s.cfg[1] + 0x2000, apu_b)) {
        a.release_storage();
        return kE_OUTOFMEMORY;
    }

    const uint32_t apu_i = apu_b + static_cast<uint32_t>(round_up64(b.size, 0x10000));
    Heap internal;
    const bool internal_ok = internal.create(0x1000, apu_i);

    // 控制台在创建堆时就预留了三块内部结构（lib.rs:227-229）；
    // 内部堆再预留 0x4c。这些块占用堆 A 的起始地址，会影响后续分配落到的位置，
    // 所以必须照做，否则 APU 地址布局与主机不一致。
    uintptr_t v0 = 0;
    uint32_t a0 = 0;
    a.alloc(0x1000, 0x100, &v0, &a0);
    a.alloc(0x14, 4, &v0, &a0);
    a.alloc(8, 4, &v0, &a0);
    if (internal_ok) internal.alloc(0x4c, 4, &v0, &a0);

    s.heaps[0] = std::move(a);
    s.heaps[1] = std::move(b);
    if (internal_ok) s.internal_heap = std::move(internal);

    const Heap& ha = *s.heaps[0];
    const Heap& hb = *s.heaps[1];
    xbgtasix::logf("[acphal] APU 堆已建立：A va=%p apu=0x%08X size=0x%X(预留 0x1000+0x14+8) / "
                   "B va=%p apu=0x%08X size=0x%X / 内部堆 apu=0x%08X%s",
                   static_cast<void*>(ha.va), ha.apu, ha.size, static_cast<void*>(hb.va), hb.apu,
                   hb.size, apu_i, internal_ok ? "" : "（创建失败，已跳过）");
    return kS_OK;
}

// 公开分配入口（lib.rs:238-254）。heap: 1=A, 2=B，其它值 E_INVALIDARG。
int32_t state_alloc(State& s, uint32_t heap, uint32_t sz, uint32_t align, uintptr_t* out_va,
                    uint32_t* out_apu) {
    int idx = -1;
    if (heap == 1) idx = 0;
    else if (heap == 2) idx = 1;
    if (idx < 0) return kE_INVALIDARG;
    if (sz == 0 || align == 0 || (align % 4) != 0 || (align & (align - 1)) != 0) return kE_INVALIDARG;

    const int32_t hr = ensure_created(s);
    if (hr < 0) return hr;

    // 最小对齐 0x100：请求 4 字节对齐实际会拿到 0x100（lib.rs:251）。
    uint32_t a = align;
    if ((a & 0xFFu) != 0) a = 0x100;

    const uint64_t rounded = (static_cast<uint64_t>(sz) + a - 1) & ~static_cast<uint64_t>(a - 1);
    if (rounded > 0xFFFFFFFFull) return kE_OUTOFMEMORY;
    if (!s.heaps[idx]->alloc(static_cast<uint32_t>(rounded), a, out_va, out_apu)) return kE_OUTOFMEMORY;
    return kS_OK;
}

int32_t state_free(State& s, uintptr_t p) {
    for (int k = 0; k < 2; ++k) {
        if (s.heaps[k].has_value() && s.heaps[k]->release(p)) return kS_OK;
    }
    return kE_INVALIDARG;
}

uint32_t state_map_va(const State& s, uintptr_t p) {
    for (int k = 0; k < 2; ++k) {
        if (!s.heaps[k].has_value()) continue;
        const uint32_t a = s.heaps[k]->map_va(p);
        if (a != 0) return a;
    }
    return 0;
}

uintptr_t state_map_apu(const State& s, uint32_t a) {
    for (int k = 0; k < 2; ++k) {
        if (!s.heaps[k].has_value()) continue;
        const uintptr_t p = s.heaps[k]->map_apu(a);
        if (p != 0) return p;
    }
    return 0;
}

// ============================== SHAPE 上下文 ==============================
// 校验规则（lib.rs:375-390）：
//   1) 6 个计数都不能超过各自的类型上限；
//   2) 只要 [0] 或 [5] 非 0，就必须满足 n[0] == n[4] + n[5]（XMA 与 PCM 上下文共用一块）。
int32_t alloc_shape_impl(uint8_t* ctx) {
    if (!ctx) return kE_INVALIDARG;

    uint32_t n[6];
    for (int k = 0; k < 6; ++k) n[k] = rd32(ctx, kDescCounts + static_cast<size_t>(k) * 4);

    for (int k = 0; k < 6; ++k) {
        if (n[k] > kShapeSizes[k].max_count) {
            xbgtasix::logf("[acphal] AcpHalAllocateShapeContexts：计数[%d]=0x%X 超过上限 0x%X -> E_INVALIDARG",
                           k, n[k], kShapeSizes[k].max_count);
            return kE_INVALIDARG;
        }
    }
    if ((n[0] != 0 || n[5] != 0) && n[0] != n[4] + n[5]) {
        xbgtasix::logf("[acphal] AcpHalAllocateShapeContexts：n[0]=0x%X != n[4]+n[5]=0x%X -> E_INVALIDARG",
                       n[0], n[4] + n[5]);
        return kE_INVALIDARG;
    }

    std::lock_guard<std::mutex> lock(state_mutex());
    State& s = state();
    if (s.shape_allocated) return kSHAPE_E_ALREADY_ALLOCATED;

    uintptr_t ptrs[6] = {};
    uint32_t apus[6] = {};
    int32_t fail = kS_OK;

    // [0..3] 各自独立分配（从堆 B）
    for (int k = 0; k < 4; ++k) {
        if (n[k] == 0) continue;
        const uint32_t bytes = n[k] * kShapeSizes[k].elem_size;
        if (state_alloc(s, 2, bytes, kShapeSizes[k].elem_align, &ptrs[k], &apus[k]) != kS_OK) {
            fail = kE_OUTOFMEMORY;
            break;
        }
    }
    // [4] XMA 与 [5] PCM 共用一块：先 XMA（0x40/个），紧跟 PCM（0x20/个），整体 0x40 对齐
    if (fail == kS_OK && n[4] + n[5] != 0) {
        const uint32_t bytes = n[4] * 0x40 + n[5] * 0x20;
        uintptr_t va = 0;
        uint32_t apu = 0;
        if (state_alloc(s, 2, bytes, 0x40, &va, &apu) != kS_OK) {
            fail = kE_OUTOFMEMORY;
        } else {
            if (n[4] != 0) {
                ptrs[4] = va;
                apus[4] = apu;
            }
            if (n[5] != 0) {
                ptrs[5] = va + static_cast<uintptr_t>(n[4]) * 0x40;
                apus[5] = apu + n[4] * 0x40;
            }
        }
    }

    if (fail != kS_OK) {
        // 回滚已分配的部分。注意参考实现的 k==4 特例：XMA 空但 PCM 非空时，
        // 那块合并分配记在 ptrs[5] 上（lib.rs:424-433）。
        for (int k = 0; k < 5; ++k) {
            const uintptr_t p = (k == 4 && ptrs[4] == 0) ? ptrs[5] : ptrs[k];
            if (p != 0) state_free(s, p);
        }
        xbgtasix::logf("[acphal] AcpHalAllocateShapeContexts：分配失败 -> 0x%08X（已回滚）",
                       static_cast<uint32_t>(fail));
        return fail;
    }

    // 回填游戏的结构体：+0x18 起 6 个 VA，+0x48 起 6 个 APU 地址
    for (int k = 0; k < 6; ++k) {
        wr64(ctx, kDescVa + static_cast<size_t>(k) * 8, static_cast<uint64_t>(ptrs[k]));
        wr32(ctx, kDescApu + static_cast<size_t>(k) * 4, apus[k]);
    }

    s.shape_allocated = true;
    for (int k = 0; k < 6; ++k) s.shape_ptrs[k] = ptrs[k];
    // [4]/[5] 是同一块，释放时只释放一次
    if (ptrs[4] == 0) s.shape_ptrs[4] = 0;

    xbgtasix::logf("[acphal] AcpHalAllocateShapeContexts：src=%u eq=%u fv=%u dma=%u xma=%u pcm=%u "
                   "-> S_OK（数组已从 APU 堆 B 分配；背后没有 SHAPE 引擎，提交命令会返回 E_NOTIMPL）",
                   n[0], n[1], n[2], n[3], n[4], n[5]);
    return kS_OK;
}

int32_t release_shape_impl() {
    std::lock_guard<std::mutex> lock(state_mutex());
    State& s = state();
    if (!s.shape_allocated) return kSHAPE_E_NOT_ALLOCATED;
    for (int k = 0; k < 6; ++k) {
        if (s.shape_ptrs[k] != 0) state_free(s, s.shape_ptrs[k]);
        s.shape_ptrs[k] = 0;
    }
    s.shape_allocated = false;
    xbgtasix::logf("[acphal] AcpHalReleaseShapeContexts -> S_OK（6 组上下文数组已归还 APU 堆）");
    return kS_OK;
}

// 诊断用：把两个堆的记账打一行（对应参考实现的 ApuHeapGetState，本模块不导出它）。
void log_heap_state(const char* why) {
    State& s = state();
    if (!heap_created(s)) {
        xbgtasix::logf("[acphal] 堆状态(%s)：尚未创建", why);
        return;
    }
    for (int k = 0; k < 2; ++k) {
        const Heap& h = *s.heaps[k];
        xbgtasix::logf("[acphal] 堆状态(%s) heap=%d apu=0x%08X free=0x%X used=0x%X overhead=0x%X "
                       "largest=0x%X count=%u",
                       why, k + 1, h.apu, h.free_bytes, h.used_bytes, h.overhead, h.largest_free(),
                       h.count);
    }
}

}  // namespace

// ============================================================================
//                              导出的 9 个符号
// ============================================================================
extern "C" {

// ============================== ApuCreateHeap ==============================
// 参考：audiorec.rs:330 / lib.rs:273-288
// 语义要点：
//   · size_a == 0 -> E_INVALIDARG（堆 A 必须有大小）
//   · 已经建过 -> APU_E_HEAP_ALREADY_CREATED（不是 S_OK：游戏用它判断"是否首次"）
//   · size_b == 0 时保留默认值，**不会**把 B 建成 0 大小
__declspec(dllexport) int32_t ApuCreateHeap(uint32_t size_a, uint32_t size_b) {
    XBTRACE("ApuCreateHeap", "sizeA=0x%X sizeB=0x%X", size_a, size_b);
    std::lock_guard<std::mutex> lock(state_mutex());
    State& s = state();

    int32_t hr;
    if (size_a == 0) {
        hr = kE_INVALIDARG;
    } else if (heap_created(s)) {
        hr = kAPU_E_HEAP_ALREADY_CREATED;
    } else {
        s.cfg[0] = size_a;
        if (size_b != 0) s.cfg[1] = size_b;
        hr = ensure_created(s);
    }
    xbgtasix::logf("[acphal] ApuCreateHeap(sizeA=0x%X, sizeB=0x%X) -> 0x%08X", size_a, size_b,
                   static_cast<uint32_t>(hr));
    if (hr == kS_OK) log_heap_state("创建后");
    return hr;
}

// ============================== ApuAlloc ==============================
// 参考：audiorec.rs:335 / lib.rs:290-316
//   ApuAlloc(&mut p, &mut apu, size, align, heap)
//   任何失败都会先把 *pp/*p_apu 置 0 —— 游戏会拿它当"分配失败时指针为空"的依据。
__declspec(dllexport) int32_t ApuAlloc(void** pp, uint32_t* p_apu, uint32_t size, uint32_t align,
                                       uint32_t heap) {
    XBTRACE("ApuAlloc", "size=0x%X align=0x%X heap=%u", size, align, heap);
    if (pp) *pp = nullptr;
    if (p_apu) *p_apu = 0;
    if (!pp) return kE_INVALIDARG;

    uintptr_t va = 0;
    uint32_t apu = 0;
    int32_t hr;
    try {
        std::lock_guard<std::mutex> lock(state_mutex());
        hr = state_alloc(state(), heap, size, align, &va, &apu);
    } catch (const std::bad_alloc&) {
        // 块描述表扩容失败：当成内存不足，别让异常穿过 C ABI 边界（会直接 terminate）
        hr = kE_OUTOFMEMORY;
    }
    if (hr == kS_OK) {
        *pp = reinterpret_cast<void*>(va);
        if (p_apu) *p_apu = apu;
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 16) {
            xbgtasix::logf("[acphal] ApuAlloc(size=0x%X, align=0x%X, heap=%u) -> va=%p apu=0x%08X",
                           size, align, heap, reinterpret_cast<void*>(va), apu);
        }
    } else {
        static std::atomic<int> logged{0};
        if (first_time("ApuAlloc-fail") || logged.fetch_add(1) < 32) {
            xbgtasix::logf("[acphal] ApuAlloc(size=0x%X, align=0x%X, heap=%u) 失败 -> 0x%08X",
                           size, align, heap, static_cast<uint32_t>(hr));
        }
    }
    return hr;
}

// ============================== ApuFree ==============================
// 参考：audiorec.rs:347 / lib.rs:318-328
// 只接受"分配时返回的那个用户指针"；传块中间地址/野指针 -> E_INVALIDARG（不是崩）。
__declspec(dllexport) int32_t ApuFree(void* p) {
    XBTRACE("ApuFree", "p=%p", p);
    if (!p) return kE_INVALIDARG;
    bool ok = false;
    try {
        std::lock_guard<std::mutex> lock(state_mutex());
        ok = state_free(state(), reinterpret_cast<uintptr_t>(p)) == kS_OK;
    } catch (const std::bad_alloc&) {
        ok = false;
    }
    if (!ok) {
        static std::atomic<int> logged{0};
        if (first_time("ApuFree") || logged.fetch_add(1) < 32) {
            xbgtasix::logf("[acphal] ApuFree(%p)：不是本堆分配出去的指针 -> E_INVALIDARG", p);
        }
        return kE_INVALIDARG;
    }
    return kS_OK;
}

// ============================== APU 地址翻译 ==============================
// 这三个函数是游戏的"APU 地址空间"视图：APU 地址是 32 位、从 0x01000000 起的一段
// 独立地址空间，只有堆里**已分配块的用户区间**才可翻译（空洞一律失败）。
// 参考：lib.rs:330-345
__declspec(dllexport) uint32_t ApuMapVirtualAddress(const void* p) {
    XBTRACE0("ApuMapVirtualAddress");
    uint32_t a;
    {
        // 必须持锁：blocks 是可变长数组，与 ApuAlloc/ApuFree 并发时不加锁就是数据竞争
        // （参考实现的 map 系列同样是 lock().map_va(...)）。
        std::lock_guard<std::mutex> lock(state_mutex());
        a = state_map_va(state(), reinterpret_cast<uintptr_t>(p));
    }
    if (first_time("ApuMapVirtualAddress")) {
        xbgtasix::logf("[acphal] ApuMapVirtualAddress(%p) -> 0x%08X（未分配区间一律返回 0）", p, a);
    }
    return a;
}

__declspec(dllexport) void* ApuMapApuAddress(uint32_t a) {
    XBTRACE("ApuMapApuAddress", "apu=0x%08X", a);
    uintptr_t p;
    {
        std::lock_guard<std::mutex> lock(state_mutex());
        p = state_map_apu(state(), a);
    }
    if (first_time("ApuMapApuAddress")) {
        xbgtasix::logf("[acphal] ApuMapApuAddress(0x%08X) -> %p（未分配区间一律返回空）", a,
                       reinterpret_cast<void*>(p));
    }
    return reinterpret_cast<void*>(p);
}

// alignment == 0 视为"不要求对齐"（参考实现把 0 当 1，lib.rs:343）。
__declspec(dllexport) int32_t ApuIsVirtualAddressValid(const void* p, uint32_t alignment) {
    XBTRACE("ApuIsVirtualAddressValid", "p=%p align=0x%X", p, alignment);
    uint32_t a;
    {
        std::lock_guard<std::mutex> lock(state_mutex());
        a = state_map_va(state(), reinterpret_cast<uintptr_t>(p));
    }
    const uint32_t al = (alignment == 0) ? 1u : alignment;
    const bool ok = (a != 0) && ((a & (al - 1)) == 0);
    if (first_time("ApuIsVirtualAddressValid")) {
        xbgtasix::logf("[acphal] ApuIsVirtualAddressValid(%p, align=0x%X) -> %d（apu=0x%08X）", p,
                       alignment, ok ? 1 : 0, a);
    }
    return ok ? 1 : 0;
}

// ============================== AcpHalAllocateShapeContexts ==============================
// 参考：audiorec.rs:368 / lib.rs:375-448
// 与旧桩的关键差别：**它只有 1 个参数**，而且必须把 VA/APU 地址回填进游戏的结构体。
// 只分配不填写，游戏后续按结构体里的空指针访问 -> 立即崩。
__declspec(dllexport) int32_t AcpHalAllocateShapeContexts(uint8_t* ctx) {
    XBTRACE("AcpHalAllocateShapeContexts", "ctx=%p", static_cast<void*>(ctx));
    int32_t hr;
    try {
        hr = alloc_shape_impl(ctx);
    } catch (const std::bad_alloc&) {
        hr = kE_OUTOFMEMORY;
    }
    if (hr != kS_OK && hr != kSHAPE_E_ALREADY_ALLOCATED) {
        // alloc_shape_impl 里已经打过详细日志，这里只补一行汇总
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 8) {
            xbgtasix::logf("[acphal] AcpHalAllocateShapeContexts(ctx=%p) -> 0x%08X",
                           static_cast<void*>(ctx), static_cast<uint32_t>(hr));
        }
    }
    return hr;
}

// ============================== AcpHalReleaseShapeContexts ==============================
// 参考：audiorec.rs:393 / lib.rs:450-462。**0 个参数**。
// 从没分配过时返回 SHAPE_E_NOT_ALLOCATED（不是 S_OK）—— 这是游戏用来判断
// "上下文是否已被别处释放"的路径，返回 S_OK 会掩盖双重释放。
__declspec(dllexport) int32_t AcpHalReleaseShapeContexts() {
    XBTRACE0("AcpHalReleaseShapeContexts");
    return release_shape_impl();
}

// ============================== AcpHalCreate ==============================
// 参考：audiorec.rs:410 / lib.rs:755-774。**1 个参数（out）**，旧桩写成 3 个。
//
// 这里**故意**返回 E_NOTIMPL，理由：
//   参考实现的 AcpHalCreate 返回一个"控制台布局的 IAcpHal" = 12 槽 COM 风格 vtable
//   （lib.rs:736-749，槽位顺序：Connect / Disconnect / SubmitCommands / PopMessage /
//   GetNumMessages / Release / 未支持 x4 / MapDriverRegion / Destroy）。游戏会直接
//   调用这些槽位里我们自己填的函数指针，而槽位顺序只能从主机 AcpHal.dll 的反汇编得到，
//   我无法用你手上的这份 GTA6 构建验证它。填错一格的后果是"游戏把 A 当 B 调"：
//   例如把 PopMessage（会往调用方指针写 28 字节消息）当成某个 setter 调用，
//   就是直接踩坏游戏内存 —— 比返回失败糟得多，正是需求里说的"比返回失败更糟"那一类。
//   此外这些槽位背后需要 SHAPE 引擎（XMA 解码 / SRC / DMA / flowgraph / fence），
//   参考实现花了 shape.rs 728 行 + xma/sr 解码器来实现，本次不移植。
//   因此这里诚实失败，让游戏走"没有音频硬件"的分支。
//
// 若以后要把这条路打通，顺序建议：
//   1) 拿主机 AcpHal.dll 核对 12 个槽位的顺序与参数（尤其 SubmitCommands 的
//      (kind, id:u64, frame, payload, fence) 与 PopMessage 的 28 字节消息布局）；
//   2) 移植 shape.rs 的引擎；
//   3) 再把 vtable 填进来（Release/Destroy 建议"只标记不真正 free"，避免 UAF）。
__declspec(dllexport) int32_t AcpHalCreate(void** out) {
    XBTRACE0("AcpHalCreate");
    if (out) *out = nullptr;
    if (!out) return kE_INVALIDARG;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 4) {
        xbgtasix::logf("[acphal] AcpHalCreate -> E_NOTIMPL：没有 SHAPE 引擎（XMA/SRC/DMA/flowgraph），"
                       "且 12 槽 IAcpHal vtable 的槽位顺序无法用本构建验证；给一个槽位可能错的对象"
                       "会让游戏把我们的函数当别的成员调用。详见本文件 AcpHalCreate 处注释。");
    }
    return kE_NOTIMPL;
}

}  // extern "C"
