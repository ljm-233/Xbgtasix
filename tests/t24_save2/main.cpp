// SPDX-License-Identifier: GPL-3.0-or-later
// t24_save2 — 存档（XGameSave 容器式）端到端
//
// 目录式（t19_save / XGameSaveFiles）是"给游戏一个目录自己读写"；主机真正用的是容器式：
//   InitializeProvider → CreateContainer → blob 写进 update → Submit → Close
// 本测试只走公开导出，覆盖：
//   1) provider 初始化（异步）→ 建容器 → 写 4KB blob → 提交 → 关容器
//   2) 重开同名容器（= 打开）→ 读回 4KB → 逐字节比较
//   3) 删容器 → 容器真的消失（不是留个空目录）→ 旧 blob 读不到
//   4) 每个 *Async 都能 XAsyncGetStatus(wait=true) 立刻完成（!= E_PENDING 且 < 1000ms）
//
// 签名逐字对照 gdk/include_clean/XGameSave.h（27 个函数全在这里用到一部分；
// 结构体字段顺序照抄，否则 type_t* 回调参数会错位）。
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xbgtasix/trace.h"

// ---- 头文件结构体（XGameSave.h:67-87；只用到这三个） ----
struct BlobInfo
{
    const char* name;
    unsigned size; // uint32_t
};

struct Blob
{
    BlobInfo info;
    unsigned char* data;
};

struct ContainerInfo
{
    const char* name;
    const char* displayName;
    unsigned blobCount; // uint32_t
    uint64_t totalSize;
    time_t lastModifiedTime;
    bool needsSync;
};

// XAsyncBlock（XAsync.h:46）：顺序必须是 queue/context/callback/internal
struct Blk
{
    void* queue;
    void* context;
    void (*callback)(Blk*);
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};

// 与头文件里的 E_GS_* / E_NOTFOUND 同值
static const long E_GS_PROVIDED_BUFFER_TOO_SMALL = static_cast<long>(0x80830007);
static const long E_GS_INVALID_CONTAINER_NAME = static_cast<long>(0x80830001);
static const long kNotFound = static_cast<long>(0x80070490); // HRESULT_FROM_WIN32(ERROR_NOT_FOUND)

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize(void);
HRESULT WINAPI XAsyncGetStatus(void* async_block, bool wait);
HRESULT WINAPI XPersistentLocalStorageGetPathSize(size_t* path_size);
HRESULT WINAPI XPersistentLocalStorageGetPath(size_t path_size, char* path, size_t* path_used);

// XGameSave.h:106-147 —— 参数顺序/类型逐字照抄
HRESULT WINAPI XGameSaveInitializeProviderAsync(void* requesting_user, const char* configuration_id,
                                                bool sync_on_demand, Blk* async);
HRESULT WINAPI XGameSaveInitializeProviderResult(Blk* async, void** provider);
void WINAPI XGameSaveCloseProvider(void* provider);

HRESULT WINAPI XGameSaveCreateContainer(void* provider, const char* container_name,
                                        void** container);
void WINAPI XGameSaveCloseContainer(void* container);
HRESULT WINAPI XGameSaveDeleteContainer(void* provider, const char* container_name);
HRESULT WINAPI XGameSaveDeleteContainerAsync(void* provider, const char* container_name,
                                             Blk* async);
HRESULT WINAPI XGameSaveDeleteContainerResult(Blk* async);
HRESULT WINAPI XGameSaveGetContainerInfo(void* provider, const char* container_name, void* context,
                                         bool (CALLBACK* callback)(const ContainerInfo*, void*));
HRESULT WINAPI XGameSaveGetRemainingQuotaAsync(void* provider, Blk* async);
HRESULT WINAPI XGameSaveGetRemainingQuotaResult(Blk* async, int64_t* remaining_quota);

HRESULT WINAPI XGameSaveCreateUpdate(void* container, const char* display_name, void** update);
void WINAPI XGameSaveCloseUpdate(void* update);
HRESULT WINAPI XGameSaveSubmitBlobWrite(void* update, const char* blob_name,
                                        const unsigned char* data, size_t byte_count);
HRESULT WINAPI XGameSaveSubmitUpdateAsync(void* update, Blk* async);
HRESULT WINAPI XGameSaveSubmitUpdateResult(Blk* async);

HRESULT WINAPI XGameSaveReadBlobData(void* container, const char** blob_names,
                                     unsigned* count_of_blobs, size_t blobs_size, Blob* blob_data);
HRESULT WINAPI XGameSaveReadBlobDataAsync(void* container, const char** blob_names,
                                          unsigned count_of_blobs, Blk* async);
HRESULT WINAPI XGameSaveReadBlobDataResult(Blk* async, size_t blobs_size, Blob* blob_data,
                                           unsigned* count_of_blobs);
HRESULT WINAPI XGameSaveEnumerateBlobInfo(void* container, void* context,
                                          bool (CALLBACK* callback)(const BlobInfo*, void*));
}

namespace {

int g_fail = 0;

void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t24]   PASS  %-40s %s", what, detail);
    } else {
        xbgtasix::logf("[t24]   FAIL  %-40s %s", what, detail);
        ++g_fail;
    }
}

std::string exe_dir() {
    char buf[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";
    std::string s(buf, n);
    const size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? "." : s.substr(0, slash);
}

bool file_exists(const std::string& p) {
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 所有 *Async 的公共断言：必须真的完成，且不能靠 5 秒超时空转。
bool wait_done(Blk* b, const char* what, long* out_hr = nullptr) {
    const ULONGLONG t0 = GetTickCount64();
    const HRESULT hr = XAsyncGetStatus(b, true);
    const ULONGLONG ms = GetTickCount64() - t0;
    char detail[192];
    std::snprintf(detail, sizeof(detail), "%s hr=0x%08lx %llums", what,
                  static_cast<unsigned long>(hr), static_cast<unsigned long long>(ms));
    ok(hr != E_PENDING, "异步完成（非 E_PENDING）", detail);
    ok(ms < 1000, "异步在 1000ms 内完成", detail);
    if (out_hr) *out_hr = hr;
    return hr != E_PENDING;
}

int g_blob_callbacks = 0;
unsigned g_blob_size = 0;

bool CALLBACK on_blob(const BlobInfo* info, void* context) {
    (void)context;
    ++g_blob_callbacks;
    if (info) g_blob_size = info->size;
    return true; // 继续枚举
}

// GetContainerInfo 的回调：typedef bool CALLBACK (const XGameSaveContainerInfo*, void*) —— 注意
// 是 (info, context)，与 XPackage 的 chunk 回调 (context, chunk_id, ...) 参数顺序相反。
struct InfoCtx {
    bool seen = false;
    bool filled = false;
};

bool CALLBACK on_container_info(const ContainerInfo* info, void* context) {
    auto* c = static_cast<InfoCtx*>(context);
    if (c) {
        c->seen = true;
        c->filled = (info != nullptr && info->blobCount == 1 && info->totalSize >= 4096 &&
                     info->lastModifiedTime > 0 && info->needsSync == false);
    }
    return true;
}

bool CALLBACK on_container_info_any(const ContainerInfo*, void* context) {
    if (context) static_cast<InfoCtx*>(context)->seen = true;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // 沙箱里 %LOCALAPPDATA% 不可写（docs/env-gotchas.md 第 1 条），测试统一隔离到 build 下。
    if (argc > 1) {
        SetEnvironmentVariableA("XBGTASIX_PLS_ROOT", argv[1]);
    } else if (GetEnvironmentVariableA("XBGTASIX_PLS_ROOT", nullptr, 0) == 0) {
        const std::string dir = exe_dir() + "\\pls_t24";
        SetEnvironmentVariableA("XBGTASIX_PLS_ROOT", dir.c_str());
    }
    ok(SUCCEEDED(XGameRuntimeInitialize()), "XGameRuntimeInitialize");

    char pls[1024] = {0};
    size_t need = 0;
    size_t used = 0;
    ok(SUCCEEDED(XPersistentLocalStorageGetPathSize(&need)), "GetPathSize");
    ok(need < sizeof(pls) && SUCCEEDED(XPersistentLocalStorageGetPath(sizeof(pls), pls, &used)),
       "GetPath", pls);

    // ---- 1) provider（异步） ----
    Blk block{};
    ok(SUCCEEDED(XGameSaveInitializeProviderAsync(nullptr, "config_t24", false, &block)),
       "InitializeProviderAsync 被受理");
    wait_done(&block, "InitializeProviderAsync");

    void* provider = nullptr;
    ok(SUCCEEDED(XGameSaveInitializeProviderResult(&block, &provider)) && provider,
       "InitializeProviderResult 给出非空 provider");
    if (!provider) {
        xbgtasix::logf("[t24] done failures=%d -> FAIL", g_fail);
        return 1;
    }

    // 容器目录的映射规则：<PLS>\Containers\<configurationId>\<containerName>
    const std::string cfg_ns = std::string(pls) + "\\Containers\\config_t24";
    ok(file_exists(cfg_ns), "provider 命名空间目录真实存在", cfg_ns.c_str());

    // ---- 2) 建容器 + 写 4KB blob ----
    const size_t kBlobSize = 4096;
    std::vector<unsigned char> payload(kBlobSize);
    for (size_t i = 0; i < kBlobSize; ++i)
        payload[i] = static_cast<unsigned char>((i * 37 + 11) & 0xFF);

    void* container = nullptr;
    ok(SUCCEEDED(XGameSaveCreateContainer(provider, "slot_a", &container)) && container,
       "CreateContainer(slot_a)");
    if (!container) {
        XGameSaveCloseProvider(provider);
        xbgtasix::logf("[t24] done failures=%d -> FAIL", g_fail);
        return 1;
    }
    const std::string container_dir = cfg_ns + "\\slot_a";
    ok(file_exists(container_dir), "容器 = PLS 下的子目录", container_dir.c_str());

    void* update = nullptr;
    ok(SUCCEEDED(XGameSaveCreateUpdate(container, "Slot A", &update)) && update, "CreateUpdate");
    ok(update && SUCCEEDED(XGameSaveSubmitBlobWrite(update, "save.blob", payload.data(),
                                                    payload.size())),
       "SubmitBlobWrite(4KB)");

    Blk submit{};
    ok(SUCCEEDED(XGameSaveSubmitUpdateAsync(update, &submit)), "SubmitUpdateAsync 被受理");
    wait_done(&submit, "SubmitUpdateAsync");
    ok(SUCCEEDED(XGameSaveSubmitUpdateResult(&submit)), "SubmitUpdateResult = S_OK");

    XGameSaveCloseUpdate(update);
    XGameSaveCloseContainer(container);

    // blob 必须真的落成文件（容器式不许只是内存里的假动作）
    ok(file_exists(container_dir + "\\save.blob"), "blob 落盘为容器目录里的文件");

    // ---- 3) 重开同名容器 → 读回 4KB ----
    void* reopened = nullptr;
    ok(SUCCEEDED(XGameSaveCreateContainer(provider, "slot_a", &reopened)) && reopened,
       "同名容器可以重开（CreateContainer = 建或复用）");

    const char* names[1] = {"save.blob"};
    std::vector<unsigned char> rbuf(sizeof(Blob) + kBlobSize, 0);
    unsigned count = 1;

    // 缓冲区为 0 时应报"太小"并回填需要的个数（不是崩溃、不是静默成功）
    unsigned probe = 1;
    ok(XGameSaveReadBlobData(reopened, names, &probe, 0, nullptr) ==
           E_GS_PROVIDED_BUFFER_TOO_SMALL &&
           probe == 1,
       "blobsSize=0 回 E_GS_PROVIDED_BUFFER_TOO_SMALL 并回填个数");

    ok(SUCCEEDED(XGameSaveReadBlobData(reopened, names, &count, rbuf.size(),
                                      reinterpret_cast<Blob*>(rbuf.data()))),
       "ReadBlobData(save.blob)");
    Blob* blobs = reinterpret_cast<Blob*>(rbuf.data());
    ok(count == 1, "读到 1 个 blob");
    ok(blobs[0].info.size == kBlobSize, "blob 大小 = 4096");
    ok(blobs[0].data != nullptr &&
           std::memcmp(blobs[0].data, payload.data(), kBlobSize) == 0,
       "4KB 内容逐字节一致");

    // ---- 4) 异步读同一条 blob（另一条路径） ----
    Blk read_async{};
    ok(SUCCEEDED(XGameSaveReadBlobDataAsync(reopened, names, 1, &read_async)),
       "ReadBlobDataAsync 被受理");
    wait_done(&read_async, "ReadBlobDataAsync");
    std::vector<unsigned char> rbuf2(sizeof(Blob) + kBlobSize, 0);
    unsigned count2 = 0;
    ok(SUCCEEDED(XGameSaveReadBlobDataResult(&read_async, rbuf2.size(),
                                             reinterpret_cast<Blob*>(rbuf2.data()), &count2)),
       "ReadBlobDataResult");
    Blob* blobs2 = reinterpret_cast<Blob*>(rbuf2.data());
    ok(count2 == 1 && blobs2[0].data &&
           std::memcmp(blobs2[0].data, payload.data(), kBlobSize) == 0,
       "异步读回 4KB 也逐字节一致");

    // ---- 5) 枚举与容器信息 ----
    g_blob_callbacks = 0;
    g_blob_size = 0;
    ok(SUCCEEDED(XGameSaveEnumerateBlobInfo(reopened, nullptr, &on_blob)), "EnumerateBlobInfo");
    ok(g_blob_callbacks == 1, "枚举到 1 个 blob（回调真的被调用）");
    ok(g_blob_size == kBlobSize, "枚举给出的 size 正确");

    InfoCtx info_ctx;
    ok(SUCCEEDED(XGameSaveGetContainerInfo(provider, "slot_a", &info_ctx, &on_container_info)) &&
           info_ctx.seen && info_ctx.filled,
       "GetContainerInfo 填了 blobCount/totalSize/时间");

    int64_t quota = -1;
    Blk qblock{};
    ok(SUCCEEDED(XGameSaveGetRemainingQuotaAsync(provider, &qblock)), "GetRemainingQuotaAsync");
    wait_done(&qblock, "GetRemainingQuotaAsync");
    ok(SUCCEEDED(XGameSaveGetRemainingQuotaResult(&qblock, &quota)) && quota > 0,
       "剩余配额为正");

    XGameSaveCloseContainer(reopened);
    reopened = nullptr;

    // ---- 6) 删容器：必须真的消失（不是留个空目录） ----
    ok(SUCCEEDED(XGameSaveDeleteContainer(provider, "slot_a")), "DeleteContainer(slot_a)");
    ok(!file_exists(container_dir), "容器目录已被递归删除");

    InfoCtx probe_ctx;
    const HRESULT gone_hr =
        XGameSaveGetContainerInfo(provider, "slot_a", &probe_ctx, &on_container_info_any);
    ok(FAILED(gone_hr), "已删除容器 GetContainerInfo 失败（不是空目录）");

    // 重开：容器可以是空的，但旧 blob 必须读不到
    void* again = nullptr;
    ok(SUCCEEDED(XGameSaveCreateContainer(provider, "slot_a", &again)) && again,
       "删除后可重新建同名容器");
    if (again) {
        g_blob_callbacks = 0;
        XGameSaveEnumerateBlobInfo(again, nullptr, &on_blob);
        ok(g_blob_callbacks == 0, "重建的容器里没有残留 blob");

        unsigned c3 = 1;
        std::vector<unsigned char> rbuf3(sizeof(Blob) + kBlobSize, 0);
        const HRESULT read_deleted = XGameSaveReadBlobData(
            again, names, &c3, rbuf3.size(), reinterpret_cast<Blob*>(rbuf3.data()));
        ok(read_deleted == kNotFound, "读已删除的 blob 回 HRESULT_FROM_WIN32(ERROR_NOT_FOUND)");
        XGameSaveCloseContainer(again);
    }

    // ---- 7) 异步删容器 + 路径穿越防护 ----
    void* victim = nullptr;
    ok(SUCCEEDED(XGameSaveCreateContainer(provider, "slot_del", &victim)) && victim,
       "CreateContainer(slot_del)");
    if (victim) XGameSaveCloseContainer(victim);
    Blk del{};
    ok(SUCCEEDED(XGameSaveDeleteContainerAsync(provider, "slot_del", &del)),
       "DeleteContainerAsync 被受理");
    wait_done(&del, "DeleteContainerAsync");
    ok(SUCCEEDED(XGameSaveDeleteContainerResult(&del)), "DeleteContainerResult = S_OK");
    ok(!file_exists(cfg_ns + "\\slot_del"), "异步删除的容器目录已消失");

    // 路径穿越防护：先用一个兄弟容器当"证人"，穿越尝试不能伤到它，也不能删到根目录外
    void* keep = nullptr;
    ok(SUCCEEDED(XGameSaveCreateContainer(provider, "keep_me", &keep)) && keep,
       "CreateContainer(keep_me)");
    if (keep) XGameSaveCloseContainer(keep);

    const HRESULT trav1 = XGameSaveDeleteContainer(provider, "..");
    const HRESULT trav2 = XGameSaveDeleteContainer(provider, "..\\..\\..\\evil");
    // 以前写成 `== E_GS_INVALID_CONTAINER_NAME || SUCCEEDED(trav)` —— 成功恰恰是漏洞：
    // 实现把 ".." sanitize 成 "default" 后当合法名删掉了（本机实测日志：
    // "[save2] DeleteContainer(..)：目录本就不存在" -> S_OK）。只有错误码才可接受。
    ok(trav1 == E_GS_INVALID_CONTAINER_NAME,
       "容器名 \"..\" 被拒绝（E_GS_INVALID_CONTAINER_NAME，不接受 S_OK）",
       trav1 == E_GS_INVALID_CONTAINER_NAME ? "" : "实现把它当合法名接受了");
    ok(trav2 == E_GS_INVALID_CONTAINER_NAME,
       "容器名含分隔符被拒绝（E_GS_INVALID_CONTAINER_NAME）",
       trav2 == E_GS_INVALID_CONTAINER_NAME ? "" : "实现把它当合法名接受了");
    ok(file_exists(cfg_ns + "\\keep_me"), "穿越尝试后兄弟容器仍在");
    ok(file_exists(cfg_ns), "provider 命名空间仍在（未发生路径穿越）");
    ok(file_exists(pls), "PLS 根目录仍在");
    const std::string pls_str(pls);
    const size_t cut = pls_str.find_last_of("\\/");
    const std::string pls_parent = cut == std::string::npos ? "." : pls_str.substr(0, cut);
    ok(!file_exists(pls_parent + "\\evil"), "根目录外没有出现 evil 目录");

    XGameSaveCloseProvider(provider);
    xbgtasix::logf("[t24] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
