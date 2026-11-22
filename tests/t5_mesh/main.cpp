// t5_mesh — L6：D3D12 mesh shader 管线
//
// 依据：docs/shader-corpus.md 统计出 21,522 条着色器里 7,498 条（34.8%）是 NGG mesh shader。
// 如果目标机器/驱动不支持 mesh shader，真实移植会在第一帧就崩，所以先把这条能力验证掉。
//
// 与 t1 的区别：PSO 必须用 D3D12_PIPELINE_STATE_STREAM_DESC（经典 desc 没有 MS 字段），
// 绘制用 ID3D12GraphicsCommandList6::DispatchMesh。
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

constexpr UINT kSize = 256;
constexpr UINT kRowPitch = kSize * 4;
constexpr UINT kReadbackBytes = kRowPitch * kSize;
constexpr float kClear[4] = {0.02f, 0.02f, 0.02f, 1.0f};

int g_fail = 0;

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        xbgtasix::logf("[t5] FAIL %s hr=0x%08lx", what, static_cast<unsigned long>(hr));
        ++g_fail;
    }
}

std::vector<unsigned char> load(const char* path) {
    std::vector<unsigned char> out;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        xbgtasix::logf("[t5] cannot open %s", path);
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

// D3D12 stream PSO 的拼装器：每个子对象 = 4 字节类型 + 载荷，8 字节对齐
struct StreamBuilder {
    std::vector<unsigned char> data;

    template <typename T>
    void add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const T& payload) {
        // 子对象起点 8 字节对齐
        while (data.size() % 8) data.push_back(0);
        const size_t off = data.size();
        const UINT t = static_cast<UINT>(type);
        data.resize(off + 4);
        std::memcpy(&data[off], &t, 4);
        // 载荷按自身对齐补齐（编译器对 alignas(void*) 结构就是这么布padding 的；
        // 少这一步会让指针落在 +4，D3D12 运行时用对齐指令读就会崩）
        while (data.size() % alignof(T)) data.push_back(0);
        const size_t p = data.size();
        data.resize(p + sizeof(T));
        std::memcpy(&data[p], &payload, sizeof(T));
    }

    // 标量载荷（枚举、格式、UINT）
    template <typename T>
    void add_scalar(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, T v) {
        add(type, v);
    }
};

}  // namespace

int main() {
    xbgtasix::logf("[t5] start, mesh shader %ux%u", kSize, kSize);
    check(XGameRuntimeInitialize(), "XGameRuntimeInitialize");

    ID3D12Device* dev = nullptr;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                            reinterpret_cast<void**>(&dev)),
          "D3D12CreateDevice");
    if (!dev) return 1;

    // ---- 先问驱动支不支持 mesh shader ----
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7{};
    const HRESULT hf = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7));
    if (FAILED(hf)) {
        xbgtasix::logf("[t5] 查询 OPTIONS7 失败 hr=0x%08lx", static_cast<unsigned long>(hf));
        dev->Release();
        return 2;
    }
    xbgtasix::logf("[t5] MeshShaderTier = %d（0=None 10=Supported）", static_cast<int>(opt7.MeshShaderTier));
    if (opt7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
        xbgtasix::logf("[t5] SKIP: 这台 GPU/驱动不支持 mesh shader");
        dev->Release();
        return 2;
    }

    xbgtasix::logf("[t5] step: QI ID3D12Device2");
    ID3D12Device2* dev2 = nullptr;
    check(dev->QueryInterface(IID_ID3D12Device2, reinterpret_cast<void**>(&dev2)), "QI ID3D12Device2");
    if (!dev2) return 1;

    xbgtasix::logf("[t5] step: root signature");
    // ---- root signature（空）----
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1;
    rs.Desc_1_0.NumParameters = 0;
    rs.Desc_1_0.NumStaticSamplers = 0;
    rs.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = nullptr;
    ID3DBlob* err = nullptr;
    check(D3D12SerializeVersionedRootSignature(&rs, &sig, &err), "SerializeVersionedRootSignature");
    if (err) {
        xbgtasix::logf("[t5] rs err: %s", static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
    }
    ID3D12RootSignature* root = nullptr;
    if (!sig) return 1;
    check(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                   IID_ID3D12RootSignature, reinterpret_cast<void**>(&root)),
          "CreateRootSignature");
    sig->Release();
    if (!root) return 1;

    xbgtasix::logf("[t5] step: load shaders");
    auto ms = load("mesh.ms.cso");
    auto ps = load("mesh.ps.cso");
    if (ms.empty() || ps.empty()) {
        xbgtasix::logf("[t5] 缺 .cso（dxc 没编 ms_6_5？）");
        return 1;
    }

    // ---- stream PSO ----
    D3D12_SHADER_BYTECODE msb{ms.data(), ms.size()};
    D3D12_SHADER_BYTECODE psb{ps.data(), ps.size()};
    D3D12_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_NONE;
    rast.DepthClipEnable = TRUE;
    D3D12_RT_FORMAT_ARRAY rt_formats{};
    rt_formats.NumRenderTargets = 1;
    rt_formats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_SAMPLE_DESC sample{1, 0};
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
    UINT node_mask = 0;
    UINT sample_mask = UINT_MAX;
    D3D12_PIPELINE_STATE_FLAGS flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    StreamBuilder sb;
    sb.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root);
    sb.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, msb);
    sb.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, psb);
    sb.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, blend);
    sb.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, rast);
    sb.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, rt_formats);
    sb.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, dsv);
    sb.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, sample);
    sb.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, sample_mask);
    sb.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, topo);
    sb.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, node_mask);
    sb.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, flags);

    xbgtasix::logf("[t5] step: stream 拼装完成 %zu 字节", sb.data.size());
    D3D12_PIPELINE_STATE_STREAM_DESC sd{};
    sd.SizeInBytes = sb.data.size();
    sd.pPipelineStateSubobjectStream = sb.data.data();

    ID3D12PipelineState* pso = nullptr;
    check(dev2->CreatePipelineState(&sd, IID_ID3D12PipelineState, reinterpret_cast<void**>(&pso)),
          "CreatePipelineState(stream)");
    xbgtasix::logf("[t5] step: PSO 完成 hr-ok=%d", pso ? 1 : 0);
    if (!pso) {
        dev2->Release();
        dev->Release();
        return 1;
    }

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
    ID3D12Resource* color = nullptr;
    check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                       IID_ID3D12Resource, reinterpret_cast<void**>(&color)),
          "CreateCommittedResource(color)");
    if (!color) return 1;

    D3D12_DESCRIPTOR_HEAP_DESC rhd{};
    rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rhd.NumDescriptors = 1;
    ID3D12DescriptorHeap* heap = nullptr;
    check(dev->CreateDescriptorHeap(&rhd, IID_ID3D12DescriptorHeap,
                                    reinterpret_cast<void**>(&heap)),
          "CreateDescriptorHeap");
    if (!heap) return 1;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(color, nullptr, rtv);

    // ---- 回读 ----
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
    ID3D12Resource* rb = nullptr;
    check(dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &bd,
                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                       IID_ID3D12Resource, reinterpret_cast<void**>(&rb)),
          "CreateCommittedResource(readback)");
    if (!rb) return 1;

    // ---- 命令 ----
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
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
    ID3D12GraphicsCommandList6* list6 = nullptr;
    if (list) {
        check(list->QueryInterface(IID_ID3D12GraphicsCommandList6,
                                   reinterpret_cast<void**>(&list6)),
              "QI ID3D12GraphicsCommandList6");
    }
    ID3D12Fence* fence = nullptr;
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence,
                           reinterpret_cast<void**>(&fence)),
          "CreateFence");
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!queue || !alloc || !list || !list6 || !fence) {
        xbgtasix::logf("[t5] 命令对象创建失败");
        return 1;
    }

    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->ClearRenderTargetView(rtv, kClear, 0, nullptr);
    D3D12_VIEWPORT vp{0, 0, float(kSize), float(kSize), 0, 1};
    D3D12_RECT sc{0, 0, LONG(kSize), LONG(kSize)};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &sc);
    list->SetPipelineState(pso);
    list->SetGraphicsRootSignature(root);
    list6->DispatchMesh(1, 1, 1);   // 1 个 threadgroup -> 1 个三角形

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
    queue->Signal(fence, 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 10000);
    }

    unsigned char* px = nullptr;
    D3D12_RANGE rr{0, kReadbackBytes};
    int rc = 1;
    if (SUCCEEDED(rb->Map(0, &rr, reinterpret_cast<void**>(&px))) && px) {
        const unsigned char* p = px + (kSize / 2) * kRowPitch + (kSize / 2) * 4;
        xbgtasix::logf("[t5] 中心像素 RGBA = (%u,%u,%u,%u)，清屏色 = (%d,%d,%d)", p[0], p[1], p[2], p[3],
                       int(kClear[0] * 255), int(kClear[1] * 255), int(kClear[2] * 255));
        const bool drew = (p[0] + p[1] + p[2]) > 30 && p[3] > 240;
        if (drew) {
            xbgtasix::logf("[t5] PASS: mesh shader 管道出图（DispatchMesh 生效）");
            rc = 0;
        } else {
            xbgtasix::logf("[t5] FAIL: 中心像素仍是清屏色");
        }
        D3D12_RANGE w{0, 0};
        rb->Unmap(0, &w);
    }

    if (list6) list6->Release();
    if (rb) rb->Release();
    if (heap) heap->Release();
    if (color) color->Release();
    if (pso) pso->Release();
    if (root) root->Release();
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (queue) queue->Release();
    if (ev) CloseHandle(ev);
    dev2->Release();
    dev->Release();

    xbgtasix::logf("[t5] done rc=%d failures=%d", rc, g_fail);
    // 返回码必须同时反映 D3D 调用失败：只看像素会让 CreateRenderTargetView/
    // CreateFence 之类的失败静默通过（审查发现 6 个测试都有这个问题）
    return rc ? rc : (g_fail ? 1 : 0);
}
