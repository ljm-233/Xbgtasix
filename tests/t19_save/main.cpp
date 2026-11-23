// t19_save — 存档（XGameSaveFiles）端到端
//
// 主机上存档走"云存档容器"；GDK 提供了一套目录式的简化接口，
// 正好可以映射到 PC 的本地目录：%LOCALAPPDATA%\Xbgtasix\<TitleId>\SaveData\<configurationId>
//
// 验证点：
//   1) 拿到存档目录，且目录真的被创建
//   2) 写入 -> 读回一致（不是只返回一个字符串）
//   3) 配额随写入减少，且永远不小于 0
//   4) 缓冲区太小时报 E_NOT_SUFFICIENT_BUFFER
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xbgtasix/trace.h"

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize(void);
HRESULT WINAPI XGameSaveFilesGetFolderWithUiAsync(void* user, const char* config_id, void* async_block);
HRESULT WINAPI XGameSaveFilesGetFolderWithUiResult(void* async_block, size_t folder_size, char* out);
HRESULT WINAPI XGameSaveFilesGetRemainingQuota(void* user, const char* config_id, int64_t* quota);
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
        xbgtasix::logf("[t19]   PASS  %-30s %s", what, detail);
    } else {
        xbgtasix::logf("[t19]   FAIL  %-30s %s", what, detail);
        ++g_fail;
    }
}

bool write_file(const std::string& path, const void* data, size_t bytes) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    fwrite(data, 1, bytes, f);
    fclose(f);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* cfg = argc > 1 ? argv[1] : "";
    if (cfg[0]) SetEnvironmentVariableA("XBGTASIX_CONFIG", cfg);
    ok(SUCCEEDED(XGameRuntimeInitialize()), "XGameRuntimeInitialize");

    // ---- 1) 存档目录 ----
    AsyncBlock block{};
    ok(SUCCEEDED(XGameSaveFilesGetFolderWithUiAsync(nullptr, "slot1", &block)), "GetFolderWithUiAsync");

    char small[4] = {0};
    ok(XGameSaveFilesGetFolderWithUiResult(&block, sizeof(small), small) == E_NOT_SUFFICIENT_BUFFER,
       "小缓冲区报 E_NOT_SUFFICIENT_BUFFER");

    std::vector<char> buf(1024, 0);
    ok(SUCCEEDED(XGameSaveFilesGetFolderWithUiResult(&block, buf.size(), buf.data())), "GetFolderWithUiResult");
    const std::string dir(buf.data());
    xbgtasix::logf("[t19] 存档目录: %s", dir.c_str());
    ok(!dir.empty(), "目录非空");
    ok(dir.find("SaveData") != std::string::npos, "位于 SaveData 下");

    const DWORD attr = GetFileAttributesA(dir.c_str());
    ok(attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY), "目录真实存在");

    // ---- 2) 配额：写之前 ----
    int64_t quota0 = -1;
    ok(SUCCEEDED(XGameSaveFilesGetRemainingQuota(nullptr, "slot1", &quota0)), "GetRemainingQuota(写前)");
    xbgtasix::logf("[t19] 写前剩余 %lld MB", static_cast<long long>(quota0 / (1024 * 1024)));
    ok(quota0 > 0, "剩余配额为正");

    // ---- 3) 写入 1 MB 再读回 ----
    const size_t n = 1024 * 1024;
    std::vector<unsigned char> payload(n);
    for (size_t i = 0; i < n; ++i) payload[i] = static_cast<unsigned char>(i * 31 + 7);

    const std::string file = dir + "\\save_slot1.bin";
    ok(write_file(file, payload.data(), n), "写入 1 MB 存档");

    std::vector<unsigned char> back(n, 0);
    FILE* f = nullptr;
    if (fopen_s(&f, file.c_str(), "rb") == 0 && f) {
        fread(back.data(), 1, n, f);
        fclose(f);
    }
    ok(std::memcmp(back.data(), payload.data(), n) == 0, "1 MB 内容逐字节一致");

    // ---- 4) 配额：写之后 ----
    int64_t quota1 = -1;
    ok(SUCCEEDED(XGameSaveFilesGetRemainingQuota(nullptr, "slot1", &quota1)), "GetRemainingQuota(写后)");
    const long long delta = static_cast<long long>(quota0 - quota1);
    xbgtasix::logf("[t19] 写后剩余 %lld MB（减少 %lld KB）",
                   static_cast<long long>(quota1 / (1024 * 1024)), delta / 1024);
    ok(quota1 >= 0, "配额不为负");
    // 审查意见：>= 对配额测试等于没断言，多扣 100MB 也能过。改为精确匹配（按 64KB 块取整）
    const long long blocks = static_cast<long long>((n + 65535) / 65536) * 65536;
    ok(delta == blocks, "配额减少量恰好等于写入量（按 64KB 圆整）");

    // 删掉文件后配额应回到初值
    DeleteFileA(file.c_str());
    int64_t quota2 = -1;
    ok(SUCCEEDED(XGameSaveFilesGetRemainingQuota(nullptr, "slot1", &quota2)), "GetRemainingQuota(删除后)");
    // 以前写 quota2 >= quota0：多还配额（算错账）也能过。删除后必须**精确**回到写前。
    ok(quota2 == quota0, "删除后配额恢复到写前的精确值（==，不是 >=）");

    xbgtasix::logf("[t19] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
