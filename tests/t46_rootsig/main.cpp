// SPDX-License-Identifier: GPL-3.0-or-later
// t46_rootsig —— 主机 root signature 标志位剥离的独立测试
//
// 被测对象：build\d3d12_xs.dll 的两个序列化导出
//   D3D12SerializeVersionedRootSignature（1.0 / 1.1 都走这条）
//   D3D12SerializeRootSignature        （1.0 专用，原来是 forward.def 里的 PE 转发器）
//
// 要证明的四件事：
//   1) 带 3 个主机位（0x01000000 / 0x02000000 / 0x80000000）的 desc：
//      我们返回 S_OK
//   2) 同一份 desc 直接交给桌面 System32\d3d12.dll：应失败（证明剥位是必需的，
//      而不是"桌面其实也接受"）
//   3) 我们返回的 blob 里能按 RTS0 段的固定偏移重新读出那 3 个位（证明写回偏移正确）
//   4) 不带主机位的普通 desc：我们的 blob 与桌面直调的结果逐字节相同
//      （证明无主机位时一个字节都没动，且剥离路径没有副作用）
//
// 说明：本文件的 blob 解析是**独立实现**，不复用 src/d3d12_xs/root_sig.h 的任何常量，
// 否则"偏移写错"会在被测代码与测试里犯同一个错，测不出来。
//
// 输出一行 "<N> checks, <M> failures"；全过 return 0，有失败 return 1，无法测试 return 2。
#include <windows.h>
#include <d3d12.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_checks = 0;
int g_fails = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("ok   : %s\n", what);
    } else {
        ++g_fails;
        std::printf("FAIL : %s\n", what);
    }
}

void info(const char* fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    std::printf("info : %s\n", line);
}

using SerializeVersionedFn =
    HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
using SerializeFn =
    HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

// 主机标志位（与参考实现 rs_xbox.rs 一致；这里独立写一遍）
constexpr std::uint32_t kMemoryBasedAbi = 0x01000000u;
constexpr std::uint32_t kCompute2Spill = 0x02000000u;
constexpr std::uint32_t kDriverDiscarded = 0x80000000u;
constexpr std::uint32_t kXboxAll = kMemoryBasedAbi | kCompute2Spill | kDriverDiscarded;  // 0x83000000

// RTS0 段体里 Flags 的偏移（本测试独立声明，不从被测代码里取）
constexpr std::size_t kRts0FlagsAt = 20;

constexpr HRESULT kInvalidArg = static_cast<HRESULT>(0x80070057u);

const char* hr_text(HRESULT hr) {
    static thread_local char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(hr));
    return buf;
}

std::vector<std::uint8_t> take(ID3DBlob* b) {
    std::vector<std::uint8_t> out;
    if (!b) return out;
    const auto* p = static_cast<const std::uint8_t*>(b->GetBufferPointer());
    out.assign(p, p + b->GetBufferSize());
    return out;
}

// GetProcAddress 的 FARPROC -> 具体函数指针。经 void* 中转，
// 避免 -Wcast-function-type 对"不兼容函数类型直接强转"的告警。
template <typename Fn>
Fn sym(HMODULE m, const char* name) {
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

// ---- 独立的 DXBC / RTS0 解析 ---------------------------------------------
// blob 是 DXBC 容器时：+0 'DXBC'，+4 16 字节摘要，+20 容器版本，+24 总长，
// +28 段数，+32 段偏移表；每个段 'XXXX' + u32 段长 + 数据。
// 段名 'RTS0' 的段体头部 24 字节，Flags 在段体 +20。
struct Rts0 {
    bool found = false;
    std::size_t body = 0;  // 段体绝对偏移
    std::size_t len = 0;   // 段体长度
    std::uint32_t body_version = 0;
    std::uint32_t num_params = 0;
    std::uint32_t header_size = 0;
    std::uint32_t body_total = 0;
};

Rts0 parse_rts0(const std::vector<std::uint8_t>& b) {
    Rts0 r;
    if (b.size() < kRts0FlagsAt + 4) return r;
    std::size_t body = 0, len = 0;
    if (b.size() >= 32 && std::memcmp(b.data(), "DXBC", 4) == 0) {
        std::uint32_t chunks = 0;
        std::memcpy(&chunks, b.data() + 28, 4);
        if (chunks == 0 || chunks > 64) return r;
        bool hit = false;
        for (std::uint32_t i = 0; i < chunks; ++i) {
            std::uint32_t at = 0;
            std::memcpy(&at, b.data() + 32 + 4 * i, 4);
            if (static_cast<std::size_t>(at) + 8 > b.size()) continue;
            if (std::memcmp(b.data() + at, "RTS0", 4) != 0) continue;
            std::uint32_t sz = 0;
            std::memcpy(&sz, b.data() + at + 4, 4);
            if (static_cast<std::size_t>(at) + 8 + sz > b.size()) return r;
            body = static_cast<std::size_t>(at) + 8;
            len = sz;
            hit = true;
            break;
        }
        if (!hit) return r;
    } else {
        body = 0;
        len = b.size();
    }
    if (len < kRts0FlagsAt + 4) return r;
    r.found = true;
    r.body = body;
    r.len = len;
    std::memcpy(&r.body_version, b.data() + body + 0, 4);
    std::memcpy(&r.num_params, b.data() + body + 4, 4);
    std::memcpy(&r.header_size, b.data() + body + 8, 4);
    std::memcpy(&r.body_total, b.data() + body + 16, 4);
    return r;
}

std::uint32_t read_flags(const std::vector<std::uint8_t>& b) {
    const Rts0 r = parse_rts0(b);
    std::uint32_t f = 0;
    if (!r.found) return 0xdeadbeefu;
    std::memcpy(&f, b.data() + r.body + kRts0FlagsAt, 4);
    return f;
}

std::uint64_t digest_words(const std::vector<std::uint8_t>& b) {
    std::uint64_t acc = 0;
    for (std::size_t i = 4; i < 20 && i < b.size(); ++i) acc = acc * 131 + b[i];
    return acc;
}

// ---- desc 构造 -----------------------------------------------------------
// 1.0 参数：D3D12_ROOT_PARAMETER（32 字节）
std::vector<D3D12_ROOT_PARAMETER> make_params_1_0() {
    std::vector<D3D12_ROOT_PARAMETER> p(3);
    for (int i = 0; i < 3; ++i) {
        p[i] = D3D12_ROOT_PARAMETER{};
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = static_cast<UINT>(i);
    }
    return p;
}

// 1.1 参数：D3D12_ROOT_PARAMETER1
std::vector<D3D12_ROOT_PARAMETER1> make_params_1_1() {
    std::vector<D3D12_ROOT_PARAMETER1> p(3);
    for (int i = 0; i < 3; ++i) {
        p[i] = D3D12_ROOT_PARAMETER1{};
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = static_cast<UINT>(i);
        p[i].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
    }
    return p;
}

struct Serialized {
    HRESULT hr = E_FAIL;
    std::vector<std::uint8_t> bytes;
    bool has_blob = false;
};

Serialized call_versioned(SerializeVersionedFn fn, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* d) {
    Serialized s;
    ID3DBlob* b = nullptr;
    ID3DBlob* e = nullptr;
    s.hr = fn(d, &b, &e);
    if (b) {
        s.bytes = take(b);
        s.has_blob = true;
        b->Release();
    }
    if (e) {
        if (static_cast<int>(e->GetBufferSize()) > 0) {
            std::printf("       (error blob: %.*s)\n", static_cast<int>(e->GetBufferSize()),
                        static_cast<const char*>(e->GetBufferPointer()));
        }
        e->Release();
    }
    return s;
}

Serialized call_1_0(SerializeFn fn, const D3D12_ROOT_SIGNATURE_DESC* d,
                    D3D_ROOT_SIGNATURE_VERSION v) {
    Serialized s;
    ID3DBlob* b = nullptr;
    ID3DBlob* e = nullptr;
    s.hr = fn(d, v, &b, &e);
    if (b) {
        s.bytes = take(b);
        s.has_blob = true;
        b->Release();
    }
    if (e) {
        if (static_cast<int>(e->GetBufferSize()) > 0) {
            std::printf("       (error blob: %.*s)\n", static_cast<int>(e->GetBufferSize()),
                        static_cast<const char*>(e->GetBufferPointer()));
        }
        e->Release();
    }
    return s;
}

// 除"RTS0 段体 +20 的 flags 4 字节"和"DXBC 摘要 16 字节"外逐字节相同。
// 前者是我们有意写回的，后者是改了段内容后必然失效、参考实现选择清零的。
bool same_except_flags_and_digest(const std::vector<std::uint8_t>& a,
                                  const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size()) return false;
    const Rts0 ra = parse_rts0(a);
    const Rts0 rb = parse_rts0(b);
    if (!ra.found || !rb.found || ra.body != rb.body) return false;
    const std::size_t fa = ra.body + kRts0FlagsAt;
    const std::size_t fb = rb.body + kRts0FlagsAt;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i >= 4 && i < 20) continue;              // 摘要
        if ((i >= fa && i < fa + 4) || (i >= fb && i < fb + 4)) continue;  // flags
        if (a[i] != b[i]) return false;
    }
    return true;
}

void describe_blob(const char* tag, const std::vector<std::uint8_t>& b) {
    const Rts0 r = parse_rts0(b);
    if (!b.empty() && std::memcmp(b.data(), "DXBC", 4) == 0) {
        info("%s: DXBC 容器 %zu 字节，摘要 word=%016llx", tag, b.size(),
             static_cast<unsigned long long>(digest_words(b)));
    } else {
        info("%s: 非 DXBC 容器 %zu 字节", tag, b.size());
    }
    if (!r.found) {
        info("%s: 找不到 RTS0 段", tag);
        return;
    }
    info("%s: RTS0 段体 @+%zu 长 %zu；段体版本=%u 参数=%u 头长=%u 段体总长=%u flags=0x%08x",
         tag, r.body, r.len, r.body_version, r.num_params, r.header_size, r.body_total,
         read_flags(b));
}

}  // namespace

int main() {
    std::printf("== t46_rootsig: 主机 root signature 标志位剥离\n");

    // ① 我们的 shim：按模块名载入（exe 同目录优先，所以拿到的就是我们这份）。
    //    和游戏一样走"按名解析"，不依赖链接期把 DLL 拉进来。
    const HMODULE ours = LoadLibraryA("d3d12_xs.dll");
    if (!ours) {
        std::printf("SKIP: 载入 d3d12_xs.dll 失败 err=%lu（exe 与 DLL 要放同一目录）\n0 checks, 0 failures\n",
                    GetLastError());
        return 2;
    }
    char ours_path[MAX_PATH] = {0};
    GetModuleFileNameA(ours, ours_path, MAX_PATH);
    info("被测 d3d12_xs.dll = %s", ours_path);

    auto our_v = sym<SerializeVersionedFn>(ours, "D3D12SerializeVersionedRootSignature");
    auto our_10 = sym<SerializeFn>(ours, "D3D12SerializeRootSignature");

    // ② 桌面 d3d12：显式 System32 全路径，避免解析回我们自己
    char sys[MAX_PATH] = {0};
    GetSystemDirectoryA(sys, MAX_PATH);
    char d3d_path[MAX_PATH] = {0};
    std::snprintf(d3d_path, sizeof(d3d_path), "%s\\d3d12.dll", sys);
    HMODULE desktop = LoadLibraryA(d3d_path);
    if (!desktop) {
        std::printf("SKIP: 载入 %s 失败 err=%lu\n0 checks, 0 failures\n", d3d_path,
                    GetLastError());
        return 2;
    }
    auto pc_v = sym<SerializeVersionedFn>(desktop, "D3D12SerializeVersionedRootSignature");
    auto pc_10 = sym<SerializeFn>(desktop, "D3D12SerializeRootSignature");

    if (!our_v || !our_10 || !pc_v || !pc_10) {
        std::printf("SKIP: 取导出失败 our_v=%p our_10=%p pc_v=%p pc_10=%p\n0 checks, 0 failures\n",
                    reinterpret_cast<void*>(our_v), reinterpret_cast<void*>(our_10),
                    reinterpret_cast<void*>(pc_v), reinterpret_cast<void*>(pc_10));
        return 2;
    }
    info("桌面 d3d12 = %s", d3d_path);

    const auto params10 = make_params_1_0();
    const auto params11 = make_params_1_1();
    const std::uint32_t xbox_flags = kXboxAll | 0x1u;   // 3 个主机位 + IA（0x83000001）
    const std::uint32_t plain_flags = 0x1u;             // 只有桌面认识的 IA

    // ======================================================================
    // 1) versioned 1.0：带 3 个主机位
    // ======================================================================
    {
        std::printf("\n-- 1) versioned 1.0 desc，flags=0x%08x（含 3 个主机位）\n", xbox_flags);
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC d = {};
        d.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        d.Desc_1_0.NumParameters = static_cast<UINT>(params10.size());
        d.Desc_1_0.pParameters = params10.data();
        d.Desc_1_0.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(xbox_flags);

        // 1a) 桌面直调 -> 必须失败
        const Serialized pc = call_versioned(pc_v, &d);
        std::printf("       桌面直调 HRESULT = %s\n", hr_text(pc.hr));
        check(FAILED(pc.hr), "1.0/带主机位：桌面 d3d12 直调应失败（证明剥位必需）");
        check(pc.hr == kInvalidArg, "1.0/带主机位：桌面返回 E_INVALIDARG");

        // 1b) 我们的导出 -> 必须成功
        const Serialized us = call_versioned(our_v, &d);
        std::printf("       d3d12_xs   HRESULT = %s\n", hr_text(us.hr));
        check(us.hr == S_OK && us.has_blob, "1.0/带主机位：d3d12_xs 应成功且给出 blob");
        if (us.has_blob) {
            describe_blob("1.0/带主机位", us.bytes);
            check(read_flags(us.bytes) == xbox_flags,
                  "1.0/带主机位：blob 里能重新读出 0x83000001（写回正确）");
            const Rts0 r = parse_rts0(us.bytes);
            check(r.found && r.header_size == 24 && r.body_total == r.len,
                  "1.0/带主机位：RTS0 段体头长 24、段体总长自洽（偏移假设成立）");

            // 1c) 与"桌面用剥后 desc 序列化"的结果比对：只允许 flags 与摘要不同
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC stripped = d;
            stripped.Desc_1_0.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(plain_flags);
            const Serialized ref = call_versioned(pc_v, &stripped);
            check(ref.hr == S_OK && ref.has_blob, "1.0/参照：桌面序列化剥后 desc 成功");
            if (ref.has_blob) {
                describe_blob("1.0/桌面剥后参照", ref.bytes);
                check(same_except_flags_and_digest(us.bytes, ref.bytes),
                      "1.0/带主机位：除 flags 4 字节与摘要外与桌面参照逐字节相同");
            }
        }
    }

    // ======================================================================
    // 2) versioned 1.1：带 3 个主机位（union 里 Desc_1_1 的 Flags 同样在 +40）
    // ======================================================================
    {
        std::printf("\n-- 2) versioned 1.1 desc，flags=0x%08x（含 3 个主机位）\n", xbox_flags);
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC d = {};
        d.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        d.Desc_1_1.NumParameters = static_cast<UINT>(params11.size());
        d.Desc_1_1.pParameters = params11.data();
        d.Desc_1_1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(xbox_flags);

        const Serialized pc = call_versioned(pc_v, &d);
        std::printf("       桌面直调 HRESULT = %s\n", hr_text(pc.hr));
        check(FAILED(pc.hr), "1.1/带主机位：桌面 d3d12 直调应失败");
        check(pc.hr == kInvalidArg, "1.1/带主机位：桌面返回 E_INVALIDARG");

        const Serialized us = call_versioned(our_v, &d);
        std::printf("       d3d12_xs   HRESULT = %s\n", hr_text(us.hr));
        check(us.hr == S_OK && us.has_blob, "1.1/带主机位：d3d12_xs 应成功且给出 blob");
        if (us.has_blob) {
            describe_blob("1.1/带主机位", us.bytes);
            const Rts0 r = parse_rts0(us.bytes);
            info("1.1/带主机位：RTS0 段体版本=%u（1.1 序列化应为 2）", r.body_version);
            check(read_flags(us.bytes) == xbox_flags,
                  "1.1/带主机位：blob 里能重新读出 0x83000001（1.1 写回偏移同样正确）");

            D3D12_VERSIONED_ROOT_SIGNATURE_DESC stripped = d;
            stripped.Desc_1_1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(plain_flags);
            const Serialized ref = call_versioned(pc_v, &stripped);
            check(ref.hr == S_OK && ref.has_blob, "1.1/参照：桌面序列化剥后 desc 成功");
            if (ref.has_blob) {
                check(same_except_flags_and_digest(us.bytes, ref.bytes),
                      "1.1/带主机位：除 flags 4 字节与摘要外与桌面参照逐字节相同");
            }
        }
    }

    // ======================================================================
    // 3) 1.0 专用入口 D3D12SerializeRootSignature（原来是 PE 转发器）
    // ======================================================================
    {
        std::printf("\n-- 3) D3D12SerializeRootSignature(1.0)，flags=0x%08x\n", xbox_flags);
        D3D12_ROOT_SIGNATURE_DESC d = {};
        d.NumParameters = static_cast<UINT>(params10.size());
        d.pParameters = params10.data();
        d.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(xbox_flags);

        const Serialized pc = call_1_0(pc_10, &d, D3D_ROOT_SIGNATURE_VERSION_1_0);
        std::printf("       桌面直调 HRESULT = %s\n", hr_text(pc.hr));
        check(FAILED(pc.hr), "1.0 入口/带主机位：桌面直调应失败");
        check(pc.hr == kInvalidArg, "1.0 入口/带主机位：桌面返回 E_INVALIDARG");

        const Serialized us = call_1_0(our_10, &d, D3D_ROOT_SIGNATURE_VERSION_1_0);
        std::printf("       d3d12_xs   HRESULT = %s\n", hr_text(us.hr));
        check(us.hr == S_OK && us.has_blob, "1.0 入口/带主机位：d3d12_xs 应成功且给出 blob");
        if (us.has_blob) {
            describe_blob("1.0 入口/带主机位", us.bytes);
            check(read_flags(us.bytes) == xbox_flags,
                  "1.0 入口/带主机位：blob 里能重新读出 0x83000001（+32 的 Flags 有效）");

            D3D12_ROOT_SIGNATURE_DESC stripped = d;
            stripped.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(plain_flags);
            const Serialized ref = call_1_0(pc_10, &stripped, D3D_ROOT_SIGNATURE_VERSION_1_0);
            check(ref.hr == S_OK && ref.has_blob, "1.0 入口/参照：桌面序列化剥后 desc 成功");
            if (ref.has_blob) {
                check(same_except_flags_and_digest(us.bytes, ref.bytes),
                      "1.0 入口/带主机位：除 flags 4 字节与摘要外与桌面参照逐字节相同");
            }
        }
    }

    // ======================================================================
    // 4) 不带主机位的普通 desc：与桌面结果逐字节相同（一个字节都不许改）
    // ======================================================================
    {
        std::printf("\n-- 4) 普通 desc（无主机位），flags=0x%08x\n", plain_flags);
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC d = {};
        d.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        d.Desc_1_0.NumParameters = static_cast<UINT>(params10.size());
        d.Desc_1_0.pParameters = params10.data();
        d.Desc_1_0.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(plain_flags);

        const Serialized pc = call_versioned(pc_v, &d);
        const Serialized us = call_versioned(our_v, &d);
        check(pc.hr == S_OK && us.hr == S_OK, "普通 desc/1.0 versioned：两边都成功");
        check(us.has_blob && pc.has_blob && us.bytes == pc.bytes,
              "普通 desc/1.0 versioned：与桌面直调逐字节相同");
        if (us.has_blob) {
            check(digest_words(us.bytes) == digest_words(pc.bytes),
                  "普通 desc/1.0 versioned：DXBC 摘要未被改动（没走剥离路径）");
        }

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC d11 = {};
        d11.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        d11.Desc_1_1.NumParameters = static_cast<UINT>(params11.size());
        d11.Desc_1_1.pParameters = params11.data();
        d11.Desc_1_1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(plain_flags);
        const Serialized pc11 = call_versioned(pc_v, &d11);
        const Serialized us11 = call_versioned(our_v, &d11);
        check(pc11.hr == S_OK && us11.hr == S_OK, "普通 desc/1.1 versioned：两边都成功");
        check(us11.has_blob && pc11.has_blob && us11.bytes == pc11.bytes,
              "普通 desc/1.1 versioned：与桌面直调逐字节相同");

        D3D12_ROOT_SIGNATURE_DESC d10 = {};
        d10.NumParameters = static_cast<UINT>(params10.size());
        d10.pParameters = params10.data();
        d10.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(plain_flags);
        const Serialized pc10 = call_1_0(pc_10, &d10, D3D_ROOT_SIGNATURE_VERSION_1_0);
        const Serialized us10 = call_1_0(our_10, &d10, D3D_ROOT_SIGNATURE_VERSION_1_0);
        check(pc10.hr == S_OK && us10.hr == S_OK, "普通 desc/1.0 入口：两边都成功");
        check(us10.has_blob && pc10.has_blob && us10.bytes == pc10.bytes,
              "普通 desc/1.0 入口：与桌面直调逐字节相同");
    }

    // ======================================================================
    // 5) 只有主机位、flags 全剥光（剥后 flags=0，桌面也接受 NONE）
    // ======================================================================
    {
        std::printf("\n-- 5) flags=0x%08x（只有 3 个主机位，剥后为 NONE）\n", kXboxAll);
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC d = {};
        d.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        d.Desc_1_0.NumParameters = static_cast<UINT>(params10.size());
        d.Desc_1_0.pParameters = params10.data();
        d.Desc_1_0.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(kXboxAll);

        const Serialized pc = call_versioned(pc_v, &d);
        std::printf("       桌面直调 HRESULT = %s\n", hr_text(pc.hr));
        check(FAILED(pc.hr), "只有主机位：桌面直调应失败");

        const Serialized us = call_versioned(our_v, &d);
        check(us.hr == S_OK && us.has_blob, "只有主机位：d3d12_xs 应成功（剥后 flags=0）");
        if (us.has_blob) {
            describe_blob("只有主机位", us.bytes);
            check(read_flags(us.bytes) == kXboxAll,
                  "只有主机位：blob 里读回 0x83000000");
        }
    }

    // ======================================================================
    // 6) 空 desc：我们的 HRESULT 必须与桌面一致（原样转发，不许自己造错误码）
    // ======================================================================
    {
        std::printf("\n-- 6) 空 desc 的转发语义\n");
        const Serialized pc = call_versioned(pc_v, nullptr);
        const Serialized us = call_versioned(our_v, nullptr);
        std::printf("       桌面=%s d3d12_xs=%s\n", hr_text(pc.hr), hr_text(us.hr));
        check(us.hr == pc.hr, "空 desc/versioned：HRESULT 与桌面一致");

        const Serialized pc10r = call_1_0(pc_10, nullptr, D3D_ROOT_SIGNATURE_VERSION_1_0);
        const Serialized us10r =
            call_1_0(our_10, nullptr, D3D_ROOT_SIGNATURE_VERSION_1_0);
        std::printf("       桌面=%s d3d12_xs=%s\n", hr_text(pc10r.hr), hr_text(us10r.hr));
        check(us10r.hr == pc10r.hr, "空 desc/1.0 入口：HRESULT 与桌面一致");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
