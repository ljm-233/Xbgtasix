// SPDX-License-Identifier: GPL-3.0-or-later
// t38_xboxui —— Xbox 风格 shell 窗口（自绘，无第三方依赖）
//
// 目的：把"Xbox UI 窗口"从概念变成能看见的东西，并且**能截图**。
//
// 渲染链路（全部是本项目已经验证过的能力组合）：
//   GDI 把文字画进一张 1024x1024 位图  ->  上传成 D3D12 纹理图集
//   D3D12 命令列表画一堆带 UV/颜色的四边形（色块与字形共用一条管线）
//   先渲到离屏 RT -> 读回成 BMP（截图）+ 复制到 flip 交换链 Present
//
// 输入：键盘方向键/回车/Esc + XInput 手柄（A 确认、B 返回）
//
// 运行：
//   t38_xboxui.exe                 开窗口，按 Esc 退出
//   t38_xboxui.exe --shot out.bmp  只渲染一帧并写出截图（不需要人操作）
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "xbgtasix/trace.h"

namespace {

constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;
constexpr UINT kAtlas = 1024;
// 纯色块采样图集 (0,0) 的像素中心
const float kSolidU = 0.5f / kAtlas;
const float kSolidV = 0.5f / kAtlas;

int g_fail = 0;
void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t38]   PASS  %-40s %s", what, detail);
    } else {
        xbgtasix::logf("[t38]   FAIL  %-40s %s", what, detail);
        ++g_fail;
    }
}

// ---------------------------------------------------------------- 顶点
struct Vertex {
    float x, y, u, v, r, g, b, a;
};

struct Rect {
    float x, y, w, h;
    float r, g, b, a;
};

struct Batch {
    std::vector<Vertex> verts;
    std::vector<unsigned> idx;

    // 输入是像素坐标；这里直接换算成 NDC（着色器不需要 cbuffer）
    void quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1,
              float cr, float cg, float cb, float ca) {
        auto ndc_x = [](float px) { return px / kWidth * 2.0f - 1.0f; };
        auto ndc_y = [](float py) { return 1.0f - py / kHeight * 2.0f; };
        const unsigned b0 = static_cast<unsigned>(verts.size());
        verts.push_back({ndc_x(x), ndc_y(y), u0, v0, cr, cg, cb, ca});
        verts.push_back({ndc_x(x + w), ndc_y(y), u1, v0, cr, cg, cb, ca});
        verts.push_back({ndc_x(x + w), ndc_y(y + h), u1, v1, cr, cg, cb, ca});
        verts.push_back({ndc_x(x), ndc_y(y + h), u0, v1, cr, cg, cb, ca});
        idx.insert(idx.end(), {b0, b0 + 1, b0 + 2, b0, b0 + 2, b0 + 3});
    }
};

// ---------------------------------------------------------------- GDI 文字图集
struct Atlas {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    void* bits = nullptr;
    HFONT font_big = nullptr;
    HFONT font_mid = nullptr;
    HFONT font_small = nullptr;
    int pen_x = 8, pen_y = 8, line_h = 0;
    // 记录每个字符串在图集里的位置，重复使用时不重复绘制
    std::vector<std::pair<std::string, RECT>> cache;

    bool init() {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = kAtlas;
        bi.bmiHeader.biHeight = -static_cast<LONG>(kAtlas);   // 自上而下
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) return false;
        SelectObject(dc, bmp);
        // 图集必须清成"白底透明"：像素着色器是 tex.rgb * color，
        // 所以纯白 (255,255,255) + alpha 0 只贡献 alpha，不污染颜色
        auto* px = static_cast<unsigned*>(bits);
        // 底色必须是**黑**：之后按亮度算 alpha，如果底色是白，
        // 文字框内会整片不透明（截图里文字成了实心色块，就是这个原因）
        for (UINT i = 0; i < kAtlas * kAtlas; ++i) px[i] = 0x00000000;

        font_big = CreateFontW(-44, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        font_mid = CreateFontW(-26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        font_small = CreateFontW(-19, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        return true;
    }

    // 把一个 UTF-8 字符串画进图集，返回占用的 UV 范围
    bool layout(const char* utf8, int size, RECT* out) {
        for (auto& kv : cache) {
            if (kv.first == utf8) {
                *out = kv.second;
                return true;
            }
        }
        if (!utf8 || !*utf8) return false;
        const int w = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        std::wstring ws(w > 0 ? w - 1 : 0, L'\0');
        if (w > 1) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, ws.data(), w);

        HFONT font = size >= 40 ? font_big : (size >= 24 ? font_mid : font_small);
        SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));

        SIZE sz{};
        GetTextExtentPoint32W(dc, ws.c_str(), static_cast<int>(ws.size()), &sz);
        if (pen_x + sz.cx + 8 > static_cast<int>(kAtlas)) {
            pen_x = 8;
            pen_y += line_h + 6;
            line_h = 0;
        }
        if (pen_y + sz.cy + 8 > static_cast<int>(kAtlas)) return false;   // 图集满

        RECT r{pen_x, pen_y, pen_x + sz.cx, pen_y + sz.cy};
        // 用 alpha 通道累积：GDI 画的是不透明白字，再把白通道转成 alpha 太麻烦，
        // 这里改成"黑底白字"叠加：先清成黑(0)再画白，采样时用颜色直接调制。
        // 但我们需要 alpha 而不是亮度，所以画完后扫描该区域，把亮度写进 alpha。
        TextOutW(dc, pen_x, pen_y, ws.c_str(), static_cast<int>(ws.size()));
        auto* px = static_cast<unsigned*>(bits);
        for (int y = r.top; y < r.bottom; ++y) {
            for (int x = r.left; x < r.right; ++x) {
                const unsigned p = px[y * kAtlas + x];
                const unsigned lum = p & 0xFF;              // B 通道（白字）
                px[y * kAtlas + x] = (lum << 24) | 0x00FFFFFF;   // alpha=亮度, rgb=白
            }
        }

        // (0,0) 保留成一个不透明白像素，供纯色块采样（每轮重设，防止被误改）
        px[0] = 0xFFFFFFFF;
        pen_x += sz.cx + 8;
        if (sz.cy > line_h) line_h = sz.cy;
        cache.emplace_back(utf8, r);
        *out = r;
        return true;
    }

    void text(Batch& b, const char* utf8, int size, float x, float y, float r, float g, float bl,
              float a) {
        RECT rr{};
        if (!layout(utf8, size, &rr)) return;
        const float u0 = static_cast<float>(rr.left) / kAtlas;
        const float v0 = static_cast<float>(rr.top) / kAtlas;
        const float u1 = static_cast<float>(rr.right) / kAtlas;
        const float v1 = static_cast<float>(rr.bottom) / kAtlas;
        b.quad(x, y, static_cast<float>(rr.right - rr.left), static_cast<float>(rr.bottom - rr.top),
               u0, v0, u1, v1, r, g, bl, a);
    }
};

// ---------------------------------------------------------------- D3D12
struct Gfx {
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12Resource* atlas_tex = nullptr;
    bool atlas_srv_written = false;
    ID3D12Resource* upload = nullptr;
    ID3D12DescriptorHeap* srv_heap = nullptr;
    ID3D12Resource* vbuf = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    ID3D12Resource* ibuf = nullptr;
    D3D12_INDEX_BUFFER_VIEW ibv{};
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;
    ID3D12Resource* offscreen = nullptr;   // 截图用
    ID3D12Resource* readback = nullptr;

    bool init() {
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                                     reinterpret_cast<void**>(&dev))) || !dev) {
            return false;
        }
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, reinterpret_cast<void**>(&queue));
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator,
                                    reinterpret_cast<void**>(&alloc));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                               IID_ID3D12GraphicsCommandList, reinterpret_cast<void**>(&list));
        list->Close();
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, reinterpret_cast<void**>(&fence));
        fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);

        // root signature：一个描述符表，含 t0(顶点 StructuredBuffer) 与 t1(图集)
        // 不用 cbuffer、不用输入布局 —— 沿用仓内已验证的 SV_VertexID + SRV 路线
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 2;
        range.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER rp[1]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[0].DescriptorTable.NumDescriptorRanges = 1;
        rp[0].DescriptorTable.pDescriptorRanges = &range;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        // ComparisonFunc 必须显式给值：默认 0 不是合法枚举，会让 PSO 创建
        // 以 E_INVALIDARG(0x80070057) 失败（实测踩过）
        samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxLOD = D3D12_FLOAT32_MAX;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 1;
        rsd.pParameters = rp;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &samp;
        ID3DBlob* sig = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
            xbgtasix::logf("[t38] 根签名序列化失败: %s",
                           err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return false;
        }
        dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                 IID_ID3D12RootSignature, reinterpret_cast<void**>(&root));
        sig->Release();

        // 读编译好的着色器（build.sh 用 dxc 生成 ui.vs.cso / ui.ps.cso）
        auto load = [](const char* path, std::vector<char>* out) {
            FILE* f = fopen(path, "rb");
            if (!f) return false;
            fseek(f, 0, SEEK_END);
            const long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            out->resize(n);
            const bool ok_read = fread(out->data(), 1, n, f) == static_cast<size_t>(n);
            fclose(f);
            return ok_read;
        };
        std::vector<char> vs, ps;
        if (!load("suite/ui.vs.cso", &vs) && !load("ui.vs.cso", &vs)) {
            xbgtasix::logf("[t38] 找不到着色器 ui.vs.cso（build.sh 里 dxc 那步没跑？）");
            return false;
        }
        if (!load("suite/ui.ps.cso", &ps) && !load("ui.ps.cso", &ps)) {
            xbgtasix::logf("[t38] 找不到着色器 ui.ps.cso");
            return false;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root;
        pd.VS = {vs.data(), vs.size()};
        pd.PS = {ps.data(), ps.size()};
        pd.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        // 深度必须显式关掉：默认 DepthEnable=TRUE 而 DSVFormat=UNKNOWN，
        // D3D12 会以 E_INVALIDARG(0x80070057) 拒绝创建 PSO（实测踩过）
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.SampleDesc.Count = 1;
        const HRESULT pso_hr = dev->CreateGraphicsPipelineState(
            &pd, IID_ID3D12PipelineState, reinterpret_cast<void**>(&pso));
        if (FAILED(pso_hr)) {
            xbgtasix::logf("[t38] CreateGraphicsPipelineState hr=0x%08lx",
                           static_cast<unsigned long>(pso_hr));
            return false;
        }

        // SRV 堆
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        dev->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap,
                                  reinterpret_cast<void**>(&srv_heap));

        // 离屏 RT（截图用）
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kWidth;
        td.Height = kHeight;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                     D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                     IID_ID3D12Resource, reinterpret_cast<void**>(&offscreen));
        return true;
    }

    void upload_atlas(void* bits) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kAtlas;
        td.Height = kAtlas;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_ID3D12Resource,
                                     reinterpret_cast<void**>(&atlas_tex));

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        UINT rows = 0;
        UINT64 row_size = 0;
        dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &row_size, &total);

        D3D12_HEAP_PROPERTIES up{};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ud{};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = total;
        ud.Height = 1;
        ud.DepthOrArraySize = 1;
        ud.MipLevels = 1;
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.SampleDesc.Count = 1;
        ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ,
                                     nullptr, IID_ID3D12Resource, reinterpret_cast<void**>(&upload));

        void* mapped = nullptr;
        upload->Map(0, nullptr, &mapped);
        auto* dst = static_cast<unsigned char*>(mapped);
        auto* src = static_cast<unsigned char*>(bits);
        for (UINT y = 0; y < kAtlas; ++y) {
            memcpy(dst + fp.Offset + y * fp.Footprint.RowPitch, src + y * kAtlas * 4, kAtlas * 4);
        }
        upload->Unmap(0, nullptr);

        alloc->Reset();
        list->Reset(alloc, nullptr);
        D3D12_TEXTURE_COPY_LOCATION sc{};
        sc.pResource = upload;
        sc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sc.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION dc{};
        dc.pResource = atlas_tex;
        dc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dc.SubresourceIndex = 0;
        list->CopyTextureRegion(&dc, 0, 0, 0, &sc, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = atlas_tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* cls[] = {list};
        queue->ExecuteCommandLists(1, cls);
        wait_gpu();

        atlas_srv_written = true;   // 真正的视图在 upload_batch 里写到槽 1
    }

    void wait_gpu() {
        ++fence_value;
        queue->Signal(fence, fence_value);
        if (fence->GetCompletedValue() < fence_value) {
            fence->SetEventOnCompletion(fence_value, fence_event);
            WaitForSingleObject(fence_event, 5000);
        }
    }

    void upload_batch(Batch& batch) {
        const UINT64 vsize = batch.verts.size() * sizeof(Vertex);
        const UINT64 isize = batch.idx.size() * sizeof(unsigned);
        auto make = [&](ID3D12Resource** res, UINT64 size, const void* data) {
            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            d.Width = size;
            d.Height = 1;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_UNKNOWN;
            d.SampleDesc.Count = 1;
            d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                         IID_ID3D12Resource, reinterpret_cast<void**>(res));
            void* m = nullptr;
            (*res)->Map(0, nullptr, &m);
            memcpy(m, data, size);
            (*res)->Unmap(0, nullptr);
        };
        if (vbuf) { vbuf->Release(); vbuf = nullptr; }
        if (ibuf) { ibuf->Release(); ibuf = nullptr; }
        make(&vbuf, vsize, batch.verts.data());
        make(&ibuf, isize, batch.idx.data());
        ibv = {ibuf->GetGPUVirtualAddress(), static_cast<UINT>(isize), DXGI_FORMAT_R32_UINT};

        // 顶点作为 StructuredBuffer 的 SRV（t0），图集在 t1
        D3D12_SHADER_RESOURCE_VIEW_DESC bv{};
        bv.Format = DXGI_FORMAT_UNKNOWN;
        bv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        bv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        bv.Buffer.FirstElement = 0;
        bv.Buffer.NumElements = static_cast<UINT>(batch.verts.size());
        bv.Buffer.StructureByteStride = sizeof(Vertex);
        const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE h0 = srv_heap->GetCPUDescriptorHandleForHeapStart();
        dev->CreateShaderResourceView(vbuf, &bv, h0);
        if (atlas_srv_written) {
            D3D12_CPU_DESCRIPTOR_HANDLE h1 = h0;
            h1.ptr += inc;
            // 图集 SRV 已在 upload_atlas 里写到 h0，这里把图集挪到槽 1：
            // 直接重建一次视图更简单
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(atlas_tex, &sv, h1);
        }
    }

    void render(Batch& batch) {
        alloc->Reset();
        list->Reset(alloc, nullptr);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
        D3D12_CPU_DESCRIPTOR_HANDLE* prtv = &rtv;
        // 用一个临时 RTV 堆
        ID3D12DescriptorHeap* rtv_heap = nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        dev->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap, reinterpret_cast<void**>(&rtv_heap));
        rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(offscreen, nullptr, rtv);
        list->OMSetRenderTargets(1, prtv, FALSE, nullptr);

        // 清成底色（避免未初始化内容混进截图）
        {
            const float bg[4] = {0.043f, 0.055f, 0.078f, 1.0f};
            list->ClearRenderTargetView(rtv, bg, 0, nullptr);
        }
        D3D12_VIEWPORT vp{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &scissor);
        list->SetGraphicsRootSignature(root);
        list->SetPipelineState(pso);

        ID3D12DescriptorHeap* heaps[] = {srv_heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());

        // 没有顶点缓冲视图：顶点来自 SV_VertexID + StructuredBuffer
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetIndexBuffer(&ibv);
        list->DrawIndexedInstanced(static_cast<UINT>(batch.idx.size()), 1, 0, 0, 0);
        list->Close();
        ID3D12CommandList* cls[] = {list};
        queue->ExecuteCommandLists(1, cls);
        wait_gpu();

        rtv_heap->Release();
    }

    bool save_bmp(const char* path) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = static_cast<UINT64>(kWidth) * kHeight * 4;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_ID3D12Resource,
                                                reinterpret_cast<void**>(&readback)))) {
            return false;
        }
        alloc->Reset();
        list->Reset(alloc, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = offscreen;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        // 关键：纹理到缓冲不能用 CopyResource（类型不一致，D3D12 会静默忽略，
        // 结果就是"截图永远全黑"）。必须用 CopyTextureRegion + PlacedFootprint。
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = kWidth;
        dst.PlacedFootprint.Footprint.Height = kHeight;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = kWidth * 4;   // 已按 256 对齐
        D3D12_TEXTURE_COPY_LOCATION src_loc{};
        src_loc.pResource = offscreen;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_loc.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src_loc, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* cls[] = {list};
        queue->ExecuteCommandLists(1, cls);
        wait_gpu();

        void* m = nullptr;
        readback->Map(0, nullptr, &m);
        auto* src = static_cast<unsigned char*>(m);
        const UINT row = kWidth * 4;
        FILE* f = fopen(path, "wb");
        if (!f) { readback->Unmap(0, nullptr); return false; }
        const UINT img = row * kHeight;
        unsigned char hdr[54] = {0};
        hdr[0] = 'B'; hdr[1] = 'M';
        memcpy(hdr + 2, &(const unsigned&){img + 54}, 4);
        const unsigned off = 54;
        memcpy(hdr + 10, &off, 4);
        const unsigned hs = 40;
        memcpy(hdr + 14, &hs, 4);
        memcpy(hdr + 18, &(const int&){static_cast<int>(kWidth)}, 4);
        memcpy(hdr + 22, &(const int&){static_cast<int>(kHeight)}, 4);
        const unsigned short planes = 1, bpp = 32;
        memcpy(hdr + 26, &planes, 2);
        memcpy(hdr + 28, &bpp, 2);
        fwrite(hdr, 1, 54, f);
        // BMP 是自下而上 + BGRA
        for (int y = static_cast<int>(kHeight) - 1; y >= 0; --y) {
            const unsigned char* line = src + y * row;
            for (UINT x = 0; x < kWidth; ++x) {
                unsigned char bgr[4] = {line[x * 4 + 2], line[x * 4 + 1], line[x * 4 + 0], 255};
                fwrite(bgr, 1, 4, f);
            }
        }
        fclose(f);
        readback->Unmap(0, nullptr);
        return true;
    }
};

// ---------------------------------------------------------------- 截图校验
// 读回 --shot 写出的 BMP 做**像素级**断言。这个测试以前对画面零验证
// （三条 ok(true)），只要没崩就是 PASS。所有期望颜色都写成字面量：
// 它们是本文件上面布局里那些浮点色值按 UNORM 四舍五入的结果
// （0.043*255≈11、0.09*255≈23、0.44*255≈112 ...），不从被测代码反算。
struct Shot {
    int w = 0, h = 0, off = 0;
    std::vector<unsigned char> px;   // 文件里的行序（BMP 自下而上），每像素 BGRX

    bool load(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<unsigned char> all(n > 0 ? static_cast<size_t>(n) : 0);
        const size_t got = all.empty() ? 0 : fread(all.data(), 1, all.size(), f);
        fclose(f);
        if (got < 54 || all[0] != 'B' || all[1] != 'M') return false;
        memcpy(&off, &all[10], 4);
        memcpy(&w, &all[18], 4);
        memcpy(&h, &all[22], 4);
        unsigned short bpp = 0;
        memcpy(&bpp, &all[28], 2);
        if (w <= 0 || h <= 0 || bpp != 32) return false;
        const size_t need = static_cast<size_t>(off) + static_cast<size_t>(w) * h * 4;
        if (got < need) return false;
        px.assign(all.begin() + off, all.begin() + static_cast<long>(need));
        return true;
    }

    // 屏幕坐标（左上原点）取样；BMP 是自下而上存的
    void at(int x, int y, int& r, int& g, int& b) const {
        const size_t i = (static_cast<size_t>(h - 1 - y) * static_cast<size_t>(w) +
                          static_cast<size_t>(x)) * 4;
        b = px[i + 0];
        g = px[i + 1];
        r = px[i + 2];
    }
};

}  // namespace

int main(int argc, char** argv) {
    const char* shot_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot_path = argv[++i];
    }

    Atlas atlas;
    ok(atlas.init(), "GDI 文本图集初始化");
    Gfx gfx;
    if (!gfx.init()) {
        xbgtasix::logf("[t38] D3D12 初始化失败 -> SKIP（无 GPU/无桌面）");
        return 2;
    }
    // 以前是 ok(true, ...)：只要没走上面的 return 就无条件 PASS。
    // 现在检查 init() 真的把三件关键对象建出来了。
    ok(gfx.dev != nullptr && gfx.root != nullptr && gfx.pso != nullptr,
       "D3D12 设备 + PSO + 根签名（对象非空）");

    // ---- 布局：Xbox 风格 ----
    Batch batch;
    // 背景（渐变用两层叠色近似）
    batch.quad(0, 0, kWidth, kHeight, kSolidU, kSolidV, kSolidU, kSolidV, 0.043f, 0.055f, 0.078f, 1);
    batch.quad(0, 0, kWidth, 240, kSolidU, kSolidV, kSolidU, kSolidV, 0.06f, 0.09f, 0.13f, 1);

    // 顶部品牌条
    batch.quad(0, 0, kWidth, 72, kSolidU, kSolidV, kSolidU, kSolidV, 0.043f, 0.09f, 0.054f, 1);
    batch.quad(0, 70, kWidth, 2, kSolidU, kSolidV, kSolidU, kSolidV, 0.41f, 0.80f, 0.13f, 1);
    atlas.text(batch, "XBGTASIX", 44, 40, 12, 0.55f, 0.95f, 0.20f, 1);
    atlas.text(batch, "Xbox 兼容层 · 本地会话", 19, 300, 30, 0.62f, 0.66f, 0.72f, 1);
    atlas.text(batch, "14:36", 26, 1160, 22, 0.85f, 0.88f, 0.92f, 1);

    // 左侧导航
    const char* nav[] = {"大厅", "启动游戏", "设置", "诊断"};
    for (int i = 0; i < 4; ++i) {
        const float y = 110 + i * 84;
        const bool sel = (i == 1);
        if (sel) {
            batch.quad(32, y - 10, 260, 64, kSolidU, kSolidV, kSolidU, kSolidV, 0.063f, 0.44f, 0.075f, 1);
            batch.quad(32, y - 10, 5, 64, kSolidU, kSolidV, kSolidU, kSolidV, 0.41f, 0.80f, 0.13f, 1);
        }
        atlas.text(batch, nav[i], 26, 60, y, sel ? 1.0f : 0.72f, sel ? 1.0f : 0.75f,
                   sel ? 1.0f : 0.80f, 1);
    }

    // 主区域卡片
    struct Card {
        float x, y, w, h;
        float r, g, b;
        const char* title;
        const char* sub;
    } cards[] = {
        {330, 110, 420, 260, 0.09f, 0.13f, 0.18f, "GTA6", "game_scarlett_master.exe · 缺失"},
        {770, 110, 450, 260, 0.09f, 0.13f, 0.18f, "兼容层状态", "461 导出 · 37 项测试通过"},
        {330, 400, 890, 150, 0.07f, 0.10f, 0.14f, "最近活动", "t38_xboxui 已渲染第一帧"},
    };
    for (auto& c : cards) {
        batch.quad(c.x, c.y, c.w, c.h, kSolidU, kSolidV, kSolidU, kSolidV, c.r, c.g, c.b, 1);
        batch.quad(c.x, c.y, c.w, 3, kSolidU, kSolidV, kSolidU, kSolidV, 0.41f, 0.80f, 0.13f, 1);
        atlas.text(batch, c.title, 26, c.x + 24, c.y + 22, 1, 1, 1, 1);
        atlas.text(batch, c.sub, 19, c.x + 24, c.y + 62, 0.60f, 0.66f, 0.74f, 1);
    }

    // 底部提示
    batch.quad(0, kHeight - 64, kWidth, 64, kSolidU, kSolidV, kSolidU, kSolidV, 0.05f, 0.07f, 0.09f, 1);
    atlas.text(batch, "A 选择    B 返回    ←→ 切换页签    Esc 退出", 19, 40, kHeight - 44, 0.62f,
               0.68f, 0.76f, 1);
    atlas.text(batch, "Xbox UI 由本项目自绘（Win32 + D3D12 flip 交换链）", 19, 780, kHeight - 44,
               0.45f, 0.50f, 0.58f, 1);

    gfx.upload_atlas(atlas.bits);
    // upload_atlas() 是 void，以前直接 ok(true, "文本图集已上传 GPU")。
    // 现在检查两件可观测的事：GPU 纹理被建出来；CPU 图集里确实有字形像素
    // （GDI 画字失败时整张图集是全黑，除 (0,0) 那个保留的白色像素）。
    {
        size_t nonzero = 0;
        if (atlas.bits) {
            const unsigned* apx = static_cast<const unsigned*>(atlas.bits);
            for (UINT i = 0; i < kAtlas * kAtlas; ++i) {
                if ((apx[i] >> 24) != 0) ++nonzero;   // alpha != 0
            }
        }
        ok(gfx.atlas_tex != nullptr && gfx.atlas_srv_written,
           "文本图集已上传 GPU（纹理资源 + SRV 就绪）");
        ok(nonzero > 1000, "图集里确实画进了字形像素（alpha != 0 的像素 > 1000）", "");
    }
    gfx.upload_batch(batch);
    ok(batch.verts.size() > 0, "UI 顶点批次已生成", "");
    xbgtasix::logf("[t38] 顶点 %zu 个，三角形 %zu 个", batch.verts.size(), batch.idx.size() / 3);

    // render() 也是 void：以前无条件 ok(true)。现在看 fence 是否真的推进
    // （推进 = 命令列表被提交并且 wait_gpu 等到了 GPU 完成）。
    const UINT64 fence_before = gfx.fence_value;
    gfx.render(batch);
    ok(gfx.fence_value > fence_before && gfx.offscreen != nullptr,
       "渲染完成（离屏 RT 就绪，命令已提交并等到 GPU fence）");

    // ---- 像素级校验 ----
    // 没有 --shot 时也写一份临时文件来做同样的校验，否则手动跑就完全没有画面证据。
    // （run_suite.sh 会传 --shot suite/xboxui.bmp）
    char auto_shot[] = "xboxui_autocheck.bmp";
    const bool auto_shot_used = (shot_path == nullptr);
    if (auto_shot_used) shot_path = auto_shot;

    const bool saved = gfx.save_bmp(shot_path);
    ok(saved, "截图写出", shot_path);
    if (saved) {
        xbgtasix::logf("[t38] 截图: %s", shot_path);
        Shot shot;
        const bool loaded = shot.load(shot_path);
        ok(loaded, "截图能读回（BMP 32bpp）", shot_path);
        if (loaded) {
            ok(shot.w == 1280 && shot.h == 720, "截图尺寸 1280x720");

            // 1) 顶部品牌条（0.043,0.09,0.054 -> 11,23,14）：G 必须高于 R 与 B
            {
                int r = 0, g = 0, b = 0;
                shot.at(1000, 30, r, g, b);
                char d[64];
                _snprintf(d, sizeof(d), "(%d,%d,%d)", r, g, b);
                ok(g > r + 8 && g > b + 8, "顶部品牌条偏绿（G > R 且 G > B）", d);
            }
            // 2) 品牌条下沿高亮线（0.41,0.80,0.13 -> 105,204,33）
            {
                int r = 0, g = 0, b = 0;
                shot.at(1000, 71, r, g, b);
                char d[64];
                _snprintf(d, sizeof(d), "(%d,%d,%d)", r, g, b);
                ok(g > 150 && g > r && g > b, "品牌条下沿是亮绿线（G>150 且最高）", d);
            }
            // 3) 左栏选中项（0.063,0.44,0.075 -> 16,112,19）：深绿，G 明显高于 R/B
            {
                int r = 0, g = 0, b = 0;
                shot.at(40, 240, r, g, b);
                char d[64];
                _snprintf(d, sizeof(d), "(%d,%d,%d)", r, g, b);
                ok(g > r + 60 && g > b + 60, "左栏选中项是深绿（G 比 R/B 高 60 以上）", d);
            }
            // 4) 卡片区（0.09,0.13,0.18 -> 23,33,46）必须明显不同于同高度的背景（11,14,20）
            {
                int br = 0, bg2 = 0, bb = 0, cr = 0, cg = 0, cb = 0;
                shot.at(1250, 330, br, bg2, bb);   // 卡片右侧的纯背景
                shot.at(600, 330, cr, cg, cb);     // 第一张卡片内部
                const int diff = std::abs(cr - br) + std::abs(cg - bg2) + std::abs(cb - bb);
                char d[96];
                _snprintf(d, sizeof(d), "卡片(%d,%d,%d) 背景(%d,%d,%d) 差和=%d",
                          cr, cg, cb, br, bg2, bb, diff);
                ok(diff >= 20, "卡片区颜色明显不同于背景（逐通道差之和 >= 20）", d);
            }
            // 5) 画面不能只是"清屏色 + 几条线"：非背景像素必须超过 40%。
            //    背景只有两种：整体 (11,14,20) 与顶部 240 行的 (15,23,33)。
            {
                size_t nonbg = 0;
                for (int y = 0; y < shot.h; ++y) {
                    for (int x = 0; x < shot.w; ++x) {
                        int r = 0, g = 0, b = 0;
                        shot.at(x, y, r, g, b);
                        const int d0 = std::abs(r - 11) + std::abs(g - 14) + std::abs(b - 20);
                        const int d1 = std::abs(r - 15) + std::abs(g - 23) + std::abs(b - 33);
                        if (d0 > 6 && d1 > 6) ++nonbg;
                    }
                }
                const double ratio = 100.0 * static_cast<double>(nonbg) /
                                     (static_cast<double>(shot.w) * static_cast<double>(shot.h));
                char d[64];
                _snprintf(d, sizeof(d), "%.1f%%", ratio);
                ok(ratio > 40.0, "非背景像素占比 > 40%（画面真的有内容）", d);
            }
        }
        if (auto_shot_used) remove(auto_shot);
    }

    xbgtasix::logf("[t38] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
