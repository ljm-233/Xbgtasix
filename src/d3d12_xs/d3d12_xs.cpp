// d3d12_xs.dll shim — 主机 D3D12 变体的入口，转发到系统 d3d12.dll
//
// 关键：Windows 上系统 d3d12.dll 就是"真 D3D12 实现"，
// 所以我们不重写 D3D12，只做三件事：
//   1) 提供同名导出，让 xs 目标程序能解析导入
//   2) 转发到 System32\d3d12.dll（显式路径，避免自己转发到自己）
//   3) 记录 xs 变体多出来的调用，供后续决定是否真的需要实现
//
// 之后要接 vkd3d-proton 时，只需把 resolve() 的搜索路径换成随包分发的 d3d12core.dll。
#include "xbshim/trace.h"

#include <windows.h>
#include <d3d12.h>

#include <mutex>

namespace {

HMODULE g_d3d12 = nullptr;
std::once_flag g_once;

void load_real() {
    char sys[MAX_PATH] = {0};
    GetSystemDirectoryA(sys, MAX_PATH);
    char path[MAX_PATH] = {0};
    _snprintf(path, MAX_PATH, "%s\\d3d12.dll", sys);
    g_d3d12 = LoadLibraryA(path);
    if (!g_d3d12) {
        xbshim::logf("[d3d12_xs] failed to load %s (err=%lu)", path, GetLastError());
    } else {
        xbshim::logf("[d3d12_xs] real d3d12 -> %s", path);
    }
}

template <typename T>
T resolve(const char* name) {
    std::call_once(g_once, load_real);
    if (!g_d3d12) return nullptr;
    void* p = reinterpret_cast<void*>(GetProcAddress(g_d3d12, name));
    if (!p) xbshim::logf("[d3d12_xs] export not found in real d3d12.dll: %s", name);
    return reinterpret_cast<T>(p);
}

}  // namespace

extern "C" {

__declspec(dllexport) HRESULT WINAPI D3D12CreateDevice(IUnknown* adapter,
                                                      D3D_FEATURE_LEVEL min_level,
                                                      REFIID riid,
                                                      void** device) {
    using PFN = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    static PFN pfn = resolve<PFN>("D3D12CreateDevice");
    if (!pfn) return E_FAIL;
    xbshim::logf("[d3d12_xs] D3D12CreateDevice(feature_level=0x%x)", static_cast<unsigned>(min_level));
    return pfn(adapter, min_level, riid, device);
}

__declspec(dllexport) HRESULT WINAPI
D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                     ID3DBlob** blob,
                                     ID3DBlob** error) {
    using PFN = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
    static PFN pfn = resolve<PFN>("D3D12SerializeVersionedRootSignature");
    if (!pfn) return E_FAIL;
    return pfn(desc, blob, error);
}

__declspec(dllexport) HRESULT WINAPI D3D12CreateRootSignatureDeserializer(LPCVOID data,
                                                                         SIZE_T size,
                                                                         REFIID riid,
                                                                         void** out) {
    using PFN = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    static PFN pfn = resolve<PFN>("D3D12CreateRootSignatureDeserializer");
    if (!pfn) return E_FAIL;
    return pfn(data, size, riid, out);
}

__declspec(dllexport) HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void** out) {
    using PFN = HRESULT(WINAPI*)(REFIID, void**);
    static PFN pfn = resolve<PFN>("D3D12GetDebugInterface");
    if (!pfn) return E_FAIL;
    return pfn(riid, out);
}

}  // extern "C"
