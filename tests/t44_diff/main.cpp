// SPDX-License-Identifier: GPL-3.0-or-later
//
// t44_diff — 差分测试（模式 A）：同一份 HLSL、同一块 GPU、两条路径各画一帧、逐像素按位比对。
//
// 路径 A（被测 = 我们的翻译层）：
//   d3d12_xs.dll 的 D3D12XboxCreateDevice 返回的**设备代理**，并走它建模过的**主机 ABI 槽位**：
//     slot 8    CreateCommandQueue        主机形参 (this, desc, iid, out)，返回队列代理
//     slot 14   CreateDescriptorHeap      返回描述符堆代理；CPU/GPU 句柄是"RAX 返回"而不是 sret
//     slot 19   CreateUnorderedAccessView 主机五参 (this, res, counter, desc, dest[栈上]) —— 桌面只有四参
//     slot 25   GetResourceAllocationInfo 输出缓冲在 RCX、设备 this 在 RDX；剥掉 PC 不认的主机资源标志位
//     slot 27   CreateCommittedResource   同形转发（8 参 → 验证第 5 个以后的栈上参数不被截断）
//     slot 73   CreateCommandListX        主机描述符 {type,2,1,0} → 命令列表代理（SetDescriptorHeaps 会解包堆）
//     slot 127  CreateCommittedResource3  主机 desc1 + 主机资源标志位（0x20000），剥位后再交给桌面
//
// 路径 B（参照 = 厂商实现）：
//   LoadLibrary(System32\d3d12.dll) 的 D3D12CreateDevice + 标准 C++ ID3D12Device 接口，
//   用**完全相同的资源**（同样的常量缓冲、同样的顶点缓冲、同样的 16×16 R32_UINT render target、
//   同样的一块 UAV 缓冲）在同一块适配器（或 WARP）上画同一个三角形。
//
// 为什么标准答案来自桌面 d3d12.dll 而不是另一份我们的代码：
//   否则"我们的两个 bug 恰好互相抵消"会被判成通过。这台机器上没有主机（Xbox）着色器编译器，
//   所以两侧用同一个 dxc 编译同一份 HLSL —— 这条测试因此证明的是 **ABI/资源翻译的等价性**
//   与 **本机 GPU 的数值语义基线**，而不是编译器之间的差异（见 docs/diff-tests.md「已知边界」）。
//
// 判定：两边读回的 256 个 32 位像素值按**位模式**比较；浮点编码的用例允许 ≤1 ULP，
// 但只要出现 ±0 就不允许容差（±0 的符号有语义：1/x 的符号、min/max 的取舍靠它区分）。
// 不一致时打印用例名 + 前 4 个不同像素的 (x,y,got,expect)。
//
// 环境变量（照抄交接文档的约定）：
//   XBGTASIX_DIFF_ONLY=<case>   只跑一条用例（名字见 CASES 表；ABI 用例也能单独跑）
//   XBGTASIX_EMIT_DUMP=<dir>    把两侧的 DXIL 与读回的像素写进该目录（文本 + .cso）
//   XBGTASIX_WARP=1             用 WARP 设备（CI 无 GPU 也能跑）
//   XBGTASIX_DXC=<path>         dxc.exe 路径；缺省扫 C:\VulkanSDK\*\Bin\dxc.exe 与 PATH
//
// 退出码：0 = PASS，2 = SKIP（缺 dxc / 建不出桌面设备），其它 = FAIL。
// 输出：最后两行必须是 "<N> checks, <M> failures" 与 "done failures=<M> -> PASS|FAIL"。

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// 路径 A 的入口。D3D12XboxCreateDevice 是主机才有的导出，MinGW 头里没有声明。
// D3D12CreateDevice / D3D12SerializeVersionedRootSignature 同时由 d3d12_xs.dll 导出
// （d3d12_xs 里是手写转发）；路径 B 一律用 GetProcAddress 拿 System32\d3d12.dll 的，
// 保证"参照侧"确实绕开了我们的 shim。
extern "C" {
HRESULT WINAPI D3D12XboxCreateDevice(IUnknown* adapter, void* params, REFIID riid, void** device);
HRESULT WINAPI D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL min_level, REFIID riid,
                                 void** device);
HRESULT WINAPI D3D12SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc, ID3DBlob** blob, ID3DBlob** error);
}

namespace {

// ===========================================================================
// 统计与输出
// ===========================================================================
int g_checks = 0;
int g_failures = 0;

void printf_failure(const char* fmt, ...) {
    std::printf("[t44] FAIL ");
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    ++g_failures;
}

// 一条断言：成功后 +1 check，失败 +1 check +1 failure。
bool check(bool condition, const char* what) {
    ++g_checks;
    if (!condition) printf_failure("%s", what);
    return condition;
}

template <class T>
struct ComDeleter {
    void operator()(T* p) const {
        if (p) p->Release();
    }
};
template <class T>
using Com = std::unique_ptr<T, ComDeleter<T>>;

void** vtable_of(void* object) {
    void** vt = nullptr;
    std::memcpy(&vt, object, sizeof(vt));
    return vt;
}

// ===========================================================================
// 小工具：路径/文件/进程
// ===========================================================================
std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring env_wide(const wchar_t* name) {
    wchar_t buffer[32768];
    const DWORD n = GetEnvironmentVariableW(name, buffer, 32768);
    if (n == 0 || n >= 32768) return std::wstring();
    return std::wstring(buffer, n);
}

bool file_exists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool read_file(const std::wstring& path, std::vector<unsigned char>& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 28)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) != 0 &&
                    read == out.size();
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

bool write_file(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) != 0 &&
                    written == size;
    CloseHandle(h);
    return ok;
}

bool write_text(const std::wstring& path, const std::string& text) {
    return write_file(path, text.data(), text.size());
}

// 把 MSYS 风格（/c/...、/d/...）的路径换成 Windows 形式；MinGW 进程不认前者。
std::wstring normalize_path(const std::wstring& path) {
    if (path.size() >= 3 && path[0] == L'/' && path[2] == L'/' &&
        ((path[1] >= L'a' && path[1] <= L'z') || (path[1] >= L'A' && path[1] <= L'Z'))) {
        std::wstring out;
        out.push_back(static_cast<wchar_t>(path[1] - L'a' + L'A'));
        out += L":\\";
        for (size_t i = 3; i < path.size(); ++i)
            out.push_back(path[i] == L'/' ? L'\\' : path[i]);
        return out;
    }
    std::wstring out = path;
    for (auto& ch : out)
        if (ch == L'/') ch = L'\\';
    return out;
}

// 用 CreateProcessW 跑一个进程，stdout+stderr 落到日志文件；返回退出码。
bool run_process(const std::wstring& command_line, const std::wstring& log_path,
                 DWORD& exit_code) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) return false;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = (nul != INVALID_HANDLE_VALUE) ? nul : log;
    si.hStdOutput = log;
    si.hStdError = log;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
    buffer.push_back(L'\0');
    const BOOL started = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    CloseHandle(log);
    if (!started) return false;
    WaitForSingleObject(pi.hProcess, 120000);
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) exit_code = static_cast<DWORD>(-1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }

// GetProcAddress 返回 FARPROC；经过 void* 中转，避免 -Wcast-function-type 噪声。
template <class Fn>
Fn proc_address(HMODULE module, const char* name) {
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

// 找一个能写盘的临时目录：%TEMP% 在受限环境下可能建不了目录（沙箱/ACL），
// 退到 exe 所在目录、再退到当前目录。找不到就 SKIP —— 测试自己的 I/O 不该判 FAIL。
std::wstring make_work_dir() {
    const std::wstring suffix = L"xbgtasix_t44_" + std::to_wstring(GetCurrentProcessId());
    std::vector<std::wstring> bases;
    wchar_t temp_root[MAX_PATH + 1] = {0};
    if (GetTempPathW(MAX_PATH, temp_root) > 0) bases.emplace_back(temp_root);
    wchar_t exe_path[MAX_PATH + 1] = {0};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        std::wstring dir(exe_path);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) bases.push_back(dir.substr(0, slash + 1));
    }
    bases.emplace_back(L".\\");
    for (const auto& base : bases) {
        const std::wstring candidate = base + suffix;
        if (CreateDirectoryW(candidate.c_str(), nullptr)) return candidate;
    }
    return std::wstring();
}

// ===========================================================================
// dxc 探测（缺失 → SKIP，绝不 FAIL）
// ===========================================================================
std::wstring find_dxc(std::vector<std::wstring>& tried) {
    std::wstring from_env = normalize_path(env_wide(L"XBGTASIX_DXC"));
    if (!from_env.empty()) {
        if (file_exists(from_env)) return from_env;
        tried.push_back(from_env);
    }

    // C:\VulkanSDK\<ver>\Bin\dxc.exe，取版本号最大的那个（字符串序对 1.4.x 够用）
    std::vector<std::wstring> versions;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(L"C:\\VulkanSDK\\*", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) versions.emplace_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(versions.begin(), versions.end());
    for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
        std::wstring candidate = L"C:\\VulkanSDK\\" + *it + L"\\Bin\\dxc.exe";
        if (file_exists(candidate)) return candidate;
        tried.push_back(candidate);
    }

    wchar_t on_path[MAX_PATH * 2] = {0};
    if (SearchPathW(nullptr, L"dxc.exe", nullptr, MAX_PATH * 2, on_path, nullptr) != 0)
        return std::wstring(on_path);
    tried.emplace_back(L"dxc.exe (PATH)");
    return std::wstring();
}

// ===========================================================================
// 夹具：全部由公式生成，仓库里没有任何二进制夹具
//
// 输入表 256 项 = 16 个"难搞语义"家族 × 16 种派生。像素 (x,y) 取 gIn[y*16+x]，
// 4 个操作按 y>>2 分组，于是每个 op 都能看到全部 16 个家族。
// ===========================================================================
constexpr UINT kInputWords = 256;
constexpr UINT kInputBytes = kInputWords * sizeof(uint32_t);
constexpr UINT kCbWords = 4;                 // gOne / gHalf / 2 pad
constexpr UINT kCbBytes = kCbWords * sizeof(uint32_t);

uint32_t input_word(uint32_t i) {
    static const uint32_t kFamilies[16] = {
        0x7FC00000u,  // +qNaN
        0xFFC00001u,  // -NaN（带 payload）
        0x7F800000u,  // +Inf
        0xFF800000u,  // -Inf
        0x00000000u,  // +0
        0x80000000u,  // -0
        0x00000001u,  // +最小 denormal
        0x80000001u,  // -最小 denormal
        0x007FFFFFu,  // 最大 denormal
        0x3F7FFFFFu,  // 1.0 - 1ulp
        0x4B000001u,  // 2^23 + 2（丢精度用）
        0x4F000000u,  // 2^31
        0x3FC00000u,  // 1.5
        0xBF800000u,  // -1.0
        0x42FE0000u,  // 127.0
        0x42FF0000u,  // 127.5
    };
    uint32_t bits = kFamilies[i & 15u];
    if ((i >> 4) & 1u) {
        // 奇数行再乘一个非 1 倍率，制造"同一家族不同量级"的输入（NaN/Inf 仍是 NaN/Inf）
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        value *= 1.0f + static_cast<float>((i >> 4) & 7u) * 0.25f;
        std::memcpy(&bits, &value, sizeof(bits));
    }
    return bits;
}

std::vector<uint32_t> make_input_words() {
    std::vector<uint32_t> words(kInputWords, 0u);
    for (UINT i = 0; i < kInputWords; ++i) words[i] = input_word(i);
    return words;
}

// 常量缓冲内容：gOne/gHalf 是**运行期**常量，防止编译器把 x*1.0 折成 mov 后测不到东西。
std::vector<uint32_t> make_cb_words() {
    std::vector<uint32_t> words(kCbWords, 0u);
    const float one = 1.0f;
    const float half = 0.5f;
    std::memcpy(&words[0], &one, sizeof(one));
    std::memcpy(&words[1], &half, sizeof(half));
    return words;
}

// ===========================================================================
// CASES 表（每条 = 一个最小像素着色器片段 + 它测什么 + 为什么期望两侧一致）
//
// 共用前缀给出：常量缓冲 b0（gIn[260]）、UAV u0（gOut）、VS（顶点缓冲里的裁剪空间三角形）、
// 以及 PS 里的取数约定（像素 (x,y) → gIn[y*16+x]，op = (y>>2)&3）。
// ===========================================================================
const char* kPrelude = R"HLSL(
// 公共前缀。夹具由公式生成（见 main.cpp 的 input_word），shader 只做取数与分派。
//
// 输入表放在 StructuredBuffer 而不是常量缓冲的数组里，是因为 HLSL 常量缓冲的**数组元素
// 各自独占 16 字节槽位**（cbufferLoadLegacy 的寄存器索引 = 元素下标），可变下标会把
// 第 i 个元素读成第 4i 个。StructuredBuffer 的 stride 就是 StructureByteStride（这里 4），
// 没有这层隐含打包。常量缓冲仍保留，用来放运行期常量（gOne/gHalf），顺带覆盖 root CBV。
cbuffer Params : register(b0) { float gOne; float gHalf; uint gPad0; uint gPad1; };
StructuredBuffer<uint> gIn : register(t0);
RWStructuredBuffer<uint> gOut : register(u0);

struct VSInput  { float2 pos : POSITION; };
struct VSOutput { float4 pos : SV_Position; };

VSOutput VSMain(VSInput vin) {
    VSOutput o;
    o.pos = float4(vin.pos, 0.0f, 1.0f);
    return o;
}
)HLSL";

struct CaseDef {
    const char* name;
    const char* body;       // 追加在 kPrelude 之后的 HLSL（Calc + PSMain）
    bool ulp_tolerance;     // 结果按 f32 解释时允许 ≤1 ULP；整数/位模式用例必须完全相同
    const char* why;        // 期望两侧一致的理由；不一致说明什么（同时写进 docs/diff-tests.md）
};

const CaseDef kRenderCases[] = {
    {"nan_propagation",
     R"HLSL(
// NaN 经过 add / mul / min / max 后的传播与符号。
// 期望一致：两侧执行同一份 DXIL、同一块 GPU 的同一组指令；shim 只翻译资源与 ABI，
// 不碰着色器。不一致 ⇒ 我们给这条路径建出的资源/描述符与桌面不同（例如 UAV 视图错、
// 常量缓冲没绑上），而不是"NaN 语义不同"。
uint Calc(uint v, uint op, uint2 xy) {
    float x = asfloat(v);
    float r;
    if (op == 0u) r = x + 1.0f;
    else if (op == 1u) r = x * 2.0f;
    else if (op == 2u) r = min(x, 1.0f);    // IEEE minNum 与 HLSL min 的分歧点就在 NaN
    else r = max(x, -1.0f);
    return asuint(r);
}
uint PSMain(VSOutput i) : SV_Target {
    uint2 xy = (uint2)i.pos.xy;
    uint v = gIn[(xy.y * 16u + xy.x) & 255u];
    uint op = (xy.y >> 2) & 3u;
    uint r = Calc(v, op, xy);
    gOut[xy.y * 16u + xy.x] = r;
    return r;
}
)HLSL",
     true,
     "NaN/Inf 的传播与 min/max 取舍。两侧同一 DXIL、同一 GPU；不一致说明资源或绑定被翻译坏了。"},

    {"denormal_flush",
     R"HLSL(
// denormal 输入是否被 flush 成 0：记录**实际行为**，两侧必须一致。
// gOne/gHalf 来自常量缓冲（运行期值），避免编译器把 *1.0 折成 mov 后什么也测不到。
// op0 = x*1.0（恒等但不可折叠）、op1 = x+x、op2 = x*0.5、op3 = x*2.0。
// 期望一致：denormal 处理是驱动/硬件行为，两条路径在同一设备上应完全相同。
// 不一致 ⇒ 我们建资源时改了格式/描述，或两侧跑的根本不是同一块设备。
uint Calc(uint v, uint op, uint2 xy) {
    float x = asfloat(v);
    float r;
    if (op == 0u) r = x * gOne;
    else if (op == 1u) r = x + x;
    else if (op == 2u) r = x * gHalf;
    else r = x * 2.0f;
    return asuint(r);
}
uint PSMain(VSOutput i) : SV_Target {
    uint2 xy = (uint2)i.pos.xy;
    uint v = gIn[(xy.y * 16u + xy.x) & 255u];
    uint op = (xy.y >> 2) & 3u;
    uint r = Calc(v, op, xy);
    gOut[xy.y * 16u + xy.x] = r;
    return r;
}
)HLSL",
     true,
     "denormal 是否被 flush 的本机行为基线。两侧同一 GPU；不一致说明资源/格式被改过。"},

    {"signed_zero",
     R"HLSL(
// +0 与 -0 的位模式：op0 = 1/+0 与 1/-0 分别给 +Inf / -Inf（符号由 0 的符号决定），
// op1 = 乘 1.0（符号应保留）、op2 = x+0.0（IEEE：-0 + +0 = +0）、op3 = min(x, +0)。
// 期望一致：这是 IEEE-754 与 HLSL 语义里最容易被"优化"掉的部分；两侧同 DXIL 必然一致。
// 不一致 ⇒ 资源被翻译成了不同格式（例如 R32_UINT 变成 float RT），或绑定错位让 shader
// 读到了别的像素。本用例**不允许 ULP 容差**，因为 ±0 的差别正是要抓的东西。
uint Calc(uint v, uint op, uint2 xy) {
    float x = asfloat(v);
    float r;
    if (op == 0u) r = 1.0f / x;
    else if (op == 1u) r = x * gOne;
    else if (op == 2u) r = x + 0.0f;
    else r = min(x, 0.0f);
    return asuint(r);
}
uint PSMain(VSOutput i) : SV_Target {
    uint2 xy = (uint2)i.pos.xy;
    uint v = gIn[(xy.y * 16u + xy.x) & 255u];
    uint op = (xy.y >> 2) & 3u;
    uint r = Calc(v, op, xy);
    gOut[xy.y * 16u + xy.x] = r;
    return r;
}
)HLSL",
     false,
     "±0 的符号（1/x 的符号）。位必须完全相同；不一致说明格式或绑定被翻译错。"},

    {"round_sat",
     R"HLSL(
// 浮点→8 位整数的舍入与饱和边界（含 NaN / Inf / -1）：
// op0 = round 后 clamp 到 [0,255]、op1 = clamp 后截断、op2 = unorm8 编码、op3 = 有符号饱和。
// NaN 走 clamp/round/转整数都是"实现定义"，所以这里只要求**两侧一致**，不要求等于某个值。
// 期望一致：同一 DXIL、同一 GPU。不一致 ⇒ 资源/RTV 格式被改（例如 R32_UINT 被换成 UNORM）。
uint Calc(uint v, uint op, uint2 xy) {
    float x = asfloat(v);
    if (op == 0u) {
        float c = clamp(round(x), 0.0f, 255.0f);
        return (uint)c;
    } else if (op == 1u) {
        float c = clamp(x, 0.0f, 255.0f);
        return (uint)c;
    } else if (op == 2u) {
        float c = saturate(x) * 255.0f + 0.5f;
        return (uint)min(c, 255.0f);
    } else {
        int t = (int)x;
        t = max(-128, min(127, t));
        return (uint)t & 0xFFu;
    }
}
uint PSMain(VSOutput i) : SV_Target {
    uint2 xy = (uint2)i.pos.xy;
    uint v = gIn[(xy.y * 16u + xy.x) & 255u];
    uint op = (xy.y >> 2) & 3u;
    uint r = Calc(v, op, xy);
    gOut[xy.y * 16u + xy.x] = r;
    return r;
}
)HLSL",
     false,
     "浮点→8 位整数转换的舍入/饱和/NaN 边界。整数结果要求位完全相同。"},

    {"bitfield_oob",
     R"HLSL(
// 位域/移位越界：(x >> 40) & 0xFFFF、算术右移 33、左移 40、以及范围内的 (x>>8)&0xFF 作对照。
// HLSL 规范把"移位量 ≥ 位宽"留成未定义，实际由编译后的指令决定（多数硬件按 5 位掩码）。
// 期望一致：两侧同一 DXIL，所以越界取值必须一样。不一致 ⇒ 输入数据被翻译错
// （例如常量缓冲的 stride/偏移不对），而不是"移位语义不同"。
uint Calc(uint v, uint op, uint2 xy) {
    uint x = v;
    if (op == 0u) return (x >> 40u) & 0xFFFFu;
    else if (op == 1u) return asuint(asint(x) >> 33u) & 0xFFFFu;
    else if (op == 2u) return (x << 40u) & 0xFFFFu;
    else return (x >> 8u) & 0xFFu;
}
uint PSMain(VSOutput i) : SV_Target {
    uint2 xy = (uint2)i.pos.xy;
    uint v = gIn[(xy.y * 16u + xy.x) & 255u];
    uint op = (xy.y >> 2) & 3u;
    uint r = Calc(v, op, xy);
    gOut[xy.y * 16u + xy.x] = r;
    return r;
}
)HLSL",
     false,
     "移位越界的实际取值（规范未定义）。记录本机行为；两侧必须一致。"},

    {"precision",
     R"HLSL(
// 大数相加丢精度、以及 mad（可融合）与分步乘加的差别：
// op0 = mad(x,k,1)、op1 = x*k+1（dxc 可能收缩成 fma，也可能不）、
// op2 = (x+2^23)-2^23（经典丢精度）、op3 = mad(x,3,1e-8)-3x（灾难性抵消）。
// 期望一致：两侧同一 DXIL、同一驱动 JIT。注意 dxc 1.4 把 op0/op1 都编成
// "fmul fast + fadd fast"，最终是否收缩由驱动决定 —— 这条用例记录的是那个决定。
// 不一致 ⇒ 常量缓冲内容/偏移被翻译错。
uint Calc(uint v, uint op, uint2 xy) {
    float x = asfloat(v);
    if (op == 0u) return asuint(mad(x, 1.0000001f, 1.0f));
    else if (op == 1u) { float a = x * 1.0000001f; return asuint(a + 1.0f); }
    else if (op == 2u) { float b = x + 8388608.0f; return asuint(b - 8388608.0f); }
    else { float c = mad(x, 3.0f, 1.0e-8f); return asuint(c - 3.0f * x); }
}
uint PSMain(VSOutput i) : SV_Target {
    uint2 xy = (uint2)i.pos.xy;
    uint v = gIn[(xy.y * 16u + xy.x) & 255u];
    uint op = (xy.y >> 2) & 3u;
    uint r = Calc(v, op, xy);
    gOut[xy.y * 16u + xy.x] = r;
    return r;
}
)HLSL",
     true,
     "大数丢精度 + fma/分步乘加。两侧同一 DXIL；不一致说明常量缓冲绑定被翻译错。"},
};

constexpr size_t kRenderCaseCount = sizeof(kRenderCases) / sizeof(kRenderCases[0]);

// ===========================================================================
// 资源描述与常量
// ===========================================================================
constexpr UINT kSize = 16;                        // 16×16 render target（交接文档的约定）
constexpr UINT kPixels = kSize * kSize;
constexpr UINT kRtRowPitch = 256;                 // D3D12 要求 RowPitch 256 对齐（16px×4B=64）
constexpr UINT kRtBytes = kRtRowPitch * kSize;    // 4096
constexpr UINT kUavBytes = kPixels * sizeof(uint32_t);
constexpr UINT kUavOffset = kRtBytes;             // 4096，满足 512 对齐
constexpr UINT kReadbackSize = kRtBytes + kUavBytes;

D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{};
    p.Type = type;
    p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

D3D12_RESOURCE_DESC texture_desc(UINT width, UINT height, DXGI_FORMAT format,
                                 D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = flags;
    return d;
}

// ===========================================================================
// 一条"路径"：设备 + 它支持哪套设备 ABI
// ===========================================================================
using SerializeFn = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**,
                                     ID3DBlob**);
using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
// 主机槽 25：输出缓冲（隐藏返回缓冲）在 RCX，设备 this 在 RDX
using AllocInfoXFn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*,
                                                              void*, UINT, UINT,
                                                              const D3D12_RESOURCE_DESC*);
// 主机槽 19：五参（多一个 counter，dest 落到栈上第 5 个参数）
using UavXFn = HRESULT(WINAPI*)(void*, ID3D12Resource*, ID3D12Resource*,
                                const D3D12_UNORDERED_ACCESS_VIEW_DESC*,
                                D3D12_CPU_DESCRIPTOR_HANDLE);
// 主机槽 127：CreateCommittedResource3（11 参）
using CreateResource3XFn = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, UINT,
                                            const D3D12_RESOURCE_DESC1*, UINT,
                                            const D3D12_CLEAR_VALUE*, void*, UINT, const void*,
                                            REFIID, void**);
// 主机槽 73：CreateCommandListX
using CreateListXFn = HRESULT(WINAPI*)(void*, const void*, ID3D12CommandAllocator*,
                                      ID3D12PipelineState*, const GUID*, void**);

struct Side {
    const char* name = "";
    bool host_abi = false;   // true = d3d12_xs 设备代理（主机 ABI 槽位）
    ID3D12Device* dev = nullptr;
    void** vtable = nullptr; // host_abi 时 = 代理虚表
    SerializeFn serialize = nullptr;
};

D3D12_CPU_DESCRIPTOR_HANDLE heap_cpu_handle(const Side& side, ID3D12DescriptorHeap* heap) {
    if (!side.host_abi) return heap->GetCPUDescriptorHandleForHeapStart();
    // 主机 ABI：句柄在 RAX 里返回（只有 heap 走 RCX）。MinGW 的 C++ 接口用的是
    // "聚集返回走隐藏缓冲"（sret），直接调会把代理解释成 sret 缓冲，所以必须走裸虚表。
    // 注意用的是**堆自己的**虚表（堆代理的 slot 9），不是设备虚表。
    using Fn = SIZE_T(WINAPI*)(void*);
    D3D12_CPU_DESCRIPTOR_HANDLE h{};
    h.ptr = reinterpret_cast<Fn>(vtable_of(heap)[9])(heap);
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE heap_gpu_handle(const Side& side, ID3D12DescriptorHeap* heap) {
    if (!side.host_abi) return heap->GetGPUDescriptorHandleForHeapStart();
    using Fn = UINT64(WINAPI*)(void*);
    D3D12_GPU_DESCRIPTOR_HANDLE h{};
    h.ptr = reinterpret_cast<Fn>(vtable_of(heap)[10])(heap);
    return h;
}

HRESULT create_uav_view(const Side& side, ID3D12Resource* resource,
                        const D3D12_UNORDERED_ACCESS_VIEW_DESC& desc,
                        D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    if (side.host_abi) {
        return reinterpret_cast<UavXFn>(side.vtable[19])(side.dev, resource, nullptr, &desc, dest);
    }
    side.dev->CreateUnorderedAccessView(resource, nullptr, &desc, dest);
    return S_OK;
}

D3D12_RESOURCE_ALLOCATION_INFO get_alloc_info(const Side& side, UINT visible_mask, UINT count,
                                              const D3D12_RESOURCE_DESC* descs) {
    if (side.host_abi) {
        D3D12_RESOURCE_ALLOCATION_INFO info{};
        reinterpret_cast<AllocInfoXFn>(side.vtable[25])(&info, side.dev, visible_mask, count, descs);
        return info;
    }
    return side.dev->GetResourceAllocationInfo(visible_mask, count, descs);
}

HRESULT create_committed(const Side& side, const D3D12_HEAP_PROPERTIES& props,
                         D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC& desc,
                         D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                         ID3D12Resource** out) {
    // 两侧走同一个桌面形态签名；对 shim 来说这落在槽 27 的同形转发上，
    // 第 5 个参数（state）之后全在栈上，正好验证转发不截断栈参数。
    return side.dev->CreateCommittedResource(&props, flags, &desc, state, clear,
                                             __uuidof(ID3D12Resource),
                                             reinterpret_cast<void**>(out));
}

HRESULT create_command_list(const Side& side, D3D12_COMMAND_LIST_TYPE type,
                            ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso,
                            ID3D12GraphicsCommandList** out) {
    if (!side.host_abi) {
        return side.dev->CreateCommandList(0, type, allocator, pso,
                                           __uuidof(ID3D12GraphicsCommandList),
                                           reinterpret_cast<void**>(out));
    }
    // 主机槽 73：描述符 {type, 2, 1, 0}（实测值）。走代理列表是为了让 SetDescriptorHeaps
    // 把描述符堆代理解包成真堆 —— 那正是我们的 slot 19 描述符被真正用起来的唯一合法路径。
    const UINT desc[4] = {static_cast<UINT>(type), 2u, 1u, 0u};
    return reinterpret_cast<CreateListXFn>(side.vtable[73])(side.dev, desc, allocator, pso,
                                                            &__uuidof(ID3D12GraphicsCommandList),
                                                            reinterpret_cast<void**>(out));
}

// ===========================================================================
// 一帧的渲染：建资源 → 录命令 → 提交 → 回读
// ===========================================================================
struct Frame {
    std::vector<uint32_t> rt;
    std::vector<uint32_t> uav;
};

struct Steps {
    std::string err;
    bool ok = true;

    bool h(HRESULT hr, const char* what) {
        if (FAILED(hr)) {
            char buffer[192];
            std::snprintf(buffer, sizeof(buffer), "%s(hr=0x%08lx) ", what,
                          static_cast<unsigned long>(hr));
            err += buffer;
            ok = false;
            return false;
        }
        return true;
    }
    bool req(bool condition, const char* what) {
        if (!condition) {
            err += what;
            err += ' ';
            ok = false;
        }
        return condition;
    }
};

struct RenderOutcome {
    Frame frame;
    std::string error;
};

RenderOutcome render_frame(const Side& side, const std::vector<unsigned char>& vs_blob,
                           const std::vector<unsigned char>& ps_blob,
                           const std::vector<uint32_t>& input_words,
                           const std::vector<uint32_t>& cb_words) {
    RenderOutcome outcome;
    Steps st;
    ID3D12Device* dev = side.dev;

    // ---- root signature：[0] root CBV b0，[1] 描述符表 {SRV t0, UAV u0} ----
    Com<ID3D12RootSignature> root;
    Com<ID3D12PipelineState> pso;
    {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].RegisterSpace = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
        rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        rs.Desc_1_0.NumParameters = 2;
        rs.Desc_1_0.pParameters = params;
        rs.Desc_1_0.NumStaticSamplers = 0;
        rs.Desc_1_0.pStaticSamplers = nullptr;
        rs.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* sig = nullptr;
        ID3DBlob* rs_error = nullptr;
        const HRESULT hr = side.serialize(&rs, &sig, &rs_error);
        if (rs_error) {
            if (FAILED(hr))
                st.err += std::string("root signature: ") +
                          static_cast<const char*>(rs_error->GetBufferPointer()) + ' ';
            rs_error->Release();
        }
        st.h(hr, "serialize root signature");
        if (sig) {
            ID3D12RootSignature* created = nullptr;
            st.h(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                          __uuidof(ID3D12RootSignature),
                                          reinterpret_cast<void**>(&created)),
                 "CreateRootSignature");
            sig->Release();
            root.reset(created);
        }
    }

    // ---- PSO：全屏三角形 + R32_UINT RT，关混合（PS 要写 UAV） ----
    if (root) {
        static const D3D12_INPUT_ELEMENT_DESC kInputElements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
             0},
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root.get();
        pd.VS = {vs_blob.data(), vs_blob.size()};
        pd.PS = {ps_blob.data(), ps_blob.size()};
        pd.StreamOutput = {};
        pd.BlendState.AlphaToCoverageEnable = FALSE;
        pd.BlendState.IndependentBlendEnable = FALSE;
        pd.BlendState.RenderTarget[0].BlendEnable = FALSE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.FrontCounterClockwise = FALSE;
        pd.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        pd.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        pd.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.RasterizerState.MultisampleEnable = FALSE;
        pd.RasterizerState.AntialiasedLineEnable = FALSE;
        pd.RasterizerState.ForcedSampleCount = 0;
        pd.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;
        pd.InputLayout = {kInputElements, 1};
        pd.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
        pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
        pd.SampleDesc.Count = 1;
        pd.SampleDesc.Quality = 0;
        pd.NodeMask = 0;
        pd.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        ID3D12PipelineState* created = nullptr;
        st.h(dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState),
                                             reinterpret_cast<void**>(&created)),
             "CreateGraphicsPipelineState");
        pso.reset(created);
    }

    // ---- 顶点缓冲（UPLOAD 堆直接当 VB；顶点坐标由公式给出） ----
    Com<ID3D12Resource> vb;
    {
        const float vertices[3][2] = {{-1.0f, -1.0f}, {3.0f, -1.0f}, {-1.0f, 3.0f}};
        ID3D12Resource* created = nullptr;
        const D3D12_RESOURCE_DESC desc = buffer_desc(sizeof(vertices), D3D12_RESOURCE_FLAG_NONE);
        if (st.h(create_committed(side, heap_props(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
                                  desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, &created),
                 "CreateCommittedResource(vb)")) {
            void* mapped = nullptr;
            if (st.h(created->Map(0, nullptr, &mapped), "Map(vb)") && mapped)
                std::memcpy(mapped, vertices, sizeof(vertices));
            created->Unmap(0, nullptr);
            vb.reset(created);
        }
    }

    // ---- 常量缓冲：gOne / gHalf（运行期常量） ----
    Com<ID3D12Resource> cb;
    if (vb) {
        ID3D12Resource* created = nullptr;
        const D3D12_RESOURCE_DESC desc = buffer_desc(kCbBytes, D3D12_RESOURCE_FLAG_NONE);
        if (st.h(create_committed(side, heap_props(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
                                  desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, &created),
                 "CreateCommittedResource(cb)")) {
            void* mapped = nullptr;
            if (st.h(created->Map(0, nullptr, &mapped), "Map(cb)") && mapped) {
                std::memcpy(mapped, cb_words.data(), kCbBytes);
            }
            created->Unmap(0, nullptr);
            cb.reset(created);
        }
    }

    // ---- 输入夹具：256 个公式生成的 u32，作为 StructuredBuffer（stride 4，无打包歧义） ----
    Com<ID3D12Resource> inputs;
    if (cb) {
        ID3D12Resource* created = nullptr;
        const D3D12_RESOURCE_DESC desc = buffer_desc(kInputBytes, D3D12_RESOURCE_FLAG_NONE);
        if (st.h(create_committed(side, heap_props(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
                                  desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, &created),
                 "CreateCommittedResource(gIn)")) {
            void* mapped = nullptr;
            if (st.h(created->Map(0, nullptr, &mapped), "Map(gIn)") && mapped)
                std::memcpy(mapped, input_words.data(), kInputBytes);
            created->Unmap(0, nullptr);
            inputs.reset(created);
        }
    }

    // ---- 16×16 R32_UINT render target ----
    Com<ID3D12Resource> rt;
    if (inputs) {
        ID3D12Resource* created = nullptr;
        const D3D12_RESOURCE_DESC desc =
            texture_desc(kSize, kSize, DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        st.h(create_committed(side, heap_props(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE, desc,
                              D3D12_RESOURCE_STATE_COMMON, nullptr, &created),
             "CreateCommittedResource(rt)");
        rt.reset(created);
    }

    // ---- UAV 缓冲：大小取自槽 25/桌面的 GetResourceAllocationInfo（功能性地用上那个 ABI） ----
    Com<ID3D12Resource> uav;
    if (rt) {
        D3D12_RESOURCE_DESC desc = buffer_desc(kUavBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const D3D12_RESOURCE_ALLOCATION_INFO info = get_alloc_info(side, 0, 1, &desc);
        // SizeInBytes 是"对齐后的真实占用"；0 或 0xFFFF... 说明这个 ABI 没翻对，
        // 这时**不要**悄悄退回请求大小，否则会把翻译错误掩盖成"跑得通"。
        if (st.req(info.SizeInBytes != 0 && info.SizeInBytes != UINT64_MAX,
                   "GetResourceAllocationInfo 返回的 SizeInBytes 不可用")) {
            if (info.SizeInBytes > desc.Width) desc.Width = info.SizeInBytes;
            ID3D12Resource* created = nullptr;
            st.h(create_committed(side, heap_props(D3D12_HEAP_TYPE_DEFAULT), D3D12_HEAP_FLAG_NONE,
                                  desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, &created),
                 "CreateCommittedResource(uav)");
            uav.reset(created);
        }
    }

    // ---- 回读缓冲 ----
    Com<ID3D12Resource> readback;
    if (uav) {
        ID3D12Resource* created = nullptr;
        const D3D12_RESOURCE_DESC desc = buffer_desc(kReadbackSize, D3D12_RESOURCE_FLAG_NONE);
        st.h(create_committed(side, heap_props(D3D12_HEAP_TYPE_READBACK), D3D12_HEAP_FLAG_NONE, desc,
                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr, &created),
             "CreateCommittedResource(readback)");
        readback.reset(created);
    }

    // ---- 描述符堆：RTV 1 项 + 着色器可见 {SRV,UAV} 2 项（走槽 14） ----
    Com<ID3D12DescriptorHeap> rtv_heap;
    Com<ID3D12DescriptorHeap> view_heap;
    if (readback) {
        const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                                  D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        const D3D12_DESCRIPTOR_HEAP_DESC view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                                   D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        ID3D12DescriptorHeap* created = nullptr;
        if (st.h(dev->CreateDescriptorHeap(&rtv_desc, __uuidof(ID3D12DescriptorHeap),
                                          reinterpret_cast<void**>(&created)),
                 "CreateDescriptorHeap(rtv)"))
            rtv_heap.reset(created);
        created = nullptr;
        if (st.h(dev->CreateDescriptorHeap(&view_desc, __uuidof(ID3D12DescriptorHeap),
                                          reinterpret_cast<void**>(&created)),
                 "CreateDescriptorHeap(srv/uav)"))
            view_heap.reset(created);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle{};
    if (rtv_heap && rt) {
        rtv_handle = heap_cpu_handle(side, rtv_heap.get());
        st.req(rtv_handle.ptr != 0, "RTV heap CPU handle is null");
        dev->CreateRenderTargetView(rt.get(), nullptr, rtv_handle);
    }

    // SRV=槽 0、UAV=槽 1 的描述符表；UAV 视图走主机槽 19（桌面只有四参，主机有五参）
    D3D12_GPU_DESCRIPTOR_HANDLE table_gpu_handle{};
    if (view_heap && inputs && uav) {
        const UINT stride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        st.req(stride != 0, "descriptor increment size is zero");
        const D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = heap_cpu_handle(side, view_heap.get());
        D3D12_CPU_DESCRIPTOR_HANDLE uav_cpu = srv_cpu;
        uav_cpu.ptr += stride;
        st.req(srv_cpu.ptr != 0, "view heap CPU handle is null");

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = 0;
        srv.Buffer.NumElements = kInputWords;
        srv.Buffer.StructureByteStride = sizeof(uint32_t);
        srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        dev->CreateShaderResourceView(inputs.get(), &srv, srv_cpu);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_view{};
        uav_view.Format = DXGI_FORMAT_UNKNOWN;
        uav_view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav_view.Buffer.FirstElement = 0;
        uav_view.Buffer.NumElements = kPixels;
        uav_view.Buffer.StructureByteStride = sizeof(uint32_t);
        uav_view.Buffer.CounterOffsetInBytes = 0;
        uav_view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        st.h(create_uav_view(side, uav.get(), uav_view, uav_cpu), "CreateUnorderedAccessView");

        table_gpu_handle = heap_gpu_handle(side, view_heap.get());
        st.req(table_gpu_handle.ptr != 0, "view heap GPU handle is null");
    }

    // ---- 队列/分配器/列表/栅栏 ----
    Com<ID3D12CommandQueue> queue;
    Com<ID3D12CommandAllocator> allocator;
    Com<ID3D12GraphicsCommandList> list;
    Com<ID3D12Fence> fence;
    if (table_gpu_handle.ptr != 0) {
        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queue_desc.Priority = 0;
        queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queue_desc.NodeMask = 0;
        {
            ID3D12CommandQueue* created = nullptr;
            if (st.h(dev->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue),
                                            reinterpret_cast<void**>(&created)),
                     "CreateCommandQueue"))
                queue.reset(created);
        }
        {
            ID3D12CommandAllocator* created = nullptr;
            if (st.h(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 __uuidof(ID3D12CommandAllocator),
                                                 reinterpret_cast<void**>(&created)),
                     "CreateCommandAllocator"))
                allocator.reset(created);
        }
        {
            ID3D12GraphicsCommandList* created = nullptr;
            if (allocator && st.h(create_command_list(side, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      allocator.get(), pso.get(), &created),
                                  "CreateCommandList"))
                list.reset(created);
        }
        {
            ID3D12Fence* created = nullptr;
            if (st.h(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                     reinterpret_cast<void**>(&created)),
                     "CreateFence"))
                fence.reset(created);
        }
    }

    // ---- 录制 ----
    if (queue && list && fence && vb && cb && rt && uav && readback) {
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(kSize),
                                      static_cast<float>(kSize), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize)};
        const D3D12_VERTEX_BUFFER_VIEW vb_view{vb->GetGPUVirtualAddress(),
                                               static_cast<UINT>(3 * sizeof(float) * 2),
                                               static_cast<UINT>(sizeof(float) * 2)};

        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[0].Transition.pResource = rt.get();
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barriers[1].Transition.pResource = uav.get();
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        // 先只把 RT 转成 RENDER_TARGET；UAV 保持 UNORDERED_ACCESS 给 PS 写
        list->ResourceBarrier(1, &barriers[0]);

        ID3D12DescriptorHeap* heaps[] = {view_heap.get()};
        list->SetDescriptorHeaps(1, heaps);
        list->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetGraphicsRootSignature(root.get());
        list->SetPipelineState(pso.get());
        list->SetGraphicsRootConstantBufferView(0, cb->GetGPUVirtualAddress());
        list->SetGraphicsRootDescriptorTable(1, table_gpu_handle);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &vb_view);
        list->DrawInstanced(3, 1, 0, 0);

        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barriers[0]);
        list->ResourceBarrier(1, &barriers[1]);

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = rt.get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_UINT;
        dst.PlacedFootprint.Footprint.Width = kSize;
        dst.PlacedFootprint.Footprint.Height = kSize;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = kRtRowPitch;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        list->CopyBufferRegion(readback.get(), kUavOffset, uav.get(), 0, kUavBytes);

        st.h(list->Close(), "CommandList::Close");
        ID3D12CommandList* lists[] = {list.get()};
        queue->ExecuteCommandLists(1, lists);
        st.h(queue->Signal(fence.get(), 1), "CommandQueue::Signal");
        HANDLE event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event_handle) {
            st.h(fence->SetEventOnCompletion(1, event_handle), "SetEventOnCompletion");
            if (WaitForSingleObject(event_handle, 30000) != WAIT_OBJECT_0)
                st.req(false, "GPU fence timeout");
            CloseHandle(event_handle);
        } else {
            st.req(false, "CreateEvent failed");
        }

        if (st.ok) {
            void* mapped = nullptr;
            D3D12_RANGE read_range{0, kReadbackSize};
            if (st.h(readback->Map(0, &read_range, &mapped), "Map(readback)") && mapped) {
                outcome.frame.rt.assign(kPixels, 0u);
                outcome.frame.uav.assign(kPixels, 0u);
                const auto* bytes = static_cast<const uint8_t*>(mapped);
                for (UINT y = 0; y < kSize; ++y) {
                    std::memcpy(outcome.frame.rt.data() + y * kSize,
                                bytes + y * kRtRowPitch, kSize * sizeof(uint32_t));
                }
                std::memcpy(outcome.frame.uav.data(), bytes + kUavOffset, kUavBytes);
                const D3D12_RANGE written_range{0, 0};
                readback->Unmap(0, &written_range);
            }
        }
    }

    outcome.error = st.err;
    return outcome;
}

// ===========================================================================
// 比对：位模式 + ≤1 ULP（±0 永不容差）
// ===========================================================================
int64_t ulp_key(uint32_t bits) {
    return (bits & 0x80000000u) ? -static_cast<int64_t>(bits & 0x7FFFFFFFu)
                                : static_cast<int64_t>(bits & 0x7FFFFFFFu);
}

// 浮点"类别"。ULP 容差必须在同类之间才成立：+Inf 加 1 ULP 就变成 NaN，
// 用 ULP 距离去容忍"Inf vs NaN"会把最严重的语义偏差当噪声放过去。
enum FloatClass { kFinite = 0, kInfinity, kNaN };
FloatClass float_class(uint32_t bits) {
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    const uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent != 0xFFu) return kFinite;
    return mantissa ? kNaN : kInfinity;
}

bool bits_close(uint32_t got, uint32_t expect, bool allow_ulp) {
    if (got == expect) return true;
    if (!allow_ulp) return false;
    // ±0 的符号有语义（1/x 的符号、min/max 的取舍），永远要求位相同
    if (got == 0x00000000u || got == 0x80000000u || expect == 0x00000000u ||
        expect == 0x80000000u)
        return false;
    if (float_class(got) != float_class(expect)) return false;
    int64_t distance = ulp_key(got) - ulp_key(expect);
    if (distance < 0) distance = -distance;
    return distance <= 1;
}

// 比对器自己也要有断言：容差策略写错的差分测试比没有差分测试更危险。
void check_comparator() {
    check(bits_close(0x3F800000u, 0x3F800000u, true), "comparator: identical bits must match");
    check(bits_close(0x3F800000u, 0x3F800001u, true), "comparator: 1 ULP must be tolerated");
    check(!bits_close(0x3F800000u, 0x3F800002u, true), "comparator: 2 ULP must be rejected");
    check(!bits_close(0x00000000u, 0x80000000u, true), "comparator: +0 vs -0 must be rejected");
    check(!bits_close(0x7F800000u, 0x7F800001u, true), "comparator: +Inf vs NaN must be rejected");
    check(!bits_close(0x3F800000u, 0x3F800001u, false),
          "comparator: integer/bit-pattern cases must be exact");
}

float as_float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

UINT compare_pixels(const char* case_name, const char* buffer_name, const std::vector<uint32_t>& got,
                    const std::vector<uint32_t>& expect, bool allow_ulp) {
    if (got.size() != kPixels || expect.size() != kPixels) {
        printf_failure("%s/%s: 回读长度不对（got=%llu expect=%llu，应为 %u）", case_name,
                       buffer_name, static_cast<unsigned long long>(got.size()),
                       static_cast<unsigned long long>(expect.size()), kPixels);
        return 1;
    }
    UINT mismatches = 0;
    UINT shown = 0;
    for (UINT i = 0; i < kPixels; ++i) {
        if (bits_close(got[i], expect[i], allow_ulp)) continue;
        ++mismatches;
        if (shown < 4) {
            const UINT x = i % kSize;
            const UINT y = i / kSize;
            std::printf(
                "[t44]   %s/%s pixel (%u,%u) got=0x%08x expect=0x%08x  (%g vs %g)\n", case_name,
                buffer_name, x, y, got[i], expect[i], static_cast<double>(as_float(got[i])),
                static_cast<double>(as_float(expect[i])));
            ++shown;
        }
    }
    return mismatches;
}

// ===========================================================================
// 两个额外的 ABI 数值用例（不是像素，而是"翻译出来的值"）
// ===========================================================================
constexpr D3D12_RESOURCE_FLAGS kHostOnlyResourceFlag = static_cast<D3D12_RESOURCE_FLAGS>(0x20000);

// 槽 25：把同一个资源（含只带主机标志位的版本）交给两侧，比较 SizeInBytes/Alignment。
// 期望一致：shim 先剥掉 0x20000 再问桌面，等价于参照侧直接问"剥干净的描述"。
// 不一致 ⇒ 剥位逻辑漏了/剥多了，或者参数位置拿错（输出缓冲与 this 交换）。
bool case_alloc_info(const Side& shim, const Side& desktop) {
    struct Probe {
        const char* what;
        D3D12_RESOURCE_DESC host_desc;
        D3D12_RESOURCE_DESC pc_desc;
    };
    Probe probes[3];
    probes[0].what = "buffer(1024, no host flag)";
    probes[0].host_desc = buffer_desc(1024, D3D12_RESOURCE_FLAG_NONE);
    probes[0].host_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_NONE);
    probes[0].pc_desc = probes[0].host_desc;

    probes[1].what = "buffer(1024, host flag 0x20004)";
    probes[1].host_desc = buffer_desc(1024, D3D12_RESOURCE_FLAG_NONE);
    probes[1].host_desc.Flags =
        static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) |
        kHostOnlyResourceFlag;
    probes[1].pc_desc = buffer_desc(1024, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    probes[2].what = "texture2d(16x16 R32_UINT, host flag 0x20001)";
    probes[2].host_desc = texture_desc(kSize, kSize, DXGI_FORMAT_R32_UINT,
                                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    probes[2].host_desc.Flags =
        static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) |
        kHostOnlyResourceFlag;
    probes[2].pc_desc =
        texture_desc(kSize, kSize, DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

    bool all_ok = true;
    for (const auto& probe : probes) {
        const D3D12_RESOURCE_ALLOCATION_INFO shim_info = get_alloc_info(shim, 0, 1, &probe.host_desc);
        const D3D12_RESOURCE_ALLOCATION_INFO pc_info = get_alloc_info(desktop, 0, 1, &probe.pc_desc);
        const bool same = shim_info.SizeInBytes == pc_info.SizeInBytes &&
                          shim_info.Alignment == pc_info.Alignment;
        if (!check(same, probe.what)) {
            std::printf(
                "[t44]   alloc_info %s: shim size=%llu align=0x%llx / desktop size=%llu "
                "align=0x%llx\n",
                probe.what, static_cast<unsigned long long>(shim_info.SizeInBytes),
                static_cast<unsigned long long>(shim_info.Alignment),
                static_cast<unsigned long long>(pc_info.SizeInBytes),
                static_cast<unsigned long long>(pc_info.Alignment));
            all_ok = false;
        }
        // 诊断：把主机原始描述直接喂给桌面会是什么答案 —— 用来证明"剥位"这一步不是装饰。
        const D3D12_RESOURCE_ALLOCATION_INFO raw = get_alloc_info(desktop, 0, 1, &probe.host_desc);
        if (raw.SizeInBytes != pc_info.SizeInBytes) {
            std::printf("[t44]   注：把带主机位的原始描述直接给桌面会得到 size=%llu（0xFFFF… 表示"
                        "描述被拒），所以剥位是必需的\n",
                        static_cast<unsigned long long>(raw.SizeInBytes));
        }
    }
    return all_ok;
}

// 槽 127：主机 desc1（带 0x20000）+ 主机 layout 3 → 桌面 desc1（剥干净）+ UAV 初始状态。
// 期望一致：剥位后建出来的资源在 GetDesc() 上与参照侧逐字段相同；建不出来就是剥位坏了。
bool case_host_flag_strip(const Side& shim, const Side& desktop) {
    // 参照侧：D3D12_RESOURCE_DESC（desc1 的前缀完全同构）
    const D3D12_RESOURCE_DESC pc_desc =
        buffer_desc(4096, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Com<ID3D12Resource> pc_resource;
    {
        ID3D12Resource* created = nullptr;
        const HRESULT hr = create_committed(desktop, heap_props(D3D12_HEAP_TYPE_DEFAULT),
                                            D3D12_HEAP_FLAG_NONE, pc_desc,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                            &created);
        if (!check(SUCCEEDED(hr), "host_flag_strip: desktop reference resource creation")) {
            std::printf("[t44]   desktop CreateCommittedResource hr=0x%08lx\n",
                        static_cast<unsigned long>(hr));
            return false;
        }
        pc_resource.reset(created);
    }

    // 被测侧：主机 ABI（desc1 + 主机资源标志 + layout=3）
    D3D12_RESOURCE_DESC1 host_desc1{};
    host_desc1.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    host_desc1.Width = 4096;
    host_desc1.Height = 1;
    host_desc1.DepthOrArraySize = 1;
    host_desc1.MipLevels = 1;
    host_desc1.Format = DXGI_FORMAT_UNKNOWN;
    host_desc1.SampleDesc.Count = 1;
    host_desc1.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    host_desc1.Flags = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) |
                       kHostOnlyResourceFlag;

    Com<ID3D12Resource> shim_resource;
    {
        ID3D12Resource* created = nullptr;
        const HRESULT hr = reinterpret_cast<CreateResource3XFn>(shim.vtable[127])(
            shim.dev, nullptr /* heapProps: shim 自己补 DEFAULT */, 0u /* heapFlags */, &host_desc1,
            3u /* 主机 layout: UNORDERED_ACCESS */, nullptr, nullptr, 0u, nullptr,
            __uuidof(ID3D12Resource), reinterpret_cast<void**>(&created));
        if (!check(SUCCEEDED(hr) && created != nullptr,
                   "host_flag_strip: shim slot 127 resource creation")) {
            std::printf("[t44]   slot 127 hr=0x%08lx out=%p\n", static_cast<unsigned long>(hr),
                        static_cast<void*>(created));
            return false;
        }
        shim_resource.reset(created);
    }

    const D3D12_RESOURCE_DESC got = shim_resource->GetDesc();
    const D3D12_RESOURCE_DESC want = pc_resource->GetDesc();
    const bool same = got.Dimension == want.Dimension && got.Width == want.Width &&
                      got.Height == want.Height &&
                      got.DepthOrArraySize == want.DepthOrArraySize &&
                      got.MipLevels == want.MipLevels && got.Format == want.Format &&
                      got.SampleDesc.Count == want.SampleDesc.Count &&
                      got.Layout == want.Layout && got.Flags == want.Flags;
    if (!check(same, "host_flag_strip: shim resource desc must match the stripped desktop desc")) {
        std::printf(
            "[t44]   shim desc: dim=%d w=%llu fmt=%d flags=0x%x | desktop: dim=%d w=%llu fmt=%d "
            "flags=0x%x\n",
            static_cast<int>(got.Dimension), static_cast<unsigned long long>(got.Width),
            static_cast<int>(got.Format), static_cast<unsigned>(got.Flags),
            static_cast<int>(want.Dimension), static_cast<unsigned long long>(want.Width),
            static_cast<int>(want.Format), static_cast<unsigned>(want.Flags));
        return false;
    }
    return true;
}

// ===========================================================================
// 用例驱动的两层：编译 / 渲染
// ===========================================================================
struct Blob {
    std::vector<unsigned char> bytes;
    bool ok = false;
};

Blob compile_shader(const std::wstring& dxc, const std::wstring& source_path,
                    const std::wstring& out_path, const std::wstring& profile,
                    const std::wstring& entry, std::wstring* log_path) {
    Blob blob;
    const std::wstring log = out_path + L".log";
    const std::wstring command_line = quote(dxc) + L" -T " + profile + L" -E " + entry +
                                      L" -O3 -Fo " + quote(out_path) + L" " + quote(source_path);
    DWORD exit_code = 1;
    if (!run_process(command_line, log, exit_code)) {
        if (log_path) *log_path = log;
        return blob;
    }
    if (log_path) *log_path = log;
    if (exit_code != 0) return blob;
    blob.ok = read_file(out_path, blob.bytes) && !blob.bytes.empty();
    return blob;
}

}  // namespace

// ===========================================================================
// main
// ===========================================================================
int main() {
    std::printf("[t44] 差分测试（模式 A）：d3d12_xs 设备代理 ABI vs 桌面 d3d12 直接调用\n");

    const std::wstring only = env_wide(L"XBGTASIX_DIFF_ONLY");
    const std::wstring dump_dir_raw = env_wide(L"XBGTASIX_EMIT_DUMP");
    const std::wstring dump_dir = dump_dir_raw.empty() ? std::wstring() : normalize_path(dump_dir_raw);
    const bool warp = env_wide(L"XBGTASIX_WARP") == L"1";

    // ---- 工具探测：没有 dxc 就 SKIP ----
    std::vector<std::wstring> tried;
    const std::wstring dxc = find_dxc(tried);
    if (dxc.empty()) {
        std::printf("[t44] SKIP: 找不到 dxc（试过 XBGTASIX_DXC、C:\\VulkanSDK\\*\\Bin\\dxc.exe、PATH）\n");
        for (const auto& path : tried) std::printf("[t44]   tried: %s\n", narrow(path).c_str());
        std::printf("[t44] done failures=0 -> SKIP\n");
        return 2;
    }
    std::printf("[t44] dxc: %s\n", narrow(dxc).c_str());

    // ---- 适配器：两侧必须用同一块 ----
    Com<IDXGIFactory4> factory;
    {
        IDXGIFactory4* created = nullptr;
        const HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4),
                                              reinterpret_cast<void**>(&created));
        if (FAILED(hr) || !created) {
            std::printf("[t44] SKIP: DXGI 工厂建不出来 hr=0x%08lx\n",
                        static_cast<unsigned long>(hr));
            std::printf("[t44] done failures=0 -> SKIP\n");
            return 2;
        }
        factory.reset(created);
    }

    Com<IDXGIAdapter> adapter;
    {
        IDXGIAdapter* created = nullptr;
        std::string label;
        if (warp) {
            const HRESULT hr = factory->EnumWarpAdapter(__uuidof(IDXGIAdapter),
                                                        reinterpret_cast<void**>(&created));
            if (FAILED(hr) || !created) {
                std::printf("[t44] SKIP: XBGTASIX_WARP=1 但 WARP 适配器建不出来 hr=0x%08lx\n",
                            static_cast<unsigned long>(hr));
                std::printf("[t44] done failures=0 -> SKIP\n");
                return 2;
            }
            label = "WARP";
        } else {
            SIZE_T best_vram = 0;
            for (UINT i = 0;; ++i) {
                IDXGIAdapter1* item = nullptr;
                const HRESULT hr = factory->EnumAdapters1(i, &item);
                if (hr == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(hr)) break;
                DXGI_ADAPTER_DESC1 desc{};
                if (FAILED(item->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    item->Release();
                    continue;
                }
                if (!created || desc.DedicatedVideoMemory > best_vram) {
                    best_vram = desc.DedicatedVideoMemory;
                    if (created) static_cast<IDXGIAdapter1*>(created)->Release();
                    created = item;   // 转成 IDXGIAdapter*
                    char name[256] = {0};
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name),
                                        nullptr, nullptr);
                    label = name;
                } else {
                    item->Release();
                }
            }
            if (!created) {
                std::printf("[t44] SKIP: 没有硬件 DXGI 适配器（设 XBGTASIX_WARP=1 可用 WARP）\n");
                std::printf("[t44] done failures=0 -> SKIP\n");
                return 2;
            }
        }
        std::printf("[t44] adapter: %s\n", label.c_str());
        adapter.reset(created);
    }

    // ---- 参照设备：System32\d3d12.dll（显式路径，绝不走我们的 shim）----
    HMODULE real_d3d12 = LoadLibraryW(L"C:\\Windows\\System32\\d3d12.dll");
    CreateDeviceFn desktop_create_device = nullptr;
    SerializeFn desktop_serialize = nullptr;
    if (real_d3d12) {
        desktop_create_device = proc_address<CreateDeviceFn>(real_d3d12, "D3D12CreateDevice");
        desktop_serialize = proc_address<SerializeFn>(real_d3d12,
                                                      "D3D12SerializeVersionedRootSignature");
    }
    if (!desktop_create_device || !desktop_serialize) {
        std::printf("[t44] SKIP: System32\\d3d12.dll 缺 D3D12CreateDevice/序列化导出\n");
        std::printf("[t44] done failures=0 -> SKIP\n");
        return 2;
    }

    Side desktop;
    desktop.name = "desktop";
    desktop.host_abi = false;
    desktop.serialize = desktop_serialize;
    {
        void* device = nullptr;
        const HRESULT hr =
            desktop_create_device(adapter.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                  &device);
        if (FAILED(hr) || !device) {
            std::printf("[t44] SKIP: 桌面 D3D12 设备建不出来 hr=0x%08lx（这台机器做不了 GPU 差分）\n",
                        static_cast<unsigned long>(hr));
            std::printf("[t44] done failures=0 -> SKIP\n");
            return 2;
        }
        desktop.dev = static_cast<ID3D12Device*>(device);
    }
    std::printf("[t44] desktop device (System32\\d3d12.dll): %p\n",
                static_cast<void*>(desktop.dev));

    // ---- 被测设备：d3d12_xs.dll 的 D3D12XboxCreateDevice → 设备代理 ----
    // 注意：这里建不出来是 **FAIL** 而不是 SKIP —— 桌面设备已经证明这台机器能做 D3D12，
    // 那么"我们的 shim 建不出设备"就是要抓的回归。
    Side shim;
    shim.name = "shim";
    shim.host_abi = true;
    shim.serialize = &D3D12SerializeVersionedRootSignature;
    {
        void* device = nullptr;
        const HRESULT hr =
            D3D12XboxCreateDevice(adapter.get(), nullptr, __uuidof(ID3D12Device), &device);
        if (!check(SUCCEEDED(hr) && device != nullptr,
                   "d3d12_xs!D3D12XboxCreateDevice must create the Xbox device proxy")) {
            std::printf("[t44]   D3D12XboxCreateDevice hr=0x%08lx out=%p\n",
                        static_cast<unsigned long>(hr), device);
            std::printf("[t44] %d checks, %d failures\n", g_checks, g_failures);
            std::printf("[t44] done failures=%d -> FAIL\n", g_failures);
            return 1;
        }
        shim.dev = static_cast<ID3D12Device*>(device);
        shim.vtable = vtable_of(shim.dev);
    }
    std::printf("[t44] shim device  (d3d12_xs.dll proxy): %p\n", static_cast<void*>(shim.dev));

    // 顺带证明 d3d12_xs!D3D12CreateDevice 仍然返回**原生**桌面设备（代理只从 Xbox 入口来）
    {
        void* plain = nullptr;
        const HRESULT hr =
            D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), &plain);
        bool native_interface = false;
        if (SUCCEEDED(hr) && plain) {
            void* device8 = nullptr;
            native_interface = SUCCEEDED(static_cast<IUnknown*>(plain)->QueryInterface(
                                   __uuidof(ID3D12Device8), &device8)) &&
                               device8 != nullptr;
            if (device8) static_cast<IUnknown*>(device8)->Release();
            static_cast<IUnknown*>(plain)->Release();
        }
        check(SUCCEEDED(hr) && plain != nullptr && native_interface,
              "d3d12_xs!D3D12CreateDevice must still return the plain desktop device");
    }

    // ---- 临时目录（HLSL 源码 + .cso + 日志）----
    const std::wstring work_dir = make_work_dir();
    if (work_dir.empty()) {
        std::printf("[t44] SKIP: 找不到可写的临时目录（%%TEMP%% / exe 目录 / 当前目录都不行）\n");
        std::printf("[t44] done failures=0 -> SKIP\n");
        return 2;
    }
    std::printf("[t44] 临时目录: %s\n", narrow(work_dir).c_str());

    if (!dump_dir.empty()) {
        CreateDirectoryW(dump_dir.c_str(), nullptr);
        const DWORD attributes = GetFileAttributesW(dump_dir.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            std::printf("[t44] 警告：XBGTASIX_EMIT_DUMP=%s 不是可用目录，诊断产物不会落盘\n",
                        narrow(dump_dir).c_str());
        } else {
            std::printf("[t44] 诊断产物目录: %s\n", narrow(dump_dir).c_str());
        }
    }

    // ---- 先验比对器本身 ----
    check_comparator();

    // ---- 逐条用例 ----
    const std::vector<uint32_t> input_words = make_input_words();
    const std::vector<uint32_t> cb_words = make_cb_words();
    int ran = 0;

    for (size_t index = 0; index < kRenderCaseCount; ++index) {
        const CaseDef& item = kRenderCases[index];
        if (!only.empty() && only != widen(item.name)) continue;

        ++ran;
        std::printf("[t44] --- case %s\n", item.name);
        std::printf("[t44]     期望：%s\n", item.why);

        const std::wstring base = work_dir + L"\\" + widen(item.name);
        const std::string source = std::string(kPrelude) + item.body;
        const std::wstring source_path = base + L".hlsl";
        if (!write_text(source_path, source)) {
            printf_failure("%s: 写不了 HLSL 临时文件", item.name);
            continue;
        }

        // 两侧各编译一次：证明"同一份源"，也让落盘的 DXIL 成对可比
        std::wstring shim_vs_log, shim_ps_log, desktop_vs_log, desktop_ps_log;
        const Blob shim_vs = compile_shader(dxc, source_path, base + L".shim.vs.cso", L"vs_6_0",
                                            L"VSMain", &shim_vs_log);
        const Blob shim_ps = compile_shader(dxc, source_path, base + L".shim.ps.cso", L"ps_6_0",
                                            L"PSMain", &shim_ps_log);
        const Blob desktop_vs = compile_shader(dxc, source_path, base + L".desktop.vs.cso",
                                               L"vs_6_0", L"VSMain", &desktop_vs_log);
        const Blob desktop_ps = compile_shader(dxc, source_path, base + L".desktop.ps.cso",
                                               L"ps_6_0", L"PSMain", &desktop_ps_log);
        const bool compiled = shim_vs.ok && shim_ps.ok && desktop_vs.ok && desktop_ps.ok;
        if (!check(compiled, "dxc 编译两侧着色器失败（这是测试自身的错误，不是差分结果）")) {
            for (const auto* log : {&shim_vs_log, &shim_ps_log, &desktop_vs_log, &desktop_ps_log}) {
                std::vector<unsigned char> text;
                if (read_file(*log, text) && !text.empty()) {
                    std::string message(text.begin(), text.end());
                    while (!message.empty() &&
                           (message.back() == '\n' || message.back() == '\r'))
                        message.pop_back();
                    std::printf("[t44]   dxc: %s\n", message.c_str());
                }
            }
            continue;
        }
        if (!check(shim_vs.bytes == desktop_vs.bytes && shim_ps.bytes == desktop_ps.bytes,
                   "两侧 DXIL 必须逐字节相同（同一份源 + 同一个 dxc）")) {
            std::printf("[t44]   vs %llu/%llu bytes, ps %llu/%llu bytes\n",
                        static_cast<unsigned long long>(shim_vs.bytes.size()),
                        static_cast<unsigned long long>(desktop_vs.bytes.size()),
                        static_cast<unsigned long long>(shim_ps.bytes.size()),
                        static_cast<unsigned long long>(desktop_ps.bytes.size()));
        }

        // 路径 A
        RenderOutcome shim_frame = render_frame(shim, shim_vs.bytes, shim_ps.bytes, input_words, cb_words);
        if (!check(shim_frame.error.empty(), "shim 路径建资源/录制/提交失败")) {
            std::printf("[t44]   shim: %s\n", shim_frame.error.c_str());
        }
        // 路径 B
        RenderOutcome desktop_frame =
            render_frame(desktop, desktop_vs.bytes, desktop_ps.bytes, input_words, cb_words);
        if (!check(desktop_frame.error.empty(), "桌面参照路径建资源/录制/提交失败")) {
            std::printf("[t44]   desktop: %s\n", desktop_frame.error.c_str());
        }

        // 逐像素比对：RT + UAV（UAV 是槽 19 建出来的描述符真正生效的地方）
        bool both_ok = shim_frame.error.empty() && desktop_frame.error.empty();
        UINT rt_mismatch = 0;
        UINT uav_mismatch = 0;
        if (both_ok) {
            rt_mismatch = compare_pixels(item.name, "rt", shim_frame.frame.rt,
                                         desktop_frame.frame.rt, item.ulp_tolerance);
            uav_mismatch = compare_pixels(item.name, "uav", shim_frame.frame.uav,
                                          desktop_frame.frame.uav, item.ulp_tolerance);
            if (!check(rt_mismatch == 0, "render target 逐像素比对不一致")) {
                std::printf("[t44]   %s: rt %u/%u 像素不同（容差 %s）\n", item.name, rt_mismatch,
                            kPixels, item.ulp_tolerance ? "≤1 ULP" : "位必须相同");
            }
            if (!check(uav_mismatch == 0, "UAV（槽 19 描述符）逐像素比对不一致")) {
                std::printf("[t44]   %s: uav %u/%u 像素不同（容差 %s）\n", item.name, uav_mismatch,
                            kPixels, item.ulp_tolerance ? "≤1 ULP" : "位必须相同");
            }
            if (rt_mismatch == 0 && uav_mismatch == 0) {
                std::printf("[t44] case %s: PASS（rt/uav 各 %u 像素按%s一致）\n", item.name,
                            kPixels, item.ulp_tolerance ? "≤1 ULP" : "位模式");
            }
        }

        // 落盘诊断产物
        if (!dump_dir.empty()) {
            const std::wstring prefix = dump_dir + L"\\" + widen(item.name);
            write_file(prefix + L".shim.vs.cso", shim_vs.bytes.data(), shim_vs.bytes.size());
            write_file(prefix + L".shim.ps.cso", shim_ps.bytes.data(), shim_ps.bytes.size());
            write_file(prefix + L".desktop.vs.cso", desktop_vs.bytes.data(),
                       desktop_vs.bytes.size());
            write_file(prefix + L".desktop.ps.cso", desktop_ps.bytes.data(),
                       desktop_ps.bytes.size());
            auto dump_pixels = [&](const wchar_t* side, const wchar_t* buffer_name,
                                   const std::vector<uint32_t>& pixels) {
                std::string text;
                char line[64];
                for (UINT i = 0; i < pixels.size(); ++i) {
                    std::snprintf(line, sizeof(line), "%u %u 0x%08x\n", i % kSize, i / kSize,
                                  pixels[i]);
                    text += line;
                }
                write_text(prefix + L"." + side + L"." + buffer_name + L".txt", text);
            };
            if (both_ok) {
                dump_pixels(L"shim", L"rt", shim_frame.frame.rt);
                dump_pixels(L"desktop", L"rt", desktop_frame.frame.rt);
                dump_pixels(L"shim", L"uav", shim_frame.frame.uav);
                dump_pixels(L"desktop", L"uav", desktop_frame.frame.uav);
            }
        }
    }

    // ---- ABI 数值用例（不是像素）----
    if (only.empty() || only == L"abi_alloc_info" || only == L"abi_host_flag_strip") {
        const bool run_alloc = only.empty() || only == L"abi_alloc_info";
        const bool run_flags = only.empty() || only == L"abi_host_flag_strip";
        if (run_alloc) {
            ++ran;
            std::printf("[t44] --- case abi_alloc_info\n");
            std::printf("[t44]     期望：剥掉主机资源标志后，SizeInBytes/Alignment 与桌面一致\n");
            case_alloc_info(shim, desktop);
        }
        if (run_flags) {
            ++ran;
            std::printf("[t44] --- case abi_host_flag_strip\n");
            std::printf("[t44]     期望：槽 127 建出的资源与剥干净的桌面描述逐字段相同\n");
            case_host_flag_strip(shim, desktop);
        }
    }

    if (ran == 0) {
        std::printf("[t44] FAIL: XBGTASIX_DIFF_ONLY=%s 不对应任何用例\n", narrow(only).c_str());
        std::printf("[t44]   可用：");
        for (const auto& item : kRenderCases) std::printf("%s ", item.name);
        std::printf("abi_alloc_info abi_host_flag_strip\n");
        ++g_failures;
    }

    // 清理临时文件（落盘目录不动）
    {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((work_dir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                DeleteFileW((work_dir + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(work_dir.c_str());
    }

    if (desktop.dev) desktop.dev->Release();
    if (shim.dev) static_cast<IUnknown*>(shim.dev)->Release();
    if (real_d3d12) FreeLibrary(real_d3d12);

    std::printf("[t44] %d checks, %d failures\n", g_checks, g_failures);
    std::printf("[t44] done failures=%d -> %s\n", g_failures, g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
