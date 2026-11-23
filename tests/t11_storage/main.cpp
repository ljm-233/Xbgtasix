// SPDX-License-Identifier: GPL-3.0-or-later
// t11_storage — 持久化本地存储（PLS）用真配置验证
//
// 验证点：
//   1) 路径来自 MicrosoftGame.Config 里的 TitleId（%LOCALAPPDATA%\Xbgtasix\<TitleId>）
//   2) 目录会被真的创建出来，能写入并读回
//   3) 容量上限来自配置里的 <SizeMB>2048</SizeMB>
//   4) 缓冲区太小时返回 E_NOT_SUFFICIENT_BUFFER（而不是崩溃或截断）
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize(void);
HRESULT WINAPI XPersistentLocalStorageGetPathSize(size_t* path_size);
HRESULT WINAPI XPersistentLocalStorageGetPath(size_t path_size, char* path, size_t* path_used);
HRESULT WINAPI XPersistentLocalStorageGetSpaceInfo(void* info);
}

namespace {

struct SpaceInfo {
    uint64_t availableFreeBytes;
    uint64_t totalFreeBytes;
    uint64_t usedBytes;
    uint64_t totalBytes;
};

int g_fail = 0;

void ok(bool cond, const char* what) {
    if (!cond) {
        ++g_fail;
        xbgtasix::logf("[t11] FAIL %s", what);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* cfg = argc > 1 ? argv[1] : "";
    if (cfg[0]) SetEnvironmentVariableA("XBGTASIX_CONFIG", cfg);
    ok(SUCCEEDED(XGameRuntimeInitialize()), "XGameRuntimeInitialize");

    // ---- 1) 路径 ----
    size_t need = 0;
    ok(SUCCEEDED(XPersistentLocalStorageGetPathSize(&need)) && need > 8, "GetPathSize");
    std::vector<char> buf(need, 0);
    size_t used = 0;
    const HRESULT hr = XPersistentLocalStorageGetPath(need, buf.data(), &used);
    ok(SUCCEEDED(hr), "GetPath");
    const std::string path(buf.data());
    xbgtasix::logf("[t11] PLS 路径: %s", path.c_str());
    ok(path.find("Xbgtasix") != std::string::npos, "路径含 Xbgtasix");
    ok(path.find("7952987A") != std::string::npos, "路径含配置里的 TitleId");

    // 缓冲区太小必须报 E_NOT_SUFFICIENT_BUFFER
    char tiny[4] = {0};
    ok(XPersistentLocalStorageGetPath(sizeof(tiny), tiny, nullptr) == E_NOT_SUFFICIENT_BUFFER,
       "小缓冲区返回 E_NOT_SUFFICIENT_BUFFER");

    // ---- 2) 能写能读 ----
    const std::string file = path + "\\t11_probe.bin";
    const char payload[64] = "xbgtasix pls roundtrip";
    FILE* f = nullptr;
    ok(fopen_s(&f, file.c_str(), "wb") == 0 && f, "打开 PLS 文件");
    if (f) {
        fwrite(payload, 1, sizeof(payload), f);
        fclose(f);
    }
    char back[64] = {0};
    ok(fopen_s(&f, file.c_str(), "rb") == 0 && f, "重开 PLS 文件");
    if (f) {
        fread(back, 1, sizeof(back), f);
        fclose(f);
    }
    ok(std::memcmp(back, payload, sizeof(payload)) == 0, "写入内容读回一致");

    // ---- 3) 容量来自配置 ----
    SpaceInfo info{};
    ok(SUCCEEDED(XPersistentLocalStorageGetSpaceInfo(&info)), "GetSpaceInfo");
    xbgtasix::logf("[t11] 容量: total=%llu MB used=%llu B free=%llu B",
                   static_cast<unsigned long long>(info.totalBytes / (1024 * 1024)),
                   static_cast<unsigned long long>(info.usedBytes),
                   static_cast<unsigned long long>(info.availableFreeBytes));
    ok(info.totalBytes == 2048ull * 1024ull * 1024ull, "totalBytes == 配置里的 2048MB");
    ok(info.usedBytes >= sizeof(payload), "usedBytes 计入刚写的文件");
    ok(info.availableFreeBytes + info.usedBytes == info.totalBytes, "free + used == total");

    DeleteFileA(file.c_str());
    xbgtasix::logf("[t11] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
