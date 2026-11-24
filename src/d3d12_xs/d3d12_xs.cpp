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
#include "xbgtasix/host_stall.h"
#include "device_abi.h"
#include "root_sig.h"

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
    // 建设备是主机侧长操作（笔记本上枚举 + 初始化可以几秒），记账给 guest 的 Sleep 看：
    // 这段时间不该算进标题 hang check 的睡眠预算，否则会被它误判成主线程卡死。
    xbgtasix::host_stall::scope host_stall;
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

// ===========================================================================
// 根签名序列化：主机标志位剥离（不是裸转发）
//
// 主机 desc 里的 Flags 带 0x01000000 / 0x02000000 / 0x80000000 三个 PC 不认的位，
// 直接交给桌面 d3d12 会 E_INVALIDARG（本机实测 0x80070057
// "Unsupported bit-flag set (root signature flags ...)"）。
// 所以在私有副本上剥掉这三位再转发，成功后把原始位写回 blob 的 RTS0 段。
// 详见 src/d3d12_xs/root_sig.h 的文件头（含移植来源与各版本偏移依据）。
//
// D3D12SerializeRootSignature（1.0）原先在 forward.def 里是 PE 转发器，
// 转发器没有代码可以插剥离逻辑，所以那一条改成这里的手写实现——
// 它同样是包内 DLL 按名导入的 2 个 d3d12_xs 函数之一，游戏会真的走到。
// ===========================================================================
__declspec(dllexport) HRESULT WINAPI
D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                     ID3DBlob** blob,
                                     ID3DBlob** error) {
    using PFN = xbgtasix::rootsig::SerializeVersionedFn;
    static PFN pfn = resolve<PFN>("D3D12SerializeVersionedRootSignature");
    if (!pfn) return E_FAIL;
    return xbgtasix::rootsig::serialize_versioned(pfn, desc, blob, error);
}

__declspec(dllexport) HRESULT WINAPI
D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc,
                           D3D_ROOT_SIGNATURE_VERSION version,
                           ID3DBlob** blob,
                           ID3DBlob** error) {
    using PFN = xbgtasix::rootsig::SerializeFn;
    static PFN pfn = resolve<PFN>("D3D12SerializeRootSignature");
    if (!pfn) return E_FAIL;
    return xbgtasix::rootsig::serialize_1_0(pfn, desc, version, blob, error);
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

// ===========================================================================
// 给真实 exe 补的两个导出（名字来自 game_scarlett_master.exe 的导入表）
//
// D3D12XboxCreateDevice：复用桌面设备创建，但用 Xbox ABI 返回该设备的代理。
// 主机特有的创建设定尚未转换；保留当前的 feature level 和选卡逻辑。
//
// D3D12ConfigureVirtualMemoryX：主机上给设备配置虚拟内存（显存保留）。PC 上由驱动
//   自己管理，这里返回 S_OK 表示"配置成功"，让游戏继续（而不是卡在内存配置阶段）。
// ===========================================================================
__declspec(dllexport) HRESULT WINAPI D3D12XboxCreateDevice(IUnknown* adapter, void* params,
                                                           REFIID riid, void** device) {
    (void)params;
    xbgtasix::logf("[d3d12_xs] D3D12XboxCreateDevice -> desktop device + Xbox ABI");
    if (device) *device = nullptr;
    const int version = xbgtasix::d3d12::device_version(riid);
    if (version < 0) return E_NOINTERFACE;
    if (!device) return D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, riid, nullptr);

    void* pc = nullptr;
    const HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, riid, &pc);
    if (FAILED(hr)) return hr;
    if (!pc) return E_FAIL;

    // The proxy takes over this reference. A plain D3D12CreateDevice still returns
    // the original desktop object, with its desktop ABI and vtable untouched.
    *device = xbgtasix::d3d12::wrap_device(static_cast<IUnknown*>(pc), version);
    return *device ? hr : E_OUTOFMEMORY;
}

__declspec(dllexport) HRESULT WINAPI D3D12ConfigureVirtualMemoryX(void* device, void* config) {
    // 取证：这个函数目前是空实现，但游戏靠它注册显存/虚拟内存区间。
    // 如果它影响了游戏对"瞬时内存块大小"的选择，这里必须看到参数形状。
    if (config) {
        MEMORY_BASIC_INFORMATION mbi{};
        const bool readable = VirtualQuery(config, &mbi, sizeof(mbi)) != 0 &&
                              mbi.State == MEM_COMMIT &&
                              (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
        if (readable) {
            unsigned long long head[8]{};
            std::memcpy(head, config, sizeof(head));
            xbgtasix::logf("[d3d12_xs] D3D12ConfigureVirtualMemoryX config=%p 前 64 字节: "
                           "%llX %llX %llX %llX %llX %llX %llX %llX",
                           config, head[0], head[1], head[2], head[3], head[4], head[5], head[6],
                           head[7]);
            // 逐项走 4 条 32 字节记录，看是不是 {va, flag/count, next, pad} 的形状
            for (int i = 0; i < 3; ++i) {
                auto* e = reinterpret_cast<const unsigned long long*>(head[i]);
                MEMORY_BASIC_INFORMATION m3{};
                if (e && VirtualQuery(reinterpret_cast<LPCVOID>(head[i]), &m3, sizeof(m3)) &&
                    m3.State == MEM_COMMIT && !(m3.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                    for (int k = 0; k < 4; ++k) {
                        xbgtasix::logf("[d3d12_xs]   逐项 config[%d][%d] @%p: va=0x%llX f=0x%llX "
                                       "next=0x%llX pad=0x%llX", i, k,
                                       static_cast<const void*>(e + k * 4), e[k * 4], e[k * 4 + 1],
                                       e[k * 4 + 2], e[k * 4 + 3]);
                    }
                }
            }
            // 跟随第一层指针（前 3 个），每个 dump 32 字节 —— 看它到底要注册什么区间
            for (int i = 0; i < 3; ++i) {
                auto* q = reinterpret_cast<const unsigned long long*>(head[i]);
                MEMORY_BASIC_INFORMATION m2{};
                if (!q || VirtualQuery(reinterpret_cast<LPCVOID>(head[i]), &m2, sizeof(m2)) == 0 ||
                    m2.State != MEM_COMMIT || (m2.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                    xbgtasix::logf("[d3d12_xs]   config[%d]=0x%llX（不可读）", i, head[i]);
                    continue;
                }
                xbgtasix::logf("[d3d12_xs]   config[%d]=0x%llX -> 0x%llX 0x%llX 0x%llX 0x%llX",
                               i, head[i], q[0], q[1], q[2], q[3]);
            }
        } else {
            xbgtasix::logf("[d3d12_xs] D3D12ConfigureVirtualMemoryX config=%p（不可读）", config);
        }
    }
    (void)device;
    // 因果检验开关：让本函数返回失败，观察游戏是否因此改变执行路径。
    // 用来验证"空实现 + S_OK 是否把游戏推上了错误路径"这个假设。
    {
        char f[16] = {0};
        if (GetEnvironmentVariableA("XBGTASIX_VMCFG_FAIL", f, sizeof(f)) > 0 && f[0] == '1') {
            xbgtasix::logf("[d3d12_xs] D3D12ConfigureVirtualMemoryX -> E_NOTIMPL（因果检验）");
            return E_NOTIMPL;
        }
    }
    xbgtasix::logf("[d3d12_xs] D3D12ConfigureVirtualMemoryX：PC 由驱动管理显存，返回 S_OK");
    return S_OK;
}

}  // extern "C"
