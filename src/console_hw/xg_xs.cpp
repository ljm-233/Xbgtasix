// SPDX-License-Identifier: GPL-3.0-or-later
// xg_xs.cpp —— Xbox 主机 GPU 纹理 swizzle 层（xg_xs.dll）的 PC 移植
//
// ============================ 许可与移植来源 ============================
// 本文件：GPL-3.0-or-later（与本项目一致）。
// 语义移植来源：TomBox，GPL-3.0（与本项目同许可，因此按 GPL-3.0 第 5 条做语义移植）
//   https://github.com/omonkeyz/xbox-emulator
//   - crates/compat/xg_xs/src/lib.rs       XGCreateTextureComputer 的 D3D12 desc 解析与返回码
//   - crates/compat/xg_xs/src/computer.rs  纹理计算器对象（12 槽 vtable）的形状
//   - docs/LIBRARY_MAP.md                  导出清单对账（xg_xs.dll 的条目）
// 参考实现是从**主机二进制**逆出来的（NDA 头文件不可得），本文件只移植其可观察语义。
//
// ============================ 本模块导出（只有 2 个）============================
//   XGSuggestSwizzleMode     游戏 game_scarlett_master.exe 的导入表实测要它
//   XGCreateTextureComputer  同上
//
// ============================ 一条重要的对账事实 ============================
// 参考实现**没有** XGSuggestSwizzleMode 这个导出：
//   - crates/compat/xg_xs/exports.txt 列了 23 个导出（XGComputeOptimalSwizzleMode、
//     XGComputeTexture2DLayout、XGCreateTextureComputer、XGEncode/DecodeDCC …），
//     其中没有 XGSuggestSwizzleMode；
//   - docs/LIBRARY_MAP.md 里 4 个 OS 版本的 xg_xs 导出表也没有它；
//   - 在参考仓库做代码检索 "XGSuggestSwizzleMode" 命中 0 条。
// 所以本移植没有它的权威参数表（见函数上方注释的处理方式）。
// 它在微软 Xbox 官方文档里确实存在（devdocs.xbox.com → graphics/xg_xs → members）：
//   "XGSuggestSwizzleMode: 为指定的资源描述计算正确的调配模式。"
// 即：入参是**资源描述**，返回 XG_SWIZZLE_MODE（主机 swizzle 模式枚举）。
//
// ============================ 为什么 PC 上要返回 0 ============================
// 主机上纹理按 XG swizzle 模式摆放在显存里，游戏自己（或用 XGCreateTextureComputer）
// 按该模式计算偏移/摆数据。PC 的 D3D12 自己决定平铺方式（D3D12_TEXTURE_LAYOUT +
// 驱动私有 tiling），任何非 0（tiled）模式都会让游戏按主机布局去写资源，
// 在 PC 上必然对不上。所以本移植一律给 0 = 线性/未指定，让游戏走"我自己按行摆"的路径，
// 再由 PC 侧 D3D12 的 ROW_MAJOR 上传路径接住。
//
// 编译：本文件只依赖 MinGW 自带的 windows.h / d3d12.h，不链接任何库。
// 日志：走 xbgtasix::logf（同目录 trace.cpp 实现），行首带 [xg_xs] 便于对账。

#include <windows.h>

#include <d3d12.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "xbgtasix/trace.h"

namespace {

// ============================ 共享小工具 ============================

// 主机接口的返回码（HRESULT 与 NTSTATUS 混用的情况在主机构建里很常见）
constexpr int32_t kS_OK = 0;
constexpr int32_t kE_NOTIMPL = static_cast<int32_t>(0x80004001u);
constexpr int32_t kE_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t kE_POINTER = static_cast<int32_t>(0x80004003u);

// 统一前缀 [xg_xs]：本模块的日志要和别的 shim（xapu/chad/acphal）混在同一个日志里，
// 前缀是唯一能按模块 grep 出来的东西。先格式化到栈上再拼前缀，避免 shim 里做堆分配。
void tg(const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    char line[1088];
    std::snprintf(line, sizeof(line), "[xg_xs] %s", body);
    ::xbgtasix::logf("%s", line);
}

// 指针是否真的可读（/可写）：用 VirtualQuery 逐段确认已提交且没有 PAGE_NOACCESS/PAGE_GUARD。
// 主机 shim 拿到的指针绝大多数来自游戏堆，正常情况一次 VirtualQuery 就够；
// 这里只在"日志探测"里用，绝不用它替代真正的参数校验。
bool probe_committed(const void* p, size_t len, bool want_writable) {
    if (p == nullptr) return false;
    auto addr = reinterpret_cast<uintptr_t>(p);
    if (len == 0) return true;
    const uintptr_t end = addr + len;
    if (end < addr) return false;  // 溢出
    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD prot = mbi.Protect & 0xffu;
        if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
        const bool ok = want_writable
                            ? (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE ||
                               prot == PAGE_EXECUTE_WRITECOPY)
                            : (prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
                               prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
                               prot == PAGE_EXECUTE_WRITECOPY);
        if (!ok) return false;
        const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (region_end <= addr) return false;
        addr = region_end;
    }
    return true;
}

// 阈值型日志：热路径（每个纹理一次）前 N 次详细打，之后每 M 次打一条摘要。
// 目的：既能看清"真机调用序列"的开头，又不会把日志刷爆。
constexpr uint32_t kDetailCalls = 64;
constexpr uint32_t kSummaryEvery = 1024;

struct CallCounter {
    uint32_t n = 0;  // 非原子：xg_xs 的调用都发生在游戏主线程/加载线程，且只影响日志
};

bool detail_call(CallCounter& c) { return c.n++ < kDetailCalls; }
bool summary_call(CallCounter& c) { return c.n % kSummaryEvery == 0; }

// ============================ XGCreateTextureComputer 的 desc ============================
// 参考实现里 XGCreateTextureComputer(desc: *const u8, out: *mut *mut c_void) -> HRESULT
// 的 desc **就是 PC 的 D3D12_RESOURCE_DESC**（对照 crates/compat/xg_xs/src/lib.rs 的
// D3D12Desc::read：dimension@0x00、width@0x10(u64)、height@0x18、depthOrArraySize@0x1c(u16)、
// mipLevels@0x1e(u16)、format@0x20、sampleCount@0x24、sampleQuality@0x28、layout@0x2c、
// flags@0x30）。这和 d3d12.h 里 D3D12_RESOURCE_DESC 的布局逐字段吻合，
// 所以这里直接用 Windows 的 D3D12_RESOURCE_DESC，不再手写偏移。

// D3D12_TEXTURE_LAYOUT：PC 语义（0=UNKNOWN 由驱动决定，1=ROW_MAJOR 行主序，2=64KB_STANDARD_SWIZZLE）
const char* layout_name(D3D12_TEXTURE_LAYOUT l) {
    switch (l) {
        case D3D12_TEXTURE_LAYOUT_UNKNOWN: return "UNKNOWN(驱动决定)";
        case D3D12_TEXTURE_LAYOUT_ROW_MAJOR: return "ROW_MAJOR(线性)";
        case D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE: return "64KB_UNDEFINED_SWIZZLE";
        case D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE: return "64KB_STANDARD_SWIZZLE(PC 平铺)";
        default: return "?";
    }
}

// ============================ 日志：把 desc 逐字段打出来 ============================
// 无论成功还是失败都打——这份日志正是用来和"真机调用序列"对账的。
void log_desc(const char* who, const D3D12_RESOURCE_DESC& d) {
    tg("%s desc{dim=%u(%s) width=%llu height=%u depthOrArray=%u mips=%u fmt=%u samples=%u quality=%u "
       "layout=%u(%s) flags=0x%x}",
       who, static_cast<unsigned>(d.Dimension),
       d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
           ? "BUFFER"
           : (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
                  ? "TEX1D"
                  : (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ? "TEX2D" : "TEX3D")),
       static_cast<unsigned long long>(d.Width), static_cast<unsigned>(d.Height),
       static_cast<unsigned>(d.DepthOrArraySize), static_cast<unsigned>(d.MipLevels),
       static_cast<unsigned>(d.Format), static_cast<unsigned>(d.SampleDesc.Count),
       static_cast<unsigned>(d.SampleDesc.Quality), static_cast<unsigned>(d.Layout), layout_name(d.Layout),
       static_cast<unsigned>(d.Flags));
}

}  // namespace

// ============================================================================================
// 导出 1：XGSuggestSwizzleMode
// ============================================================================================
// 语义（微软 Xbox 文档）：为给定的资源描述**建议**一个 swizzle 模式，返回 XG_SWIZZLE_MODE。
//
// 参数表的依据与不确定性（必须说清）：
//   * 参考实现没有这个导出（exports.txt / LIBRARY_MAP / 代码检索都为 0，见文件头），
//     所以不存在"从参考实现确认签名"这回事；
//   * 官方文档只给了语义（"为指定的资源描述计算正确的调配模式"）和返回类型 XG_SWIZZLE_MODE
//     （32 位枚举），没有公开 x64 ABI 的参数列表（头文件在 NDA 内）；
//   * 因此这里按 **ABI 安全**的方式声明：前两个整型/指针参数在 x64 调用约定下必有定义，
//     多传的实参由调用方负责、被调方忽略即可；本函数**从不信任**入参语义，
//     只用 VirtualQuery 确认指针可读后做日志探测，绝不据此改变返回值。
//   参考实现里等价的"资源描述 → 模式"函数是 XGComputeOptimalSwizzleMode（8 个参数、
//   返回模式枚举），本函数的两参数形态只是"能安全接住调用"的最小外壳。
//
// 返回值：0 = 线性 / 未指定 swizzle。
//   依据：参考实现 layout.rs 的 swizzle→layout 映射里 0 对应"线性(ROW_MAJOR)"，
//   且参考实现 D3D12Desc::to_tex2d 在 layout==ROW_MAJOR 时就把 swizzle_mode 置 0。
//   于是游戏拿到 0 会走"我自己按行摆"的路径，PC 侧 D3D12 用 ROW_MAJOR 上传即可对上。
//   返回 0 的另一个好处：若调用方把返回值当 HRESULT 用，0 也是 S_OK，不会误判成失败。
//
// 返回值宽度：用 uint32_t（枚举宽度）。x64 下返回 0 会零扩展到 RAX，
// 所以即便调用方按 64 位读（当指针/句柄用）也是确定的 0，不会读到残值。
extern "C" __declspec(dllexport) std::uint32_t __stdcall XGSuggestSwizzleMode(const void* pDesc,
                                                                              std::uint32_t a2) {
    static CallCounter counter;

    if (detail_call(counter)) {
        // 只做"能安全读就顺便打出来"的探测：pDesc 可能是 XG_RESOURCE_DESC，也可能是
        // D3D12_RESOURCE_DESC（两者字段顺序在头部不一定相同），所以这里**不解析语义**，
        // 只在它看起来像 D3D12_RESOURCE_DESC 时把字段打出来供人工对账。
        if (probe_committed(pDesc, sizeof(D3D12_RESOURCE_DESC), false)) {
            const auto* d = static_cast<const D3D12_RESOURCE_DESC*>(pDesc);
            // 合理性门槛：dimension 1..3、width 非 0 且不超过 16K、mips<=16、depth/array<=2048。
            // 不满足就只打原始字节，避免把随机内存当 desc 解释出误导性日志。
            const bool plausible = d->Dimension >= 1 && d->Dimension <= 3 && d->Width != 0 && d->Width <= 0x4000 &&
                                   d->MipLevels <= 16 && d->DepthOrArraySize <= 2048;
            if (plausible) {
                log_desc("XGSuggestSwizzleMode", *d);
            } else {
                const auto* raw = static_cast<const std::uint32_t*>(pDesc);
                tg("XGSuggestSwizzleMode arg1=%p (不像 D3D12_RESOURCE_DESC，只打头 8 个 dword: "
                   "%08x %08x %08x %08x %08x %08x %08x %08x) a2=0x%x -> 0(LINEAR/未指定)",
                   pDesc, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], a2);
            }
        } else {
            tg("XGSuggestSwizzleMode arg1=%p (不可读，未探测) a2=0x%x -> 0(LINEAR/未指定)", pDesc, a2);
        }
    } else if (summary_call(counter)) {
        tg("XGSuggestSwizzleMode 已调用 %u 次，全部返回 0(LINEAR/未指定)", counter.n);
    }

    // 恒定返回"线性/未指定"：PC 上 D3D12 自己决定平铺，主机 swizzle 模式没有对应物。
    return 0;
}

// ============================ 纹理计算器：探针对象 ============================
// 为什么必须有它：游戏拿到 E_NOTIMPL（*out=null）之后**立刻**在 game+0x16F71EA
// 解引用返回值（`mov (%rcx),%r10` / `call *0x40(%r10)` = 槽 8 = copy_into），
// 于是 0xC0000005。也就是说"诚实失败"在这条调用点上是**不可用**的 —— 调用方不检查 out。
//
// 这里给的是**探针对象**，不是纹理计算器：12 槽 vtable（槽序来自参考实现 computer.rs），
// 每个槽记一行"第几次被调 + 前两个参数"并返回 0（槽 0/1 = AddRef/Release 返回 1）。
// 它不假装算得出主机布局 —— 返回 0 意味着布局全零，游戏若拿它去搬 texel 会得到错数据，
// 所以每次调用都大声记日志，且 XBGTASIX_TC_PROBE=0 能退回原来的 E_NOTIMPL。
// 目的只有一条：把"调用序列 + 槽序 + 参数形状"采下来，再照着实现真语义。
namespace {

constexpr int kTcSlots = 12;          // add_ref, release, get_resource_layout, get_size,
                                      // get_align, get_mip_offset, get_element_offset,
                                      // get_texel_coordinate, copy_into, copy_from,
                                      // tiled, view_descriptor
constexpr int kTcAddRefSlot = 0;
constexpr int kTcReleaseSlot = 1;

alignas(16) unsigned char g_tc_object[64];
void* g_tc_vtable[kTcSlots];
bool g_tc_ready = false;
std::uint32_t g_tc_slot_hits[kTcSlots];

std::intptr_t tc_slot_hit(int slot, const void* a, const void* b) {
    const std::uint32_t n = ++g_tc_slot_hits[slot];
    if (n <= 8) {
        tg("纹理计算器 槽%d 第 %u 次调用 a=%p b=%p（探针对象，返回 %s）", slot, n, a, b,
           (slot == kTcAddRefSlot || slot == kTcReleaseSlot) ? "1" : "0");
    } else if ((n % 4096) == 0) {
        tg("纹理计算器 槽%d 已调用 %u 次", slot, n);
    }
    return (slot == kTcAddRefSlot || slot == kTcReleaseSlot) ? 1 : 0;
}

// 槽 8 = CopyIntoSubresource：形状由逆向证据钉死（scarlett_render_reverse
// docs/EVIDENCE_INDEX.md 0x1416F7100 / pseudocode 03_resource_memory.cpp）：
//     CopyIntoSubresource(this, tiledBase, plane, subresource, linear, rowPitch, slicePitch)
// 主机上它做 swizzle（线性 -> 硬件 tiled 布局）。**我们对外报的是线性**
// （XGSuggestSwizzleMode 恒返回 0，见导出 1 的注释），所以这里按线性语义搬：
// 目标与源都是行主序紧凑排布，一次 memcpy slicePitch 字节即可 —— 这不是"假装成功"，
// 是与我们自己对外声明的布局保持一致。若哪天 swizzle 模式不再是线性，这里必须重写。
constexpr int kTcCopyIntoSlot = 8;

std::intptr_t copy_into_subresource(void* dest_tiled, std::uint32_t plane,
                                    std::uint32_t subresource, const void* src_linear,
                                    std::uint32_t row_pitch, std::uint32_t slice_pitch) {
    static std::uint32_t calls = 0;
    const std::uint32_t n = ++calls;

    const bool dest_ok = probe_committed(dest_tiled, slice_pitch, true);
    const bool src_ok = probe_committed(src_linear, slice_pitch, false);

    if (n <= 16 || (n % 256) == 0) {
        tg("CopyIntoSubresource #%u dst=%p src=%p plane=%u sub=%u rowPitch=%u slicePitch=%u "
           "(dst可写=%d src可读=%d)",
           n, dest_tiled, src_linear, plane, subresource, row_pitch, slice_pitch,
           dest_ok ? 1 : 0, src_ok ? 1 : 0);
    }

    if (!dest_ok || !src_ok) {
        tg("CopyIntoSubresource #%u 跳过：%s（线性布局的 memcpy 需要两端都可访问）", n,
           dest_ok ? "源不可读" : "目标不可写");
        return 0;
    }

    std::memcpy(dest_tiled, src_linear, slice_pitch);
    return 0;
}

#define XBG_TC_SLOT(n)                                                                    \
    std::intptr_t WINAPI tc_slot_##n(const void* a, const void* b, const void* c,          \
                                     const void* d) {                                     \
        (void)c;                                                                          \
        (void)d;                                                                          \
        return tc_slot_hit(n, a, b);                                                      \
    }
XBG_TC_SLOT(0) XBG_TC_SLOT(1) XBG_TC_SLOT(2) XBG_TC_SLOT(3)
XBG_TC_SLOT(4) XBG_TC_SLOT(5) XBG_TC_SLOT(6) XBG_TC_SLOT(7)
XBG_TC_SLOT(9) XBG_TC_SLOT(10) XBG_TC_SLOT(11)
#undef XBG_TC_SLOT

// 槽 8 是 7 个参数（含 this），单独一份，不走上面那个 4 参的探针壳。
std::intptr_t WINAPI tc_slot_8(void* self, void* dest_tiled, std::uint32_t plane,
                               std::uint32_t subresource, const void* src_linear,
                               std::uint32_t row_pitch, std::uint32_t slice_pitch) {
    (void)self;
    return copy_into_subresource(dest_tiled, plane, subresource, src_linear, row_pitch, slice_pitch);
}

void* const g_tc_slot_fns[kTcSlots] = {
    reinterpret_cast<void*>(&tc_slot_0),  reinterpret_cast<void*>(&tc_slot_1),
    reinterpret_cast<void*>(&tc_slot_2),  reinterpret_cast<void*>(&tc_slot_3),
    reinterpret_cast<void*>(&tc_slot_4),  reinterpret_cast<void*>(&tc_slot_5),
    reinterpret_cast<void*>(&tc_slot_6),  reinterpret_cast<void*>(&tc_slot_7),
    reinterpret_cast<void*>(&tc_slot_8),  reinterpret_cast<void*>(&tc_slot_9),
    reinterpret_cast<void*>(&tc_slot_10), reinterpret_cast<void*>(&tc_slot_11),
};

bool tc_probe_enabled() {
    static const bool on = []() {
        char buf[8]{};
        const DWORD n = ::GetEnvironmentVariableA("XBGTASIX_TC_PROBE", buf, sizeof(buf));
        return !(n > 0 && buf[0] == '0');
    }();
    return on;
}

void* tc_object() {
    if (!g_tc_ready) {
        for (int i = 0; i < kTcSlots; ++i) g_tc_vtable[i] = g_tc_slot_fns[i];
        *reinterpret_cast<void***>(g_tc_object) = g_tc_vtable;
        g_tc_ready = true;
    }
    return g_tc_object;
}

}  // namespace

// ============================================================================================
// 导出 2：XGCreateTextureComputer
// ============================================================================================
// 参考实现的签名（crates/compat/xg_xs/src/lib.rs:361）：
//     XGCreateTextureComputer(desc: *const D3D12_RESOURCE_DESC, out: *mut *mut c_void) -> HRESULT
// 成功时 *out = 一个 12 槽 vtable 的"纹理地址计算器"（computer.rs）：
//     0 AddRef 1? —— 准确顺序见 computer.rs：add_ref, release, get_resource_layout, get_size,
//     get_align, get_mip_offset, get_element_offset, get_texel_coordinate, copy_into, copy_from,
//     tiled, view_descriptor（该对象**没有** QueryInterface，第一个槽就是 AddRef）。
// 游戏拿它算主机布局（XG_RESOURCE_LAYOUT 结构体）并做 texel 搬运。
//
// 本移植为什么**诚实失败**：
//   1) get_resource_layout 要写回主机 XG_RESOURCE_LAYOUT 的二进制结构（参考实现里是
//      LAYOUT_BYTES 字节的定长 blob），该结构的字段布局没有公开文档，参考实现是靠
//      layout.rs/equations*.rs 那套 25KB 的 swizzle 方程反推的；照抄等于把主机私有布局
//      硬编码进 PC 构建，且没有可信的对账数据支撑；
//   2) PC 上真正等价的东西是 D3D12 的 GetResourceTiling，但它返回的是**PC 平铺**
//      （256B tile / 64KB standard swizzle），既不是主机 XG swizzle 的排布，
//      也填不出主机那个 layout blob —— 所以"用 GetResourceTiling 等价实现"这条路不成立；
//   3) 假成功（返回 S_OK 但给个填不对 layout 的对象）会让游戏拿着错偏移去写纹理，
//      表现为花屏/踩内存，比明确失败难查得多。
//   因此这里返回 E_NOTIMPL 并把 desc 全量打进日志：游戏侧会看到"没有纹理计算器"，
//   本项目的其它层（d3d12_xs / console_hw 的 GPU 路径）可以据此走线性上传分支。
//
// 注意 out 的写法：先确认 out 可写再写 nullptr（主机调用方可能传非法指针，
// 但那属于调用方 bug；这里只保证 nullptr 时不崩）。
extern "C" __declspec(dllexport) HRESULT __stdcall XGCreateTextureComputer(const void* desc, void** out) {
    static CallCounter counter;

    if (out == nullptr) {
        tg("XGCreateTextureComputer -> E_POINTER (out=null)");
        return static_cast<HRESULT>(kE_POINTER);
    }
    *out = nullptr;

    if (desc == nullptr) {
        tg("XGCreateTextureComputer -> E_INVALIDARG (desc=null)");
        return static_cast<HRESULT>(kE_INVALIDARG);
    }

    const bool readable = probe_committed(desc, sizeof(D3D12_RESOURCE_DESC), false);
    if (readable) {
        // desc 一定是 D3D12_RESOURCE_DESC（参考实现如此），所以无条件按它解析并打日志。
        const auto& d = *static_cast<const D3D12_RESOURCE_DESC*>(desc);
        if (detail_call(counter) || summary_call(counter)) {
            log_desc("XGCreateTextureComputer", d);
            tg("XGCreateTextureComputer -> E_NOTIMPL：PC 上不提供主机 swizzle 布局计算器"
               "（XG_RESOURCE_LAYOUT blob 布局未公开；D3D12 GetResourceTiling 给的是 PC 平铺，"
               "不是主机 swizzle），*out=null；本调用第 %u 次",
               counter.n);
        }
    } else {
        if (detail_call(counter) || summary_call(counter)) {
            tg("XGCreateTextureComputer desc=%p 不可读 -> E_NOTIMPL (第 %u 次)", desc, counter.n);
        }
    }

    // 默认给探针对象（E_NOTIMPL 会让调用方立刻解引用 null 崩掉）；XBGTASIX_TC_PROBE=0 退回失败。
    if (tc_probe_enabled()) {
        *out = tc_object();
        tg("XGCreateTextureComputer -> S_OK + 探针对象 %p（12 槽；layout 语义尚未实现）", *out);
        return static_cast<HRESULT>(kS_OK);
    }

    // 明确失败，不假成功。
    return static_cast<HRESULT>(kE_NOTIMPL);
}
