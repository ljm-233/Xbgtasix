// SPDX-License-Identifier: GPL-3.0-or-later
// pso_matrix —— 把落盘的 53 个游戏 compute shader 逐个喂给"某个桌面 D3D12 设备"，
// 记录每个 PSO 的 HRESULT。用来回答一个具体问题：
//
//   同一批 DXIL，在 Windows 硬件驱动里会 DEVICE_REMOVED（transient-block-wall.md §30），
//   那么在另一个 D3D12 实现（vkd3d-proton → Vulkan）上，它编不编得过？
//
// 为什么是"另一个 D3D12 实现"而不是"写 Vulkan 代码"
// --------------------------------------------------
// 建 PSO 不需要任何 Vulkan 头：vkd3d-proton 的 d3d12.dll 就是一个桌面 D3D12 设备。
// 本程序只 `LoadLibraryA` 它、`GetProcAddress("D3D12CreateDevice")` 取一个入口，
// 其余全部走接口虚表（虚调用不产生导入符号，所以**不链接**任何 d3d12 导入库）。
//
// 与 t49_pso_warp 的区别（别混用）
//   * t49 用 WARP（软件驱动）区分"DXIL 是否合法"；
//   * 本工具用**指定的** d3d12.dll（--d3d12），默认走 vkd3d-proton，区分"Vulkan 路线能不能吃"。
//   * 每对 (cs, rs) 独立判一次；设备被移除时换一台新设备**重试一次**，并把重试记进表里
//     —— t49 上一版就是一台设备连编 53 个，第一次 DEVICE_REMOVED 之后全被连带失败，
//     只留下"48/5"的汇总、没有名单（rts0-analysis.md §1）。
//
// 输入（TSV，由 tools/pso_matrix.py 生成）：
//   cs_文件 <TAB> rs_文件     # 每行一对，路径是 Windows 风格（Z:\...）
// 输出（TSV，表头固定，便于回归对比）：
//   cs  rs  shader_len  rs_len  rs_hr  pso_hr  device  retried  verdict
//
// 退出码：0 = 每个 shader 都拿到确定结果；1 = 有 shader 没能得出结果；2 = 环境不满足（SKIP）
#include <windows.h>
#include <d3d12.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// 只取这三个 IID（写成常量，避免 DEFINE_GUID / __uuidof 在 mingw 下的可移植性坑）
constexpr GUID kIidDevice = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};
constexpr GUID kIidRootSignature = {0xc54a6b66, 0x72df, 0x4ee8, {0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14}};
constexpr GUID kIidPipelineState = {0x765a30f3, 0xf624, 0x4c6f, {0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45}};

constexpr HRESULT kDeviceRemoved = static_cast<HRESULT>(0x887A0005u);

using D3D12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

bool read_file(const std::string& path, std::vector<unsigned char>* out) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > (256ll << 20)) {
        CloseHandle(f);
        return false;
    }
    out->resize(static_cast<std::size_t>(size.QuadPart));
    DWORD got = 0;
    const bool ok = ReadFile(f, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) != 0;
    CloseHandle(f);
    return ok && got == out->size();
}

std::string base_name(const std::string& p) {
    const std::size_t cut = p.find_last_of("\\/");
    return cut == std::string::npos ? p : p.substr(cut + 1);
}

struct Pair {
    std::string cs;
    std::string rs;
};

std::vector<Pair> read_pairs(const std::string& path) {
    std::vector<Pair> pairs;
    std::vector<unsigned char> blob;
    if (!read_file(path, &blob)) return pairs;
    std::string text(blob.begin(), blob.end());
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        pairs.push_back(Pair{line.substr(0, tab), line.substr(tab + 1)});
    }
    return pairs;
}

struct Device {
    ID3D12Device* dev = nullptr;
    int index = 0;
};

bool new_device(D3D12CreateDeviceFn make, int index, Device* out) {
    ID3D12Device* dev = nullptr;
    const HRESULT hr = make(nullptr, D3D_FEATURE_LEVEL_11_0, kIidDevice,
                            reinterpret_cast<void**>(&dev));
    if (FAILED(hr) || !dev) {
        std::printf("# D3D12CreateDevice(#%d) 失败 hr=0x%08lX\n", index,
                    static_cast<unsigned long>(hr));
        return false;
    }
    out->dev = dev;
    out->index = index;
    return true;
}

void release(Device* d) {
    if (d->dev) { d->dev->Release(); d->dev = nullptr; }
}

}  // namespace

int main(int argc, char** argv) {
    std::string d3d12_path;
    std::string pairs_path;
    std::string out_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_next = i + 1 < argc;
        if (a == "--d3d12" && has_next) d3d12_path = argv[++i];
        else if (a == "--pairs" && has_next) pairs_path = argv[++i];
        else if (a == "--out" && has_next) out_path = argv[++i];
    }
    if (d3d12_path.empty() || pairs_path.empty()) {
        std::printf("用法: pso_matrix --d3d12 <d3d12.dll 全路径> --pairs <pairs.tsv> [--out <tsv>]\n");
        return 2;
    }

    // LOAD_WITH_ALTERED_SEARCH_PATH：vkd3d-proton 的 d3d12.dll 按"自己所在目录"找
    // d3d12core.dll（字符串里是 `\d3d12core.dll`），所以必须用这个标志，否则会去找系统的。
    HMODULE mod = LoadLibraryExA(d3d12_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        std::printf("# LoadLibraryExA 失败 err=%lu path=%s\n", GetLastError(), d3d12_path.c_str());
        return 2;
    }
    auto make = reinterpret_cast<D3D12CreateDeviceFn>(
        reinterpret_cast<void*>(GetProcAddress(mod, "D3D12CreateDevice")));
    if (!make) {
        std::printf("# d3d12.dll 里没有 D3D12CreateDevice\n");
        return 2;
    }
    std::printf("# d3d12 = %s\n", d3d12_path.c_str());

    Device device;
    if (!new_device(make, 0, &device)) {
        std::printf("# 建不出设备 —— 检查 Vulkan ICD / VK_LOADER_LAYERS_DISABLE\n");
        return 2;
    }
    std::printf("# 设备 #0 就绪\n");

    const std::vector<Pair> pairs = read_pairs(pairs_path);
    if (pairs.empty()) {
        std::printf("# pairs 为空：%s\n", pairs_path.c_str());
        release(&device);
        return 2;
    }

    std::FILE* out = out_path.empty() ? nullptr : std::fopen(out_path.c_str(), "wb");
    auto emit = [&](const char* line) {
        std::printf("%s", line);
        if (out) std::fputs(line, out);
    };

    emit("cs\trs\tshader_len\trs_len\trs_hr\tpso_hr\tdevice\tretried\tverdict\n");

    int ok_count = 0, fail_count = 0, retried_count = 0, next_device = 1;

    for (const Pair& p : pairs) {
        std::vector<unsigned char> shader, rs_blob;
        if (!read_file(p.cs, &shader) || !read_file(p.rs, &rs_blob)) {
            char line[1024];
            std::snprintf(line, sizeof(line), "%s\t%s\t0\t0\t-\t-\t-\t-\tread_fail\n",
                          base_name(p.cs).c_str(), base_name(p.rs).c_str());
            emit(line);
            ++fail_count;
            continue;
        }

        bool retried = false;
        HRESULT rs_hr = E_FAIL, pso_hr = E_FAIL;

        // 最多两次：第一次用当前设备；若设备已被移除，换新设备重试一次（并记账）。
        for (int attempt = 0; attempt < 2; ++attempt) {
            ID3D12RootSignature* rs = nullptr;
            ID3D12PipelineState* pso = nullptr;

            rs_hr = device.dev->CreateRootSignature(0, rs_blob.data(), rs_blob.size(),
                                                    kIidRootSignature,
                                                    reinterpret_cast<void**>(&rs));
            pso_hr = E_FAIL;
            if (SUCCEEDED(rs_hr) && rs) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
                desc.pRootSignature = rs;
                desc.CS.pShaderBytecode = shader.data();
                desc.CS.BytecodeLength = shader.size();
                desc.NodeMask = 0;
                desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
                pso_hr = device.dev->CreateComputePipelineState(
                    &desc, kIidPipelineState, reinterpret_cast<void**>(&pso));
            }
            if (pso) pso->Release();
            if (rs) rs->Release();

            const bool removed = (rs_hr == kDeviceRemoved || pso_hr == kDeviceRemoved);
            if (removed && attempt == 0) {
                // 设备被移除：换一台新的，重试这一条
                retried = true;
                ++retried_count;
                release(&device);
                if (!new_device(make, next_device++, &device)) {
                    std::printf("# 重试时建不出新设备，后续全部记 unknown\n");
                    break;
                }
                continue;
            }
            break;
        }

        const char* verdict = pso_hr == S_OK ? "ok" : "fail";
        if (pso_hr == S_OK) ++ok_count; else ++fail_count;
        char line[1024];
        std::snprintf(line, sizeof(line), "%s\t%s\t%zu\t%zu\t0x%08lX\t0x%08lX\t%d\t%s\t%s\n",
                      base_name(p.cs).c_str(), base_name(p.rs).c_str(), shader.size(),
                      rs_blob.size(), static_cast<unsigned long>(rs_hr),
                      static_cast<unsigned long>(pso_hr), device.index, retried ? "yes" : "no",
                      verdict);
        emit(line);
    }

    std::printf("# 汇总 ok=%d fail=%d 重试=%d 设备数=%d\n", ok_count, fail_count, retried_count,
                next_device);
    if (out) std::fclose(out);
    release(&device);
    return fail_count == 0 ? 0 : 1;
}
