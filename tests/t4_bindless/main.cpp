// t4_bindless — 98 槽描述符表 + 动态索引
//
// 依据：docs/shader-corpus.md 统计出 21,522 条着色器的绑定槽 中位 14 / p90 46 / p99 98，
// 说明游戏是 bindless 式设计。这个测试证明"98 槽 + 动态索引"在 D3D12 上可行，
// 并且索引确实选到了正确的描述符（三张纯色贴图轮换，索引 97 -> 第 1 张 = 绿）。
#include <windows.h>
#include <d3d12.h>

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

constexpr UINT kSlots = 98;     // = 语料统计的 p99
constexpr UINT kSize = 8;       // 8x8 纯色贴图
constexpr UINT kRowPitch = kSize * 4;
constexpr UINT kReadbackBytes = kRowPitch * kSize;
constexpr UINT kBigHeap = 65536;  // 顺带测一下"大描述符堆"能不能建

int g_fail = 0;

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        xbgtasix::logf("[t4] FAIL %s hr=0x%08lx", what, static_cast<unsigned long>(hr));
        ++g_fail;
    }
}

std::vector<unsigned char> load(const char* path) {
    std::vector<unsigned char> out;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        xbgtasix::logf("[t4] cannot open %s", path);
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

// 创建一张纯色贴图（走 UPLOAD 堆直接拷贝）
ID3D12Resource* make_solid(ID3D12Device* dev, ID3D12CommandAllocator* alloc,
                           ID3D12GraphicsCommandList* list, ID3D12CommandQueue* q,
                           ID3D12Fence* fence, HANDLE ev, UINT64* fence_val,
                           unsigned char r, unsigned char g, unsigned char b) {
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

    ID3D12Resource* tex = nullptr;
    check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                       IID_ID3D12Resource, reinterpret_cast<void**>(&tex)),
          "CreateCommittedResource(tex)");
    if (!tex) return nullptr;

    D3D12_HEAP_PROPERTIES uhp{};
    uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kReadbackBytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* up = nullptr;
    check(dev->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &bd,
                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                       IID_ID3D12Resource, reinterpret_cast<void**>(&up)),
          "CreateCommittedResource(upload)");
    if (!up) {
        tex->Release();
        return nullptr;
    }
    void* p = nullptr;
    D3D12_RANGE none{0, 0};
    up->Map(0, &none, &p);
    if (p) {
        auto* px = static_cast<unsigned char*>(p);
        for (UINT i = 0; i < kSize * kSize; ++i) {
            px[i * 4 + 0] = r;
            px[i * 4 + 1] = g;
            px[i * 4 + 2] = b;
            px[i * 4 + 3] = 255;
        }
    }

    check(alloc->Reset(), "Allocator::Reset(make_solid)");
    check(list->Reset(alloc, nullptr), "List::Reset(make_solid)");
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = up;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = kSize;
    src.PlacedFootprint.Footprint.Height = kSize;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = kRowPitch;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tex;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &barrier);
    check(list->Close(), "List::Close");
    ID3D12CommandList* lists[] = {list};
    q->ExecuteCommandLists(1, lists);
    ++(*fence_val);
    q->Signal(fence, *fence_val);
    if (fence->GetCompletedValue() < *fence_val) {
        fence->SetEventOnCompletion(*fence_val, ev);
        WaitForSingleObject(ev, 10000);
    }
    up->Release();
    return tex;
}

}  // namespace

int main() {
    xbgtasix::logf("[t4] start, bindless %u slots", kSlots);
    check(XGameRuntimeInitialize(), "XGameRuntimeInitialize");

    ID3D12Device* dev = nullptr;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                            reinterpret_cast<void**>(&dev)),
          "D3D12CreateDevice");
    if (!dev) return 1;

    // ---- 顺带验证"大描述符堆"能否建立（bindless 的容量前提）----
    D3D12_DESCRIPTOR_HEAP_DESC big{};
    big.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    big.NumDescriptors = kBigHeap;
    big.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* big_heap = nullptr;
    const HRESULT hbig = dev->CreateDescriptorHeap(&big, IID_ID3D12DescriptorHeap,
                                                   reinterpret_cast<void**>(&big_heap));
    if (SUCCEEDED(hbig) && big_heap) {
        xbgtasix::logf("[t4] 大堆 OK: %u 个 CBV/SRV/UAV 描述符", kBigHeap);
        big_heap->Release();
    } else {
        xbgtasix::logf("[t4] 大堆失败 hr=0x%08lx（%u 个）", static_cast<unsigned long>(hbig), kBigHeap);
        ++g_fail;
    }

    // ---- root signature: [0] 32 位常量, [1] SRV 表 98 槽 ----
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = kSlots;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 1;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxAnisotropy = 1;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = 0.0f;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1;
    rs.Desc_1_0.NumParameters = 2;
    rs.Desc_1_0.pParameters = params;
    rs.Desc_1_0.NumStaticSamplers = 1;
    rs.Desc_1_0.pStaticSamplers = &samp;

    ID3DBlob* sig = nullptr;
    ID3DBlob* err = nullptr;
    check(D3D12SerializeVersionedRootSignature(&rs, &sig, &err), "SerializeVersionedRootSignature");
    if (err) {
        xbgtasix::logf("[t4] rs err: %s", static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
    }
    ID3D12RootSignature* root = nullptr;
    if (!sig) return 1;
    check(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                   IID_ID3D12RootSignature, reinterpret_cast<void**>(&root)),
          "CreateRootSignature");
    sig->Release();
    if (!root) return 1;

    auto vs = load("bindless.vs.cso");
    auto ps = load("bindless.ps.cso");
    if (vs.empty() || ps.empty()) {
        xbgtasix::logf("[t4] 缺 .cso");
        return 1;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root;
    pd.VS = {vs.data(), vs.size()};
    pd.PS = {ps.data(), ps.size()};
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.SampleDesc.Count = 1;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D12PipelineState* pso = nullptr;
    check(dev->CreateGraphicsPipelineState(&pd, IID_ID3D12PipelineState,
                                           reinterpret_cast<void**>(&pso)),
          "CreateGraphicsPipelineState");
    if (!pso) return 1;

    // ---- 命令对象 ----
    ID3D12CommandQueue* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, reinterpret_cast<void**>(&queue)),
          "CreateCommandQueue");
    ID3D12CommandAllocator* alloc = nullptr;
    check(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator,
                                      reinterpret_cast<void**>(&alloc)),
          "CreateCommandAllocator");
    ID3D12GraphicsCommandList* list = nullptr;
    check(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                 IID_ID3D12GraphicsCommandList,
                                 reinterpret_cast<void**>(&list)),
          "CreateCommandList");
    ID3D12Fence* fence = nullptr;
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence,
                           reinterpret_cast<void**>(&fence)),
          "CreateFence");
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    UINT64 fv = 0;
    if (!queue || !alloc || !list || !fence) return 1;

    list->Close();  // 让 make_solid 能 Reset

    // ---- 三张纯色贴图：红 / 绿 / 蓝 ----
    ID3D12Resource* tex[3] = {
        make_solid(dev, alloc, list, queue, fence, ev, &fv, 255, 0, 0),
        make_solid(dev, alloc, list, queue, fence, ev, &fv, 0, 255, 0),
        make_solid(dev, alloc, list, queue, fence, ev, &fv, 0, 0, 255),
    };
    if (!tex[0] || !tex[1] || !tex[2]) return 1;

    // ---- 98 槽描述符表：i % 3 轮换三张贴图 ----
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSlots;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* heap = nullptr;
    check(dev->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap, reinterpret_cast<void**>(&heap)),
          "CreateDescriptorHeap");
    if (!heap) return 1;

    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    for (UINT i = 0; i < kSlots; ++i) {
        dev->CreateShaderResourceView(tex[i % 3], &sd, cpu);
        cpu.ptr += inc;
    }

    // ---- 颜色目标 + 回读 ----
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 64;
    td.Height = 64;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    const float clear[4] = {0, 0, 0, 1};
    std::memcpy(cv.Color, clear, sizeof(clear));
    ID3D12Resource* color = nullptr;
    check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                       IID_ID3D12Resource, reinterpret_cast<void**>(&color)),
          "CreateCommittedResource(color)");
    if (!color) return 1;

    D3D12_DESCRIPTOR_HEAP_DESC rd{};
    rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rd.NumDescriptors = 1;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    check(dev->CreateDescriptorHeap(&rd, IID_ID3D12DescriptorHeap,
                                    reinterpret_cast<void**>(&rtv_heap)),
          "CreateDescriptorHeap(rtv)");
    if (!rtv_heap) return 1;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(color, nullptr, rtv);

    const UINT rb_row = 64 * 4;
    const UINT rb_bytes = rb_row * 64;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = rb_bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES rhp{};
    rhp.Type = D3D12_HEAP_TYPE_READBACK;
    ID3D12Resource* rb = nullptr;
    check(dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &bd,
                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                       IID_ID3D12Resource, reinterpret_cast<void**>(&rb)),
          "CreateCommittedResource(readback)");
    if (!rb) return 1;

    // ---- 画一帧，索引 97 -> 97 % 3 = 1 -> 绿 ----
    check(alloc->Reset(), "Allocator::Reset");
    check(list->Reset(alloc, pso), "List::Reset");
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, 64.0f, 64.0f, 0, 1};
    D3D12_RECT sc{0, 0, 64, 64};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    list->SetPipelineState(pso);
    list->SetGraphicsRootSignature(root);
    list->SetDescriptorHeaps(1, &heap);
    list->SetGraphicsRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    const UINT idx = 97;
    list->SetGraphicsRoot32BitConstant(0, idx, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    list->DrawInstanced(4, 1, 0, 0);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = color;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = 64;
    dst.PlacedFootprint.Footprint.Height = 64;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = rb_row;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = color;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    check(list->Close(), "List::Close");
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    ++fv;
    queue->Signal(fence, fv);
    if (fence->GetCompletedValue() < fv) {
        fence->SetEventOnCompletion(fv, ev);
        WaitForSingleObject(ev, 10000);
    }

    unsigned char* px = nullptr;
    D3D12_RANGE rr{0, rb_bytes};
    int rc = 1;
    if (SUCCEEDED(rb->Map(0, &rr, reinterpret_cast<void**>(&px))) && px) {
        const unsigned char* p = px + 32 * rb_row + 32 * 4;
        xbgtasix::logf("[t4] 中心像素 RGBA = (%u,%u,%u,%u)，期望绿 (0,255,0,255)", p[0], p[1], p[2], p[3]);
        if (p[0] < 8 && p[1] > 200 && p[2] < 8 && p[3] > 240) {
            xbgtasix::logf("[t4] PASS: 98 槽描述符表的动态索引生效");
            rc = 0;
        } else {
            xbgtasix::logf("[t4] FAIL: 索引选错了描述符");
        }
        D3D12_RANGE w{0, 0};
        rb->Unmap(0, &w);
    }

    for (int i = 0; i < 3; ++i) if (tex[i]) tex[i]->Release();
    if (rb) rb->Release();
    if (rtv_heap) rtv_heap->Release();
    if (color) color->Release();
    if (heap) heap->Release();
    if (pso) pso->Release();
    if (root) root->Release();
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (queue) queue->Release();
    if (ev) CloseHandle(ev);
    dev->Release();

    xbgtasix::logf("[t4] done rc=%d failures=%d", rc, g_fail);
    // 返回码必须同时反映 D3D 调用失败：只看像素会让 CreateRenderTargetView/
    // CreateFence 之类的失败静默通过（审查发现 6 个测试都有这个问题）
    return rc ? rc : (g_fail ? 1 : 0);
}
