// SPDX-License-Identifier: GPL-3.0-or-later
// t49_pso_warp —— 用 WARP（D3D12 软件驱动）编译游戏真实送来的 DXIL PSO 描述
//
// 为什么需要它
// ------------
// 前几轮确认：游戏送进 `CreateComputePipelineState` 的字节码是标准 DXBC 容器，
// 程序块是 `DXIL`、版本 0x00050065（Shader Model 6.0）。
// 但 **Intel 的 igc 与 NVIDIA 驱动在编译它们时都崩**（Intel 侧直接导致
// DEVICE_REMOVED），把游戏推进的流程打断。
//
// 于是要区分两种可能：
//   A) 这批 DXIL 本身不合法/不全（主机侧裁剪过）→ 任何驱动都不该能编
//   B) DXIL 合法，是硬件驱动面对它崩 → 软件驱动（WARP）应该能编过
//
// 本测试就是判据：拿**同一批** shader 字节码按 `D3D12_COMPUTE_PIPELINE_STATE_DESC`
// 建 PSO，看 HRESULT。
//
// ★ 2026-11-25 修正两处（不修的话这份测试的结论站不住）
// -----------------------------------------------------
// 1) **`D3D12CreateDevice(nullptr, …)` 选的不是 WARP**，是默认（硬件）适配器。
//    要真正拿到 WARP 必须 `IDXGIFactory4::EnumWarpAdapter`。上一版用的就是 nullptr，
//    所以"WARP 编译成功 48/53"这句话实际上是**Intel 驱动在独立进程里编过 48 个**。
//    证据：`build/warp1.log`（XBGTASIX_FORCE_WARP=1 那次）里 `igd12um64xeh.dll`
//    出现 75 次、`d3d10warp.dll` 出现 **0 次**，崩溃栈帧 #4..#14 全在 igd12um64xeh。
//    本版显式枚举 WARP，并把选中的适配器描述打出来，避免再次混称。
// 2) **每个 shader 一台全新设备**。上一版一台设备连编 53 个，一次 `0x887A0005`
//    之后剩下的全被连带失败，于是只留下"48/5"的汇总、没有名单。
//
// 数据来源：`XBGTASIX_DUMP_SHADERS=<目录>` 落盘的 `cs_*.bin` 与 `rs_*.bin`
// （在 build/ 下，不入库）。没有 dump 时 SKIP（exit 2）——本测试不携带任何二进制夹具。
//
// 输出约定（便于 grep）：
//   [t49] RESULT <文件名> len=<n> hr=0x%08X <ok|fail> [removed=0x%08X]
//   [t49] FAIL <文件名> len=<n> hr=0x%08X removed=0x%08X   ← 失败清单，逐条
//
// 退出码：0 = PASS（每个 shader 都得到确定结果，且至少一个能编过），
//         1 = FAIL（有非预期失败），2 = SKIP（没有 dump 或没有 WARP）。
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int g_checks = 0;
int g_failures = 0;
int g_compiled = 0;
int g_rejected = 0;
int g_device_lost = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("[FAIL] %s\n", what);
    } else {
        std::printf("[ok]   %s\n", what);
    }
}

bool read_file(const char* path, std::vector<unsigned char>* out) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > (64ll << 20)) {
        CloseHandle(f);
        return false;
    }
    out->resize(static_cast<std::size_t>(size.QuadPart));
    DWORD got = 0;
    const bool ok = ReadFile(f, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) != 0;
    CloseHandle(f);
    return ok && got == out->size();
}

// 宽字符转 UTF-8（适配器描述是 WCHAR，控制台按 UTF-8 输出，混用 %ls 在 mingw 下不稳）
std::string narrow(const wchar_t* w) {
    if (!w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    // n 含结尾 NUL，所以先按 n 分配再缩到 n-1，别写越界
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    s.resize(static_cast<std::size_t>(n - 1));
    return s;
}

// 枚举 WARP（软件）适配器。返回 nullptr 表示拿不到 —— 调用方**不要**默默退回
// `D3D12CreateDevice(nullptr)`，那会变成硬件适配器，把结论搞反。
ComPtr<IDXGIAdapter1> find_warp_adapter() {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
        return ComPtr<IDXGIAdapter1>();
    }

    ComPtr<IDXGIAdapter1> warp;
    if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && warp) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(warp->GetDesc1(&d))) {
            const std::string name = narrow(d.Description);
            std::printf("[t49] 适配器: %s（flags=0x%X，专用显存 %llu MB，LUID=%08lX:%08lX）\n",
                        name.c_str(), d.Flags,
                        static_cast<unsigned long long>(d.DedicatedVideoMemory >> 20),
                        static_cast<unsigned long>(d.AdapterLuid.HighPart),
                        static_cast<unsigned long>(d.AdapterLuid.LowPart));
        }
        return warp;
    }

    // 退化路径：EnumWarpAdapter 不可用时就按"软件适配器"标志找
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> cand;
        if (factory->EnumAdapters1(i, &cand) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        if (cand && SUCCEEDED(cand->GetDesc1(&d)) && (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            const std::string name = narrow(d.Description);
            std::printf("[t49] 适配器（退化找到的软件适配器）: %s\n", name.c_str());
            return cand;
        }
    }
    return ComPtr<IDXGIAdapter1>();
}

}  // namespace

int main() {
    std::printf("t49_pso_warp —— 用 WARP 编译游戏送来的 DXIL PSO 描述（每 shader 一台设备）\n");

    char dir[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("XBGTASIX_DUMP_SHADERS", dir, sizeof(dir)) == 0 || !dir[0]) {
        // 默认去找 build/dumps
        std::snprintf(dir, sizeof(dir), "build\\dumps");
    }

    // 枚举 cs_*.bin
    char pattern[MAX_PATH] = {0};
    std::snprintf(pattern, sizeof(pattern), "%s\\cs_*.bin", dir);
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("SKIP: 没有找到 %s\\cs_*.bin（先设 XBGTASIX_DUMP_SHADERS 跑一次游戏）\n", dir);
        std::printf("done failures=0 -> SKIP\n");
        return 2;
    }
    std::vector<std::string> files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName);
    } while (FindNextFile(h, &fd));
    FindClose(h);
    std::printf("[t49] 找到 %zu 个 dump 文件于 %s\n", files.size(), dir);

    // 根签名 blob 读进内存：每台设备要重建对象，但不必重读盘
    std::vector<std::pair<std::string, std::vector<unsigned char>>> rs_blobs;
    {
        char rpat[MAX_PATH] = {0};
        std::snprintf(rpat, sizeof(rpat), "%s\\rs_*.bin", dir);
        WIN32_FIND_DATAA rfd{};
        HANDLE rh = FindFirstFileA(rpat, &rfd);
        if (rh != INVALID_HANDLE_VALUE) {
            do {
                if (rfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string rp = std::string(dir) + "\\" + rfd.cFileName;
                std::vector<unsigned char> blob;
                if (!read_file(rp.c_str(), &blob)) continue;
                rs_blobs.push_back(std::make_pair(std::string(rfd.cFileName), blob));
            } while (FindNextFileA(rh, &rfd));
            FindClose(rh);
        }
    }
    std::printf("[t49] 读到根签名 blob %zu 个（每台设备上重建一次）\n", rs_blobs.size());

    // WARP 适配器：只枚举一次，后面每个 shader 都用它建新设备
    ComPtr<IDXGIAdapter1> warp = find_warp_adapter();
    if (!warp) {
        std::printf("SKIP: 拿不到 WARP 适配器（EnumWarpAdapter 与软件适配器枚举都失败）\n");
        std::printf("      ★ 不要退回 D3D12CreateDevice(nullptr) —— 那选的是硬件适配器，\n");
        std::printf("        上一版就是这么把 Intel 的编译结果记成 WARP 的。\n");
        std::printf("done failures=0 -> SKIP\n");
        return 2;
    }

    // 空根签名（RTS1.0）也留一份作对照：正常应为 E_INVALIDARG
    ComPtr<ID3DBlob> empty_blob;
    {
        D3D12_ROOT_SIGNATURE_DESC empty_rs{};
        empty_rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> err;
        if (FAILED(D3D12SerializeRootSignature(&empty_rs, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                               &empty_blob, &err))) {
            empty_blob.Reset();
        }
    }

    struct Result {
        std::string name;
        std::size_t len;
        unsigned int hr;       // HRESULT 一律按 32 位看，避免 long 位宽不同导致符号扩展
        unsigned int removed;
    };
    std::vector<Result> results;

    // ---- 每个 shader 一台**全新** WARP 设备 ----
    for (std::size_t fi = 0; fi < files.size(); ++fi) {
        const std::string& name = files[fi];
        std::string path = dir;
        path += "\\";
        path += name;
        std::vector<unsigned char> code;
        if (!read_file(path.c_str(), &code)) {
            std::printf("[t49] %s 读不出来，跳过\n", name.c_str());
            continue;
        }

        ComPtr<ID3D12Device> device;
        HRESULT hr_dev = D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0,
                                           IID_PPV_ARGS(&device));
        if (FAILED(hr_dev) || !device) {
            std::printf("[t49] RESULT %-24s len=%-6zu hr=0x%08X fail（设备建不出来）\n",
                        name.c_str(), code.size(), static_cast<unsigned int>(hr_dev));
            results.push_back(Result{name, code.size(), static_cast<unsigned int>(hr_dev), 0});
            ++g_rejected;
            continue;
        }

        // 这台设备上的游戏根签名
        std::vector<ComPtr<ID3D12RootSignature>> roots;
        for (std::size_t i = 0; i < rs_blobs.size(); ++i) {
            ComPtr<ID3D12RootSignature> obj;
            if (SUCCEEDED(device->CreateRootSignature(0, rs_blobs[i].second.data(),
                                                      rs_blobs[i].second.size(),
                                                      IID_PPV_ARGS(&obj)))) {
                roots.push_back(obj);
            }
        }
        ComPtr<ID3D12RootSignature> empty_obj;
        if (empty_blob) {
            device->CreateRootSignature(0, empty_blob->GetBufferPointer(),
                                        empty_blob->GetBufferSize(),
                                        IID_PPV_ARGS(&empty_obj));
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.CS.pShaderBytecode = code.data();
        desc.CS.BytecodeLength = code.size();

        HRESULT hr = E_FAIL;
        ComPtr<ID3D12PipelineState> pso;
        // 先空根签名，再逐个游戏根签名；任一成功即说明这份 DXIL 能被这台编译器编过
        if (empty_obj) {
            desc.pRootSignature = empty_obj.Get();
            hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
        }
        if (FAILED(hr)) {
            for (std::size_t i = 0; i < roots.size(); ++i) {
                ComPtr<ID3D12PipelineState> p2;
                desc.pRootSignature = roots[i].Get();
                hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&p2));
                if (SUCCEEDED(hr)) {
                    pso = p2;
                    break;
                }
            }
        }

        const unsigned int removed =
            static_cast<unsigned int>(device->GetDeviceRemovedReason());
        if (SUCCEEDED(hr)) {
            ++g_compiled;
            std::printf("[t49] RESULT %-24s len=%-6zu hr=0x00000000 ok\n", name.c_str(),
                        code.size());
            results.push_back(Result{name, code.size(), 0u, removed});
        } else {
            ++g_rejected;
            if (static_cast<unsigned int>(hr) == 0x887A0005u) ++g_device_lost;
            std::printf("[t49] RESULT %-24s len=%-6zu hr=0x%08X fail  removed=0x%08X\n",
                        name.c_str(), code.size(), static_cast<unsigned int>(hr), removed);
            results.push_back(Result{name, code.size(), static_cast<unsigned int>(hr), removed});
        }
    }

    // ---- 失败清单 ----
    std::printf("\n[t49] ===== 失败清单（逐条，grep \"[t49] FAIL\"）=====\n");
    std::size_t fail_n = 0;
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (results[i].hr == 0u) continue;
        ++fail_n;
        std::printf("[t49] FAIL %-24s len=%-6zu hr=0x%08X removed=0x%08X\n",
                    results[i].name.c_str(), results[i].len, results[i].hr, results[i].removed);
    }
    if (fail_n == 0) std::printf("[t49] （无失败）\n");

    check(g_compiled + g_rejected == static_cast<int>(results.size()),
          "每个 dump 都得到了确定结果（没有中途退出）");
    std::printf("[t49] 汇总：编译成功 %d / 被拒 %d / 共 %zu（DEVICE_REMOVED %d 次）\n",
                g_compiled, g_rejected, results.size(), g_device_lost);
    // 判据：只要有一个能编过，就说明"至少这份 DXIL 能被这个编译器编过"
    check(g_compiled > 0, "至少有一个 shader 能被 WARP 编译（说明 DXIL 本身合法）");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    std::printf("done failures=%d -> %s\n", g_failures, g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
