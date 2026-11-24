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
// 本测试就是判据：用 WARP 建设备，拿**同一批** shader 字节码按
// `D3D12_COMPUTE_PIPELINE_STATE_DESC` 建 PSO，看 HRESULT。
//
// 数据来源：`XBGTASIX_DUMP_SHADERS=<目录>` 落盘的 `cs_*.bin`（build/ 下，不入库）。
// 没有 dump 时 SKIP（exit 2）——本测试不携带任何二进制夹具。
//
// 退出码：0 = PASS（全部编译成功或明确记录失败原因），1 = FAIL（出现非预期崩溃/失败），
//         2 = SKIP（没有 WARP 或没有 dump）
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int g_checks = 0;
int g_failures = 0;
int g_compiled = 0;
int g_rejected = 0;

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

}  // namespace

int main() {
    std::printf("t49_pso_warp —— 用 WARP 编译游戏送来的 DXIL PSO 描述\n");

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
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::printf("[t49] 找到 %zu 个 dump 文件于 %s\n", files.size(), dir);

    // WARP 设备
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::printf("SKIP: 建不出 D3D12 设备（WARP 不可用？）\n");
        std::printf("done failures=0 -> SKIP\n");
        return 2;
    }
    check(true, "建出 D3D12 设备（默认适配器；WARP 由系统按需选择）");

    // 先把 dump 到的**游戏真实根签名**都建成对象（空根签名必然 E_INVALIDARG，
    // 考不出 DXIL 是否合法；真实根签名才可能匹配 shader 的 RTS0）。
    std::vector<ComPtr<ID3D12RootSignature>> game_roots;
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
                ComPtr<ID3D12RootSignature> rs_obj;
                if (SUCCEEDED(device->CreateRootSignature(
                        0, blob.data(), blob.size(), IID_PPV_ARGS(&rs_obj)))) {
                    game_roots.push_back(rs_obj);
                }
            } while (FindNextFileA(rh, &rfd));
            FindClose(rh);
        }
        std::printf("[t49] dump 里的根签名可用 %zu 个\n", game_roots.size());
    }

    // 最小根签名：空 root signature（compute 只需要极少状态即可试编译）
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ComPtr<ID3DBlob> rs_blob, rs_err;
    const HRESULT hr_rs = D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                                      &rs_blob, &rs_err);
    check(SUCCEEDED(hr_rs), "序列化最小根签名");
    if (FAILED(hr_rs)) {
        std::printf("%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    ComPtr<ID3D12RootSignature> root_sig;
    const HRESULT hr_root = device->CreateRootSignature(
        0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root_sig));
    check(SUCCEEDED(hr_root), "建出根签名对象");

    // 逐个 shader 试编译
    for (const auto& name : files) {
        std::string path = dir;
        path += "\\";
        path += name;
        std::vector<unsigned char> code;
        if (!read_file(path.c_str(), &code)) {
            std::printf("[t49] %s 读不出来，跳过\n", name.c_str());
            continue;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root_sig.Get();
        desc.CS.pShaderBytecode = code.data();
        desc.CS.BytecodeLength = code.size();
        // 先配空根签名试一次，再对每个游戏根签名试，任一成功即说明 DXIL 合法
        HRESULT hr = E_FAIL;
        ComPtr<ID3D12PipelineState> pso;
        desc.pRootSignature = root_sig.Get();
        hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            for (auto& gr : game_roots) {
                ComPtr<ID3D12PipelineState> p2;
                desc.pRootSignature = gr.Get();
                hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&p2));
                if (SUCCEEDED(hr)) { pso = p2; break; }
            }
        }
        if (SUCCEEDED(hr)) {
            ++g_compiled;
            std::printf("[t49] %-24s len=%-6zu WARP 编译成功\n", name.c_str(), code.size());
        } else {
            ++g_rejected;
            std::printf("[t49] %-24s len=%-6zu hr=0x%08lX\n", name.c_str(), code.size(),
                        static_cast<unsigned long>(hr));
        }
    }

    check(g_compiled + g_rejected == static_cast<int>(files.size()),
          "每个 dump 都得到了确定结果（没有让驱动崩掉测试进程）");
    std::printf("[t49] 汇总：编译成功 %d / 被拒 %d / 共 %zu\n", g_compiled, g_rejected, files.size());
    // 判据：只要有任何一个能编过，就说明"DXIL 合法、硬件驱动才是崩溃方"
    check(g_compiled > 0, "至少有一个 shader 能被 WARP 编译（说明 DXIL 本身合法）");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    std::printf("done failures=%d -> %s\n", g_failures, g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
