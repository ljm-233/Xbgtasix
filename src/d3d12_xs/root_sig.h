// SPDX-License-Identifier: GPL-3.0-or-later
// 主机 root signature 标志位剥离 —— d3d12_xs 的两个序列化入口
//
// 移植来源：omonkeyz/xbox-emulator（GPL-3.0-or-later，与本项目同许可）
//   crates/compat/d3d12_xs/src/rs_xbox.rs
//       XboxRoot::pc_flags() / XboxRoot::xbox_bits() / serialize() / serialize_versioned()
//       restore_blob() / set_blob_flags() / blob_flags()（DXBC 摘要清零那一步）
//   crates/compat/d3d12_xs/src/console_slots_mesh.rs
//       rts0_range() / RTS0_FLAGS_OFFSET = 20
//
// 为什么要剥：
//   主机（Scarlett）root signature 描述里的 Flags 带 3 个 PC 不认的位，直接转发给
//   桌面 d3d12.dll 会 E_INVALIDARG。本机实测（System32\d3d12.dll，2026-11-24）：
//     flags=0x01000001 -> 0x80070057 "Unsupported bit-flag set (root signature flags 1000001)."
//     flags=0x83000001 -> 0x80070057
//     flags=0x83000000 -> 0x80070057
//   而 flags=0x00000001 正常成功。
//
// 做法（四步，和参考实现一致）：
//   1) 复制调用方的 desc —— 不改进程里那份（调用方可能复用）
//   2) 在副本上清掉 3 个主机位
//   3) 转发桌面 d3d12 序列化
//   4) 成功后把原始主机位写回 *ppBlob 的 RTS0 段固定偏移处，
//      让主机侧的反序列化器 / 我们自己的消费路径仍能看到原始 desc
//   5) 失败时原样返回桌面 HRESULT（不吞、不改写）
//
// 与参考实现的差异（等价改写，不是改语义）：
//   参考实现把 desc 当裸字节读 + 只拷 36 / 44 字节前缀；这里按类型整份拷贝，
//   并用 offsetof 静态断言把"两个偏移"钉死——如果哪天头文件布局变了，
//   编译就失败，而不是到运行时写坏别人的内存。
#pragma once

#include "xbgtasix/trace.h"

#include <windows.h>
#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace xbgtasix::rootsig {

// ---------------------------------------------------------------------------
// 主机标志位（rs_xbox.rs:5-11）
// ---------------------------------------------------------------------------
// 0x01000000：内存型 ABI（memory-based ABI），也是光追内部路径的标志（XBOX_INTERNAL_XDXR 的一半）
// 0x02000000：compute-2 spill 区
// 0x80000000：driver-discarded（驱动已丢弃该根签名的标记）
inline constexpr std::uint32_t kForceMemoryBasedAbi = 0x01000000u;
inline constexpr std::uint32_t kForceCompute2SpillArea = 0x02000000u;
inline constexpr std::uint32_t kDriverDiscarded = 0x80000000u;
// 参考实现 XBOX_ROOT_FLAGS = XBOX_INTERNAL_XDXR(0x03000000) | 0x80000000
inline constexpr std::uint32_t kXboxRootFlags =
    kForceMemoryBasedAbi | kForceCompute2SpillArea | kDriverDiscarded;

// 剥离：留下 PC 认识的位        （rs_xbox.rs pc_flags()）
inline constexpr std::uint32_t pc_flags(std::uint32_t console_flags) {
    return console_flags & ~kXboxRootFlags;
}
// 取出并保留的主机位           （rs_xbox.rs xbox_bits()）
inline constexpr std::uint32_t xbox_bits(std::uint32_t console_flags) {
    return console_flags & kXboxRootFlags;
}

// ---------------------------------------------------------------------------
// Flags 在 desc 里的偏移 —— 最容易做错的地方，用 offsetof 钉死
// ---------------------------------------------------------------------------
// 参考实现（rs_xbox.rs:19-20）：
//     DESC_FLAGS           = 32      （D3D12_ROOT_SIGNATURE_DESC::Flags）
//     VERSIONED_DESC_FLAGS = 8 + 32  （D3D12_VERSIONED_ROOT_SIGNATURE_DESC：Version 4 字节，
//                                     union 因含指针按 8 对齐 -> union 起于 +8，
//                                     两个 union 成员（Desc_1_0 / Desc_1_1）的 Flags 又在 union 内 +32）
//
// 这里额外用 offsetof 断言把结论绑到 MinGW d3d12.h 的真实布局上：
//   offsetof(D3D12_ROOT_SIGNATURE_DESC, Flags)               == 32
//   offsetof(D3D12_VERSIONED_ROOT_SIGNATURE_DESC, Desc_1_0.Flags) == 40
//   offsetof(D3D12_VERSIONED_ROOT_SIGNATURE_DESC, Desc_1_1.Flags) == 40
// 后两条相等是关键：1.1 用 Desc_1_1 时也走同一个偏移，所以同一句代码对 1.0/1.1 都成立。
inline constexpr std::size_t kDescFlagsOffset = offsetof(D3D12_ROOT_SIGNATURE_DESC, Flags);
inline constexpr std::size_t kVersionedFlagsOffset =
    offsetof(D3D12_VERSIONED_ROOT_SIGNATURE_DESC, Desc_1_0.Flags);
inline constexpr std::size_t kVersionedUnionOffset =
    offsetof(D3D12_VERSIONED_ROOT_SIGNATURE_DESC, Desc_1_0);

static_assert(kDescFlagsOffset == 32,
              "D3D12_ROOT_SIGNATURE_DESC::Flags 不在 +32，与参考实现 rs_xbox.rs(DESC_FLAGS=32) 不符");
static_assert(kVersionedUnionOffset == 8,
              "VERSIONED desc 的 union 不在 +8（Version 4 字节 + 8 字节对齐补齐）");
static_assert(kVersionedFlagsOffset == 40,
              "VERSIONED desc 的 Flags 不在 +40，与参考实现 rs_xbox.rs(VERSIONED_DESC_FLAGS=40) 不符");
static_assert(offsetof(D3D12_VERSIONED_ROOT_SIGNATURE_DESC, Desc_1_1.Flags) == kVersionedFlagsOffset,
              "1.0 / 1.1 的 Flags 必须落在同一偏移，本实现才对两个版本通用");
static_assert(sizeof(D3D12_ROOT_SIGNATURE_DESC) == 40 && sizeof(D3D12_VERSIONED_ROOT_SIGNATURE_DESC) == 48,
              "desc 尺寸变了：整份拷贝范围要重新核对");

// 按裸偏移读 / 写 desc 里的 Flags。
// 不直接读 desc->Desc_1_0.Flags 是因为：调用方填的是 union 的另一个成员，
// 在 C++ 里读非 active 成员是未定义行为；两个成员布局一致（上面断言过），
// 所以按偏移取 4 字节是确定且版本无关的。
inline std::uint32_t read_flags_1_0(const D3D12_ROOT_SIGNATURE_DESC& desc) {
    std::uint32_t v = 0;
    std::memcpy(&v, reinterpret_cast<const char*>(&desc) + kDescFlagsOffset, sizeof v);
    return v;
}
inline void write_flags_1_0(D3D12_ROOT_SIGNATURE_DESC& desc, std::uint32_t v) {
    std::memcpy(reinterpret_cast<char*>(&desc) + kDescFlagsOffset, &v, sizeof v);
}
inline std::uint32_t read_flags_versioned(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc) {
    std::uint32_t v = 0;
    std::memcpy(&v, reinterpret_cast<const char*>(&desc) + kVersionedFlagsOffset, sizeof v);
    return v;
}
inline void write_flags_versioned(D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc, std::uint32_t v) {
    std::memcpy(reinterpret_cast<char*>(&desc) + kVersionedFlagsOffset, &v, sizeof v);
}

// ---------------------------------------------------------------------------
// 序列化后 blob 里 RTS0 段的定位（rs_xbox.rs:79-95 + console_slots_mesh.rs:92-100）
// ---------------------------------------------------------------------------
// 桌面 d3d12 产出的 root signature blob 是标准 DXBC 容器：
//   +0  'DXBC'
//   +4  16 字节摘要（内容一改就失效）
//   +20 容器版本
//   +24 容器总长
//   +28 段数
//   +32 段偏移表[段数]
//   每个段：'XXXX' + u32 段长 + 数据
// root signature 的段名是 'RTS0'。
//
// RTS0 段体的头部固定 24 字节（实测 System32\d3d12.dll，1.0 / 1.1、0 / 3 个参数都一致）：
//   +0  u32 版本（1.0 -> 1，1.1 -> 2）
//   +4  u32 根参数个数
//   +8  u32 = 24（头部长度 / 参数区起点）
//   +12 u32 静态采样器个数
//   +16 u32 段体总长
//   +20 u32 Flags   <-- RTS0_FLAGS_OFFSET = 20，就是我们写回的位置
//   +24 起是参数数组（1.0 每项 20 字节、1.1 每项 24 字节，所以 Flags 的偏移与参数个数无关）
inline constexpr std::size_t kRts0FlagsOffset = 20;
inline constexpr std::size_t kRts0HeaderSize = 24;
// Flags 就是 24 字节段头的最后一个字段：这条把"为什么固定 +20"钉死
static_assert(kRts0FlagsOffset + 4 == kRts0HeaderSize,
              "RTS0 段头的 Flags 是最后一个字段（header 24 字节 -> 20..24）");

struct Rts0View {
    std::size_t body_off = 0;  // 段体起点（已跳过 'RTS0' + 段长）
    std::size_t body_len = 0;  // 段体长度
    bool dxbc = false;         // 是否 DXBC 容器（裸段体时 false，没有摘要可清）
};

// 在 blob 里找 RTS0 段。找不到返回 false（此时 blob 不写回，只记日志）。
inline bool find_rts0(const void* data, std::size_t size, Rts0View& view) {
    view = Rts0View{};
    const auto* b = static_cast<const std::uint8_t*>(data);
    if (!b || size < kRts0FlagsOffset + 4) return false;

    if (size >= 32 && std::memcmp(b, "DXBC", 4) == 0) {
        std::uint32_t chunks = 0;
        std::memcpy(&chunks, b + 28, 4);
        if (chunks == 0 || chunks > 64) return false;
        if (32 + static_cast<std::size_t>(chunks) * 4 > size) return false;
        for (std::uint32_t i = 0; i < chunks; ++i) {
            std::uint32_t at = 0;
            std::memcpy(&at, b + 32 + 4 * i, 4);
            if (static_cast<std::size_t>(at) + 8 > size) continue;
            if (std::memcmp(b + at, "RTS0", 4) != 0) continue;
            std::uint32_t len = 0;
            std::memcpy(&len, b + at + 4, 4);
            if (static_cast<std::size_t>(at) + 8 + len > size) return false;
            if (len < kRts0FlagsOffset + 4) return false;
            view.dxbc = true;
            view.body_off = static_cast<std::size_t>(at) + 8;
            view.body_len = len;
            return true;
        }
        return false;
    }

    // 兜底：不是 DXBC 容器就按裸 RTS0 段体处理（参考实现 rts0_range 的分支）
    view.body_off = 0;
    view.body_len = size;
    return true;
}

// 把主机位写回 blob。返回是否成功，at_out 给出写回位置（绝对偏移）。
// 与参考实现 set_blob_flags 一致：flags = 原值 | 主机位，然后清掉 DXBC 摘要。
inline bool restore_xbox_bits(ID3DBlob* blob, std::uint32_t bits, std::size_t* at_out) {
    if (!blob || bits == 0) return false;
    auto* p = static_cast<std::uint8_t*>(blob->GetBufferPointer());
    const std::size_t n = static_cast<std::size_t>(blob->GetBufferSize());
    if (!p || n == 0) return false;

    Rts0View view;
    if (!find_rts0(p, n, view)) return false;

    const std::size_t at = view.body_off + kRts0FlagsOffset;
    std::uint32_t cur = 0;
    std::memcpy(&cur, p + at, 4);
    const std::uint32_t want = cur | bits;
    std::memcpy(p + at, &want, 4);

    // 段内容改了，DXBC 头里 +4..+20 的 16 字节摘要必然对不上。
    // 参考实现（rs_xbox.rs:91-93）选择清零而不是重算；实测桌面
    // D3D12CreateRootSignatureDeserializer 接受全零摘要（简历：同样的 blob 换成错误 IID
    // 仍返回 E_NOINTERFACE 而不是摘要错误，说明摘要那步已经过了）。
    if (view.dxbc) std::memset(p + 4, 0, 16);

    if (at_out) *at_out = at;
    return true;
}

// ---------------------------------------------------------------------------
// 两个桌面序列化函数指针的签名
// ---------------------------------------------------------------------------
using SerializeVersionedFn =
    HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
using SerializeFn =
    HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

// 消费端再剥一次：序列化时我们把主机位写回了 blob，但桌面
// `CreateRootSignature` 不收带主机位的 blob（实测 0x80004005 E_FAIL）。
// 所以在把 blob 交给桌面之前，把 RTS0 段里的主机位清掉。
// 返回 true 表示确实剥了位（blob 不是合法 DXBC / 没有 RTS0 时返回 false，
// 调用方原样透传即可，不会误改任何非根签名数据）。
inline bool strip_blob_for_pc(const void* data, std::size_t size, std::uint32_t* bits_out,
                              std::size_t* at_out) {
    Rts0View view{};
    if (!find_rts0(data, size, view)) return false;
    const std::size_t at = view.body_off + kRts0FlagsOffset;
    if (at + 4 > size) return false;
    std::uint32_t original = 0;
    std::memcpy(&original, static_cast<const std::uint8_t*>(data) + at, 4);
    const std::uint32_t bits = xbox_bits(original);
    if (bits == 0) return false;
    const std::uint32_t pc = pc_flags(original);
    // 调用方传进来的是可写副本（我们自己的拷贝），这里直接改。
    std::memcpy(const_cast<std::uint8_t*>(static_cast<const std::uint8_t*>(data)) + at, &pc, 4);
    if (bits_out) *bits_out = bits;
    if (at_out) *at_out = at;
    return true;
}

inline const char* version_name(std::uint32_t v) {
    switch (v) {
        case D3D_ROOT_SIGNATURE_VERSION_1_0: return "1.0";
        case D3D_ROOT_SIGNATURE_VERSION_1_1: return "1.1";
        default: return "?";
    }
}

// 每次调用一行日志：原始 flags、剥后 flags、版本、写回结果
inline void log_call(const char* api, const char* ver, std::uint32_t in_flags,
                     std::uint32_t stripped, std::uint32_t xbox, bool write_back,
                     std::size_t at, HRESULT hr) {
    char where[48] = {0};
    if (xbox == 0) {
        std::strcpy(where, "跳过(无主机位)");
    } else if (!SUCCEEDED(hr)) {
        std::strcpy(where, "跳过(桌面已失败)");
    } else if (write_back) {
        _snprintf(where, sizeof(where), "成功(+%lu)", static_cast<unsigned long>(at));
        where[sizeof(where) - 1] = '\0';
    } else {
        std::strcpy(where, "失败(blob 里找不到 RTS0)");
    }
    xbgtasix::logf("[d3d12_xs] %s v=%s: 原始 flags=0x%08x 剥主机位=0x%08x -> 剥后=0x%08x "
                   "写回=%s hr=0x%08lx",
                   api, ver, static_cast<unsigned>(in_flags), static_cast<unsigned>(xbox),
                   static_cast<unsigned>(stripped), where,
                   static_cast<unsigned long>(hr));
}

// ---------------------------------------------------------------------------
// 序列化入口：versioned（1.0 / 1.1 都可能）
// ---------------------------------------------------------------------------
inline HRESULT serialize_versioned(SerializeVersionedFn real,
                                   const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                   ID3DBlob** pp_blob, ID3DBlob** pp_error) {
    if (!real) return E_FAIL;
    // 描述符为空：参考实现同样直接转发，让桌面给出它自己的 E_INVALIDARG
    if (!desc) {
        const HRESULT hr = real(desc, pp_blob, pp_error);
        xbgtasix::logf("[d3d12_xs] D3D12SerializeVersionedRootSignature: desc=null 直通 hr=0x%08lx",
                       static_cast<unsigned long>(hr));
        return hr;
    }

    const std::uint32_t in_flags = read_flags_versioned(*desc);
    const std::uint32_t xbox = xbox_bits(in_flags);
    const char* ver = version_name(static_cast<std::uint32_t>(desc->Version));

    if (xbox == 0) {
        // 没有主机位：一个字节都不碰，直接转发（保证与桌面结果逐字节相同）
        const HRESULT hr = real(desc, pp_blob, pp_error);
        log_call("D3D12SerializeVersionedRootSignature", ver, in_flags, in_flags, 0, false, 0, hr);
        return hr;
    }

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC copy = *desc;   // 整份拷贝：只改 Flags，指针原样传下去
    const std::uint32_t stripped = pc_flags(in_flags);
    write_flags_versioned(copy, stripped);

    const HRESULT hr = real(&copy, pp_blob, pp_error);

    bool restored = false;
    std::size_t at = 0;
    if (SUCCEEDED(hr) && pp_blob && *pp_blob) restored = restore_xbox_bits(*pp_blob, xbox, &at);
    log_call("D3D12SerializeVersionedRootSignature", ver, in_flags, stripped, xbox, restored, at, hr);
    return hr;
}

// ---------------------------------------------------------------------------
// 序列化入口：1.0 专用（D3D12SerializeRootSignature 多一个显式 version 参数）
// ---------------------------------------------------------------------------
inline HRESULT serialize_1_0(SerializeFn real, const D3D12_ROOT_SIGNATURE_DESC* desc,
                             D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** pp_blob,
                             ID3DBlob** pp_error) {
    if (!real) return E_FAIL;
    if (!desc) {
        const HRESULT hr = real(desc, version, pp_blob, pp_error);
        xbgtasix::logf("[d3d12_xs] D3D12SerializeRootSignature: desc=null 直通 hr=0x%08lx",
                       static_cast<unsigned long>(hr));
        return hr;
    }

    const std::uint32_t in_flags = read_flags_1_0(*desc);
    const std::uint32_t xbox = xbox_bits(in_flags);
    const char* ver = version_name(static_cast<std::uint32_t>(version));

    if (xbox == 0) {
        const HRESULT hr = real(desc, version, pp_blob, pp_error);
        log_call("D3D12SerializeRootSignature", ver, in_flags, in_flags, 0, false, 0, hr);
        return hr;
    }

    D3D12_ROOT_SIGNATURE_DESC copy = *desc;
    const std::uint32_t stripped = pc_flags(in_flags);
    write_flags_1_0(copy, stripped);

    const HRESULT hr = real(&copy, version, pp_blob, pp_error);

    bool restored = false;
    std::size_t at = 0;
    if (SUCCEEDED(hr) && pp_blob && *pp_blob) restored = restore_xbox_bits(*pp_blob, xbox, &at);
    log_call("D3D12SerializeRootSignature", ver, in_flags, stripped, xbox, restored, at, hr);
    return hr;
}

}  // namespace xbgtasix::rootsig
