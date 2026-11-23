// SPDX-License-Identifier: GPL-3.0-or-later
// host_stall.cpp —— "仅主机侧停顿"的账本 + 主模块 Sleep 的 IAT 钩子
//
// 编进 xmem.dll（主 exe 的静态导入，DllMain 最早跑）。设计理由与边界见
// include/xbgtasix/host_stall.h 的注释。
//
// 关键不变量
// ----------
//  * 累加器是**单调**的：只增不减，统计永远以"从某个快照以来的增量"形式使用，
//    所以多个线程各自取快照互不干扰；
//  * **进行中**的停顿也要算进去（`stalled_ns()` 在 depth > 0 时加上 now-start）。
//    否则 watchdog 在停顿期间会把"醒着的时间"攒够，Sleep 提前返回，那一轮
//    就不到 2 s，标题的计数不会被重置 —— 修复等于没做；
//  * thunk 只碰时间与真实 Sleep，不写日志、不分配内存、不加锁（watchdog 线程
//    可能在任何时刻调用它）。
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "xbgtasix/host_stall.h"

namespace {

using SleepFn = void(WINAPI*)(DWORD);

std::atomic<std::uint64_t> g_stall_total_ns{0};   // 已结束的停顿累计
std::atomic<std::uint32_t> g_stall_depth{0};      // 进行中的嵌套层数
std::atomic<std::uint64_t> g_stall_start_ns{0};   // 最外层开始时刻
std::atomic<int> g_installed{0};

SleepFn g_real_sleep = nullptr;                   // 原 Sleep

std::uint64_t now_ns() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    static const std::uint64_t freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<std::uint64_t>(f.QuadPart ? f.QuadPart : 1);
    }();
    // 先除后乘会丢精度，这里用 128 位以上的整数运算：ns = c * 1e9 / freq
    const std::uint64_t c64 = static_cast<std::uint64_t>(c.QuadPart);
    const std::uint64_t sec = c64 / freq;
    const std::uint64_t rem = c64 % freq;
    return sec * 1000000000ull + (rem * 1000000000ull) / freq;
}

// 到目前为止（含进行中的停顿）累计的停顿纳秒数
std::uint64_t stalled_ns() {
    std::uint64_t total = g_stall_total_ns.load(std::memory_order_relaxed);
    if (g_stall_depth.load(std::memory_order_acquire) > 0) {
        const std::uint64_t start = g_stall_start_ns.load(std::memory_order_relaxed);
        const std::uint64_t now = now_ns();
        if (now > start) total += now - start;
    }
    return total;
}

void stall_begin() {
    if (g_stall_depth.fetch_add(1, std::memory_order_acq_rel) == 0) {
        g_stall_start_ns.store(now_ns(), std::memory_order_release);
    }
}

void stall_end() {
    if (g_stall_depth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        const std::uint64_t start = g_stall_start_ns.load(std::memory_order_acquire);
        const std::uint64_t now = now_ns();
        if (now > start) g_stall_total_ns.fetch_add(now - start, std::memory_order_acq_rel);
    }
}

// ------------------------------------------------------------------ Sleep
// n >= 1 s：必须攒满"停顿之外"的 n ms 才返回。这是让标题自己那条
// "本轮超过 2 s 就把计数重来"的规则生效的唯一手段。
void WINAPI sleep_thunk(DWORD ms) {
    SleepFn real = g_real_sleep;
    if (true || !real || ms < 1000) {   // MUTATION
        if (real) {
            real(ms);
        } else {
            ::Sleep(ms);
        }
        return;
    }

    const std::uint64_t want_ns = static_cast<std::uint64_t>(ms) * 1000000ull;
    const std::uint64_t t0 = now_ns();
    const std::uint64_t s0 = stalled_ns();

    for (;;) {
        const std::uint64_t elapsed = now_ns() - t0;
        const std::uint64_t stalled = stalled_ns() - s0;
        const std::uint64_t awake = elapsed > stalled ? elapsed - stalled : 0;
        if (awake >= want_ns) return;

        // 分段睡：留出重算的粒度，同时不把 CPU 空转掉
        std::uint64_t left_ms = (want_ns - awake) / 1000000ull;
        if (left_ms == 0) left_ms = 1;
        if (left_ms > 100) left_ms = 100;
        real(static_cast<DWORD>(left_ms));
    }
}

// ------------------------------------------------------------------ IAT 改写
bool patch_slot(void** slot, void* replacement) {
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = replacement;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return true;
}

bool name_is(const char* name, const char* want) { return std::strcmp(name, want) == 0; }

// 只改主模块的 IAT：绝不碰系统 DLL。任何解析失败都返回 false（调用方只记一行）。
bool patch_main_module_sleep() {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) return false;
    auto* base = reinterpret_cast<unsigned char*>(exe);

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew <= 0) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;

    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) return false;

    // 原函数指针从任何一个"确实是 Sleep"的槽里取，beats 硬编码 kernel32 的地址
    void** first_slot = nullptr;
    for (const auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         imp->Name; ++imp) {
        const char* dll = reinterpret_cast<const char*>(base + imp->Name);
        const bool plausible =
            _stricmp(dll, "KERNEL32.dll") == 0 || _stricmp(dll, "KERNELBASE.dll") == 0 ||
            _strnicmp(dll, "api-ms-win-core-synch", 21) == 0;
        if (!plausible) continue;
        if (!imp->OriginalFirstThunk || !imp->FirstThunk) continue;

        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
        for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
            if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;   // 按序号导入，不是 Sleep
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                base + names[i].u1.AddressOfData);
            if (!name_is(reinterpret_cast<const char*>(by_name->Name), "Sleep")) continue;

            void** slot = reinterpret_cast<void**>(&slots[i].u1.Function);
            if (slot && *slot) {
                if (!first_slot) first_slot = slot;
                if (!patch_slot(slot, reinterpret_cast<void*>(&sleep_thunk))) return false;
                // 继续找：同一个 exe 可能从多个 DLL 各导入一次 Sleep
            }
        }
    }

    return first_slot != nullptr;   // 主模块没导入 Sleep = 机制无处生效
}

const xbgtasix::host_stall::vtable kApi{&stall_begin, &stall_end,
                                        [] { return g_installed.load(); }};

}  // namespace

// 单一导出：客户端（d3d12_xs.dll）用一张表把三个入口取走，
// 这样 xmem.dll 的导出数只 +1。
extern "C" __declspec(dllexport) const xbgtasix::host_stall::vtable* WINAPI
XbgtasixHostStallApi() {
    return &kApi;
}

namespace xbgtasix::host_stall {

bool install() {
    static std::atomic<int> done{0};
    if (done.load()) return g_installed.load() != 0;
    if (done.exchange(1)) return g_installed.load() != 0;

    // 真实 Sleep 一律走 kernel32 的实现：我们只改主模块的 IAT，
    // 本 DLL 自己的导入槽不受影响，所以 &::Sleep 就是原函数。
    g_real_sleep = &::Sleep;
    const bool ok = patch_main_module_sleep();
    g_installed.store(ok ? 1 : 0);
    return ok;
}

int installed() { return g_installed.load(); }

}  // namespace xbgtasix::host_stall
