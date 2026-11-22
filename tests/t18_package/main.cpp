// t18_package — XPackage 包挂载与虚拟路径
//
// 游戏读自己资源包的第一步是"我这个包在哪"：
//   XPackageGetCurrentProcessPackageIdentifier -> XPackageMount -> XPackageGetMountPath
// 主机上包是"已安装的注册包"，PC 上等价于"内容目录在哪"。
//
// 用真数据验证：包身份取自 MicrosoftGame.Config 的 <Identity Name="...">，
// 挂载后的路径必须真的存在，且能在里面读到 MicrosoftGame.Config。
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize(void);
HRESULT WINAPI XPackageGetCurrentProcessPackageIdentifier(size_t buffer_size, char* buffer);
bool WINAPI XPackageIsPackagedProcess(void);
HRESULT WINAPI XPackageMountWithUiAsync(const char* package_id, void* async_block);
HRESULT WINAPI XPackageMountWithUiResult(void* async_block, void** mount);
HRESULT WINAPI XPackageGetMountPathSize(void* mount, size_t* path_size);
HRESULT WINAPI XPackageGetMountPath(void* mount, size_t path_size, char* path);
void WINAPI XPackageCloseMountHandle(void* mount);
}

namespace {

struct AsyncBlock {
    void* context;
    void* queue;
    void (*callback)(AsyncBlock*);
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};

int g_fail = 0;

void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t18]   PASS  %-34s %s", what, detail);
    } else {
        xbgtasix::logf("[t18]   FAIL  %-34s %s", what, detail);
        ++g_fail;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* cfg = argc > 1 ? argv[1] : "";
    const char* root = argc > 2 ? argv[2] : "";
    if (cfg[0]) SetEnvironmentVariableA("XBGTASIX_CONFIG", cfg);
    if (root[0]) SetEnvironmentVariableA("XBGTASIX_PACKAGE_ROOT", root);
    ok(SUCCEEDED(XGameRuntimeInitialize()), "XGameRuntimeInitialize");

    // ---- 1) 包身份来自真配置 ----
    ok(XPackageIsPackagedProcess(), "IsPackagedProcess（读到了 config）");
    char id[256] = {0};
    ok(SUCCEEDED(XPackageGetCurrentProcessPackageIdentifier(sizeof(id), id)), "GetPackageIdentifier");
    xbgtasix::logf("[t18] 包身份: %s", id);
    ok(std::strstr(id, "RockstarGamesInc.") != nullptr, "包身份含发布者前缀", id);

    // 缓冲区太小要报错而不是截断
    char tiny[4] = {0};
    ok(XPackageGetCurrentProcessPackageIdentifier(sizeof(tiny), tiny) == E_NOT_SUFFICIENT_BUFFER,
       "小缓冲区返回 E_NOT_SUFFICIENT_BUFFER");

    // ---- 2) 挂载 + 取路径 ----
    AsyncBlock block{};
    ok(SUCCEEDED(XPackageMountWithUiAsync(id, &block)), "MountWithUiAsync");
    void* mount = nullptr;
    ok(SUCCEEDED(XPackageMountWithUiResult(&block, &mount)) && mount, "MountWithUiResult");
    if (!mount) {
        xbgtasix::logf("[t18] done failures=%d -> FAIL", g_fail);
        return 1;
    }

    size_t need = 0;
    ok(SUCCEEDED(XPackageGetMountPathSize(mount, &need)) && need > 3, "GetMountPathSize");
    std::vector<char> path(need, 0);
    ok(SUCCEEDED(XPackageGetMountPath(mount, need, path.data())), "GetMountPath");
    const std::string mp(path.data());
    xbgtasix::logf("[t18] 挂载点: %s", mp.c_str());

    const DWORD attr = GetFileAttributesA(mp.c_str());
    ok(attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY), "挂载点是真实目录");

    // 能在挂载点里读到配置 —— 证明路径不是拼出来的摆设
    const std::string cfg_in_mount = mp + "\\MicrosoftGame.Config";
    FILE* f = nullptr;
    ok(fopen_s(&f, cfg_in_mount.c_str(), "rb") == 0 && f, "挂载点内可打开 MicrosoftGame.Config");
    if (f) fclose(f);

    // ---- 3) 挂不存在的包必须失败，而不是给个假路径 ----
    AsyncBlock bad{};
    ok(SUCCEEDED(XPackageMountWithUiAsync("nonexistent.package.id", &bad)), "MountWithUiAsync(坏包)");
    void* badmount = nullptr;
    const HRESULT badhr = XPackageMountWithUiResult(&bad, &badmount);
    xbgtasix::logf("[t18] 坏包结果 hr=0x%08lx mount=%p", static_cast<unsigned long>(badhr),
                   badmount);
    ok(FAILED(badhr) && !badmount, "坏包返回失败且不给句柄");

    XPackageCloseMountHandle(mount);
    xbgtasix::logf("[t18] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
