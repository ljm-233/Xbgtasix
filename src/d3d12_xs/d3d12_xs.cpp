// SPDX-License-Identifier: GPL-3.0-or-later
// d3d12_xs.dll shim — 主机 D3D12 变体的入口，转发到系统 d3d12.dll
//
// 关键：Windows 上系统 d3d12.dll 就是"真 D3D12 实现"，
// 所以我们不重写 D3D12，只做三件事：
//   1) 提供同名导出，让 xs 目标程序能解析导入
//   2) 转发到 System32\d3d12.dll（显式路径，避免自己转发到自己）
//   3) 记录 xs 变体多出来的调用，供后续决定是否真的需要实现
//
// 之后要接 vkd3d-proton 时，只需把 resolve() 的搜索路径换成随包分发的 d3d12core.dll。
#include "xbgtasix/trace.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

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
        xbgtasix::logf("[d3d12_xs] failed to load %s (err=%lu)", path, GetLastError());
    } else {
        xbgtasix::logf("[d3d12_xs] real d3d12 -> %s", path);
    }
}

template <typename T>
T resolve(const char* name) {
    std::call_once(g_once, load_real);
    if (!g_d3d12) return nullptr;
    void* p = reinterpret_cast<void*>(GetProcAddress(g_d3d12, name));
    if (!p) xbgtasix::logf("[d3d12_xs] export not found in real d3d12.dll: %s", name);
    return reinterpret_cast<T>(p);
}

// 选卡：默认偏好"非软件适配器 + 显存最大"的独显。
// 笔记本上 D3D12CreateDevice(nullptr) 常选中核显，游戏会莫名其妙跑在 iGPU 上。
//
// 环境变量 XBGTASIX_ADAPTER：
//   best（默认） 挑独显
//   default      保持系统默认（传 nullptr 给真 d3d12）
//   0/1/2...     指定 DXGI 枚举下标
// 选卡结果缓存：DXGI 枚举 + 每候选 GetDesc1 在有些驱动上要几毫秒，
// 而基准实测"每次 D3D12CreateDevice( nullptr ) 都重新枚举"会把启动拖慢。
// 缓存持有一个引用，每次返回时再 AddRef 给调用方释放。
std::mutex g_pick_mu;
IDXGIAdapter1* g_picked = nullptr;
bool g_picked_valid = false;

IDXGIAdapter1* pick_adapter() {
    char mode[32] = {0};
    GetEnvironmentVariableA("XBGTASIX_ADAPTER", mode, sizeof(mode));
    if (_stricmp(mode, "default") == 0) return nullptr;
    {
        std::lock_guard<std::mutex> lock(g_pick_mu);
        if (g_picked_valid) {
            if (g_picked) g_picked->AddRef();
            return g_picked;
        }
    }

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void**>(&factory))) || !factory)
        return nullptr;

    int want = -1;
    if (mode[0] >= '0' && mode[0] <= '9') want = atoi(mode);

    IDXGIAdapter1* best = nullptr;
    SIZE_T best_vram = 0;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        if (!a) break;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) { a->Release(); continue; }
        const bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (want >= 0) {
            if (static_cast<int>(i) == want) { best = a; break; }
        } else if (!software && (!best || d.DedicatedVideoMemory > best_vram)) {
            if (best) best->Release();
            best = a;
            best_vram = d.DedicatedVideoMemory;
            continue;
        }
        a->Release();
    }
    factory->Release();
    // 只在"真的选中了非软件适配器"时才缓存；枚举失败（best==nullptr）不缓存，
    // 否则一次瞬时失败会变成永久失败（之后再也不重试，排障时极难定位）。
    bool owned_by_cache = false;
    {
        std::lock_guard<std::mutex> lock(g_pick_mu);
        if (!g_picked_valid && best) {
            g_picked = best;          // 缓存持有这份引用
            g_picked_valid = true;
        }
        // 比较必须在锁内：原来在锁外读 g_picked，另一个线程可能刚把它换掉（数据竞争）
        owned_by_cache = (best && best == g_picked);
    }
    if (owned_by_cache) best->AddRef();   // 给本次调用方
    if (best) {
        DXGI_ADAPTER_DESC1 d{};
        best->GetDesc1(&d);
        char name[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
        xbgtasix::logf("[d3d12_xs] 选卡 -> %s（%.1f GB）", name, d.DedicatedVideoMemory / 1024.0 / 1024 / 1024);
    }
    return best;
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
    xbgtasix::logf("[d3d12_xs] D3D12CreateDevice(feature_level=0x%x)", static_cast<unsigned>(min_level));
    // 调用方传 nullptr 时替它挑一块合适的卡（笔记本上默认常常是核显）
    IDXGIAdapter1* picked = nullptr;
    if (!adapter) {
        picked = pick_adapter();
        if (picked) adapter = picked;
    }
    HRESULT hr = pfn(adapter, min_level, riid, device);
    if (FAILED(hr) && picked) {
        // 挑中的独显可能不支持要求的最小 feature level 或驱动异常；
        // 回落一次系统默认，而不是让整个游戏起不来
        xbgtasix::logf("[d3d12_xs] 选定适配器失败 hr=0x%08lx，回落到系统默认",
                       static_cast<unsigned long>(hr));
        hr = pfn(nullptr, min_level, riid, device);
    }
    if (picked) picked->Release();
    return hr;
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
