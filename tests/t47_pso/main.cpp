// SPDX-License-Identifier: GPL-3.0-or-later
// t47 —— 主机 PSO 槽（55 / 60 / 61 / 62 / 66 / 67）"参数 dump 桩"验收
//
// 为什么用"直接包含 device_abi.h + 本地捕获 logf"这种方式：
//   * 这一轮的交付是**桩 + 诊断信息**，不是新导出；桩的行为（8 个参数、64 B dump、
//     形状自检、E_NOTIMPL）全部在头文件的 inline 函数里，不需要启动游戏、不需要
//     游戏数据，也不需要 d3d12_xs.dll 是最新构建（本机正在被别的改动占着）。
//   * logf 换成本地捕获器后，可以直接断言"日志里到底写了什么"，比翻日志文件可靠。
//
// 覆盖内容：
//   1) 槽位接线：55/60/61/62/66/67 用专用桩；相邻的 56/59/63/64/65 仍是通用桩。
//   2) 调用返回 E_NOTIMPL；日志含槽号、调用方 RVA（+0x...）、全部 8 个参数。
//   3) 可读指针的前 64 B 被逐字节 dump；不可读指针（含非规范地址）不崩、记为不可读。
//   4) 桌面 GRAPHICS/COMPUTE desc 形状自检：合法 desc 通过；逐字段破坏后给出**偏移**；
//      0xFF 垃圾、不可读地址被拒。
//   5) 实验入口默认关闭；打开 XBGTASIX_HOST_PSO_TRANSLATE=1 后：形状自检通过但
//      self->pc 为空 → 记"跳过"，仍返回 E_NOTIMPL（绝不把来源不明的指针写回游戏）。
//   6) 有真实 D3D12 设备时：槽 10/11 的桌面转发路径确实走到
//      CreateGraphics/ComputePipelineState（非法 desc 返回 E_INVALIDARG，**不是**
//      E_NOTIMPL）—— 即"PSO 创建路径被走到时不返回 E_NOTIMPL"。
//      没有设备（无 WARP/无适配器）则 SKIP，不算失败。
//
// 输出：<N> checks, <M> failures；成功 return 0；一条 check 都跑不了时 return 2。

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "d3d12_xs/device_abi.h"

namespace {
std::string g_log;
}  // namespace

// 把 shim 的日志接进内存，测试里直接对文本断言。
namespace xbgtasix {
void logf(const char* fmt, ...) {
    char buffer[4096];
    va_list ap;
    va_start(ap, fmt);
    const int written = std::vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    if (written > 0) {
        g_log.append(buffer, static_cast<std::size_t>(written) < sizeof(buffer)
                                  ? static_cast<std::size_t>(written)
                                  : sizeof(buffer) - 1);
    }
    g_log.push_back('\n');
}
}  // namespace xbgtasix

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("[t47] FAIL: %s\n", what);
    }
}

bool log_has(const std::string& needle) { return g_log.find(needle) != std::string::npos; }

std::string arg_token(char tag, UINT64 value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%c=0x%llx", tag,
                  static_cast<unsigned long long>(value));
    return buffer;
}

std::string hex_bytes(const void* data, std::size_t count) {
    std::string text;
    const auto* bytes = static_cast<const unsigned char*>(data);
    char item[8];
    for (std::size_t i = 0; i < count; ++i) {
        std::snprintf(item, sizeof(item), "%02x ", static_cast<unsigned>(bytes[i]));
        text += item;
    }
    return text;
}

// ---------------------------------------------------------------- 假主机对象
// 冒充 COM 对象的头 8 B（vtable 指针）：指向本测试模块里的一个函数，
// 于是 host_is_module_address() 判定"在模块内"。
void fake_com_vtable_target() {}
struct FakeComObject {
    void* vtable;
};
FakeComObject g_fake_root{reinterpret_cast<void*>(&fake_com_vtable_target)};

// 冒充着色器字节码 / 输入布局：形状自检只要求"可读、长度合理"，不解析内容。
unsigned char g_fake_shader[64] = {0x44, 0x58, 0x42, 0x43};  // "DXBC" 只是占位
D3D12_INPUT_ELEMENT_DESC g_fake_input[2] = {};

D3D12_GRAPHICS_PIPELINE_STATE_DESC make_graphics_desc() {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = reinterpret_cast<ID3D12RootSignature*>(&g_fake_root);
    desc.VS = {g_fake_shader, sizeof(g_fake_shader)};
    desc.PS = {g_fake_shader, sizeof(g_fake_shader)};
    desc.BlendState.AlphaToCoverageEnable = FALSE;
    desc.BlendState.IndependentBlendEnable = FALSE;
    for (auto& target : desc.BlendState.RenderTarget) {
        target.BlendEnable = FALSE;
        target.LogicOpEnable = FALSE;
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_ZERO;
        target.BlendOp = D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_ZERO;
        target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.MultisampleEnable = FALSE;
    desc.RasterizerState.AntialiasedLineEnable = FALSE;
    desc.RasterizerState.ForcedSampleCount = 0;
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    desc.DepthStencilState.StencilEnable = FALSE;
    for (auto* face : {&desc.DepthStencilState.FrontFace, &desc.DepthStencilState.BackFace}) {
        face->StencilFailOp = D3D12_STENCIL_OP_KEEP;
        face->StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        face->StencilPassOp = D3D12_STENCIL_OP_KEEP;
        face->StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    }
    desc.InputLayout = {g_fake_input, 0};
    desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.NodeMask = 1;
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    return desc;
}

D3D12_COMPUTE_PIPELINE_STATE_DESC make_compute_desc() {
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = reinterpret_cast<ID3D12RootSignature*>(&g_fake_root);
    desc.CS = {g_fake_shader, sizeof(g_fake_shader)};
    desc.NodeMask = 1;
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    return desc;
}

// ---------------------------------------------------------------- 槽位调用
using detail_t = xbgtasix::d3d12::detail;

using PsoSlotFn = HRESULT(WINAPI*)(detail_t::DeviceProxy*, UINT64, UINT64, UINT64, UINT64, UINT64,
                                   UINT64, UINT64, UINT64);

template <std::size_t N>
PsoSlotFn pso_slot() {
    return reinterpret_cast<PsoSlotFn>(detail_t::device_slot<N>());
}

template <std::size_t N>
bool is_dedicated_stub() {
    return detail_t::device_slot<N>() !=
           reinterpret_cast<detail_t::Slot>(&detail_t::device_extension_stub_n<N>);
}

template <std::size_t N>
bool is_generic_stub() {
    return detail_t::device_slot<N>() ==
           reinterpret_cast<detail_t::Slot>(&detail_t::device_extension_stub_n<N>);
}

// ---------------------------------------------------------------- 1) 接线
void check_slot_wiring() {
    check(is_dedicated_stub<55>(), "slot 55 装上 PSO 专用桩");
    check(is_dedicated_stub<60>(), "slot 60 装上 PSO 专用桩");
    check(is_dedicated_stub<61>(), "slot 61 装上 PSO 专用桩");
    check(is_dedicated_stub<62>(), "slot 62 装上 PSO 专用桩");
    check(is_dedicated_stub<66>(), "slot 66 装上 PSO 专用桩");
    check(is_dedicated_stub<67>(), "slot 67 装上 PSO 专用桩");
    // 不误伤邻居：56/59/63/64/65 仍是通用"未建模槽"桩
    check(is_generic_stub<56>(), "slot 56 仍是通用桩");
    check(is_generic_stub<59>(), "slot 59 仍是通用桩");
    check(is_generic_stub<63>(), "slot 63 仍是通用桩");
    check(is_generic_stub<64>(), "slot 64 仍是通用桩");
    check(is_generic_stub<65>(), "slot 65 仍是通用桩");
}

// ---------------------------------------------------------------- 2/3) dump
void check_graphics_dump(detail_t::DeviceProxy& proxy) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = make_graphics_desc();
    const UINT64 args[8] = {
        reinterpret_cast<UINT64>(&desc),
        0x2222333344445555ull,   // 非规范地址：不可读
        0x6666777788889999ull,   // 非规范地址：不可读
        0x1234,                  // 小整数：不是指针，不该尝试读
        0xAAAAAAAAAAAAAAAAull,   // 非规范地址
        0xBBBBBBBBBBBBBBBBull,   // 非规范地址
        0xCCCCull,
        0xDDDDull,
    };
    g_log.clear();
    const HRESULT hr = pso_slot<60>()(&proxy, args[0], args[1], args[2], args[3], args[4], args[5],
                                      args[6], args[7]);

    check(hr == E_NOTIMPL, "slot 60 的桩返回 E_NOTIMPL（不伪造成功）");
    check(log_has("主机 PSO 槽 slot=60"), "日志含槽号 slot=60");
    check(log_has("graphicsPSO(参考)"), "日志含该槽的可读标签（并标注来自参考实现）");
    check(log_has("调用方=+0x"), "日志含调用方模块内 RVA");
    check(log_has("-> E_NOTIMPL"), "日志明确写出返回 E_NOTIMPL");

    bool all_args = true;
    for (int i = 0; i < 8; ++i) all_args = all_args && log_has(arg_token(static_cast<char>('a' + i), args[i]));
    check(all_args, "日志含全部 8 个参数（a..h）");

    // a 指向 desc：前 64 B 必须逐字节 dump 出来（4 行 x 16 B）
    const auto* bytes = reinterpret_cast<const unsigned char*>(&desc);
    bool dumped = true;
    for (std::size_t off = 0; off < 64; off += 16) {
        char prefix[64];
        std::snprintf(prefix, sizeof(prefix), "slot=60 a+0x%02zx: ", off);
        dumped = dumped && log_has(std::string(prefix) + hex_bytes(bytes + off, 16));
    }
    check(dumped, "slot 60 把参数 a 指向的 desc 前 64 B 完整 dump（4x16）");
    check(log_has("区域基址=") && log_has("可读=0x40 B"), "dump 附带区域信息与可读字节数");
    check(log_has("slot=60 b=0x2222333344445555 不可读"), "不可读指针记为'不可读'并跳过 dump");
    check(log_has("slot=60 e=0xaaaaaaaaaaaaaaaa 不可读"), "非规范地址不会崩，只记'不可读'");
    check(log_has("形状自检=通过"), "slot 60 对指向桌面 desc 的参数判'形状自检通过'");
}

// ---------------------------------------------------------------- 4) 形状自检
void check_shape_validator() {
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC good = make_graphics_desc();
    check(detail_t::check_graphics_pso_shape(&good).shape == detail_t::PsoShape::kPlausible,
          "合法桌面图形 desc 形状自检通过");

    auto expect_bad = [](const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                         detail_t::PsoShape want, std::size_t want_offset, const char* what) {
        const auto result = detail_t::check_graphics_pso_shape(&desc);
        check(result.shape == want, what);
        check(result.offset == want_offset, (std::string(what) + "（偏移定位正确）").c_str());
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC bad = good;
    bad.NumRenderTargets = 9;
    expect_bad(bad, detail_t::PsoShape::kBadRenderTargets,
               offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, NumRenderTargets),
               "NumRenderTargets=9 被判非法");

    bad = good;
    bad.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(9);
    expect_bad(bad, detail_t::PsoShape::kBadTopology,
               offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, PrimitiveTopologyType),
               "PrimitiveTopologyType=9 被判非法");

    bad = good;
    bad.IBStripCutValue = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(7);
    expect_bad(bad, detail_t::PsoShape::kBadStripCut,
               offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, IBStripCutValue),
               "IBStripCutValue=7 被判非法");

    bad = good;
    bad.SampleDesc.Count = 3;
    expect_bad(bad, detail_t::PsoShape::kBadSample,
               offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, SampleDesc) +
                   offsetof(DXGI_SAMPLE_DESC, Count),
               "SampleDesc.Count=3 被判非法");

    bad = good;
    bad.NodeMask = 0x10;
    expect_bad(bad, detail_t::PsoShape::kBadNodeMask,
               offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, NodeMask),
               "NodeMask=0x10 被判非法（单节点主机）");

    bad = good;
    bad.Flags = static_cast<D3D12_PIPELINE_STATE_FLAGS>(0x80);
    expect_bad(bad, detail_t::PsoShape::kBadFlags,
               offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, Flags), "Flags=0x80 被判非法");

    bad = good;
    bad.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    expect_bad(bad, detail_t::PsoShape::kBadRtvFormat,
               offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, RTVFormats),
               "有 RTV 但格式为 UNKNOWN 被判非法");

    bad = good;
    bad.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(0);
    expect_bad(bad, detail_t::PsoShape::kBadDepthStencil,
               offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, DepthStencilState) +
                   offsetof(D3D12_DEPTH_STENCIL_DESC, DepthFunc),
               "DepthFunc=0 被判非法");

    bad = good;
    bad.RasterizerState.FillMode = static_cast<D3D12_FILL_MODE>(0);
    expect_bad(bad, detail_t::PsoShape::kBadRasterizer,
               offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, RasterizerState) +
                   offsetof(D3D12_RASTERIZER_DESC, FillMode),
               "FillMode=0 被判非法");

    bad = good;
    bad.BlendState.RenderTarget[0].SrcBlend = static_cast<D3D12_BLEND>(0);
    expect_bad(bad, detail_t::PsoShape::kBadBlend,
               offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, BlendState) +
                   offsetof(D3D12_BLEND_DESC, RenderTarget) +
                   offsetof(D3D12_RENDER_TARGET_BLEND_DESC, SrcBlend),
               "SrcBlend=0 被判非法");

    bad = good;
    bad.VS.BytecodeLength = 64u << 20;  // 超过 16 MB 上界
    expect_bad(bad, detail_t::PsoShape::kBadShaderBytecode,
               offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, VS) +
                   offsetof(D3D12_SHADER_BYTECODE, BytecodeLength),
               "超大着色器长度被判非法");

    bad = good;
    unsigned char not_com[8] = {0};
    bad.pRootSignature = reinterpret_cast<ID3D12RootSignature*>(not_com);
    expect_bad(bad, detail_t::PsoShape::kBadRootSignature,
               offsetof(D3D12_GRAPHICS_PIPELINE_STATE_DESC, pRootSignature),
               "根签名首 8 B 不在任何模块内 → 判非法");

    // 0xFF 填充的"垃圾 desc"必须被拒（任何一项检查都行）
    D3D12_GRAPHICS_PIPELINE_STATE_DESC garbage;
    std::memset(&garbage, 0xFF, sizeof(garbage));
    check(detail_t::check_graphics_pso_shape(&garbage).shape != detail_t::PsoShape::kPlausible,
          "0xFF 填充的垃圾内存不会通过形状自检");

    // 不可读地址：不崩，返回 kNotReadable
    check(detail_t::check_graphics_pso_shape(reinterpret_cast<const void*>(0x10)).shape ==
              detail_t::PsoShape::kNotReadable,
          "不可读地址返回 kNotReadable（不解引用）");
    check(detail_t::check_graphics_pso_shape(nullptr).shape ==
              detail_t::PsoShape::kNotReadable,
          "空指针返回 kNotReadable");

    // 计算 PSO desc
    const D3D12_COMPUTE_PIPELINE_STATE_DESC good_compute = make_compute_desc();
    check(detail_t::check_compute_pso_shape(&good_compute).shape == detail_t::PsoShape::kPlausible,
          "合法桌面计算 desc 形状自检通过");
    D3D12_COMPUTE_PIPELINE_STATE_DESC bad_compute = good_compute;
    bad_compute.NodeMask = 2;
    check(detail_t::check_compute_pso_shape(&bad_compute).shape ==
              detail_t::PsoShape::kBadNodeMask,
          "计算 desc NodeMask=2 被判非法");
    bad_compute = good_compute;
    bad_compute.CS.BytecodeLength = 0;
    check(detail_t::check_compute_pso_shape(&bad_compute).shape ==
              detail_t::PsoShape::kBadShaderBytecode,
          "计算 desc 空字节码被判非法");
}

// ---------------------------------------------------------------- 5) 实验开关
void check_translate_gate(detail_t::DeviceProxy& proxy) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = make_graphics_desc();

    SetEnvironmentVariableA("XBGTASIX_HOST_PSO_TRANSLATE", nullptr);
    g_log.clear();
    check(pso_slot<60>()(&proxy, reinterpret_cast<UINT64>(&desc), 0, 0, 0, 0, 0, 0, 0) == E_NOTIMPL,
          "实验开关关闭时 slot 60 返回 E_NOTIMPL");
    check(!log_has("实验直通"), "实验开关关闭时**不**尝试调用驱动");

    SetEnvironmentVariableA("XBGTASIX_HOST_PSO_TRANSLATE", "1");
    g_log.clear();
    const HRESULT hr =
        pso_slot<60>()(&proxy, reinterpret_cast<UINT64>(&desc), 0, 0, 0, 0, 0, 0, 0);
    check(hr == E_NOTIMPL, "实验开关打开后桩仍返回 E_NOTIMPL（out 参数位置未知，不写回）");
    check(log_has("实验直通跳过"), "self->pc 为空时实验入口记'跳过'而不是崩");
    SetEnvironmentVariableA("XBGTASIX_HOST_PSO_TRANSLATE", nullptr);
}

// ---------------------------------------------------------------- 其余 PSO 槽
void check_other_pso_slots(detail_t::DeviceProxy& proxy) {
    struct Case {
        std::size_t slot;
        int (*call)(detail_t::DeviceProxy&);
        const char* label;
        const char* expect_text;
    };
    const Case cases[] = {
        {55, [](detail_t::DeviceProxy& p) { return static_cast<int>(pso_slot<55>()(&p, 1, 2, 3, 4, 5, 6, 7, 8)); },
         "slot 55", "slot=55（派生PSO"},
        {61, [](detail_t::DeviceProxy& p) { return static_cast<int>(pso_slot<61>()(&p, 1, 2, 3, 4, 5, 6, 7, 8)); },
         "slot 61", "slot=61（samplerX"},
        {62, [](detail_t::DeviceProxy& p) { return static_cast<int>(pso_slot<62>()(&p, 1, 2, 3, 4, 5, 6, 7, 8)); },
         "slot 62", "slot=62（computePSO"},
        {66, [](detail_t::DeviceProxy& p) { return static_cast<int>(pso_slot<66>()(&p, 1, 2, 3, 4, 5, 6, 7, 8)); },
         "slot 66", "slot=66（PSO序列化"},
        {67, [](detail_t::DeviceProxy& p) { return static_cast<int>(pso_slot<67>()(&p, 1, 2, 3, 4, 5, 6, 7, 8)); },
         "slot 67", "slot=67（PSO反序列化"},
    };
    for (const auto& item : cases) {
        g_log.clear();
        const HRESULT hr = static_cast<HRESULT>(item.call(proxy));
        check(hr == E_NOTIMPL, (std::string(item.label) + " 返回 E_NOTIMPL").c_str());
        check(log_has(item.expect_text),
              (std::string(item.label) + " 的日志含槽号与标签").c_str());
    }

    // 计算 PSO 槽也要对 desc 参数做形状自检
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute = make_compute_desc();
    g_log.clear();
    pso_slot<62>()(&proxy, reinterpret_cast<UINT64>(&compute), 0, 0, 0, 0, 0, 0, 0);
    check(log_has("slot=62 a=0x") && log_has("COMPUTE_PIPELINE_STATE_DESC"),
          "slot 62 对计算 desc 参数给出形状自检结论");
}

// ---------------------------------------------------------------- 6) 桌面转发
ID3D12Device* create_test_device() {
    ID3D12Device* device = nullptr;
    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                    reinterpret_cast<void**>(&device))) &&
        device) {
        return device;
    }
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory))) ||
        !factory) {
        return nullptr;
    }
    IDXGIAdapter* adapter = nullptr;
    const HRESULT warp = factory->EnumWarpAdapter(__uuidof(IDXGIAdapter),
                                                  reinterpret_cast<void**>(&adapter));
    factory->Release();
    if (FAILED(warp) || !adapter) return nullptr;
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                 reinterpret_cast<void**>(&device)))) {
        device = nullptr;
    }
    adapter->Release();
    return device;
}

void check_forwarded_pso_path() {
    ID3D12Device* device = create_test_device();
    if (!device) {
        std::printf("[t47] SKIP: 没有可用 D3D12 设备（无适配器且 WARP 不可用），跳过槽 10/11 转发验收\n");
        return;
    }
    detail_t::DeviceProxy native{};
    native.pc = reinterpret_cast<IUnknown*>(device);
    native.version = 11;

    using ForwardFn = uintptr_t(WINAPI*)(void*, uintptr_t, uintptr_t, uintptr_t);
    const auto forward_graphics = reinterpret_cast<ForwardFn>(detail_t::device_slot<10>());
    const auto forward_compute = reinterpret_cast<ForwardFn>(detail_t::device_slot<11>());

    // 全零 desc：根签名为空、枚举非法 —— 桌面一定拒绝，但**不是** E_NOTIMPL。
    // 这一步证明"同号转发路径确实走到了真正的 CreateGraphics/ComputePipelineState"。
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
    void* out = nullptr;
    const GUID graphics_iid = __uuidof(ID3D12PipelineState);
    const HRESULT hr_graphics = static_cast<HRESULT>(forward_graphics(
        &native, reinterpret_cast<uintptr_t>(&graphics),
        reinterpret_cast<uintptr_t>(&graphics_iid), reinterpret_cast<uintptr_t>(&out)));
    std::printf("[t47] slot 10（桌面 CreateGraphicsPipelineState）非法 desc -> hr=0x%08lx\n",
                static_cast<unsigned long>(hr_graphics));
    check(FAILED(hr_graphics), "槽 10 非法 desc 返回失败");
    check(hr_graphics != E_NOTIMPL, "槽 10 PSO 创建路径不是桩（未返回 E_NOTIMPL）");
    if (out) {
        static_cast<IUnknown*>(out)->Release();
        out = nullptr;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    const HRESULT hr_compute = static_cast<HRESULT>(forward_compute(
        &native, reinterpret_cast<uintptr_t>(&compute),
        reinterpret_cast<uintptr_t>(&graphics_iid), reinterpret_cast<uintptr_t>(&out)));
    std::printf("[t47] slot 11（桌面 CreateComputePipelineState）非法 desc -> hr=0x%08lx\n",
                static_cast<unsigned long>(hr_compute));
    check(FAILED(hr_compute), "槽 11 非法 desc 返回失败");
    check(hr_compute != E_NOTIMPL, "槽 11 计算 PSO 创建路径不是桩（未返回 E_NOTIMPL）");
    if (out) static_cast<IUnknown*>(out)->Release();

    device->Release();
}

}  // namespace

int main() {
    std::printf("[t47] 主机 PSO 槽参数 dump 桩验收（不启动游戏、不使用游戏数据）\n");

    detail_t::DeviceProxy proxy{};  // pc 为空：所有"真实创建"路径都会被安全跳过

    check_slot_wiring();
    check_graphics_dump(proxy);
    check_shape_validator();
    check_translate_gate(proxy);
    check_other_pso_slots(proxy);
    check_forwarded_pso_path();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_checks == 0) return 2;  // 一条都没跑成 = 无法测试
    return g_failures == 0 ? 0 : 1;
}
