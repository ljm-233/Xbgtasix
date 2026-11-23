// SPDX-License-Identifier: GPL-3.0-or-later
// xcrdapi.cpp —— 主机版 DirectStorage 接口（XCRDAPI.dll）的 PC 移植
//
// ============================ 许可与移植来源 ============================
// 本文件：GPL-3.0-or-later（与本项目一致）。
// 语义移植来源：TomBox，GPL-3.0（与本项目同许可，因此按 GPL-3.0 第 5 条做语义移植）
//   https://github.com/omonkeyz/xbox-emulator
//   - crates/compat/xcrdapi/src/lib.rs      DStorageGetFactory 的入口语义
//   - crates/compat/xcrdapi/src/abi.rs      IID / HRESULT / DSTORAGE_REQUEST 位域 / 结构体大小
//   - crates/compat/xcrdapi/src/objects.rs  IDStorageFactoryX/FileX/QueueX/StatusArrayX 的
//                                           vtable 槽位顺序与每个槽位的真机语义
//   - crates/compat/xcrdapi/src/request.rs  DSTORAGE_REQUEST 主机布局、校验规则、压缩路径
//   - docs/LIBRARY_MAP.md                   "自己实现主机布局的 IDStorageFactoryX/FileX/
//                                           QueueX/StatusArrayX 对象 + CPU 文件/zlib 后端"
//
// ============================ 本模块导出（只有 1 个）============================
//   DStorageGetFactory(iid, out)
//
// ============================ 为什么必须自己造对象 ============================
// 游戏请求的 IID 是 GDK 的 {62959D11-D45F-4884-A0A3-F7862EA243AE}（IDStorageFactoryX）；
// PC 上所有 DirectStorage 版本（1.0.0 / 1.0.2 / 1.3.0 实测）的工厂 IID 都是
// {6924ea0c-c3cd-4826-b10a-f64f4ed927c1}。把主机的 IID 转给 PC 的 dstorage.dll 必然
// E_NOINTERFACE（实测 hr=0x80004002），所以**转发这条路彻底不通**，只能自己实现。
//
// 而且不只是 IID 不同：主机版的 DSTORAGE_REQUEST / DSTORAGE_QUEUE_DESC 布局也和 PC 不一样
// （见下面 XRequest / kQueueDescSize 的注释，都是从参考实现的字节偏移反推出来的），
// 所以"拿 PC 的 IDStorageQueue 套一层壳"也不成立——请求结构体本身就不兼容。
//
// ============================ vtable 布局（槽位 → 方法）============================
// 主机 IDStorageFactoryX（16 槽，前 9 个有实现，后 7 个是"未知槽位"陷阱）：
//   0 QueryInterface   1 AddRef   2 Release
//   3 CreateQueue(desc, riid, out)
//   4 OpenFile(LPCWSTR path, riid, out)
//   5 CreateStatusArray(capacity, LPCSTR name, riid, out)
//   6 SetCpuAffinity(UINT64 mask)
//   7 SetDebugFlags(UINT32 flags)          <-- 参考实现里是 void（无返回值）
//   8 SetStagingBufferSize(UINT32 size)    <-- 游戏实测传 0x2000000(32MB)，调用后继续跑
//   9..15 未知（陷阱：默认停止，见下）
// 顺序依据：控制器/PC 的 dstorage.h 也是 CreateQueue→OpenFile→CreateStatusArray 这个顺序
// （微软文档页按字母序列方法，会显示成 CreateQueue/CreateStatusArray/OpenFile，**不是**槽位序，
// 别被文档表格误导）；而 6/7/8 与游戏实测的 vtable[8]=SetStagingBufferSize(0x2000000) 完全吻合。
//
// ============================ 真实现 / 诚实失败 ============================
// 真实现：OpenFile（CreateFileW 打开真实文件、DuplicateHandle 给游戏）、CreateQueue、
//         CreateStatusArray、EnqueueRequest/EnqueueStatus/EnqueueSignal/Submit（CPU 后端：
//         ReadFile / memcpy / 自带 inflate 填 status 数组）、Query、RetrieveErrorRecord、
//         GetErrorEvent、CancelRequestsWithTag、Close、三个工厂设置项。
// 诚实失败：BCPack 压缩块（参考实现自带 3 万行 RANS+BC 解码表，本项目不引第三方库，
//         故明确返回 E_DSTORAGE_DECOMPRESSION_ERROR 并记录日志，绝不假成功）；
//         主机 swizzle 位（PC D3D12 无对应物，按参考实现的 OPT_KNOWN_MASK 规则拒绝）；
//         未知 vtable 槽位（走项目既有 XB_UNIMPL 策略：默认打印后停止，不猜语义）。
//
// 日志：行首 [xcrdapi]，每个槽位都记（槽号 + 关键参数）。热路径（Enqueue/Submit）
// 前若干次打详细行，之后按计数打摘要，避免把日志刷爆。

#include <windows.h>

// 仅用于 ID3D12Fence::Signal 的**编译期正确槽位**（只做虚调用，不链接 d3d12.lib）。
// 自己写死槽位号在 PC 的 ID3D12Fence 上很容易错（IUnknown 3 槽 + GetDevice/GetCompletedValue/
// SetEventOnCompletion/Signal），直接让头文件给出顺序更稳。
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "xbgtasix/trace.h"

namespace {

// ============================================================================================
// 常量：全部照抄 crates/compat/xcrdapi/src/abi.rs（那是参考实现从真机逆出来的值）
// ============================================================================================

constexpr int32_t kS_OK = 0;
constexpr int32_t kE_NOTIMPL = static_cast<int32_t>(0x80004001u);
constexpr int32_t kE_NOINTERFACE = static_cast<int32_t>(0x80004002u);
constexpr int32_t kE_POINTER = static_cast<int32_t>(0x80004003u);
constexpr int32_t kE_PENDING = static_cast<int32_t>(0x8000000au);
constexpr int32_t kE_OUTOFMEMORY = static_cast<int32_t>(0x8007000eu);
constexpr int32_t kE_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t kHR_NOACCESS = static_cast<int32_t>(0x800703e6u);
constexpr int32_t kHR_OPERATION_ABORTED = static_cast<int32_t>(0x800703e3u);

constexpr int32_t kE_DSTORAGE_INVALID_QUEUE_CAPACITY = static_cast<int32_t>(0x89240003u);
constexpr int32_t kE_DSTORAGE_END_OF_FILE = static_cast<int32_t>(0x89240007u);
constexpr int32_t kE_DSTORAGE_ACCESS_VIOLATION = static_cast<int32_t>(0x89240009u);
constexpr int32_t kE_DSTORAGE_RESERVED_FIELDS = static_cast<int32_t>(0x8924000cu);
constexpr int32_t kE_DSTORAGE_INVALID_DESTINATION_SIZE = static_cast<int32_t>(0x8924000fu);
constexpr int32_t kE_DSTORAGE_QUEUE_CLOSED = static_cast<int32_t>(0x89240010u);
constexpr int32_t kE_DSTORAGE_INVALID_QUEUE_PRIORITY = static_cast<int32_t>(0x89240013u);
constexpr int32_t kE_DSTORAGE_INDEX_BOUND = static_cast<int32_t>(0x89240015u);
constexpr int32_t kE_DSTORAGE_INVALID_FILE_HANDLE = static_cast<int32_t>(0x89240017u);
constexpr int32_t kE_DSTORAGE_INVALID_SOURCE_TYPE = static_cast<int32_t>(0x8924001bu);
constexpr int32_t kE_DSTORAGE_INVALID_INTERMEDIATE_SIZE = static_cast<int32_t>(0x8924001cu);
constexpr int32_t kE_DSTORAGE_STAGING_BUFFER_LOCKED = static_cast<int32_t>(0x8924001fu);
constexpr int32_t kE_DSTORAGE_INVALID_FENCE = static_cast<int32_t>(0x89240022u);
constexpr int32_t kE_DSTORAGE_INVALID_STATUS_ARRAY = static_cast<int32_t>(0x89240023u);
constexpr int32_t kE_DSTORAGE_INVALID_MEMORY_QUEUE_PRIORITY = static_cast<int32_t>(0x89240024u);
constexpr int32_t kE_DSTORAGE_DECOMPRESSION_ERROR = static_cast<int32_t>(0x89240030u);
constexpr int32_t kE_DSTORAGE_ZLIB_BAD_HEADER = static_cast<int32_t>(0x89240031u);
constexpr int32_t kE_DSTORAGE_ZLIB_BAD_DATA = static_cast<int32_t>(0x89240032u);
constexpr int32_t kE_DSTORAGE_ZLIB_PARITY_FAIL = static_cast<int32_t>(0x89240033u);
constexpr int32_t kE_DSTORAGE_PASSTHROUGH_ERROR = static_cast<int32_t>(0x89240037u);
constexpr int32_t kE_DSTORAGE_INVALID_QUEUE_TYPE = static_cast<int32_t>(0x892400f5u);

// DSTORAGE_REQUEST 的 options 位域（主机版在 PC 的 8/9 位之外还带 zlib/BCPack/swizzle）
constexpr uint64_t kOptZlib = 1ull << 0;              // 参考实现 OPT_ZLIB
constexpr uint32_t kBcpackShift = 1;                  // 位 1..4
constexpr uint64_t kBcpackMask = 0xfull;
constexpr uint32_t kSwizzleShift = 5;                 // 位 5..20（主机专用）
constexpr uint64_t kSwizzleMask = 0xffffull;
constexpr uint64_t kOptSourceMemory = 1ull << 22;     // 源是内存而非文件
constexpr uint64_t kOptExt10Valid = 1ull << 21;
constexpr uint64_t kOptExt28Valid = 1ull << 23;
// 参考实现的 OPT_KNOWN_MASK：只有 zlib / BCPack / memory-source 三位被认作"认识"。
// 其余位（含主机 swizzle）落到 E_DSTORAGE_RESERVED_FIELDS —— 我们照抄这个判据。
constexpr uint64_t kOptKnownMask = kOptZlib | (kBcpackMask << kBcpackShift) | kOptSourceMemory;

constexpr uint32_t kQueueSourceFile = 0;
constexpr uint32_t kQueueSourceMemory = 1;
constexpr int8_t kPriorityLow = -1;
constexpr int8_t kPriorityRealtime = 2;

// 主机结构体大小（真机实测值，见 abi.rs）
constexpr size_t kRequestSize = 0x48;      // DSTORAGE_REQUEST（主机布局，比 PC 的 0x40 大 8）
constexpr size_t kQueueDescSize = 0x18;    // DSTORAGE_QUEUE_DESC（主机布局）
constexpr size_t kQueueInfoSize = 0x1c;    // DSTORAGE_QUEUE_INFO
constexpr size_t kErrorRecordSize = 0x290; // DSTORAGE_ERROR_RECORD
constexpr size_t kErrFilenameOff = 0x24;   // 记录里 UTF-16 文件路径的偏移
constexpr size_t kErrFilenameChars = 0x104;
constexpr size_t kErrNameOff = 0x250;      // 记录里 ASCII 请求名的偏移
constexpr size_t kErrNameChars = 0x40;
constexpr size_t kStatusNameChars = 0x20;

constexpr uint32_t kCommandTypeNone = 0xffffffffu;
constexpr uint32_t kCommandTypeRequest = 0;
constexpr uint32_t kCommandTypeStatus = 1;
constexpr uint32_t kCommandTypeSignal = 2;

// ============================ IID ============================
// IUnknown：{00000000-0000-0000-C000-000000000046}
const GUID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
// GTA6 实测向 DStorageGetFactory 请求的 IID（GDK 的 IDStorageFactoryX）
const GUID kIidFactoryX = {0x62959D11, 0xD45F, 0x4884, {0xA0, 0xA3, 0xF7, 0x86, 0x2E, 0xA2, 0x43, 0xAE}};
// 参考实现用的工厂 IID（另一个 GDK 版本）。保留为兼容别名，命中时日志会标 ALT。
const GUID kIidFactoryXRef = {0xFB3B54AC, 0xBF82, 0x463A, {0xB7, 0x77, 0x73, 0x19, 0x58, 0x37, 0x86, 0x4D}};
// 子对象 IID（来自 abi.rs）
const GUID kIidFileX = {0x43704F9F, 0x27C6, 0x4A9F, {0x88, 0x15, 0xD1, 0x65, 0x27, 0x89, 0x4D, 0xF4}};
const GUID kIidQueueX = {0xA3F6649A, 0x8C2C, 0x4B3A, {0x8B, 0x6E, 0x10, 0x18, 0x6A, 0x04, 0xB9, 0xBC}};
const GUID kIidStatusArrayX = {0xCD66D752, 0x2786, 0x44F7, {0x90, 0x70, 0xE2, 0xA6, 0xFD, 0xAD, 0x3F, 0x1B}};
// PC DirectStorage 的工厂 IID：明确拒绝，并在日志里说清原因（这条是排障关键信息）
const GUID kIidFactoryPc = {0x6924EA0C, 0xC3CD, 0x4826, {0xB1, 0x0A, 0xF6, 0x4F, 0x4E, 0xD9, 0x27, 0xC1}};
// PC 的 ID3D12Fence IID：{0a753dcf-c4d8-4b91-adf6-be5a60d95a76}（abi.rs 的 IID_D3D12_FENCE_PC，
// 与 d3d12.h 的 IID_ID3D12Fence 同一个值）。
// 这里**手写字节**而不是用 d3d12.h 的 IID_ID3D12Fence 常量：那是 d3d12.lib 里的外部符号，
// 而 build.sh 给这几个 shim 的链接行只有 "-shared <src> src/trace.cpp"、没有 -ld3d12，
// 用了就会 undefined reference to `IID_ID3D12Fence`（实测踩到）。
const GUID kIidD3D12FencePc = {0x0A753DCF, 0xC4D8, 0x4B91, {0xAD, 0xF6, 0xBE, 0x5A, 0x60, 0xD9, 0x5A, 0x76}};

// ============================================================================================
// 日志 / IID 工具 / 安全内存探测
// ============================================================================================

void xl(const char* fmt, ...) {
    char body[1200];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    char line[1280];
    std::snprintf(line, sizeof(line), "[xcrdapi] %s", body);
    ::xbgtasix::logf("%s", line);
}

bool iid_eq(const GUID* a, const GUID& b) {
    if (a == nullptr) return false;
    // 直接按 16 字节比较：GUID 的内存布局就是 IID 的线上布局（前三个字段小端）
    return std::memcmp(a, &b, sizeof(GUID)) == 0;
}

void iid_text(const GUID* g, char* out, size_t cap) {
    if (g == nullptr) {
        std::snprintf(out, cap, "<null>");
        return;
    }
    std::snprintf(out, cap, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  static_cast<unsigned long>(g->Data1), static_cast<unsigned>(g->Data2),
                  static_cast<unsigned>(g->Data3), g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
                  g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

bool range_accessible(const void* p, size_t len, bool want_writable) {
    if (p == nullptr) return false;
    if (len == 0) return true;
    auto addr = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = addr + len;
    if (end < addr) return false;
    while (addr < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
        const DWORD prot = mbi.Protect & 0xffu;
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

// 安全读一段 ASCII C 字符串（最多 max_chars 个字符）；不可读时返回 "<unreadable>"
std::string safe_cstr(const char* p, size_t max_chars) {
    if (p == nullptr) return std::string();
    if (!range_accessible(p, 1, false)) return std::string("<unreadable>");
    std::string out;
    out.reserve(32);
    for (size_t i = 0; i < max_chars; ++i) {
        // 每 64 字节确认一次可读性，避免贴页尾的字符串把进程读死
        if ((i % 64) == 0 && !range_accessible(p + i, 1, false)) break;
        if (p[i] == '\0') break;
        out.push_back(p[i]);
    }
    return out;
}

// 安全读一段 UTF-16 字符串（主机路径都是宽字符）
std::wstring safe_wstr(const wchar_t* p, size_t max_chars) {
    std::wstring out;
    if (p == nullptr) return out;
    if (!range_accessible(p, sizeof(wchar_t), false)) return out;
    for (size_t i = 0; i < max_chars; ++i) {
        if ((i % 32) == 0 && !range_accessible(p + i, sizeof(wchar_t), false)) break;
        if (p[i] == L'\0') break;
        out.push_back(p[i]);
    }
    return out;
}

int32_t hresult_from_win32(uint32_t code) {
    if (code == 0) return kS_OK;
    if (static_cast<int32_t>(code) < 0) return static_cast<int32_t>(code);
    return static_cast<int32_t>(0x80070000u | (code & 0xffffu));
}

// 热路径日志节流：每个对象前 N 次详细打，之后每 M 次一条摘要
constexpr uint32_t kDetailCalls = 16;
constexpr uint32_t kSummaryEvery = 512;

inline bool detail_or_summary(std::atomic<uint32_t>& n) {
    const uint32_t c = n.fetch_add(1, std::memory_order_relaxed) + 1;
    return c <= kDetailCalls || (c % kSummaryEvery) == 0;
}

// ============================================================================================
// DSTORAGE_REQUEST（主机布局 0x48）—— 字段偏移逐字节对齐 abi.rs / request.rs
// ============================================================================================
//  0x00 u64 options        位域：bit0=zlib, bit1..4=BCPack 模式, bit5..20=swizzle,
//                                bit21/23=ext10/ext28 有效, bit22=源是内存
//  0x08 u64 destination
//  0x10 u16 ext10          （ext10 有效时才有意义）
//  0x18 u32 destination_size
//  0x20 u64 source         文件请求=P 文件对象指针；内存请求=P 源缓冲
//  0x28 u64 source_offset  文件偏移；内存请求时这个位置是 u16 ext28
//  0x30 u32 source_size
//  0x34 u32 intermediate_size（zlib+BCPack 时的中间缓冲大小，参考实现叫 field34）
//  0x38 u64 cancellation_tag
//  0x40 u64 name           指向 ASCII 名字的**指针**（PC 版这里是内联 char 数组）
//  0x48 结束
// 注意 PC 的 DSTORAGE_REQUEST 是 0x40 且字段顺序不同（DestinationSize 在 0x10、
// Source 在 0x18、SourceSize 在 0x20、SourceOffset 在 0x28、Name 内联在 0x38），
// 所以主机请求**不能**直接喂给 PC 的 IDStorageQueue —— 这也是必须自己实现队列的原因。
struct XRequest {
    uint64_t options = 0;
    uint64_t destination = 0;
    uint16_t ext10 = 0;
    uint32_t destination_size = 0;
    uint64_t source = 0;
    uint64_t source_offset = 0;
    uint16_t ext28 = 0;
    uint32_t source_size = 0;
    uint32_t intermediate_size = 0;
    uint64_t cancellation_tag = 0;
    uint64_t name = 0;

    bool memory_source() const { return (options & kOptSourceMemory) != 0; }
    bool zlib() const { return (options & kOptZlib) != 0; }
    uint8_t bcpack_mode() const {
        return static_cast<uint8_t>((options >> kBcpackShift) & kBcpackMask);
    }
    uint16_t swizzle_mode() const {
        return static_cast<uint16_t>((options >> kSwizzleShift) & kSwizzleMask);
    }
};

inline uint64_t rd_u64(const uint8_t* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
inline uint32_t rd_u32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
inline uint16_t rd_u16(const uint8_t* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

XRequest decode_request(const uint8_t* b) {
    XRequest r;
    r.options = rd_u64(b + 0x00);
    r.destination = rd_u64(b + 0x08);
    r.ext10 = ((r.options & kOptExt10Valid) != 0) ? rd_u16(b + 0x10) : 0;
    r.destination_size = rd_u32(b + 0x18);
    r.source = rd_u64(b + 0x20);
    const bool mem = r.memory_source();
    r.source_offset = mem ? 0 : rd_u64(b + 0x28);
    r.ext28 = (mem && (r.options & kOptExt28Valid) != 0) ? rd_u16(b + 0x28) : 0;
    r.source_size = rd_u32(b + 0x30);
    r.intermediate_size = rd_u32(b + 0x34);
    r.cancellation_tag = rd_u64(b + 0x38);
    r.name = rd_u64(b + 0x40);
    return r;
}

// ============================================================================================
// inflate：RFC1950(zlib 头/adler32) + RFC1951(deflate)
// ============================================================================================
// 为什么自带实现：主机的 zlib 压缩块是**必需**路径（参考实现用 miniz_oxide 的 inflate），
// 而本项目不允许引第三方库；Windows 自带的 cabinet.dll/ntdll!RtlDecompressBuffer 只认
// MSZIP/LZNT1/XPRESS，没有 zlib（deflate + adler32）解码器。
// 本实现是照 RFC 1950/1951 写的独立实现（不是拷贝任何库），已在开发机实测：
//   30/30 压缩变体（level 1/6/9 + Z_FIXED + Z_HUFFMAN_ONLY + stored）、
//   24/24 "目标缓冲恰好只有输出大小" 用例、以及坏头/坏数据/adler 失配 3 条错误路径全部符合预期。
// 输出缓冲按"非环绕"处理：回引用直接从输出起点回看（dst 必须是完整输出区），
// 因此不需要 32KB 滑窗，浅引用越界会返回 kBadData。
namespace inflate_impl {

enum Result : int { kOk = 0, kBadHeader = 1, kBadData = 2, kParityFail = 3, kNeedMoreOutput = 4 };

struct Bits {
    const uint8_t* p = nullptr;
    size_t nbits = 0;
    size_t pos = 0;

    void init(const uint8_t* data, size_t len) {
        p = data;
        nbits = len * 8;
        pos = 0;
    }
    // 读 n 位（LSB 在前）；越界返回 false
    bool get(uint32_t n, uint32_t* out) {
        if (pos + n > nbits) return false;
        uint32_t v = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const size_t bit = pos + i;
            v |= static_cast<uint32_t>((p[bit >> 3] >> (bit & 7)) & 1u) << i;
        }
        pos += n;
        *out = v;
        return true;
    }
    void align() { pos = (pos + 7) & ~static_cast<size_t>(7); }
};

struct Huffman {
    uint16_t count[16];
    uint16_t symbol[288];
};

int huff_build(Huffman* h, const uint8_t* lengths, int n) {
    for (int i = 0; i < 16; ++i) h->count[i] = 0;
    for (int i = 0; i < n; ++i) h->count[lengths[i]]++;
    if (h->count[0] == n) return 0;  // 没有码字
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;  // 过订阅
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 2; len < 16; ++len) offs[len] = static_cast<uint16_t>(offs[len - 1] + h->count[len - 1]);
    for (int i = 0; i < n; ++i)
        if (lengths[i] != 0) h->symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    return left > 0 ? 1 : 0;  // 1 = 不完整（允许）
}

int huff_decode(Bits* b, const Huffman* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        uint32_t bit = 0;
        if (!b->get(1, &bit)) return -1;
        code |= static_cast<int>(bit);
        const int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                               3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,   2,   3,   4,   5,    7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

int inflate_block_body(Bits* b, const Huffman* lit, const Huffman* dist, uint8_t* dst, size_t cap,
                       size_t* out_pos) {
    for (;;) {
        const int sym = huff_decode(b, lit);
        if (sym < 0) return kBadData;
        if (sym < 256) {
            if (*out_pos >= cap) return kNeedMoreOutput;
            dst[(*out_pos)++] = static_cast<uint8_t>(sym);
            continue;
        }
        if (sym == 256) return kOk;  // 块结束
        const int li = sym - 257;
        if (li >= 29) return kBadData;
        uint32_t extra = 0;
        if (kLenExtra[li] && !b->get(kLenExtra[li], &extra)) return kBadData;
        const size_t len = static_cast<size_t>(kLenBase[li]) + extra;
        const int ds = huff_decode(b, dist);
        if (ds < 0 || ds >= 30) return kBadData;
        uint32_t dextra = 0;
        if (kDistExtra[ds] && !b->get(kDistExtra[ds], &dextra)) return kBadData;
        const size_t d = static_cast<size_t>(kDistBase[ds]) + dextra;
        if (d > *out_pos) return kBadData;  // 回引用越界
        if (*out_pos + len > cap) return kNeedMoreOutput;
        for (size_t i = 0; i < len; ++i) dst[*out_pos + i] = dst[*out_pos + i - d];
        *out_pos += len;
    }
}

int inflate_dynamic(Bits* b, uint8_t* dst, size_t cap, size_t* out_pos) {
    uint32_t hlit = 0, hdist = 0, hclen = 0;
    if (!b->get(5, &hlit) || !b->get(5, &hdist) || !b->get(4, &hclen)) return kBadData;
    const int nlit = static_cast<int>(hlit) + 257;
    const int ndist = static_cast<int>(hdist) + 1;
    const int nclen = static_cast<int>(hclen) + 4;
    if (nlit > 286 || ndist > 30) return kBadData;
    static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t clen[19] = {0};
    for (int i = 0; i < nclen; ++i) {
        uint32_t v = 0;
        if (!b->get(3, &v)) return kBadData;
        clen[kOrder[i]] = static_cast<uint8_t>(v);
    }
    Huffman ch{};
    if (huff_build(&ch, clen, 19) < 0) return kBadData;
    uint8_t lengths[286 + 30] = {0};
    const int total = nlit + ndist;
    for (int i = 0; i < total;) {
        const int sym = huff_decode(b, &ch);
        if (sym < 0) return kBadData;
        if (sym < 16) {
            lengths[i++] = static_cast<uint8_t>(sym);
            continue;
        }
        int rep = 0;
        uint8_t val = 0;
        if (sym == 16) {
            if (i == 0) return kBadData;
            uint32_t v = 0;
            if (!b->get(2, &v)) return kBadData;
            rep = 3 + static_cast<int>(v);
            val = lengths[i - 1];
        } else if (sym == 17) {
            uint32_t v = 0;
            if (!b->get(3, &v)) return kBadData;
            rep = 3 + static_cast<int>(v);
            val = 0;
        } else {
            uint32_t v = 0;
            if (!b->get(7, &v)) return kBadData;
            rep = 11 + static_cast<int>(v);
            val = 0;
        }
        if (i + rep > total) return kBadData;
        for (int k = 0; k < rep; ++k) lengths[i++] = val;
    }
    if (lengths[256] == 0) return kBadData;  // 必须有块结束码
    Huffman lit{}, dist{};
    if (huff_build(&lit, lengths, nlit) < 0) return kBadData;
    if (huff_build(&dist, lengths + nlit, ndist) < 0) return kBadData;
    return inflate_block_body(b, &lit, &dist, dst, cap, out_pos);
}

int inflate_fixed(Bits* b, uint8_t* dst, size_t cap, size_t* out_pos) {
    uint8_t lengths[288 + 30] = {0};
    for (int i = 0; i < 144; ++i) lengths[i] = 8;
    for (int i = 144; i < 256; ++i) lengths[i] = 9;
    for (int i = 256; i < 280; ++i) lengths[i] = 7;
    for (int i = 280; i < 288; ++i) lengths[i] = 8;
    for (int i = 0; i < 30; ++i) lengths[288 + i] = 5;
    Huffman lit{}, dist{};
    if (huff_build(&lit, lengths, 288) < 0) return kBadData;
    if (huff_build(&dist, lengths + 288, 30) < 0) return kBadData;
    return inflate_block_body(b, &lit, &dist, dst, cap, out_pos);
}

int inflate_stored(Bits* b, uint8_t* dst, size_t cap, size_t* out_pos) {
    b->align();
    uint32_t len = 0, nlen = 0;
    if (!b->get(16, &len) || !b->get(16, &nlen)) return kBadData;
    if ((len ^ 0xFFFFu) != nlen) return kBadData;
    if (*out_pos + len > cap) return kNeedMoreOutput;
    for (uint32_t i = 0; i < len; ++i) {
        uint32_t byte = 0;
        if (!b->get(8, &byte)) return kBadData;
        dst[(*out_pos)++] = static_cast<uint8_t>(byte);
    }
    return kOk;
}

uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

// 完整 zlib 流（RFC1950 头 + deflate + adler32）。成功时 *written 为输出字节数。
int zlib_inflate(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap, size_t* written) {
    if (src == nullptr || dst == nullptr) return kBadData;
    *written = 0;
    if (src_len < 2) return kBadHeader;
    const uint8_t cmf = src[0], flg = src[1];
    if ((cmf & 0x0f) != 8) return kBadHeader;  // CM 必须是 deflate
    if ((cmf >> 4) > 7) return kBadHeader;     // CINFO > 7（窗口 > 32K）
    if ((static_cast<uint32_t>(cmf) * 256u + flg) % 31u != 0) return kBadHeader;
    if ((flg & 0x20) != 0) return kBadHeader;  // 预设字典不支持
    Bits b;
    b.init(src + 2, src_len - 2);
    size_t out_pos = 0;
    uint32_t last = 0;
    do {
        if (!b.get(1, &last)) return kBadData;
        uint32_t type = 0;
        if (!b.get(2, &type)) return kBadData;
        int rc = kBadData;
        if (type == 0)
            rc = inflate_stored(&b, dst, dst_cap, &out_pos);
        else if (type == 1)
            rc = inflate_fixed(&b, dst, dst_cap, &out_pos);
        else if (type == 2)
            rc = inflate_dynamic(&b, dst, dst_cap, &out_pos);
        else
            return kBadData;
        if (rc != kOk) return rc;
    } while (last == 0);
    b.align();
    const size_t byte_end = b.pos >> 3;
    if (src_len - 2 < byte_end + 4) return kBadData;
    const uint8_t* tail = src + 2 + byte_end;
    const uint32_t want = (static_cast<uint32_t>(tail[0]) << 24) | (static_cast<uint32_t>(tail[1]) << 16) |
                          (static_cast<uint32_t>(tail[2]) << 8) | static_cast<uint32_t>(tail[3]);
    if (want != adler32(dst, out_pos)) return kParityFail;
    *written = out_pos;
    return kOk;
}

}  // namespace inflate_impl

// 把 inflate 的返回码翻成 DSTORAGE 的 HRESULT（对应 request.rs 的映射）
int32_t inflate_hr(int rc) {
    switch (rc) {
        case inflate_impl::kOk: return kS_OK;
        case inflate_impl::kBadHeader: return kE_DSTORAGE_ZLIB_BAD_HEADER;
        case inflate_impl::kBadData: return kE_DSTORAGE_ZLIB_BAD_DATA;
        case inflate_impl::kParityFail: return kE_DSTORAGE_ZLIB_PARITY_FAIL;
        default: return kE_DSTORAGE_DECOMPRESSION_ERROR;  // 含 kNeedMoreOutput
    }
}

// ============================================================================================
// 对象定义
// ============================================================================================
struct FileObj;
struct StatusObj;
struct QueueObj;

// ---------------------------- IDStorageFileX（12 槽）----------------------------
// 0 QI 1 AddRef 2 Release 3 Close 4 GetHandle 5? 6? 7 GetId 8..11?
// 依据 objects.rs 的 FileVtbl：close 在 3、get_handle 在 4、get_id 在 7，
// 5/6/8..11 是参考实现也没实现的槽位（它同样是陷阱）。
struct FileVtbl {
    HRESULT(__stdcall* QueryInterface)(FileObj*, const GUID*, void**);
    uint32_t(__stdcall* AddRef)(FileObj*);
    uint32_t(__stdcall* Release)(FileObj*);
    void(__stdcall* Close)(FileObj*);
    HANDLE(__stdcall* GetHandle)(FileObj*);
    int32_t(__stdcall* Slot5)(FileObj*);
    int32_t(__stdcall* Slot6)(FileObj*);
    uint32_t(__stdcall* GetId)(FileObj*);
    int32_t(__stdcall* Slot8)(FileObj*);
    int32_t(__stdcall* Slot9)(FileObj*);
    int32_t(__stdcall* Slot10)(FileObj*);
    int32_t(__stdcall* Slot11)(FileObj*);
};

struct FileObj {
    const FileVtbl* vt;
    std::atomic<uint32_t> refs{1};
    uint32_t id = 0;
    std::wstring path;
    HANDLE handle = INVALID_HANDLE_VALUE;       // 本模块内部读用（seek+read）
    HANDLE give_handle = INVALID_HANDLE_VALUE;  // 给游戏 GetHandle 用：**另开**一个句柄，
                                                // 否则 DuplicateHandle 与原句柄共享文件指针，
                                                // 游戏侧读会打乱我们的 seek 偏移
    std::mutex io;  // 主机上同一文件对象可被多个队列并发读；seek+read 必须串行化
};

// 关掉"给游戏用"的那份句柄（调用方必须持有 o->io）
void close_give_handle(FileObj* o) {
    if (o->give_handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(o->give_handle);
        o->give_handle = INVALID_HANDLE_VALUE;
    }
}

// ---------------------------- IDStorageStatusArrayX（10 槽）----------------------------
// 0 QI 1 AddRef 2 Release 3 IsComplete 4 GetHResult 5..9?
// 依据 objects.rs 的 StatusVtbl（与 PC 的 IDStorageStatusArray 一致）。
struct StatusVtbl {
    HRESULT(__stdcall* QueryInterface)(StatusObj*, const GUID*, void**);
    uint32_t(__stdcall* AddRef)(StatusObj*);
    uint32_t(__stdcall* Release)(StatusObj*);
    bool(__stdcall* IsComplete)(StatusObj*, uint32_t);
    int32_t(__stdcall* GetHResult)(StatusObj*, uint32_t);
    int32_t(__stdcall* Slot5)(StatusObj*);
    int32_t(__stdcall* Slot6)(StatusObj*);
    int32_t(__stdcall* Slot7)(StatusObj*);
    int32_t(__stdcall* Slot8)(StatusObj*);
    int32_t(__stdcall* Slot9)(StatusObj*);
};

struct StatusObj {
    const StatusVtbl* vt;
    std::atomic<uint32_t> refs{1};
    std::string name;
    std::vector<std::atomic<int32_t>> slots;
};

// ---------------------------- IDStorageQueueX（18 槽）----------------------------
// 0 QI 1 AddRef 2 Release 3 EnqueueRequest 4 EnqueueStatus 5 EnqueueSignal 6 Submit
// 7 CancelRequestsWithTag 8 Close 9 GetErrorEvent 10 RetrieveErrorRecord 11 Query 12..17?
// 依据 objects.rs 的 QueueVtbl：前 12 个与 PC 的 IDStorageQueue 槽位一致，
// 12..17 是主机多出来的部分（参考实现同样只放陷阱）。
struct QueueVtbl {
    HRESULT(__stdcall* QueryInterface)(QueueObj*, const GUID*, void**);
    uint32_t(__stdcall* AddRef)(QueueObj*);
    uint32_t(__stdcall* Release)(QueueObj*);
    void(__stdcall* EnqueueRequest)(QueueObj*, const void*);
    void(__stdcall* EnqueueStatus)(QueueObj*, void*, uint32_t);
    void(__stdcall* EnqueueSignal)(QueueObj*, void*, uint64_t);
    void(__stdcall* Submit)(QueueObj*);
    void(__stdcall* CancelRequestsWithTag)(QueueObj*, uint64_t, uint64_t);
    void(__stdcall* Close)(QueueObj*);
    HANDLE(__stdcall* GetErrorEvent)(QueueObj*);
    void(__stdcall* RetrieveErrorRecord)(QueueObj*, void*);
    void(__stdcall* Query)(QueueObj*, void*);
    int32_t(__stdcall* Slot12)(QueueObj*);
    int32_t(__stdcall* Slot13)(QueueObj*);
    int32_t(__stdcall* Slot14)(QueueObj*);
    int32_t(__stdcall* Slot15)(QueueObj*);
    int32_t(__stdcall* Slot16)(QueueObj*);
    int32_t(__stdcall* Slot17)(QueueObj*);
};

// 待处理条目：请求 / 状态写回 / fence 信号，按入队顺序在 Submit 里顺序执行
enum class EntryKind { Request, Status, Signal };

struct Entry {
    EntryKind kind = EntryKind::Request;
    XRequest req;
    FileObj* file = nullptr;    // 文件源；已持有一份引用
    StatusObj* status = nullptr;  // 状态数组；已持有一份引用
    uint32_t index = 0;
    ID3D12Fence* fence = nullptr;  // 已 QI 成功并持有引用的 PC fence
    uint64_t signal_value = 0;
    uint64_t seq = 0;
    bool aborted = false;
    int32_t pre_error = kS_OK;  // 入队时静态校验的失败码
};

struct ErrorState {
    uint32_t count = 0;
    int32_t hr = kS_OK;
    uint32_t command = kCommandTypeNone;
    XRequest req;
    std::wstring filename;
    std::string name;
    uint64_t object = 0;
    uint64_t value = 0;
    bool has_request = false;
};

struct QueueObj {
    const QueueVtbl* vt;
    std::atomic<uint32_t> refs{1};
    uint8_t raw_desc[kQueueDescSize] = {0};
    uint32_t source_type = kQueueSourceFile;
    uint16_t capacity = 0;
    int8_t priority = 0;
    std::string name;
    bool closed = false;
    std::mutex mu;
    std::mutex err_mu;  // 单独护 error_count/first_error：queue_fail 会被 Submit 与入队路径同时调用
    std::vector<Entry> pending;
    uint32_t error_count = 0;
    ErrorState first_error;
    uint64_t next_seq = 1;
    HANDLE error_event = nullptr;
    uint64_t completed = 0;
    std::atomic<uint32_t> enq_log{0};
    std::atomic<uint32_t> submit_log{0};
};

constexpr uint64_t kLiveNone = 0;
uint64_t g_live_queues = 0;  // 受 g_live_mu 保护：SetStagingBufferSize 要看这两个计数
uint64_t g_live_files = 0;
std::mutex g_live_mu;
std::atomic<uint32_t> g_next_file_id{1};

void live_add_queue() {
    std::lock_guard<std::mutex> g(g_live_mu);
    ++g_live_queues;
}
void live_del_queue() {
    std::lock_guard<std::mutex> g(g_live_mu);
    if (g_live_queues > 0) --g_live_queues;
}
void live_add_file() {
    std::lock_guard<std::mutex> g(g_live_mu);
    ++g_live_files;
}
void live_del_file() {
    std::lock_guard<std::mutex> g(g_live_mu);
    if (g_live_files > 0) --g_live_files;
}
std::pair<uint64_t, uint64_t> live_counts() {
    std::lock_guard<std::mutex> g(g_live_mu);
    return {g_live_queues, g_live_files};
}

// 全局设置项（对应工厂的 3 个 set 方法）
std::atomic<uint32_t> g_debug_flags{0};
std::atomic<uint64_t> g_cpu_affinity{0};
std::atomic<uint32_t> g_staging_buffer_size{0};

// ============================================================================================
// 文件 I/O
// ============================================================================================

// 把 Win32 路径候选逐个试一遍。主机路径通常是 "\\.\d:" 之外的 XVD 挂载路径或盘符路径，
// 在 PC 上直接 CreateFileW 往往不命中，所以按"原样 → game_root\原样(去前导分隔符) →
// game_root\文件名"三级回退，每一级都记日志（只说成功的那级用的是什么路径，
// 失败不写死结论，避免日志里出现误导性"文件不存在"）。
HANDLE open_path_candidates(const std::wstring& wanted, std::wstring* used) {
    std::vector<std::wstring> cands;
    cands.push_back(wanted);
    const char* root_ansi = ::xbgtasix::game_root();
    if (root_ansi != nullptr && root_ansi[0] != '\0') {
        wchar_t root_w[1024] = {0};
        if (::MultiByteToWideChar(CP_UTF8, 0, root_ansi, -1, root_w, 1024) > 0) {
            std::wstring root(root_w);
            std::wstring rel = wanted;
            while (!rel.empty() && (rel.front() == L'\\' || rel.front() == L'/')) rel.erase(rel.begin());
            if (!rel.empty()) {
                std::wstring a = root;
                if (a.back() != L'\\') a.push_back(L'\\');
                a += rel;
                cands.push_back(a);
            }
            const size_t slash = wanted.find_last_of(L"\\/");
            if (slash != std::wstring::npos && slash + 1 < wanted.size()) {
                std::wstring b = root;
                if (b.back() != L'\\') b.push_back(L'\\');
                b += wanted.substr(slash + 1);
                cands.push_back(b);
            }
        }
    }
    for (const auto& c : cands) {
        HANDLE h = ::CreateFileW(c.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            if (used != nullptr) *used = c;
            return h;
        }
    }
    if (used != nullptr) *used = wanted;
    return INVALID_HANDLE_VALUE;
}

// 从文件读一段（等价参考实现的 seek_read 循环）。调用方必须持有 FileObj 存活引用。
int32_t read_at(FileObj* f, uint64_t offset, void* dst, size_t len) {
    if (f == nullptr) return kE_DSTORAGE_INVALID_FILE_HANDLE;
    std::lock_guard<std::mutex> g(f->io);
    if (f->handle == INVALID_HANDLE_VALUE) return kE_DSTORAGE_INVALID_FILE_HANDLE;
    LARGE_INTEGER off{};
    off.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(f->handle, off, nullptr, FILE_BEGIN)) {
        return hresult_from_win32(::GetLastError());
    }
    size_t done = 0;
    auto* p = static_cast<uint8_t*>(dst);
    while (done < len) {
        DWORD want = static_cast<DWORD>(std::min<size_t>(len - done, 0x10000000u));
        DWORD got = 0;
        if (!::ReadFile(f->handle, p + done, want, &got, nullptr)) {
            const DWORD e = ::GetLastError();
            if (e == ERROR_HANDLE_EOF) return kE_DSTORAGE_END_OF_FILE;
            return hresult_from_win32(e);
        }
        if (got == 0) {
            // 读到 EOF：区分"请求越界"和"传输错误"（照 request.rs 的判据）
            LARGE_INTEGER size{};
            const bool have_size = ::GetFileSizeEx(f->handle, &size) != 0;
            if (have_size && offset + len > static_cast<uint64_t>(size.QuadPart)) return kE_DSTORAGE_END_OF_FILE;
            return kE_DSTORAGE_PASSTHROUGH_ERROR;
        }
        done += got;
    }
    return kS_OK;
}

// ============================================================================================
// 请求静态校验（照 request.rs::validate 的规则逐条移植）
// ============================================================================================
int32_t validate_request(const XRequest& r, uint32_t queue_source_type, char* why, size_t why_cap) {
    const uint64_t unknown = r.options & ~kOptKnownMask;
    if (unknown != 0) {
        std::snprintf(why, why_cap,
                      "options 带主机专用位 0x%llx（zlib=%d bcpack=%u swizzle=0x%x ext10=0x%x ext28=0x%x "
                      "intermediate=0x%x dst=0x%x src=0x%x）：PC 无对应语义，拒绝",
                      static_cast<unsigned long long>(unknown), r.zlib() ? 1 : 0,
                      static_cast<unsigned>(r.bcpack_mode()), static_cast<unsigned>(r.swizzle_mode()),
                      static_cast<unsigned>(r.ext10), static_cast<unsigned>(r.ext28),
                      static_cast<unsigned>(r.intermediate_size), static_cast<unsigned>(r.destination_size),
                      static_cast<unsigned>(r.source_size));
        return kE_DSTORAGE_RESERVED_FIELDS;
    }
    const bool mem = r.memory_source();
    if (mem != (queue_source_type == kQueueSourceMemory)) {
        std::snprintf(why, why_cap, "%s 请求放在 %s 队列上", mem ? "内存源" : "文件源",
                      queue_source_type == kQueueSourceMemory ? "内存" : "文件");
        return kE_DSTORAGE_INVALID_SOURCE_TYPE;
    }
    if (r.destination == 0) {
        std::snprintf(why, why_cap, "destination 为空");
        return kE_DSTORAGE_ACCESS_VIOLATION;
    }
    if (mem && r.source == 0) {
        std::snprintf(why, why_cap, "内存源指针为空");
        return kE_DSTORAGE_ACCESS_VIOLATION;
    }
    const uint8_t mode = r.bcpack_mode();
    if (mode > 7) {
        std::snprintf(why, why_cap, "options 0x%llx 里的 BCPack 模式 %u 不是合法 DSTORAGE_BCPACK_MODE",
                      static_cast<unsigned long long>(r.options), static_cast<unsigned>(mode));
        return kE_DSTORAGE_DECOMPRESSION_ERROR;
    }
    if (mode != 0) {
        // 诚实失败：BCPack 解码器（RANS + BC1/BC7 块表）没有移植，PC 侧也没有对应硬件路径。
        std::snprintf(why, why_cap,
                      "BCPack 模式 %u 未移植（参考实现自带完整 RANS/BC 解码表，本项目不引第三方库）；"
                      "该请求会以 E_DSTORAGE_DECOMPRESSION_ERROR 失败，不假成功",
                      static_cast<unsigned>(mode));
        return kE_DSTORAGE_DECOMPRESSION_ERROR;
    }
    if (!r.zlib()) {
        if (r.destination_size != r.source_size) {
            std::snprintf(why, why_cap, "未压缩请求 destination_size 0x%x != source_size 0x%x",
                          static_cast<unsigned>(r.destination_size), static_cast<unsigned>(r.source_size));
            return kE_DSTORAGE_INVALID_DESTINATION_SIZE;
        }
    } else {
        if (r.destination_size <= r.source_size) {
            std::snprintf(why, why_cap, "zlib 请求 destination_size 0x%x 不大于 source_size 0x%x",
                          static_cast<unsigned>(r.destination_size), static_cast<unsigned>(r.source_size));
            return kE_DSTORAGE_INVALID_DESTINATION_SIZE;
        }
    }
    if (r.intermediate_size != 0) {
        std::snprintf(why, why_cap, "intermediate_size 0x%x 只对 zlib+BCPack 有意义", static_cast<unsigned>(r.intermediate_size));
        return kE_DSTORAGE_INVALID_INTERMEDIATE_SIZE;
    }
    return kS_OK;
}

// ============================================================================================
// 队列内部：执行一条请求（CPU 后端）
// ============================================================================================
int32_t run_request(const XRequest& r, FileObj* file) {
    if (!range_accessible(reinterpret_cast<const void*>(r.destination), r.destination_size, true)) {
        return kE_DSTORAGE_ACCESS_VIOLATION;
    }
    auto* dst = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(r.destination));
    if (r.zlib()) {
        // 压缩路径：先把源（文件或内存）读进临时缓冲，再 inflate 到目标
        std::vector<uint8_t> packed;
        if (r.memory_source()) {
            if (!range_accessible(reinterpret_cast<const void*>(r.source), r.source_size, false)) {
                return kE_DSTORAGE_ACCESS_VIOLATION;
            }
            packed.assign(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(r.source)),
                          reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(r.source)) + r.source_size);
        } else {
            if (file == nullptr) return kE_DSTORAGE_INVALID_FILE_HANDLE;
            packed.resize(r.source_size);
            const int32_t hr = read_at(file, r.source_offset, packed.data(), packed.size());
            if (hr < 0) return hr;
        }
        size_t written = 0;
        const int rc = inflate_impl::zlib_inflate(packed.data(), packed.size(), dst, r.destination_size, &written);
        if (rc != inflate_impl::kOk) return inflate_hr(rc);
        if (written != r.destination_size) {
            xl("request: zlib 流在 0x%zx/0x%x 目标字节处结束（off=0x%llx）", written,
               static_cast<unsigned>(r.destination_size), static_cast<unsigned long long>(r.source_offset));
        }
        return kS_OK;
    }
    // 未压缩路径：直接 ReadFile / memcpy 到目标
    if (r.memory_source()) {
        if (!range_accessible(reinterpret_cast<const void*>(r.source), r.source_size, false)) {
            return kE_DSTORAGE_ACCESS_VIOLATION;
        }
        std::memcpy(dst, reinterpret_cast<const void*>(static_cast<uintptr_t>(r.source)), r.destination_size);
        return kS_OK;
    }
    if (file == nullptr) return kE_DSTORAGE_INVALID_FILE_HANDLE;
    return read_at(file, r.source_offset, dst, r.destination_size);
}

// 错误记录：布局照 objects.rs::write_error_record
void write_error_record(const QueueObj* q, uint8_t* rec) {
    std::memset(rec, 0, kErrorRecordSize);
    if (q->error_count == 0 || q->first_error.command == kCommandTypeNone) {
        const uint32_t none = kCommandTypeNone;
        std::memcpy(rec + 0x0c, &none, sizeof(none));
        return;
    }
    std::memcpy(rec + 0x00, &q->error_count, sizeof(q->error_count));
    std::memcpy(rec + 0x08, &q->first_error.hr, sizeof(q->first_error.hr));
    std::memcpy(rec + 0x0c, &q->first_error.command, sizeof(q->first_error.command));
    if (q->first_error.command == kCommandTypeRequest) {
        const XRequest& r = q->first_error.req;
        std::memcpy(rec + 0x10, &r.options, sizeof(r.options));
        std::memcpy(rec + 0x18, &r.destination, sizeof(r.destination));
        std::memcpy(rec + 0x20, &r.destination_size, sizeof(r.destination_size));
        if (r.memory_source()) {
            std::memcpy(rec + 0x238, &r.source, sizeof(r.source));
        } else {
            const size_t n = std::min<size_t>(q->first_error.filename.size(), kErrFilenameChars - 1);
            for (size_t i = 0; i < n; ++i) {
                const uint16_t c = static_cast<uint16_t>(q->first_error.filename[i]);
                std::memcpy(rec + kErrFilenameOff + i * 2, &c, sizeof(c));
            }
        }
        std::memcpy(rec + 0x230, &r.source_offset, sizeof(r.source_offset));
        std::memcpy(rec + 0x240, &r.source_size, sizeof(r.source_size));
        std::memcpy(rec + 0x248, &r.cancellation_tag, sizeof(r.cancellation_tag));
        const size_t nn = std::min<size_t>(q->first_error.name.size(), kErrNameChars - 1);
        std::memcpy(rec + kErrNameOff, q->first_error.name.data(), nn);
    } else if (q->first_error.command == kCommandTypeStatus) {
        std::memcpy(rec + 0x10, &q->first_error.object, sizeof(q->first_error.object));
        const uint32_t v = static_cast<uint32_t>(q->first_error.value);
        std::memcpy(rec + 0x18, &v, sizeof(v));
    } else {
        std::memcpy(rec + 0x10, &q->first_error.object, sizeof(q->first_error.object));
        std::memcpy(rec + 0x18, &q->first_error.value, sizeof(q->first_error.value));
    }
}

// ============================================================================================
// 各对象的 QI / 引用计数
// ============================================================================================
// 未知 IID 的处理策略（重要）：
//   CreateQueue/OpenFile/CreateStatusArray 都把 riid **透传**给 QI，而 GTA6 用的 riid 我们
//   无从得知（参考实现只知道它自己那 4 个版本用过的值）。主机上这个对象本来就是"当场按
//   riid 生成"的，所以对**未知** IID 我们返回同一对象并记日志；但如果请求的是一个我们
//   **已知的别的**接口（比如把 file 当 queue 要），必须 E_NOINTERFACE，否则就是把错 vtable
//   交出去，必定崩。

uint32_t file_add_ref(FileObj* o) { return o->refs.fetch_add(1, std::memory_order_acq_rel) + 1; }
uint32_t status_add_ref(StatusObj* o) { return o->refs.fetch_add(1, std::memory_order_acq_rel) + 1; }
uint32_t queue_add_ref(QueueObj* o) { return o->refs.fetch_add(1, std::memory_order_acq_rel) + 1; }

bool is_other_known_iid(const GUID* riid, const GUID& own) {
    return (iid_eq(riid, kIidFileX) || iid_eq(riid, kIidQueueX) || iid_eq(riid, kIidStatusArrayX) ||
            iid_eq(riid, kIidFactoryX) || iid_eq(riid, kIidFactoryXRef)) &&
           !iid_eq(riid, own);
}

HRESULT file_qi(FileObj* o, const GUID* riid, void** ppv) {
    if (ppv == nullptr) return static_cast<HRESULT>(kE_POINTER);
    *ppv = nullptr;
    char t[40];
    iid_text(riid, t, sizeof(t));
    if (riid == nullptr) {
        xl("IDStorageFileX::QueryInterface(slot0) riid=null -> E_POINTER");
        return static_cast<HRESULT>(kE_POINTER);
    }
    if (iid_eq(riid, kIidUnknown) || iid_eq(riid, kIidFileX) || !is_other_known_iid(riid, kIidFileX)) {
        file_add_ref(o);
        *ppv = o;
        xl("IDStorageFileX::QueryInterface(slot0) riid=%s -> S_OK file=%p", t, static_cast<void*>(o));
        return static_cast<HRESULT>(kS_OK);
    }
    xl("IDStorageFileX::QueryInterface(slot0) riid=%s -> E_NOINTERFACE（这是别的接口的 IID）", t);
    return static_cast<HRESULT>(kE_NOINTERFACE);
}

uint32_t file_release(FileObj* o) {
    const uint32_t n = o->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (n == 0) {
        {
            std::lock_guard<std::mutex> g(o->io);
            if (o->handle != INVALID_HANDLE_VALUE) {
                ::CloseHandle(o->handle);
                o->handle = INVALID_HANDLE_VALUE;
                live_del_file();
            }
            close_give_handle(o);
        }
        xl("IDStorageFileX::Release(slot2) id=%u 最后一个引用 -> 关闭文件", o->id);
        delete o;
    }
    return n;
}

HRESULT status_qi(StatusObj* o, const GUID* riid, void** ppv) {
    if (ppv == nullptr) return static_cast<HRESULT>(kE_POINTER);
    *ppv = nullptr;
    char t[40];
    iid_text(riid, t, sizeof(t));
    if (riid == nullptr) return static_cast<HRESULT>(kE_POINTER);
    if (iid_eq(riid, kIidUnknown) || iid_eq(riid, kIidStatusArrayX) || !is_other_known_iid(riid, kIidStatusArrayX)) {
        status_add_ref(o);
        *ppv = o;
        xl("IDStorageStatusArrayX::QueryInterface(slot0) riid=%s -> S_OK array=%p", t, static_cast<void*>(o));
        return static_cast<HRESULT>(kS_OK);
    }
    xl("IDStorageStatusArrayX::QueryInterface(slot0) riid=%s -> E_NOINTERFACE", t);
    return static_cast<HRESULT>(kE_NOINTERFACE);
}

uint32_t status_release(StatusObj* o) {
    const uint32_t n = o->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (n == 0) {
        xl("IDStorageStatusArrayX::Release(slot2) name='%s' 最后一个引用 -> 释放", o->name.c_str());
        delete o;
    }
    return n;
}

HRESULT queue_qi(QueueObj* o, const GUID* riid, void** ppv) {
    if (ppv == nullptr) return static_cast<HRESULT>(kE_POINTER);
    *ppv = nullptr;
    char t[40];
    iid_text(riid, t, sizeof(t));
    if (riid == nullptr) return static_cast<HRESULT>(kE_POINTER);
    if (iid_eq(riid, kIidUnknown) || iid_eq(riid, kIidQueueX) || !is_other_known_iid(riid, kIidQueueX)) {
        queue_add_ref(o);
        *ppv = o;
        xl("IDStorageQueueX::QueryInterface(slot0) riid=%s -> S_OK queue=%p", t, static_cast<void*>(o));
        return static_cast<HRESULT>(kS_OK);
    }
    xl("IDStorageQueueX::QueryInterface(slot0) riid=%s -> E_NOINTERFACE", t);
    return static_cast<HRESULT>(kE_NOINTERFACE);
}

// ============================================================================================
// 陷阱槽位：主机 vtable 里参考实现也没实现的槽位
// ============================================================================================
// 策略：走项目既有的 XB_UNIMPL（默认打印调用名后停止；XBGTASIX_UNIMPL=log 时只记日志）。
// 理由：这些槽位如果真的被调用，说明"我们对布局的假设错了"，继续跑只会用错的语义，
// 而项目 trace.h 的既有策略就是"不猜语义、当场停"。
// 用宏而不是函数模板：GCC 下 "int32_t __stdcall f(T*)" 的 __stdcall 会被当成返回类型上的
// 属性，模板特化后函数类型对不上 vtable 字段（实测报 "void (*)(FileObj*) -> uint32_t (*)"）。
// 宏展开成普通非模板函数，签名一目了然，也不会踩这个坑。
#define XCRD_TRAP(NAME, TYPE, IFACE, SLOT)                                                              \
    int32_t __stdcall NAME(TYPE*) {                                                                    \
        char n_[96];                                                                                   \
        std::snprintf(n_, sizeof(n_), IFACE " vtable slot %d (unknown to console ABI)", SLOT);          \
        ::xbgtasix::unimpl(n_);                                                                        \
        return kE_NOTIMPL;                                                                             \
    }

XCRD_TRAP(file_trap5, FileObj, "IDStorageFileX", 5)
XCRD_TRAP(file_trap6, FileObj, "IDStorageFileX", 6)
XCRD_TRAP(file_trap8, FileObj, "IDStorageFileX", 8)
XCRD_TRAP(file_trap9, FileObj, "IDStorageFileX", 9)
XCRD_TRAP(file_trap10, FileObj, "IDStorageFileX", 10)
XCRD_TRAP(file_trap11, FileObj, "IDStorageFileX", 11)

XCRD_TRAP(status_trap5, StatusObj, "IDStorageStatusArrayX", 5)
XCRD_TRAP(status_trap6, StatusObj, "IDStorageStatusArrayX", 6)
XCRD_TRAP(status_trap7, StatusObj, "IDStorageStatusArrayX", 7)
XCRD_TRAP(status_trap8, StatusObj, "IDStorageStatusArrayX", 8)
XCRD_TRAP(status_trap9, StatusObj, "IDStorageStatusArrayX", 9)

XCRD_TRAP(queue_trap12, QueueObj, "IDStorageQueueX", 12)
XCRD_TRAP(queue_trap13, QueueObj, "IDStorageQueueX", 13)
XCRD_TRAP(queue_trap14, QueueObj, "IDStorageQueueX", 14)
XCRD_TRAP(queue_trap15, QueueObj, "IDStorageQueueX", 15)
XCRD_TRAP(queue_trap16, QueueObj, "IDStorageQueueX", 16)
XCRD_TRAP(queue_trap17, QueueObj, "IDStorageQueueX", 17)

// ============================================================================================
// IDStorageFileX 方法实现
// ============================================================================================
void __stdcall file_close(FileObj* o) {
    bool was = false;
    {
        std::lock_guard<std::mutex> g(o->io);
        if (o->handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(o->handle);
            o->handle = INVALID_HANDLE_VALUE;
            live_del_file();
            was = true;
        }
        close_give_handle(o);
    }
    xl("IDStorageFileX::Close(slot3) id=%u 已关闭=%d", o->id, was ? 1 : 0);
}

HANDLE __stdcall file_get_handle(FileObj* o) {
    static std::atomic<uint32_t> calls{0};
    std::lock_guard<std::mutex> g(o->io);
    const HANDLE src = (o->give_handle != INVALID_HANDLE_VALUE) ? o->give_handle : o->handle;
    if (src == INVALID_HANDLE_VALUE) {
        xl("IDStorageFileX::GetHandle(slot4) id=%u 已关闭 -> 0", o->id);
        return nullptr;
    }
    HANDLE dup = nullptr;
    if (!::DuplicateHandle(::GetCurrentProcess(), src, ::GetCurrentProcess(), &dup, 0, FALSE,
                           DUPLICATE_SAME_ACCESS)) {
        const int32_t hr = hresult_from_win32(::GetLastError());
        xl("IDStorageFileX::GetHandle(slot4) id=%u DuplicateHandle 失败 hr=0x%08x", o->id, static_cast<unsigned>(hr));
        return nullptr;
    }
    if (detail_or_summary(calls)) {
        xl("IDStorageFileX::GetHandle(slot4) id=%u -> HANDLE %p（%s句柄的副本，调用方负责 CloseHandle）", o->id, dup,
           o->give_handle != INVALID_HANDLE_VALUE ? "独立" : "内部");
    }
    return dup;
}

uint32_t __stdcall file_get_id(FileObj* o) {
    static std::atomic<uint32_t> calls{0};
    std::lock_guard<std::mutex> g(o->io);
    const uint32_t id = (o->handle != INVALID_HANDLE_VALUE) ? o->id : 0xffffffffu;
    if (detail_or_summary(calls)) {
        xl("IDStorageFileX::GetId(slot7) id=%u 已关闭=%d -> 0x%x", o->id, o->handle == INVALID_HANDLE_VALUE ? 1 : 0,
           id);
    }
    return id;
}

const FileVtbl kFileVtbl = {file_qi,
                            file_add_ref,
                            file_release,
                            file_close,
                            file_get_handle,
                            file_trap5,
                            file_trap6,
                            file_get_id,
                            file_trap8,
                            file_trap9,
                            file_trap10,
                            file_trap11};

// ============================================================================================
// IDStorageStatusArrayX 方法实现
// ============================================================================================
bool __stdcall status_is_complete(StatusObj* o, uint32_t index) {
    if (index >= o->slots.size()) return false;
    return o->slots[index].load(std::memory_order_acquire) != kE_PENDING;
}

int32_t __stdcall status_get_hresult(StatusObj* o, uint32_t index) {
    if (index >= o->slots.size()) {
        xl("IDStorageStatusArrayX::GetHResult(slot4) name='%s' index=%u 越界(容量 %zu) -> E_DSTORAGE_INDEX_BOUND",
           o->name.c_str(), index, o->slots.size());
        return kE_DSTORAGE_INDEX_BOUND;
    }
    const int32_t hr = o->slots[index].load(std::memory_order_acquire);
    // 只在非 pending 时记录，避免游戏轮询时刷屏
    static std::atomic<uint32_t> polls{0};
    if (hr != kE_PENDING && detail_or_summary(polls)) {
        xl("IDStorageStatusArrayX::GetHResult(slot4) name='%s' index=%u -> hr=0x%08x", o->name.c_str(), index,
           static_cast<unsigned>(hr));
    }
    return hr;
}

const StatusVtbl kStatusVtbl = {status_qi,
                                status_add_ref,
                                status_release,
                                status_is_complete,
                                status_get_hresult,
                                status_trap5,
                                status_trap6,
                                status_trap7,
                                status_trap8,
                                status_trap9};

// ============================================================================================
// IDStorageQueueX 方法实现（CPU 后端：Submit 时同步完成）
// ============================================================================================
// 为什么同步：主机版 Submit 之后游戏靠状态数组/fence 判断完成。本移植在 Submit 内用
// ReadFile/memcpy/inflate 直接把数据写进目标、并把状态数组填成结果，然后返回。
// 语义上是合法的（游戏轮询时看到的是"已完成"），代价是 Submit 会阻塞调用线程。
// 参考实现用 worker 线程池做到真异步；本项目先保证"数据正确 + 不假成功"，
// 异步化留作后续（日志里会明确标 CPU-sync 后端，见 DStorageGetFactory 的初始化行）。
void queue_fail(QueueObj* q, const ErrorState& e) {
    {
        std::lock_guard<std::mutex> g(q->err_mu);
        ++q->error_count;
        if (q->first_error.command == kCommandTypeNone) q->first_error = e;
    }
    if (q->error_event != nullptr) ::SetEvent(q->error_event);
}

void __stdcall queue_enqueue_request(QueueObj* q, const void* preq) {
    if (preq == nullptr) {
        xl("IDStorageQueueX::EnqueueRequest(slot3) queue='%s' 请求指针为空 -> 丢弃", q->name.c_str());
        return;
    }
    if (!range_accessible(preq, kRequestSize, false)) {
        xl("IDStorageQueueX::EnqueueRequest(slot3) queue='%s' 请求 %p 不可读 -> 丢弃（E_DSTORAGE_ACCESS_VIOLATION）",
           q->name.c_str(), preq);
        ErrorState e;
        e.hr = kE_DSTORAGE_ACCESS_VIOLATION;
        e.command = kCommandTypeRequest;
        queue_fail(q, e);
        return;
    }
    uint8_t raw[kRequestSize];
    std::memcpy(raw, preq, kRequestSize);
    const XRequest req = decode_request(raw);

    char why[512] = {0};
    int32_t pre_error = validate_request(req, q->source_type, why, sizeof(why));
    const uint32_t n = q->enq_log.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool verbose = n <= kDetailCalls || (n % kSummaryEvery) == 0;
    if (pre_error < 0) {
        xl("IDStorageQueueX::EnqueueRequest(slot3) queue='%s' 静态校验失败 hr=0x%08x：%s（请求照样入队，Submit 时失败）",
           q->name.c_str(), static_cast<unsigned>(pre_error), why);
    }

    Entry e;
    e.kind = EntryKind::Request;
    e.req = req;
    e.pre_error = pre_error;
    e.seq = q->next_seq++;

    if (pre_error == kS_OK && !req.memory_source()) {
        if (req.source == 0) {
            xl("IDStorageQueueX::EnqueueRequest(slot3) queue='%s' 文件源指针为空", q->name.c_str());
            e.pre_error = kHR_NOACCESS;
        } else if (!range_accessible(reinterpret_cast<const void*>(static_cast<uintptr_t>(req.source)), 8, false)) {
            e.pre_error = kE_DSTORAGE_INVALID_FILE_HANDLE;
            xl("IDStorageQueueX::EnqueueRequest(slot3) queue='%s' source=%p 不可读，不是本运行时的 IDStorageFileX",
               q->name.c_str(), reinterpret_cast<const void*>(static_cast<uintptr_t>(req.source)));
        } else {
            auto* f = reinterpret_cast<FileObj*>(static_cast<uintptr_t>(req.source));
            if (f->vt != &kFileVtbl) {
                e.pre_error = kE_DSTORAGE_INVALID_FILE_HANDLE;
                xl("IDStorageQueueX::EnqueueRequest(slot3) queue='%s' source=%p 不是本运行时的 IDStorageFileX"
                   "（vtbl=%p，期望 %p）",
                   q->name.c_str(), static_cast<void*>(f), static_cast<const void*>(f->vt),
                   static_cast<const void*>(&kFileVtbl));
            } else {
                file_add_ref(f);  // 入队即持有一份引用，防止游戏在 Submit 前 Release
                e.file = f;
            }
        }
    }

    if (verbose) {
        const std::string nm = safe_cstr(reinterpret_cast<const char*>(static_cast<uintptr_t>(req.name)), 64);
        xl("IDStorageQueueX::EnqueueRequest(slot3) queue='%s' seq=%llu opts=0x%llx zlib=%d bcpack=%u "
           "dst=0x%llx dsize=0x%x src=0x%llx src_off=0x%llx ssize=0x%x name='%s' tag=0x%llx (第 %u 次)",
           q->name.c_str(), static_cast<unsigned long long>(e.seq),
           static_cast<unsigned long long>(req.options), req.zlib() ? 1 : 0,
           static_cast<unsigned>(req.bcpack_mode()), static_cast<unsigned long long>(req.destination),
           static_cast<unsigned>(req.destination_size), static_cast<unsigned long long>(req.source),
           static_cast<unsigned long long>(req.source_offset), static_cast<unsigned>(req.source_size),
           nm.c_str(), static_cast<unsigned long long>(req.cancellation_tag), n);
    }

    {
        std::lock_guard<std::mutex> g(q->mu);
        if (q->closed) {
            xl("IDStorageQueueX::EnqueueRequest(slot3) queue='%s' 已关闭 -> E_DSTORAGE_QUEUE_CLOSED", q->name.c_str());
            if (e.file != nullptr) file_release(e.file);
            return;
        }
        q->pending.push_back(std::move(e));
    }
}

void __stdcall queue_enqueue_status(QueueObj* q, void* array, uint32_t index) {
    if (array == nullptr) {
        xl("IDStorageQueueX::EnqueueStatus(slot4) queue='%s' 数组为空", q->name.c_str());
        return;
    }
    if (!range_accessible(array, 8, false)) {
        xl("IDStorageQueueX::EnqueueStatus(slot4) queue='%s' 数组 %p 不可读", q->name.c_str(), array);
        return;
    }
    auto* a = static_cast<StatusObj*>(array);
    if (a->vt != &kStatusVtbl) {
        xl("IDStorageQueueX::EnqueueStatus(slot4) queue='%s' %p 不是本运行时的 IDStorageStatusArrayX -> "
           "E_DSTORAGE_INVALID_STATUS_ARRAY（记录错误）",
           q->name.c_str(), array);
        ErrorState e;
        e.hr = kE_DSTORAGE_INVALID_STATUS_ARRAY;
        e.command = kCommandTypeStatus;
        e.object = reinterpret_cast<uint64_t>(array);
        e.value = index;
        queue_fail(q, e);
        return;
    }
    if (index >= a->slots.size()) {
        xl("IDStorageQueueX::EnqueueStatus(slot4) queue='%s' index=%u >= 容量 %zu -> E_DSTORAGE_INDEX_BOUND",
           q->name.c_str(), index, a->slots.size());
        ErrorState e;
        e.hr = kE_DSTORAGE_INDEX_BOUND;
        e.command = kCommandTypeStatus;
        e.object = reinterpret_cast<uint64_t>(array);
        e.value = index;
        queue_fail(q, e);
        return;
    }
    a->slots[index].store(kE_PENDING, std::memory_order_release);
    {
        std::lock_guard<std::mutex> g(q->mu);
        if (q->closed) {
            a->slots[index].store(kE_DSTORAGE_QUEUE_CLOSED, std::memory_order_release);
            xl("IDStorageQueueX::EnqueueStatus(slot4) queue='%s' 已关闭", q->name.c_str());
            return;
        }
        Entry e;
        e.kind = EntryKind::Status;
        e.status = a;
        e.index = index;
        e.seq = q->next_seq++;
        status_add_ref(a);
        q->pending.push_back(std::move(e));
    }
    xl("IDStorageQueueX::EnqueueStatus(slot4) queue='%s' array='%s' index=%u（该槽先置 E_PENDING）", q->name.c_str(),
       a->name.c_str(), index);
}

void __stdcall queue_enqueue_signal(QueueObj* q, void* fence, uint64_t value) {
    ID3D12Fence* pc = nullptr;
    // 主机 fence 的 IID 没有 PC 实现；只有能 QI 出 **PC 的 ID3D12Fence** 时我们才真的 Signal，
    // 否则按参考实现记 E_DSTORAGE_INVALID_FENCE（不假装"已发信号"，否则游戏会永久等一个不会来的信号）。
    if (fence != nullptr && range_accessible(fence, 8, false)) {
        auto** vt = *reinterpret_cast<void***>(fence);
        if (vt != nullptr && range_accessible(vt, 8, false)) {
            using QiFn = HRESULT(__stdcall*)(void*, const GUID*, void**);
            auto qi = reinterpret_cast<QiFn>(vt[0]);
            void* out = nullptr;
            const HRESULT hr = qi(fence, &kIidD3D12FencePc, &out);
            if (hr >= 0 && out != nullptr) pc = static_cast<ID3D12Fence*>(out);
        }
    }
    if (pc == nullptr) {
        xl("IDStorageQueueX::EnqueueSignal(slot5) queue='%s' fence=%p value=%llu：拿不到 PC ID3D12Fence，"
           "按 E_DSTORAGE_INVALID_FENCE 记录（不会假装已发信号）",
           q->name.c_str(), fence, static_cast<unsigned long long>(value));
        ErrorState e;
        e.hr = kE_DSTORAGE_INVALID_FENCE;
        e.command = kCommandTypeSignal;
        e.object = reinterpret_cast<uint64_t>(fence);
        e.value = value;
        queue_fail(q, e);
        return;
    }
    {
        std::lock_guard<std::mutex> g(q->mu);
        if (q->closed) {
            xl("IDStorageQueueX::EnqueueSignal(slot5) queue='%s' 已关闭", q->name.c_str());
            pc->Release();
            return;
        }
        Entry e;
        e.kind = EntryKind::Signal;
        e.fence = pc;  // 已持有引用（QI 已 AddRef）
        e.signal_value = value;
        e.seq = q->next_seq++;
        q->pending.push_back(std::move(e));
    }
    xl("IDStorageQueueX::EnqueueSignal(slot5) queue='%s' fence=%p value=%llu（PC fence，Submit 时 Signal）",
       q->name.c_str(), fence, static_cast<unsigned long long>(value));
}

void __stdcall queue_submit(QueueObj* q) {
    std::vector<Entry> batch;
    {
        std::lock_guard<std::mutex> g(q->mu);
        if (q->closed) {
            xl("IDStorageQueueX::Submit(slot6) queue='%s' 已关闭", q->name.c_str());
            return;
        }
        batch.swap(q->pending);
    }
    const uint32_t n = q->submit_log.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool verbose = n <= kDetailCalls || (n % kSummaryEvery) == 0;

    uint32_t ok = 0, failed = 0;
    int32_t pending_err_since_status = kS_OK;  // 上一个 Status 之后累计的第一条错误（照参考实现语义）
    bool has_err = false;
    for (auto& e : batch) {
        switch (e.kind) {
            case EntryKind::Request: {
                int32_t hr = e.pre_error;
                if (hr == kS_OK && e.aborted) hr = kHR_OPERATION_ABORTED;
                if (hr == kS_OK) hr = run_request(e.req, e.file);
                if (hr < 0) {
                    ++failed;
                    if (!has_err) {
                        pending_err_since_status = hr;
                        has_err = true;
                    }
                    if (hr != kHR_OPERATION_ABORTED) {
                        ErrorState err;
                        err.hr = hr;
                        err.command = kCommandTypeRequest;
                        err.req = e.req;
                        err.filename = (e.file != nullptr) ? e.file->path : std::wstring();
                        err.name = safe_cstr(reinterpret_cast<const char*>(static_cast<uintptr_t>(e.req.name)), kErrNameChars);
                        xl("queue '%s' 请求失败 hr=0x%08x opts=0x%llx off=0x%llx size=0x%x dst=0x%llx dsize=0x%x",
                           q->name.c_str(), static_cast<unsigned>(hr), static_cast<unsigned long long>(e.req.options),
                           static_cast<unsigned long long>(e.req.source_offset),
                           static_cast<unsigned>(e.req.source_size),
                           static_cast<unsigned long long>(e.req.destination),
                           static_cast<unsigned>(e.req.destination_size));
                        queue_fail(q, err);
                    }
                } else {
                    ++ok;
                }
                if (e.file != nullptr) file_release(e.file);
                break;
            }
            case EntryKind::Status: {
                const int32_t hr = has_err ? pending_err_since_status : kS_OK;
                has_err = false;
                pending_err_since_status = kS_OK;
                if (e.status != nullptr) {
                    e.status->slots[e.index].store(hr, std::memory_order_release);
                    status_release(e.status);
                }
                break;
            }
            case EntryKind::Signal: {
                if (e.fence != nullptr) {
                    const HRESULT hr = e.fence->Signal(e.signal_value);
                    if (FAILED(hr)) {
                        ErrorState err;
                        err.hr = static_cast<int32_t>(hr);
                        err.command = kCommandTypeSignal;
                        err.object = reinterpret_cast<uint64_t>(e.fence);
                        err.value = e.signal_value;
                        xl("queue '%s' ID3D12Fence::Signal(%llu) 失败 hr=0x%08x", q->name.c_str(),
                           static_cast<unsigned long long>(e.signal_value), static_cast<unsigned>(hr));
                        queue_fail(q, err);
                    }
                    e.fence->Release();
                    e.fence = nullptr;
                }
                break;
            }
        }
        ++q->completed;
    }
    if (verbose) {
        xl("IDStorageQueueX::Submit(slot6) queue='%s' 一批 %zu 条（成功 %u 失败 %u，累计完成 %llu，"
           "CPU-sync 后端：返回即已完成）第 %u 次 Submit",
           q->name.c_str(), batch.size(), ok, failed, static_cast<unsigned long long>(q->completed), n);
    }
}

void __stdcall queue_cancel(QueueObj* q, uint64_t mask, uint64_t value) {
    size_t n = 0;
    {
        std::lock_guard<std::mutex> g(q->mu);
        for (auto& e : q->pending) {
            if (e.kind == EntryKind::Request && (e.req.cancellation_tag & mask) == value) {
                e.aborted = true;
                ++n;
            }
        }
    }
    if ((mask & value) != value) {
        xl("IDStorageQueueX::CancelRequestsWithTag(slot7) queue='%s' mask=0x%llx & value=0x%llx != value，"
           "这个组合匹配不到任何请求",
           q->name.c_str(), static_cast<unsigned long long>(mask), static_cast<unsigned long long>(value));
    }
    xl("IDStorageQueueX::CancelRequestsWithTag(slot7) queue='%s' mask=0x%llx value=0x%llx -> 未提交命中 %zu 条"
       "（Submit 时以 HR_OPERATION_ABORTED 失败）",
       q->name.c_str(), static_cast<unsigned long long>(mask), static_cast<unsigned long long>(value), n);
}

void __stdcall queue_close(QueueObj* q) {
    std::vector<Entry> left;
    {
        std::lock_guard<std::mutex> g(q->mu);
        if (q->closed) {
            xl("IDStorageQueueX::Close(slot8) queue='%s' 重复关闭", q->name.c_str());
            return;
        }
        q->closed = true;
        left.swap(q->pending);
    }
    for (auto& e : left) {
        if (e.kind == EntryKind::Status && e.status != nullptr) {
            e.status->slots[e.index].store(kE_DSTORAGE_QUEUE_CLOSED, std::memory_order_release);
            status_release(e.status);
        } else if (e.kind == EntryKind::Request && e.file != nullptr) {
            file_release(e.file);
        } else if (e.kind == EntryKind::Signal && e.fence != nullptr) {
            e.fence->Release();
        }
    }
    xl("IDStorageQueueX::Close(slot8) queue='%s' 丢弃未提交 %zu 条，累计完成 %llu", q->name.c_str(), left.size(),
       static_cast<unsigned long long>(q->completed));
}

HANDLE __stdcall queue_get_error_event(QueueObj* q) {
    if (q->closed) {
        xl("IDStorageQueueX::GetErrorEvent(slot9) queue='%s' 已关闭 -> 0", q->name.c_str());
        return nullptr;
    }
    HANDLE dup = nullptr;
    if (!::DuplicateHandle(::GetCurrentProcess(), q->error_event, ::GetCurrentProcess(), &dup, 0, FALSE,
                           DUPLICATE_SAME_ACCESS)) {
        xl("IDStorageQueueX::GetErrorEvent(slot9) queue='%s' DuplicateHandle 失败 err=%lu", q->name.c_str(),
           ::GetLastError());
        return nullptr;
    }
    xl("IDStorageQueueX::GetErrorEvent(slot9) queue='%s' -> HANDLE %p（调用方负责 CloseHandle）", q->name.c_str(),
       dup);
    return dup;
}

void __stdcall queue_retrieve_error_record(QueueObj* q, void* out) {
    if (out == nullptr) {
        xl("IDStorageQueueX::RetrieveErrorRecord(slot10) queue='%s' 记录指针为空", q->name.c_str());
        return;
    }
    if (!range_accessible(out, kErrorRecordSize, true)) {
        xl("IDStorageQueueX::RetrieveErrorRecord(slot10) queue='%s' 记录 %p 不可写", q->name.c_str(), out);
        return;
    }
    std::lock_guard<std::mutex> g(q->err_mu);
    write_error_record(q, static_cast<uint8_t*>(out));
    xl("IDStorageQueueX::RetrieveErrorRecord(slot10) queue='%s' 错误数=%u 首条 hr=0x%08x cmd=%u", q->name.c_str(),
       q->error_count, static_cast<unsigned>(q->first_error.hr), q->first_error.command);
}

void __stdcall queue_query(QueueObj* q, void* out) {
    static std::atomic<uint32_t> calls{0};
    if (out == nullptr) {
        xl("IDStorageQueueX::Query(slot11) queue='%s' info 指针为空", q->name.c_str());
        return;
    }
    if (!range_accessible(out, kQueueInfoSize, true)) {
        xl("IDStorageQueueX::Query(slot11) queue='%s' info %p 不可写", q->name.c_str(), out);
        return;
    }
    auto* b = static_cast<uint8_t*>(out);
    std::memset(b, 0, kQueueInfoSize);
    uint16_t pending = 0;
    {
        std::lock_guard<std::mutex> g(q->mu);
        std::memcpy(b, q->raw_desc, kQueueDescSize);
        pending = static_cast<uint16_t>(std::min<size_t>(q->pending.size(), 0xffff));
    }
    const uint16_t cap = q->capacity;
    const uint16_t empty = static_cast<uint16_t>(cap > pending ? cap - 1 - pending : 0);
    const uint16_t half = static_cast<uint16_t>(std::max<uint32_t>(cap / 2, 1));
    const uint16_t until = static_cast<uint16_t>(half > pending ? half - pending : half);
    std::memcpy(b + 0x18, &empty, sizeof(empty));
    std::memcpy(b + 0x1a, &until, sizeof(until));
    if (detail_or_summary(calls)) {
        xl("IDStorageQueueX::Query(slot11) queue='%s' capacity=%u pending=%u empty=%u until=%u", q->name.c_str(), cap,
           pending, empty, until);
    }
}

uint32_t __stdcall queue_release(QueueObj* o);  // 先声明：kQueueVtbl 要在它定义前引用

const QueueVtbl kQueueVtbl = {queue_qi,
                              queue_add_ref,
                              queue_release,
                              queue_enqueue_request,
                              queue_enqueue_status,
                              queue_enqueue_signal,
                              queue_submit,
                              queue_cancel,
                              queue_close,
                              queue_get_error_event,
                              queue_retrieve_error_record,
                              queue_query,
                              queue_trap12,
                              queue_trap13,
                              queue_trap14,
                              queue_trap15,
                              queue_trap16,
                              queue_trap17};

uint32_t __stdcall queue_release(QueueObj* o) {
    const uint32_t n = o->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (n == 0) {
        // 先按 Close 的语义收尾（未提交条目要释放引用），再删对象
        if (!o->closed) queue_close(o);
        if (o->error_event != nullptr) ::CloseHandle(o->error_event);
        live_del_queue();
        xl("IDStorageQueueX::Release(slot2) queue='%s' 最后一个引用 -> 释放", o->name.c_str());
        delete o;
    }
    return n;
}

// ============================================================================================
// IDStorageFactoryX 方法实现
// ============================================================================================
struct FactoryObj {
    const struct FactoryVtbl* vt;
};

struct FactoryVtbl {
    HRESULT(__stdcall* QueryInterface)(FactoryObj*, const GUID*, void**);
    uint32_t(__stdcall* AddRef)(FactoryObj*);
    uint32_t(__stdcall* Release)(FactoryObj*);
    HRESULT(__stdcall* CreateQueue)(FactoryObj*, const void*, const GUID*, void**);
    HRESULT(__stdcall* OpenFile)(FactoryObj*, const wchar_t*, const GUID*, void**);
    HRESULT(__stdcall* CreateStatusArray)(FactoryObj*, uint32_t, const char*, const GUID*, void**);
    HRESULT(__stdcall* SetCpuAffinity)(FactoryObj*, uint64_t);
    void(__stdcall* SetDebugFlags)(FactoryObj*, uint32_t);
    HRESULT(__stdcall* SetStagingBufferSize)(FactoryObj*, uint32_t);
    int32_t(__stdcall* Slot9)(FactoryObj*);
    int32_t(__stdcall* Slot10)(FactoryObj*);
    int32_t(__stdcall* Slot11)(FactoryObj*);
    int32_t(__stdcall* Slot12)(FactoryObj*);
    int32_t(__stdcall* Slot13)(FactoryObj*);
    int32_t(__stdcall* Slot14)(FactoryObj*);
    int32_t(__stdcall* Slot15)(FactoryObj*);
};

HRESULT factory_qi(FactoryObj* self, const GUID* riid, void** ppv) {
    if (ppv == nullptr) return static_cast<HRESULT>(kE_POINTER);
    *ppv = nullptr;
    char t[40];
    iid_text(riid, t, sizeof(t));
    if (riid == nullptr) {
        xl("IDStorageFactoryX::QueryInterface(slot0) riid=null -> E_POINTER");
        return static_cast<HRESULT>(kE_POINTER);
    }
    if (iid_eq(riid, kIidFactoryX) || iid_eq(riid, kIidUnknown)) {
        *ppv = self;
        xl("IDStorageFactoryX::QueryInterface(slot0) riid=%s -> S_OK", t);
        return static_cast<HRESULT>(kS_OK);
    }
    if (iid_eq(riid, kIidFactoryXRef)) {
        *ppv = self;
        xl("IDStorageFactoryX::QueryInterface(slot0) riid=%s（参考实现那版 GDK 的工厂 IID，ALT 接受）-> S_OK", t);
        return static_cast<HRESULT>(kS_OK);
    }
    if (iid_eq(riid, kIidFactoryPc)) {
        xl("IDStorageFactoryX::QueryInterface(slot0) riid=%s 是 **PC** DirectStorage 的工厂 IID -> E_NOINTERFACE"
           "（主机对象不实现 PC 接口；游戏要的是 62959D11-… 这个主机 IID）",
           t);
        return static_cast<HRESULT>(kE_NOINTERFACE);
    }
    xl("IDStorageFactoryX::QueryInterface(slot0) riid=%s -> E_NOINTERFACE", t);
    return static_cast<HRESULT>(kE_NOINTERFACE);
}

uint32_t factory_add_ref(FactoryObj*) {
    // 工厂是进程级单例（参考实现同样恒返回 1，且 release 也映射到 add_ref）
    return 1;
}

HRESULT factory_create_queue(FactoryObj*, const void* desc, const GUID* riid, void** ppv) {
    if (ppv == nullptr) {
        xl("IDStorageFactoryX::CreateQueue(slot3) out=null -> E_POINTER");
        return static_cast<HRESULT>(kE_POINTER);
    }
    *ppv = nullptr;
    if (desc == nullptr) {
        xl("IDStorageFactoryX::CreateQueue(slot3) desc=null -> E_INVALIDARG");
        return static_cast<HRESULT>(kE_INVALIDARG);
    }
    if (!range_accessible(desc, kQueueDescSize, false)) {
        xl("IDStorageFactoryX::CreateQueue(slot3) desc=%p 不可读 -> E_INVALIDARG", desc);
        return static_cast<HRESULT>(kE_INVALIDARG);
    }
    uint8_t raw[kQueueDescSize];
    std::memcpy(raw, desc, kQueueDescSize);
    // 主机 DSTORAGE_QUEUE_DESC 布局（0x18）：
    //   0x00 u32 SourceType（0=文件 1=内存）
    //   0x08 u16 Capacity
    //   0x0a i8  Priority（-1..2）
    //   0x10 u64 Name（指向 ASCII 名字的指针 —— PC 版这里是内联 char[16]）
    const uint32_t source_type = rd_u32(raw + 0x00);
    const uint16_t capacity = rd_u16(raw + 0x08);
    const int8_t priority = static_cast<int8_t>(raw[0x0a]);
    const uint64_t name_ptr = rd_u64(raw + 0x10);
    const std::string name = safe_cstr(reinterpret_cast<const char*>(static_cast<uintptr_t>(name_ptr)), 0x40);

    if (priority < kPriorityLow || priority > kPriorityRealtime) {
        xl("IDStorageFactoryX::CreateQueue(slot3) priority=%d -> E_DSTORAGE_INVALID_QUEUE_PRIORITY",
           static_cast<int>(priority));
        return static_cast<HRESULT>(kE_DSTORAGE_INVALID_QUEUE_PRIORITY);
    }
    if (source_type != kQueueSourceFile && source_type != kQueueSourceMemory) {
        xl("IDStorageFactoryX::CreateQueue(slot3) source_type=%u -> E_DSTORAGE_INVALID_QUEUE_TYPE", source_type);
        return static_cast<HRESULT>(kE_DSTORAGE_INVALID_QUEUE_TYPE);
    }
    if (source_type == kQueueSourceMemory && priority != kPriorityRealtime) {
        xl("IDStorageFactoryX::CreateQueue(slot3) 内存队列 priority=%d（必须 %d）-> "
           "E_DSTORAGE_INVALID_MEMORY_QUEUE_PRIORITY",
           static_cast<int>(priority), static_cast<int>(kPriorityRealtime));
        return static_cast<HRESULT>(kE_DSTORAGE_INVALID_MEMORY_QUEUE_PRIORITY);
    }
    if (capacity < 2) {
        xl("IDStorageFactoryX::CreateQueue(slot3) capacity=%u -> E_DSTORAGE_INVALID_QUEUE_CAPACITY", capacity);
        return static_cast<HRESULT>(kE_DSTORAGE_INVALID_QUEUE_CAPACITY);
    }

    auto* q = new (std::nothrow) QueueObj;
    if (q == nullptr) {
        xl("IDStorageFactoryX::CreateQueue(slot3) 分配队列失败 -> E_OUTOFMEMORY");
        return static_cast<HRESULT>(kE_OUTOFMEMORY);
    }
    q->vt = &kQueueVtbl;
    std::memcpy(q->raw_desc, raw, kQueueDescSize);
    q->source_type = source_type;
    q->capacity = capacity;
    q->priority = priority;
    q->name = name;
    q->error_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (q->error_event == nullptr) {
        const int32_t hr = hresult_from_win32(::GetLastError());
        xl("IDStorageFactoryX::CreateQueue(slot3) CreateEventW 失败 hr=0x%08x", static_cast<unsigned>(hr));
        delete q;
        return static_cast<HRESULT>(hr);
    }
    live_add_queue();
    const HRESULT hr = queue_qi(q, riid, ppv);
    queue_release(q);  // 交给调用方一份引用（queue_qi 已 AddRef）
    char rt[40];
    iid_text(riid, rt, sizeof(rt));
    xl("IDStorageFactoryX::CreateQueue(slot3) type=%s capacity=%u priority=%d name='%s' riid=%s -> hr=0x%08x queue=%p",
       source_type == kQueueSourceMemory ? "内存" : "文件", capacity, static_cast<int>(priority), name.c_str(), rt,
       static_cast<unsigned>(hr), *ppv);
    return hr;
}

HRESULT factory_open_file(FactoryObj*, const wchar_t* path, const GUID* riid, void** ppv) {
    if (ppv == nullptr) {
        xl("IDStorageFactoryX::OpenFile(slot4) out=null -> E_POINTER");
        return static_cast<HRESULT>(kE_POINTER);
    }
    *ppv = nullptr;
    if (path == nullptr) {
        xl("IDStorageFactoryX::OpenFile(slot4) path=null -> E_INVALIDARG");
        return static_cast<HRESULT>(kE_INVALIDARG);
    }
    const std::wstring wanted = safe_wstr(path, 0x8000);
    std::wstring used;
    HANDLE h = open_path_candidates(wanted, &used);
    if (h == INVALID_HANDLE_VALUE) {
        const int32_t hr = hresult_from_win32(::GetLastError());
        xl("IDStorageFactoryX::OpenFile(slot4) 打不开 '%ls'（尝试过原路径与 game_root 回退）hr=0x%08x -> 返回失败",
           wanted.c_str(), static_cast<unsigned>(hr));
        return static_cast<HRESULT>(hr);
    }
    auto* f = new (std::nothrow) FileObj;
    if (f == nullptr) {
        ::CloseHandle(h);
        xl("IDStorageFactoryX::OpenFile(slot4) 分配文件对象失败 -> E_OUTOFMEMORY");
        return static_cast<HRESULT>(kE_OUTOFMEMORY);
    }
    f->vt = &kFileVtbl;
    f->id = g_next_file_id.fetch_add(1, std::memory_order_relaxed);
    f->path = wanted;
    f->handle = h;
    // 另开一个句柄专供 GetHandle（见 FileObj::give_handle 的注释）。失败就退化成共用内部句柄，
    // 只是并发读有偏移风险，记一条日志说明。
    f->give_handle = ::CreateFileW(used.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f->give_handle == INVALID_HANDLE_VALUE) {
        xl("IDStorageFactoryX::OpenFile(slot4) '%ls' 第二次打开(给 GetHandle)失败 err=%lu，退化为共用内部句柄",
           used.c_str(), ::GetLastError());
    }
    live_add_file();
    const HRESULT hr = file_qi(f, riid, ppv);
    const uint32_t fid = f->id;  // release 后对象可能已被析构，先取值再 release
    file_release(f);
    xl("IDStorageFactoryX::OpenFile(slot4) '%ls' -> hr=0x%08x file=%p id=%u 实际打开路径='%ls'", wanted.c_str(),
       static_cast<unsigned>(hr), *ppv, fid, used.c_str());
    return hr;
}

HRESULT factory_create_status_array(FactoryObj*, uint32_t capacity, const char* name, const GUID* riid, void** ppv) {
    if (ppv == nullptr) {
        xl("IDStorageFactoryX::CreateStatusArray(slot5) out=null -> E_POINTER");
        return static_cast<HRESULT>(kE_POINTER);
    }
    *ppv = nullptr;
    auto* a = new (std::nothrow) StatusObj;
    if (a == nullptr) {
        xl("IDStorageFactoryX::CreateStatusArray(slot5) capacity=%u 分配失败 -> E_OUTOFMEMORY", capacity);
        return static_cast<HRESULT>(kE_OUTOFMEMORY);
    }
    a->vt = &kStatusVtbl;
    a->name = safe_cstr(name, kStatusNameChars);
    try {
        a->slots = std::vector<std::atomic<int32_t>>(capacity);
    } catch (...) {
        xl("IDStorageFactoryX::CreateStatusArray(slot5) capacity=%u 分配槽位失败 -> E_OUTOFMEMORY", capacity);
        delete a;
        return static_cast<HRESULT>(kE_OUTOFMEMORY);
    }
    for (auto& s : a->slots) s.store(kE_PENDING, std::memory_order_relaxed);
    const HRESULT hr = status_qi(a, riid, ppv);
    const std::string nm = a->name;  // 同上：先复制再 release
    status_release(a);
    char rt[40];
    iid_text(riid, rt, sizeof(rt));
    xl("IDStorageFactoryX::CreateStatusArray(slot5) capacity=%u name='%s' riid=%s -> hr=0x%08x array=%p（各槽初始 E_PENDING）",
       capacity, nm.c_str(), rt, static_cast<unsigned>(hr), *ppv);
    return hr;
}

HRESULT factory_set_cpu_affinity(FactoryObj*, uint64_t mask) {
    DWORD_PTR proc = 0, sys = 0;
    if (!::GetProcessAffinityMask(::GetCurrentProcess(), &proc, &sys)) {
        xl("IDStorageFactoryX::SetCpuAffinity(slot6) mask=0x%llx GetProcessAffinityMask 失败 err=%lu -> S_OK(不生效)",
           static_cast<unsigned long long>(mask), ::GetLastError());
        return static_cast<HRESULT>(kS_OK);
    }
    const uint64_t usable = mask & static_cast<uint64_t>(proc);
    if (usable == 0) {
        xl("IDStorageFactoryX::SetCpuAffinity(slot6) mask=0x%llx 与本进程亲和性 0x%llx 无交集 -> E_INVALIDARG",
           static_cast<unsigned long long>(mask), static_cast<unsigned long long>(proc));
        return static_cast<HRESULT>(kE_INVALIDARG);
    }
    g_cpu_affinity.store(usable, std::memory_order_relaxed);
    xl("IDStorageFactoryX::SetCpuAffinity(slot6) mask=0x%llx -> S_OK（记下 %llx；本移植的 CPU-sync 后端不动线程亲和性）",
       static_cast<unsigned long long>(mask), static_cast<unsigned long long>(usable));
    return static_cast<HRESULT>(kS_OK);
}

// 注意：参考实现里这个槽位的返回类型是 **void**（objects.rs: factory_set_debug_flags），
// 不是 HRESULT。照抄，别改成 HRESULT —— 否则 EAX 里的返回值语义就和真机不一致了。
void factory_set_debug_flags(FactoryObj*, uint32_t flags) {
    g_debug_flags.store(flags, std::memory_order_relaxed);
    xl("IDStorageFactoryX::SetDebugFlags(slot7) flags=0x%x -> (void，无返回值)", flags);
}

HRESULT factory_set_staging_buffer_size(FactoryObj*, uint32_t size) {
    const auto live = live_counts();
    if (live.first != kLiveNone || live.second != kLiveNone) {
        // 参考实现（也就是真机行为）在已有队列/文件时拒绝改暂存缓冲大小
        xl("IDStorageFactoryX::SetStagingBufferSize(slot8) size=0x%x 但仍有 %llu 个队列 / %llu 个文件打开 -> "
           "E_DSTORAGE_STAGING_BUFFER_LOCKED",
           size, static_cast<unsigned long long>(live.first), static_cast<unsigned long long>(live.second));
        return static_cast<HRESULT>(kE_DSTORAGE_STAGING_BUFFER_LOCKED);
    }
    g_staging_buffer_size.store(size, std::memory_order_relaxed);
    xl("IDStorageFactoryX::SetStagingBufferSize(slot8) size=0x%x (%u MB) -> S_OK"
       "（CPU 解码路径直接解到目标缓冲，不分配暂存缓冲；只记录参数）",
       size, size >> 20);
    return static_cast<HRESULT>(kS_OK);
}

XCRD_TRAP(factory_trap9, FactoryObj, "IDStorageFactoryX", 9)
XCRD_TRAP(factory_trap10, FactoryObj, "IDStorageFactoryX", 10)
XCRD_TRAP(factory_trap11, FactoryObj, "IDStorageFactoryX", 11)
XCRD_TRAP(factory_trap12, FactoryObj, "IDStorageFactoryX", 12)
XCRD_TRAP(factory_trap13, FactoryObj, "IDStorageFactoryX", 13)
XCRD_TRAP(factory_trap14, FactoryObj, "IDStorageFactoryX", 14)
XCRD_TRAP(factory_trap15, FactoryObj, "IDStorageFactoryX", 15)

const FactoryVtbl kFactoryVtbl = {factory_qi,
                                  factory_add_ref,
                                  factory_add_ref,  // Release：单例，恒返回 1（参考实现同样映射到 add_ref）
                                  factory_create_queue,
                                  factory_open_file,
                                  factory_create_status_array,
                                  factory_set_cpu_affinity,
                                  factory_set_debug_flags,
                                  factory_set_staging_buffer_size,
                                  factory_trap9,
                                  factory_trap10,
                                  factory_trap11,
                                  factory_trap12,
                                  factory_trap13,
                                  factory_trap14,
                                  factory_trap15};

FactoryObj g_factory{&kFactoryVtbl};

// 进程级一次性初始化日志：把后端形态和已知限制一次说清，省得后面靠猜
void backend_init_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        xl("=== 主机 DirectStorage 移植后端就绪 ===");
        xl("对象布局：主机版 IDStorageFactoryX(16 槽) / FileX(12 槽) / StatusArrayX(10 槽) / QueueX(18 槽)");
        xl("后端：CPU 文件后端 + 自带 inflate(RFC1950/1951)；Submit 内同步完成（返回即数据就绪）");
        xl("已实现：OpenFile/CreateQueue/CreateStatusArray/Enqueue*/Submit/Query/RetrieveErrorRecord/"
           "GetErrorEvent/CancelRequestsWithTag/Close/SetCpuAffinity/SetDebugFlags/SetStagingBufferSize");
        xl("未实现（会以明确失败码返回，不假成功）：BCPack 解压、主机 swizzle 位、未知 vtable 槽位");
    });
}

}  // namespace

// ============================================================================================
// 导出：DStorageGetFactory
// ============================================================================================
// 参考实现的签名（crates/compat/xcrdapi/src/lib.rs:60）：
//     DStorageGetFactory(riid: *const IID, ppv: *mut *mut c_void) -> HRESULT
// 行为：
//   - ppv 为空 -> E_POINTER
//   - 主机 IDStorageFactoryX 的 IID（GTA6 实测 {62959D11-D45F-4884-A0A3-F7862EA243AE}）-> 返回本模块
//     自己实现的工厂对象（16 槽 vtable，见文件头）
//   - IUnknown -> 同一对象
//   - 参考实现那版 GDK 的工厂 IID（{fb3b54ac-…}）-> 同一对象，日志标 ALT（多版本兼容，不影响主路径）
//   - PC DirectStorage 的 IID（{6924ea0c-…}）-> 明确 E_NOINTERFACE 并说明原因（这条最容易误判）
//   - 其它 -> E_NOINTERFACE
extern "C" __declspec(dllexport) HRESULT __stdcall DStorageGetFactory(const GUID* riid, void** ppv) {
    backend_init_once();

    if (ppv == nullptr) {
        xl("DStorageGetFactory riid=%p ppv=null -> E_POINTER", static_cast<const void*>(riid));
        return static_cast<HRESULT>(kE_POINTER);
    }
    *ppv = nullptr;

    char t[40];
    iid_text(riid, t, sizeof(t));
    if (riid == nullptr) {
        xl("DStorageGetFactory riid=null -> E_POINTER");
        return static_cast<HRESULT>(kE_POINTER);
    }

    if (iid_eq(riid, kIidFactoryX)) {
        *ppv = &g_factory;
        xl("DStorageGetFactory riid=%s -> S_OK factory=%p（主机布局 IDStorageFactoryX，CPU 文件/zlib 后端）", t,
           *ppv);
        return static_cast<HRESULT>(kS_OK);
    }
    if (iid_eq(riid, kIidUnknown)) {
        *ppv = &g_factory;
        xl("DStorageGetFactory riid=%s (IUnknown) -> S_OK factory=%p", t, *ppv);
        return static_cast<HRESULT>(kS_OK);
    }
    if (iid_eq(riid, kIidFactoryXRef)) {
        *ppv = &g_factory;
        xl("DStorageGetFactory riid=%s（参考实现那版 GDK 的工厂 IID，ALT）-> S_OK factory=%p", t, *ppv);
        return static_cast<HRESULT>(kS_OK);
    }
    if (iid_eq(riid, kIidFactoryPc)) {
        // 这条日志是关键排障信息：说明我们**知道**游戏拿到了 PC 的 IID 时不接受，
        // 而不是笼统的"接口不支持"。
        xl("DStorageGetFactory riid=%s 是 PC DirectStorage 的工厂 IID -> E_NOINTERFACE"
           "（主机接口与 PC 接口的 IID/结构体布局都不同，不能互转）",
           t);
        return static_cast<HRESULT>(kE_NOINTERFACE);
    }
    xl("DStorageGetFactory riid=%s -> E_NOINTERFACE（只接受 62959D11-… 主机工厂 IID 与 IUnknown）", t);
    return static_cast<HRESULT>(kE_NOINTERFACE);
}
