// SPDX-License-Identifier: GPL-3.0-or-later
// t2_textured — L4：描述符表 + CBV + SRV + 采样器 + 深度缓冲
//
// 验证方式（全部离屏，无桌面可跑）：
//   同一场景用两种 tint（1.0 与 0.5）各渲一次、各回读中心像素，
//   断言 (a) 两次像素不同  (b) p1 ≈ 2 × p2  (c) alpha=255
//   像素为 0 或全等于清屏色都算失败 —— 这样"绑错描述符/采样器没生效/深度全裁掉"都会被抓到。
#include <windows.h>
#include <d3d12.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize(void);
HRESULT WINAPI D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL min_level, REFIID riid,
                                 void** device);
HRESULT WINAPI D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                                     ID3DBlob** blob, ID3DBlob** error);
}

namespace {

constexpr UINT kSize = 256;
constexpr UINT kRowPitch = kSize * 4;
constexpr UINT kReadbackBytes = kRowPitch * kSize;
constexpr UINT kTexSize = 64;
constexpr float kClear[4] = {0.05f, 0.05f, 0.05f, 1.0f};

int g_fail = 0;

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        xbgtasix::logf("[t2] FAIL %s hr=0x%08lx", what, static_cast<unsigned long>(hr));
        ++g_fail;
    }
}

std::vector<unsigned char> load(const char* path) {
    std::vector<unsigned char> out;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        xbgtasix::logf("[t2] cannot open %s", path);
        return out;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize(static_cast<size_t>(n));
        if (fread(out.data(), 1, out.size(), f) != size_t(n)) out.clear();
    }
    fclose(f);
    return out;
}

struct Renderer {
    ID3D12Device* dev = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* tex = nullptr;
    ID3D12Resource* cb = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12DescriptorHeap* dsv_heap = nullptr;
    ID3D12DescriptorHeap* srv_heap = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    float* cb_mapped = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};

    void release() {
        if (fence) fence->Release();
        if (list) list->Release();
        if (alloc) alloc->Release();
        if (queue) queue->Release();
        if (srv_heap) srv_heap->Release();
        if (dsv_heap) dsv_heap->Release();
        if (rtv_heap) rtv_heap->Release();
        if (readback) readback->Release();
        if (cb) cb->Release();
        if (tex) tex->Release();
        if (depth) depth->Release();
        if (color) color->Release();
        if (pso) pso->Release();
        if (root) root->Release();
        if (fence_event) CloseHandle(fence_event);
        if (dev) dev->Release();
    }

    bool init() {
        check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                                reinterpret_cast<void**>(&dev)),
              "D3D12CreateDevice");
        if (!dev) return false;

        // ---- root signature：1 张描述符表(CBV b0 + SRV t0) + 1 个静态采样器 ----
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].RegisterSpace = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;

        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 2;
        param.DescriptorTable.pDescriptorRanges = ranges;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MipLODBias = 0;
        samp.MaxAnisotropy = 1;
        samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samp.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        samp.MinLOD = 0.0f;
        samp.MaxLOD = 0.0f;  // 单 mip
        samp.ShaderRegister = 0;
        samp.RegisterSpace = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
        rs.Version = D3D_ROOT_SIGNATURE_VERSION_1;
        rs.Desc_1_0.NumParameters = 1;
        rs.Desc_1_0.pParameters = &param;
        rs.Desc_1_0.NumStaticSamplers = 1;
        rs.Desc_1_0.pStaticSamplers = &samp;
        rs.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* sig = nullptr;
        ID3DBlob* err = nullptr;
        check(D3D12SerializeVersionedRootSignature(&rs, &sig, &err), "SerializeVersionedRootSignature");
        if (err) {
            xbgtasix::logf("[t2] rs error: %s", static_cast<const char*>(err->GetBufferPointer()));
            err->Release();
        }
        if (!sig) return false;
        check(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                       IID_ID3D12RootSignature, reinterpret_cast<void**>(&root)),
              "CreateRootSignature");
        sig->Release();
        if (!root) return false;

        // ---- PSO（带深度）----
        auto vs = load("quad.vs.cso");
        auto ps = load("quad.ps.cso");
        if (vs.empty() || ps.empty()) {
            xbgtasix::logf("[t2] 缺 .cso");
            return false;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root;
        pd.VS = {vs.data(), vs.size()};
        pd.PS = {ps.data(), ps.size()};
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pd.SampleMask = UINT_MAX;
        pd.SampleDesc.Count = 1;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        check(dev->CreateGraphicsPipelineState(&pd, IID_ID3D12PipelineState,
                                               reinterpret_cast<void**>(&pso)),
              "CreateGraphicsPipelineState");
        if (!pso) return false;

        // ---- 颜色目标 ----
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kSize;
        td.Height = kSize;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        std::memcpy(cv.Color, kClear, sizeof(kClear));
        check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                           D3D12_RESOURCE_STATE_COMMON, &cv, IID_ID3D12Resource,
                                           reinterpret_cast<void**>(&color)),
              "CreateCommittedResource(color)");

        // ---- 深度 ----
        D3D12_RESOURCE_DESC dd = td;
        dd.Format = DXGI_FORMAT_D32_FLOAT;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE dcv{};
        dcv.Format = DXGI_FORMAT_D32_FLOAT;
        dcv.DepthStencil.Depth = 1.0f;
        dcv.DepthStencil.Stencil = 0;
        check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd,
                                           D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv,
                                           IID_ID3D12Resource, reinterpret_cast<void**>(&depth)),
              "CreateCommittedResource(depth)");

        // ---- 描述符堆 ----
        D3D12_DESCRIPTOR_HEAP_DESC rhd{};
        rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rhd.NumDescriptors = 1;
        check(dev->CreateDescriptorHeap(&rhd, IID_ID3D12DescriptorHeap,
                                        reinterpret_cast<void**>(&rtv_heap)),
              "CreateDescriptorHeap(rtv)");

        D3D12_DESCRIPTOR_HEAP_DESC dhd{};
        dhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dhd.NumDescriptors = 1;
        check(dev->CreateDescriptorHeap(&dhd, IID_ID3D12DescriptorHeap,
                                        reinterpret_cast<void**>(&dsv_heap)),
              "CreateDescriptorHeap(dsv)");

        D3D12_DESCRIPTOR_HEAP_DESC shd{};
        shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        shd.NumDescriptors = 2;
        shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(dev->CreateDescriptorHeap(&shd, IID_ID3D12DescriptorHeap,
                                        reinterpret_cast<void**>(&srv_heap)),
              "CreateDescriptorHeap(srv)");
        if (!rtv_heap || !dsv_heap || !srv_heap) return false;

        rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
        dev->CreateRenderTargetView(color, nullptr, rtv);
        dev->CreateDepthStencilView(depth, nullptr, dsv);

        // ---- 纹理（64x64 棋盘）----
        const UINT tex_row = kTexSize * 4;
        std::vector<unsigned char> pixels(tex_row * kTexSize);
        for (UINT y = 0; y < kTexSize; ++y) {
            for (UINT x = 0; x < kTexSize; ++x) {
                const bool odd = ((x / 8) + (y / 8)) & 1;
                unsigned char* p = &pixels[y * tex_row + x * 4];
                p[0] = odd ? 240 : 40;   // R
                p[1] = 60;               // G
                p[2] = odd ? 40 : 240;   // B
                p[3] = 255;
            }
        }

        D3D12_RESOURCE_DESC tdesc{};
        tdesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tdesc.Width = kTexSize;
        tdesc.Height = kTexSize;
        tdesc.DepthOrArraySize = 1;
        tdesc.MipLevels = 1;
        tdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        tdesc.SampleDesc.Count = 1;
        tdesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &tdesc,
                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           IID_ID3D12Resource, reinterpret_cast<void**>(&tex)),
              "CreateCommittedResource(tex)");
        if (!tex) return false;

        D3D12_HEAP_PROPERTIES uhp{};
        uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ud{};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = tex_row * kTexSize;
        ud.Height = 1;
        ud.DepthOrArraySize = 1;
        ud.MipLevels = 1;
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.SampleDesc.Count = 1;
        ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ID3D12Resource* upload = nullptr;
        check(dev->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &ud,
                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                           IID_ID3D12Resource, reinterpret_cast<void**>(&upload)),
              "CreateCommittedResource(upload)");
        if (!upload) return false;

        void* up = nullptr;
        D3D12_RANGE none{0, 0};
        upload->Map(0, &none, &up);
        if (!up) return false;
        std::memcpy(up, pixels.data(), pixels.size());

        // ---- CBV 缓冲（UPLOAD，256B 对齐）----
        D3D12_RESOURCE_DESC cbd{};
        cbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbd.Width = 256;
        cbd.Height = 1;
        cbd.DepthOrArraySize = 1;
        cbd.MipLevels = 1;
        cbd.Format = DXGI_FORMAT_UNKNOWN;
        cbd.SampleDesc.Count = 1;
        cbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(dev->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &cbd,
                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                           IID_ID3D12Resource, reinterpret_cast<void**>(&cb)),
              "CreateCommittedResource(cbv)");
        if (!cb) return false;
        cb->Map(0, &none, reinterpret_cast<void**>(&cb_mapped));

        // ---- 回读 ----
        D3D12_RESOURCE_DESC rbd = cbd;
        rbd.Width = kReadbackBytes;
        D3D12_HEAP_PROPERTIES rhp{};
        rhp.Type = D3D12_HEAP_TYPE_READBACK;
        check(dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &rbd,
                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           IID_ID3D12Resource, reinterpret_cast<void**>(&readback)),
              "CreateCommittedResource(readback)");

        // ---- 描述符内容 ----
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
        cbv.BufferLocation = cb->GetGPUVirtualAddress();
        cbv.SizeInBytes = 256;
        dev->CreateConstantBufferView(&cbv, srv_heap->GetCPUDescriptorHandleForHeapStart());

        D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
        srvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvd.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
        srv_cpu.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        dev->CreateShaderResourceView(tex, &srvd, srv_cpu);

        // ---- 队列/分配器/列表 ----
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, reinterpret_cast<void**>(&queue)),
              "CreateCommandQueue");
        check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator,
                                          reinterpret_cast<void**>(&alloc)),
              "CreateCommandAllocator");
        check(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                     IID_ID3D12GraphicsCommandList,
                                     reinterpret_cast<void**>(&list)),
              "CreateCommandList");
        check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence,
                               reinterpret_cast<void**>(&fence)),
              "CreateFence");
        fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!queue || !alloc || !list || !fence) return false;

        // 上传纹理（一次性）：buffer(footprint) -> texture(subresource)
        D3D12_TEXTURE_COPY_LOCATION up_src{};
        up_src.pResource = upload;
        up_src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        up_src.PlacedFootprint.Offset = 0;
        up_src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        up_src.PlacedFootprint.Footprint.Width = kTexSize;
        up_src.PlacedFootprint.Footprint.Height = kTexSize;
        up_src.PlacedFootprint.Footprint.Depth = 1;
        up_src.PlacedFootprint.Footprint.RowPitch = tex_row;

        D3D12_TEXTURE_COPY_LOCATION up_dst{};
        up_dst.pResource = tex;
        up_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        up_dst.SubresourceIndex = 0;
        list->CopyTextureRegion(&up_dst, 0, 0, 0, &up_src, nullptr);

        D3D12_RESOURCE_BARRIER tb{};
        tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        tb.Transition.pResource = tex;
        tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        tb.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &tb);

        // 颜色目标从 COMMON 起步，之后每一轮都从 COPY_SOURCE 开始
        D3D12_RESOURCE_BARRIER cb_barrier{};
        cb_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        cb_barrier.Transition.pResource = color;
        cb_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cb_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        cb_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &cb_barrier);

        check(list->Close(), "List::Close");
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        wait_gpu();
        upload->Release();
        return true;
    }

    void wait_gpu() {
        ++fence_value_;
        queue->Signal(fence, fence_value_);
        if (fence->GetCompletedValue() < fence_value_) {
            fence->SetEventOnCompletion(fence_value_, fence_event);
            WaitForSingleObject(fence_event, 10000);
        }
    }

    bool draw_and_read(float r, float g, float b, unsigned char out[4]) {
        cb_mapped[0] = r;
        cb_mapped[1] = g;
        cb_mapped[2] = b;
        cb_mapped[3] = 1.0f;

        check(alloc->Reset(), "Allocator::Reset");
        check(list->Reset(alloc, pso), "List::Reset");

        D3D12_RESOURCE_BARRIER bs[2]{};
        bs[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bs[0].Transition.pResource = color;
        bs[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bs[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bs[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &bs[0]);

        list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        list->ClearRenderTargetView(rtv, kClear, 0, nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        D3D12_VIEWPORT vp{0, 0, float(kSize), float(kSize), 0, 1};
        D3D12_RECT sc{0, 0, LONG(kSize), LONG(kSize)};
        list->RSSetViewports(1, &vp);
        list->RSSetScissorRects(1, &sc);
        list->SetPipelineState(pso);
        list->SetGraphicsRootSignature(root);
        list->SetDescriptorHeaps(1, &srv_heap);
        list->SetGraphicsRootDescriptorTable(0, srv_heap->GetGPUDescriptorHandleForHeapStart());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        list->DrawInstanced(4, 1, 0, 0);

        bs[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bs[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &bs[0]);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = kSize;
        dst.PlacedFootprint.Footprint.Height = kSize;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = kRowPitch;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = color;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        check(list->Close(), "List::Close");
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        wait_gpu();

        unsigned char* px = nullptr;
        D3D12_RANGE rr{0, kReadbackBytes};
        if (FAILED(readback->Map(0, &rr, reinterpret_cast<void**>(&px))) || !px) {
            xbgtasix::logf("[t2] readback Map failed");
            return false;
        }
        const UINT row = kSize / 2, col = kSize / 2;
        std::memcpy(out, px + row * kRowPitch + col * 4, 4);
        D3D12_RANGE written{0, 0};
        readback->Unmap(0, &written);
        return true;
    }

    UINT64 fence_value_ = 0;
};

}  // namespace

int main() {
    xbgtasix::logf("[t2] start, L4 texture + CBV + depth, %ux%u", kSize, kSize);
    check(XGameRuntimeInitialize(), "XGameRuntimeInitialize");

    Renderer r;
    int rc = 1;
    if (r.init()) {
        unsigned char a[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0};
        if (r.draw_and_read(1.0f, 1.0f, 1.0f, a) && r.draw_and_read(0.5f, 0.5f, 0.5f, b)) {
            xbgtasix::logf("[t2] tint=1.0 -> (%u,%u,%u,%u)", a[0], a[1], a[2], a[3]);
            xbgtasix::logf("[t2] tint=0.5 -> (%u,%u,%u,%u)", b[0], b[1], b[2], b[3]);

            // 只比 "rgb 之和 > 30" 会放过清屏色：kClear = 0.05 -> (13,13,13)，
            // 和 = 39 > 30，所以"根本没画东西"也能过。改为逐通道与清屏色 (13,13,13)
            // 相差都 > 20（实际纹理是 (240,60,40)，三个通道差分别是 227 / 47 / 27）。
            const bool drawn = a[3] == 255 &&
                               std::abs(int(a[0]) - 13) > 20 &&
                               std::abs(int(a[1]) - 13) > 20 &&
                               std::abs(int(a[2]) - 13) > 20;
            const bool halved = std::abs(int(a[0]) - 2 * int(b[0])) <= 4 &&
                                std::abs(int(a[1]) - 2 * int(b[1])) <= 4 &&
                                std::abs(int(a[2]) - 2 * int(b[2])) <= 4;
            if (drawn && halved) {
                xbgtasix::logf("[t2] PASS: 纹理采样 + CBV 调制 + 深度都生效");
                rc = 0;
            } else {
                xbgtasix::logf("[t2] FAIL drawn=%d halved=%d", drawn ? 1 : 0, halved ? 1 : 0);
            }
        }
    }
    r.release();
    xbgtasix::logf("[t2] done rc=%d failures=%d", rc, g_fail);
    // 返回码必须同时反映 D3D 调用失败：只看像素会让 CreateRenderTargetView/
    // CreateFence 之类的失败静默通过（审查发现 6 个测试都有这个问题）
    return rc ? rc : (g_fail ? 1 : 0);
}
