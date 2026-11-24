// platform.h —— "当前平台"的概念：真 Windows 还是 Wine（Linux 上的 Windows 兼容层）。
//
// 为什么需要它：本项目原来只跑在 Windows 上，现在要经 Wine 在 Linux 上跑。
// 两者语义不同的地方必须能分开走，而不是到处写 `#ifdef _WIN32`（那分不出 Wine）。
//
// 取值来源（优先级从高到低）：
//   1. 环境变量 XBGTASIX_PLATFORM=windows|wine —— 手动钉死，用于复现/对照实验
//   2. 运行时探测：ntdll.dll 里能不能拿到 `wine_get_version`（Wine 的规范标识）
//
// 头文件实现（inline）：exe 与 8 个 shim 都能直接用，不需要改各自的目标文件列表。
// 只依赖 <windows.h>；读真实硬件时走 Z:\（Wine 把 Z: 映射到 /），不碰 Linux 专属 API。
#ifndef XBGTASIX_PLATFORM_H
#define XBGTASIX_PLATFORM_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

namespace xbgtasix {

enum class HostPlatform {
    Windows,   // 真 Windows（或未识别出 Wine）
    Wine,      // Wine / Proton
};

inline HostPlatform host_platform() {
    static HostPlatform cached = [] {
        wchar_t buf[32]{};
        const DWORD n = GetEnvironmentVariableW(L"XBGTASIX_PLATFORM", buf, 32);
        if (n > 0 && n < 32) {
            std::wstring v(buf, n);
            for (auto& c : v) c = static_cast<wchar_t>(towlower(c));
            if (v == L"wine") return HostPlatform::Wine;
            if (v == L"windows") return HostPlatform::Windows;
        }
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll && GetProcAddress(ntdll, "wine_get_version")) return HostPlatform::Wine;
        return HostPlatform::Windows;
    }();
    return cached;
}

inline bool host_is_wine() { return host_platform() == HostPlatform::Wine; }

inline const char* host_platform_name() {
    return host_is_wine() ? "wine" : "windows";
}

// Wine 的版本串（真 Windows 上返回空）。写日志/报告时用它区分"哪个 Wine"。
inline std::string host_wine_version() {
    if (!host_is_wine()) return {};
    using Fn = const char*(*)();
    // 先过一手 void*：直接从 FARPROC 转函数指针会吃 -Wcast-function-type。
    auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")));
    if (!fn) return {};
    const char* v = fn();
    return v ? std::string(v) : std::string();
}

// ------------------------------------------------------------------ 真实硬件
// 为什么需要：Wine 下 D3D12 走 vkd3d-proton，DXGI 报出来的适配器可能是假的
// （本机实测：报 "Intel(R) HD Graphics 4000, 8086:0162, 显存 1.5 GB"，
//  而真机是 RTX 5060 Laptop + Intel ARL 核显）。doctor 如果只信 DXGI，
// 就会把假卡当真卡，排障方向从一开始就错。
//
// 读法：Wine 把 Z: 映射到 /，所以 C:\... 那一套 API 可以直接读 /sys/class/drm。
// 只读 vendor/device/subsystem_* 这几个十六进制文件，不解析 uevent。
struct HostGpu {
    std::string name;        // "NVIDIA 10DE:2D58"（有 lspci 名字时才带名字）
    unsigned vendor = 0;
    unsigned device = 0;
    unsigned subsys_vendor = 0;
    unsigned subsys_device = 0;
    bool boot_vga = false;   // 开机主显卡（笔记本上通常是核显）
};

namespace detail {

inline bool read_hex_line(const std::wstring& path, unsigned* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[32]{};
    DWORD got = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    if (!ok || got == 0) return false;
    unsigned v = 0;
    int i = 0;
    while (i < static_cast<int>(got) && (buf[i] == ' ' || buf[i] == '\t')) ++i;
    if (i + 1 < static_cast<int>(got) && buf[i] == '0' && (buf[i + 1] == 'x' || buf[i + 1] == 'X'))
        i += 2;
    for (; i < static_cast<int>(got); ++i) {
        const char c = buf[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
        else break;
        v = v * 16 + d;
        if (v > 0xFFFF) return false;
    }
    *out = v;
    return true;
}

inline bool file_exists_w(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace detail

// Wine 下枚举真实显卡（读 Z:\sys\class\drm\cardN\device\*）。真 Windows 上返回空。
inline std::vector<HostGpu> host_probe_gpus() {
    std::vector<HostGpu> out;
    if (!host_is_wine()) return out;
    for (int i = 0; i < 10; ++i) {
        const std::wstring base =
            L"Z:\\sys\\class\\drm\\card" + std::to_wstring(i) + L"\\device\\";
        if (!detail::file_exists_w(base + L"vendor")) continue;
        HostGpu g;
        if (!detail::read_hex_line(base + L"vendor", &g.vendor)) continue;
        detail::read_hex_line(base + L"device", &g.device);
        detail::read_hex_line(base + L"subsystem_vendor", &g.subsys_vendor);
        detail::read_hex_line(base + L"subsystem_device", &g.subsys_device);
        // boot_vga: 1 表示固件选的主显卡。文件不存在一般是 0
        unsigned boot = 0;
        if (detail::read_hex_line(base + L"boot_vga", &boot)) g.boot_vga = boot != 0;

        const char* vend = g.vendor == 0x10DE ? "NVIDIA"
                         : g.vendor == 0x8086 ? "Intel"
                         : g.vendor == 0x1002 ? "AMD"
                         : g.vendor == 0x13B5 ? "ARM"
                                              : "其它";
        char tmp[128];
        std::snprintf(tmp, sizeof(tmp), "%s %04X:%04X (sub %04X:%04X)%s", vend,
                      g.vendor, g.device, g.subsys_vendor, g.subsys_device,
                      g.boot_vga ? "  [boot_vga]" : "");
        g.name = tmp;
        out.push_back(g);
    }
    return out;
}

}  // namespace xbgtasix

#endif  // XBGTASIX_PLATFORM_H
