// t1_triangle — L3：root signature + PSO + descriptor heap + draw + 回读校验
//
// 全部离屏渲染（不建 swapchain），所以无桌面会话也能跑，并且能真正验证像素：
//   清屏色 (0.10,0.10,0.10) → 画三角形 → CopyTextureRegion 到 READBACK buffer
//   → Map 读中心像素 → 断言不是清屏色
#include <windows.h>
#include <d3d12.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "xbshim/trace.h"

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
constexpr float kClear[4] = {0.10f, 0.10f, 0.10f, 1.0f};

int g_fail = 0;

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        xbshim::logf("[t1] FAIL %s hr=0x%08lx", what, static_cast<unsigned long>(hr));
        ++g_fail;
    }
}

std::vector<unsigned char> load(const char* path) {
    std::vector<unsigned char> out;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        xbshim::logf("[t1] cannot open %s", path);
        return out;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize(static_cast<size_t>(n));
        if (fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    }
    fclose(f);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const bool keep = (argc > 1 && std::strcmp(argv[1], "--keep") == 0);
    xbshim::logf("[t1] start, offscreen %ux%u", kSize, kSize);

    check(XGameRuntimeInitialize(), "XGameRuntimeInitialize");

    ID3D12Device* dev = nullptr;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                            reinterpret_cast<void**>(&dev)),
          "D3D12CreateDevice");
    if (!dev) return 1;

    // ---- root signature（空：无参数、无静态采样器）----
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1;
    rs.Desc_1_0.NumParameters = 0;
    rs.Desc_1_0.pParameters = nullptr;
    rs.Desc_1_0.NumStaticSamplers = 0;
    rs.Desc_1_0.pStaticSamplers = nullptr;
    rs.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob* sig = nullptr;
    ID3DBlob* err = nullptr;
    check(D3D12SerializeVersionedRootSignature(&rs, &sig, &err), "D3D12SerializeVersionedRootSignature");
    if (err) {
        xbshim::logf("[t1] rs error: %s", static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
        err = nullptr;
    }
    ID3D12RootSignature* root = nullptr;
    if (sig) {
        check(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                       IID_ID3D12RootSignature, reinterpret_cast<void**>(&root)),
              "CreateRootSignature");
        sig->Release();
    }
    if (!root) return 1;

    // ---- shader ----
    auto vs = load("triangle.vs.cso");
    auto ps = load("triangle.ps.cso");
    if (vs.empty() || ps.empty()) {
        xbshim::logf("[t1] 缺少 .cso（dxc 没跑？看 build.sh）");
        return 1;
    }

    // ---- PSO ----
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root;
    pd.VS = {vs.data(), vs.size()};
    pd.PS = {ps.data(), ps.size()};
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.FrontCounterClockwise = FALSE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.SampleDesc.Count = 1;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN;

    ID3D12PipelineState* pso = nullptr;
    check(dev->CreateGraphicsPipelineState(&pd, IID_ID3D12PipelineState,
                                           reinterpret_cast<void**>(&pso)),
          "CreateGraphicsPipelineState");
    if (!pso) return 1;

    // ---- 渲染目标 ----
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

    ID3D12Resource* target = nullptr;
    check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                       D3D12_RESOURCE_STATE_COMMON, &cv,
                                       IID_ID3D12Resource, reinterpret_cast<void**>(&target)),
          "CreateCommittedResource(target)");
    if (!target) return 1;

    // ---- RTV ----
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    check(dev->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap,
                                    reinterpret_cast<void**>(&rtv_heap)),
          "CreateDescriptorHeap");
    if (!rtv_heap) return 1;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(target, nullptr, rtv);

    // ---- 回读缓冲 ----
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = kReadbackBytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES rhp{};
    rhp.Type = D3D12_HEAP_TYPE_READBACK;

    ID3D12Resource* readback = nullptr;
    check(dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &bd,
                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                       IID_ID3D12Resource, reinterpret_cast<void**>(&readback)),
          "CreateCommittedResource(readback)");
    if (!readback) return 1;

    // ---- 命令 ----
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
                                 IID_ID3D12GraphicsCommandList, reinterpret_cast<void**>(&list)),
          "CreateCommandList");
    if (!queue || !alloc || !list) return 1;

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = target;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &b);

    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, kClear, 0, nullptr);

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize)};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    list->SetPipelineState(pso);
    list->SetGraphicsRootSignature(root);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = target;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = kSize;
    dst.PlacedFootprint.Footprint.Height = kSize;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kRowPitch;

    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    check(list->Close(), "CommandList::Close");
    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);

    ID3D12Fence* fence = nullptr;
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence,
                           reinterpret_cast<void**>(&fence)),
          "CreateFence");
    if (fence) {
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        queue->Signal(fence, 1);
        if (fence->GetCompletedValue() < 1) {
            fence->SetEventOnCompletion(1, ev);
            WaitForSingleObject(ev, 10000);
        }
        CloseHandle(ev);
    }

    // ---- 读中心像素 ----
    unsigned char* px = nullptr;
    D3D12_RANGE rr{0, kReadbackBytes};
    check(readback->Map(0, &rr, reinterpret_cast<void**>(&px)), "readback Map");
    int rc = 1;
    if (px) {
        const UINT row = kSize / 2, col = kSize / 2;
        const unsigned char* p = px + row * kRowPitch + col * 4;
        const unsigned char clear_r = static_cast<unsigned char>(kClear[0] * 255.0f + 0.5f);
        xbshim::logf("[t1] center pixel RGBA = (%u,%u,%u,%u), clear R = %u",
                     p[0], p[1], p[2], p[3], clear_r);
        const bool drew = (std::abs(static_cast<int>(p[0]) - clear_r) > 4) ||
                          (std::abs(static_cast<int>(p[1]) - clear_r) > 4) ||
                          (std::abs(static_cast<int>(p[2]) - clear_r) > 4);
        if (drew && p[3] == 255) {
            xbshim::logf("[t1] PASS: 三角形画上去了");
            rc = 0;
        } else {
            xbshim::logf("[t1] FAIL: 中心像素仍是清屏色（draw 没生效）");
        }
        D3D12_RANGE written{0, 0};
        readback->Unmap(0, &written);
    }

    if (!keep) {
        if (fence) fence->Release();
        if (list) list->Release();
        if (alloc) alloc->Release();
        if (queue) queue->Release();
        if (readback) readback->Release();
        if (rtv_heap) rtv_heap->Release();
        if (target) target->Release();
        if (pso) pso->Release();
        if (root) root->Release();
        dev->Release();
    }
    xbshim::logf("[t1] done rc=%d failures=%d", rc, g_fail);
    return rc;
}
