// t6_rt — L7：DXR 1.0（BLAS + TLAS + 状态对象 + SBT + DispatchRays）
//
// 依据：docs/shader-corpus.md 统计出 64 个 BVH64 光追着色器，说明光追是渲染管线的一部分。
// 这里验证最小闭环：一个三角形 BLAS -> 单实例 TLAS -> raygen/closesthit -> 64x64 UAV 出图。
// 判定：中心像素必须是绿（命中），四角必须是蓝（miss）——两个方向都验证，防止"全屏同色"蒙对。
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

constexpr UINT kDim = 64;
constexpr UINT kRowPitch = kDim * 4;
constexpr UINT kReadbackBytes = kRowPitch * kDim;
constexpr UINT kRecordSize = 64;   // SBT 记录：32 字节 shader id + 余量，64 对齐最稳

int g_fail = 0;

// 分段计时：找出这 30 秒花在哪
double ms_since_start() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER t; QueryPerformanceFrequency(&t); return t; }();
    static LARGE_INTEGER start = [] { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return 1000.0 * double(now.QuadPart - start.QuadPart) / double(f.QuadPart);
}

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        xbgtasix::logf("[t6] FAIL %s hr=0x%08lx", what, static_cast<unsigned long>(hr));
        ++g_fail;
    }
}

std::vector<unsigned char> load(const char* path) {
    std::vector<unsigned char> out;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        xbgtasix::logf("[t6] cannot open %s", path);
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

ID3D12Resource* make_buffer(ID3D12Device* dev, UINT64 size, D3D12_HEAP_TYPE heap,
                            D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = size;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = flags;
    ID3D12Resource* r = nullptr;
    check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr,
                                       IID_ID3D12Resource, reinterpret_cast<void**>(&r)),
          "CreateCommittedResource(buffer)");
    return r;
}

}  // namespace

int main() {
    xbgtasix::logf("[t6] start, DXR %ux%u", kDim, kDim);
    check(XGameRuntimeInitialize(), "XGameRuntimeInitialize");

    ID3D12Device* dev = nullptr;
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device,
                            reinterpret_cast<void**>(&dev)),
          "D3D12CreateDevice");
    if (!dev) return 1;

    // ---- 光追能力 ----
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
    check(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5)),
          "CheckFeatureSupport(OPTIONS5)");
    xbgtasix::logf("[t6] RaytracingTier = %d（10 = 1.0，11 = 1.1）", static_cast<int>(opt5.RaytracingTier));
    if (opt5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
        xbgtasix::logf("[t6] SKIP: 这台 GPU/驱动不支持 DXR");
        dev->Release();
        return 2;
    }

    ID3D12Device5* dev5 = nullptr;
    check(dev->QueryInterface(IID_ID3D12Device5, reinterpret_cast<void**>(&dev5)), "QI ID3D12Device5");
    if (!dev5) return 1;

    // ---- root signature：u0 (UAV) + t0 (SRV) ----
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &ranges[0];
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &ranges[1];
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1;
    rs.Desc_1_0.NumParameters = 2;
    rs.Desc_1_0.pParameters = params;
    rs.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob* sig = nullptr;
    ID3DBlob* err = nullptr;
    check(D3D12SerializeVersionedRootSignature(&rs, &sig, &err), "SerializeVersionedRootSignature");
    if (err) {
        xbgtasix::logf("[t6] rs err: %s", static_cast<const char*>(err->GetBufferPointer()));
        err->Release();
    }
    ID3D12RootSignature* root = nullptr;
    if (!sig) return 1;
    check(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                   IID_ID3D12RootSignature, reinterpret_cast<void**>(&root)),
          "CreateRootSignature");
    sig->Release();
    if (!root) return 1;

    // ---- ray tracing 状态对象 ----
    auto lib = load("rt.lib.cso");
    if (lib.empty()) {
        xbgtasix::logf("[t6] 缺 rt.lib.cso（dxc -T lib_6_3）");
        return 1;
    }

    D3D12_EXPORT_DESC exports[3]{};
    exports[0].Name = L"RayGen";
    exports[1].Name = L"Miss";
    exports[2].Name = L"ClosestHit";

    D3D12_DXIL_LIBRARY_DESC dxil{};
    dxil.DXILLibrary.pShaderBytecode = lib.data();
    dxil.DXILLibrary.BytecodeLength = lib.size();
    dxil.NumExports = 3;
    dxil.pExports = exports;

    D3D12_HIT_GROUP_DESC hitgroup{};
    hitgroup.HitGroupExport = L"hitgroup";
    hitgroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitgroup.ClosestHitShaderImport = L"ClosestHit";

    D3D12_RAYTRACING_SHADER_CONFIG shader_cfg{};
    shader_cfg.MaxPayloadSizeInBytes = sizeof(float) * 4;
    shader_cfg.MaxAttributeSizeInBytes = sizeof(float) * 2;

    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_cfg{};
    pipeline_cfg.MaxTraceRecursionDepth = 1;

    D3D12_GLOBAL_ROOT_SIGNATURE global_rs{};
    global_rs.pGlobalRootSignature = root;

    D3D12_STATE_SUBOBJECT subobjects[5]{};
    subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[0].pDesc = &dxil;
    subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subobjects[1].pDesc = &hitgroup;
    subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subobjects[2].pDesc = &shader_cfg;
    subobjects[3].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobjects[3].pDesc = &pipeline_cfg;
    subobjects[4].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    subobjects[4].pDesc = &global_rs;

    D3D12_STATE_OBJECT_DESC so{};
    so.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    so.NumSubobjects = 5;
    so.pSubobjects = subobjects;

    xbgtasix::logf("[t6] t=%.0fms 创建状态对象前", ms_since_start());
    ID3D12StateObject* state = nullptr;
    check(dev5->CreateStateObject(&so, IID_ID3D12StateObject, reinterpret_cast<void**>(&state)),
          "CreateStateObject");
    if (!state) {
        dev5->Release();
        dev->Release();
        return 1;
    }
    xbgtasix::logf("[t6] t=%.0fms 状态对象完成（这段是 DXIL 运行期编译）", ms_since_start());
    ID3D12StateObjectProperties* props = nullptr;
    check(state->QueryInterface(IID_ID3D12StateObjectProperties, reinterpret_cast<void**>(&props)),
          "QI StateObjectProperties");
    if (!props) return 1;

    // ---- 命令对象（先准备好，构建加速结构要用）----
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
    ID3D12GraphicsCommandList4* list4 = nullptr;
    if (list) {
        check(list->QueryInterface(IID_ID3D12GraphicsCommandList4,
                                   reinterpret_cast<void**>(&list4)),
              "QI ID3D12GraphicsCommandList4");
    }
    ID3D12Fence* fence = nullptr;
    check(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence,
                           reinterpret_cast<void**>(&fence)),
          "CreateFence");
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    UINT64 fence_val = 0;
    if (!queue || !alloc || !list || !list4 || !fence) {
        xbgtasix::logf("[t6] 命令对象创建失败");
        return 1;
    }

    // ---- 三角形顶点 ----
    struct V { float x, y, z; };
    const V verts[3] = {{-0.8f, -0.8f, 0.0f}, {0.8f, -0.8f, 0.0f}, {0.0f, 0.9f, 0.0f}};
    ID3D12Resource* vb = make_buffer(dev, sizeof(verts), D3D12_HEAP_TYPE_UPLOAD,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    if (!vb) return 1;
    void* vptr = nullptr;
    D3D12_RANGE none{0, 0};
    vb->Map(0, &none, &vptr);
    std::memcpy(vptr, verts, sizeof(verts));

    D3D12_RAYTRACING_GEOMETRY_DESC geo{};
    geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geo.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
    geo.Triangles.VertexBuffer.StrideInBytes = sizeof(V);
    geo.Triangles.VertexCount = 3;
    geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;

    // ---- BLAS ----
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blas_inputs{};
    blas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    blas_inputs.NumDescs = 1;
    blas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blas_inputs.pGeometryDescs = &geo;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blas_info{};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&blas_inputs, &blas_info);
    xbgtasix::logf("[t6] BLAS 需要 result=%llu B scratch=%llu B",
                   static_cast<unsigned long long>(blas_info.ResultDataMaxSizeInBytes),
                   static_cast<unsigned long long>(blas_info.ScratchDataSizeInBytes));
    if (blas_info.ResultDataMaxSizeInBytes == 0) {
        xbgtasix::logf("[t6] BLAS prebuild 返回 0");
        return 1;
    }

    ID3D12Resource* blas = make_buffer(dev, blas_info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12Resource* blas_scratch = make_buffer(dev, blas_info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                               D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE);
    if (!blas || !blas_scratch) return 1;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blas_build{};
    blas_build.Inputs = blas_inputs;
    blas_build.DestAccelerationStructureData = blas->GetGPUVirtualAddress();
    blas_build.ScratchAccelerationStructureData = blas_scratch->GetGPUVirtualAddress();

    list4->BuildRaytracingAccelerationStructure(&blas_build, 0, nullptr);
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = blas;
    list4->ResourceBarrier(1, &uav);
    check(list->Close(), "List::Close");
    {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        ++fence_val;
        queue->Signal(fence, fence_val);
        if (fence->GetCompletedValue() < fence_val) {
            const HRESULT hs = fence->SetEventOnCompletion(fence_val, ev);
            const DWORD w = WaitForSingleObject(ev, 10000);
            xbgtasix::logf("[t6] BLAS 等待: setev=0x%08lx wait=%lu", static_cast<unsigned long>(hs), w);
        }
    }

    // ---- TLAS ----
    D3D12_RAYTRACING_INSTANCE_DESC inst{};
    inst.Transform[0][0] = 1.0f; inst.Transform[1][1] = 1.0f; inst.Transform[2][2] = 1.0f;
    inst.InstanceMask = 0xFF;
    inst.InstanceContributionToHitGroupIndex = 0;
    inst.AccelerationStructure = blas->GetGPUVirtualAddress();

    ID3D12Resource* inst_buf = make_buffer(dev, 64, D3D12_HEAP_TYPE_UPLOAD,
                                           D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    if (!inst_buf) return 1;
    void* iptr = nullptr;
    inst_buf->Map(0, &none, &iptr);
    std::memcpy(iptr, &inst, sizeof(inst));

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlas_inputs{};
    tlas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlas_inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tlas_inputs.NumDescs = 1;
    tlas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlas_inputs.InstanceDescs = inst_buf->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlas_info{};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&tlas_inputs, &tlas_info);
    xbgtasix::logf("[t6] TLAS 需要 result=%llu B scratch=%llu B",
                   static_cast<unsigned long long>(tlas_info.ResultDataMaxSizeInBytes),
                   static_cast<unsigned long long>(tlas_info.ScratchDataSizeInBytes));
    if (tlas_info.ResultDataMaxSizeInBytes == 0) return 1;

    ID3D12Resource* tlas = make_buffer(dev, tlas_info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12Resource* tlas_scratch = make_buffer(dev, tlas_info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                               D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE);
    if (!tlas || !tlas_scratch) return 1;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlas_build{};
    tlas_build.Inputs = tlas_inputs;
    tlas_build.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
    tlas_build.ScratchAccelerationStructureData = tlas_scratch->GetGPUVirtualAddress();

    check(alloc->Reset(), "Allocator::Reset(tlas)");
    check(list->Reset(alloc, nullptr), "List::Reset(tlas)");
    list4->BuildRaytracingAccelerationStructure(&tlas_build, 0, nullptr);
    uav.UAV.pResource = tlas;
    list4->ResourceBarrier(1, &uav);
    check(list->Close(), "List::Close(tlas)");
    {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        ++fence_val;
        queue->Signal(fence, fence_val);
        if (fence->GetCompletedValue() < fence_val) {
            const HRESULT hs = fence->SetEventOnCompletion(fence_val, ev);
            const DWORD w = WaitForSingleObject(ev, 10000);
            xbgtasix::logf("[t6] TLAS 等待: setev=0x%08lx wait=%lu", static_cast<unsigned long>(hs), w);
        }
    }

    // ---- 输出纹理（UAV）----
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kDim;
    td.Height = kDim;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* out_tex = nullptr;
    check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                       IID_ID3D12Resource, reinterpret_cast<void**>(&out_tex)),
          "CreateCommittedResource(uav tex)");
    if (!out_tex) return 1;

    // ---- 描述符 ----
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap* heap = nullptr;
    check(dev->CreateDescriptorHeap(&hd, IID_ID3D12DescriptorHeap, reinterpret_cast<void**>(&heap)),
          "CreateDescriptorHeap");
    if (!heap) return 1;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uavd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(out_tex, nullptr, &uavd, heap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_UNKNOWN;
    srvd.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.RaytracingAccelerationStructure.Location = tlas->GetGPUVirtualAddress();
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = heap->GetCPUDescriptorHandleForHeapStart();
    srv_cpu.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    dev->CreateShaderResourceView(nullptr, &srvd, srv_cpu);

    // ---- SBT ----
    struct Record { unsigned char id[32]; unsigned char pad[32]; };
    ID3D12Resource* sbt = make_buffer(dev, kRecordSize * 3, D3D12_HEAP_TYPE_UPLOAD,
                                      D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    if (!sbt) return 1;
    void* sbptr = nullptr;
    sbt->Map(0, &none, &sbptr);
    std::memset(sbptr, 0, kRecordSize * 3);
    auto put = [&](UINT slot, const wchar_t* name) {
        void* id = props->GetShaderIdentifier(name);
        if (!id) {
            xbgtasix::logf("[t6] GetShaderIdentifier 失败: %ls", name);
            ++g_fail;
            return;
        }
        std::memcpy(static_cast<unsigned char*>(sbptr) + slot * kRecordSize, id, 32);
    };
    put(0, L"RayGen");
    put(1, L"Miss");
    put(2, L"hitgroup");

    const D3D12_GPU_VIRTUAL_ADDRESS sbt_base = sbt->GetGPUVirtualAddress();

    // ---- 回读缓冲 ----
    ID3D12Resource* rb = make_buffer(dev, kReadbackBytes, D3D12_HEAP_TYPE_READBACK,
                                     D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    if (!rb) return 1;

    // ---- DispatchRays ----
    check(alloc->Reset(), "Allocator::Reset(rays)");
    check(list->Reset(alloc, nullptr), "List::Reset(rays)");
    list->SetComputeRootSignature(root);
    list->SetDescriptorHeaps(1, &heap);
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = heap->GetGPUDescriptorHandleForHeapStart();
    srv_gpu.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    list->SetComputeRootDescriptorTable(1, srv_gpu);

    D3D12_DISPATCH_RAYS_DESC dr{};
    dr.RayGenerationShaderRecord.StartAddress = sbt_base;
    dr.RayGenerationShaderRecord.SizeInBytes = kRecordSize;
    dr.MissShaderTable.StartAddress = sbt_base + kRecordSize;
    dr.MissShaderTable.SizeInBytes = kRecordSize;
    dr.MissShaderTable.StrideInBytes = kRecordSize;
    dr.HitGroupTable.StartAddress = sbt_base + kRecordSize * 2;
    dr.HitGroupTable.SizeInBytes = kRecordSize;
    dr.HitGroupTable.StrideInBytes = kRecordSize;
    dr.Width = kDim;
    dr.Height = kDim;
    dr.Depth = 1;
    list4->SetPipelineState1(state);
    list4->DispatchRays(&dr);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = out_tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = kDim;
    dst.PlacedFootprint.Footprint.Height = kDim;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kRowPitch;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = out_tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    check(list->Close(), "List::Close(rays)");
    xbgtasix::logf("[t6] t=%.0fms DispatchRays 录完并 Close", ms_since_start());
    {
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        xbgtasix::logf("[t6] t=%.0fms ExecuteCommandLists 返回", ms_since_start());
        ++fence_val;
        queue->Signal(fence, fence_val);
        xbgtasix::logf("[t6] t=%.0fms Signal 返回", ms_since_start());
        if (fence->GetCompletedValue() < fence_val) {
            const HRESULT hs = fence->SetEventOnCompletion(fence_val, ev);
            const DWORD w = WaitForSingleObject(ev, 20000);
            xbgtasix::logf("[t6] DispatchRays 等待: setev=0x%08lx wait=%lu", static_cast<unsigned long>(hs), w);
            if (w == WAIT_TIMEOUT) xbgtasix::logf("[t6] 超时，设备移除原因=0x%08lx",
                                                  static_cast<unsigned long>(dev->GetDeviceRemovedReason()));
        }
    }

    xbgtasix::logf("[t6] t=%.0fms 命令提交与等待完成", ms_since_start());
    // ---- 判定：中心绿（命中）、四角蓝（miss）----
    unsigned char* px = nullptr;
    D3D12_RANGE rr{0, kReadbackBytes};
    int rc = 1;
    if (SUCCEEDED(rb->Map(0, &rr, reinterpret_cast<void**>(&px))) && px) {
        auto at = [&](UINT x, UINT y) { return px + y * kRowPitch + x * 4; };
        xbgtasix::logf("[t6] t=%.0fms Map 返回", ms_since_start());
        const unsigned char* c = at(kDim / 2, kDim / 2);
        const unsigned char* corner = at(1, 1);
        xbgtasix::logf("[t6] 中心 = (%u,%u,%u,%u)  角落 = (%u,%u,%u,%u)",
                       c[0], c[1], c[2], c[3], corner[0], corner[1], corner[2], corner[3]);
        const bool hit = c[1] > 200 && c[0] < 60 && c[2] < 60;
        const bool miss = corner[2] > 200 && corner[0] < 60 && corner[1] < 60;
        if (hit && miss) {
            xbgtasix::logf("[t6] PASS: 光追命中/未命中都正确");
            rc = 0;
        } else {
            xbgtasix::logf("[t6] FAIL hit=%d miss=%d", hit ? 1 : 0, miss ? 1 : 0);
        }
        D3D12_RANGE w{0, 0};
        rb->Unmap(0, &w);
    }

    if (rb) rb->Release();
    if (sbt) sbt->Release();
    if (heap) heap->Release();
    if (out_tex) out_tex->Release();
    if (tlas_scratch) tlas_scratch->Release();
    if (tlas) tlas->Release();
    if (inst_buf) inst_buf->Release();
    if (blas_scratch) blas_scratch->Release();
    if (blas) blas->Release();
    if (vb) vb->Release();
    if (props) props->Release();
    if (state) state->Release();
    if (root) root->Release();
    if (fence) fence->Release();
    if (list4) list4->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (queue) queue->Release();
    if (ev) CloseHandle(ev);
    xbgtasix::logf("[t6] t=%.0fms 全部结束", ms_since_start());
    dev5->Release();
    dev->Release();

    xbgtasix::logf("[t6] done rc=%d failures=%d", rc, g_fail);
    return rc;
}
