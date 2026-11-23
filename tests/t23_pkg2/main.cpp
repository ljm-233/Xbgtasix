// SPDX-License-Identifier: GPL-3.0-or-later
// t23_pkg2 — package_more.cpp（第二批 XPackage 接口）的独立冒烟测试
//
// 覆盖 7 个同步 API：GetPackageKind / EnumerateFeatures / RegisterPackageInstalled /
// UnregisterPackageInstalled / InstallChunks（同步版）/ UninstallPackage /
// UninstallUWPInstance。
//
// 本批 7 个接口里没有任何 *Async（XPackageInstallChunks 的同步版就是同步的），
// 所以没有"XAsyncBlock 永远不完成"的断言对象。但"安装类入口不许卡住"这条必须守：
// InstallChunks 用 GetTickCount64 量了耗时，断言 < 1000ms（防有人把它改成等 5 秒）。
//
// 夹具：本测试自己造一个包根目录（exe 同目录的 t23_pkg2_root，失败则落到 %TEMP%），
// 里面放 features.txt + dlc01 子目录，全程不碰真实包目录，也不删任何东西。
//
// 原型逐条对照 gdk/include_clean/XPackage.h（行号写在每条上面）。上一轮有代理凭记忆
// 写参数顺序，结果 E_INVALIDARG + 回调一次都没触发，这里不敢省这一步。
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xbgtasix/trace.h"

// ---- 逐字抄自 XPackage.h 的类型（不做任何"等价替换"） ----

// XPackage.h:88-96
struct T23Feature
{
    const char* id;            // _Field_z_
    const char* displayName;   // _Field_z_
    const char* tags;          // _Field_z_
    bool hidden;               // 注意：bool 后面有 3 字节 padding
    uint32_t storeIdCount;
    const char** storeIds;
};

// XTaskQueue.h:78-81
struct T23RegToken
{
    uint64_t token;
};

// XPackage.h:111（typedef bool CALLBACK XPackageFeatureEnumerationCallback(void*, const XPackageFeature*)）
typedef bool(CALLBACK* T23FeatureCb)(void* context, const T23Feature* feature);
// XPackage.h:113（typedef void CALLBACK XPackageInstalledCallback(void*, const XPackageDetails*)）
typedef void(CALLBACK* T23InstalledCb)(void* context, const void* details);

// ABI 自检：bool + uint32_t + 指针的布局与 DLL 侧必须一致，否则 storeIds 会读错位
#if defined(_WIN64)
static_assert(sizeof(T23Feature) == 40, "T23Feature 布局必须与 XPackageFeature 同宽");
static_assert(offsetof(T23Feature, storeIds) == 32, "storeIds 偏移错位会读到垃圾指针");
#endif
static_assert(sizeof(T23RegToken) == sizeof(uint64_t), "XTaskQueueRegistrationToken 就是 8 字节");

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize(void);

// XPackage.h:116
HRESULT WINAPI XPackageGetCurrentProcessPackageIdentifier(size_t bufferSize, char* buffer);
// XPackage.h:228（XPackageKind 是 enum class : uint32_t，用 unsigned 承接，指针宽度一致）
HRESULT WINAPI XPackageGetPackageKind(const char* packageIdentifier, unsigned* kind);
// XPackage.h:245
HRESULT WINAPI XPackageEnumerateFeatures(const char* packageIdentifier, void* context,
                                         T23FeatureCb callback);
// XPackage.h:233
HRESULT WINAPI XPackageRegisterPackageInstalled(void* queue, void* context,
                                                T23InstalledCb callback,
                                                T23RegToken* token);
// XPackage.h:240（token 按值传结构体，不能拆成裸 uint64，32 位上调用约定会变）
bool WINAPI XPackageUnregisterPackageInstalled(T23RegToken token, bool wait);
// XPackage.h:183 —— 7 个参数，middle 是 selectorCount/selectors/interval/suppressUi/queue
HRESULT WINAPI XPackageInstallChunks(const char* packageIdentifier, uint32_t selectorCount,
                                     void* selectors, uint32_t minimumUpdateIntervalMs,
                                     bool suppressUserConfirmation, void* queue,
                                     void** installationMonitor);
// XPackage.h:290
bool WINAPI XPackageUninstallPackage(const char* packageIdentifier);
// XPackage.h:286
HRESULT WINAPI XPackageUninstallUWPInstance(const char* packageName);
}

namespace {

int g_fail = 0;
int g_count = 0;
int g_limit = -1;          // >=0 时回调返回 false，用于验证"调用方喊停就停"
std::vector<std::string> g_ids;
std::vector<std::string> g_names;
std::vector<std::string> g_tags;
std::vector<bool> g_hidden;
std::vector<uint32_t> g_store_count;
std::vector<bool> g_store_ptr_ok;
std::string g_first_store;

void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t23]   PASS  %-40s %s", what, detail);
    } else {
        xbgtasix::logf("[t23]   FAIL  %-40s %s", what, detail);
        ++g_fail;
    }
}

const char* detail_hr(HRESULT hr) {
    static char buf[48];
    std::snprintf(buf, sizeof(buf), "hr=0x%08lx", static_cast<unsigned long>(hr));
    return buf;
}

// XPackageFeatureEnumerationCallback：bool 返回，false = 停止枚举
bool CALLBACK on_feature(void* context, const T23Feature* f) {
    (void)context;
    if (!f) {
        ++g_fail;
        return false;
    }
    ++g_count;
    // 指针只在回调期间有效，全部拷成 std::string（这正是本次要验的生命周期）
    g_ids.emplace_back(f->id ? f->id : "(null)");
    g_names.emplace_back(f->displayName ? f->displayName : "(null)");
    g_tags.emplace_back(f->tags ? f->tags : "(null)");
    g_hidden.push_back(f->hidden);
    g_store_count.push_back(f->storeIdCount);
    const bool ptr_ok = (f->storeIdCount == 0) ? true : (f->storeIds != nullptr);
    g_store_ptr_ok.push_back(ptr_ok);
    if (g_first_store.empty() && f->storeIdCount > 0 && f->storeIds && f->storeIds[0])
        g_first_store = f->storeIds[0];
    if (g_limit >= 0 && g_count >= g_limit) return false;
    return true;
}

void CALLBACK on_installed(void* context, const void* details) {
    (void)context;
    (void)details;
}

// ---------------- 夹具目录 ----------------

bool ensure_dir(const std::string& path) {
    if (CreateDirectoryA(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool write_file(const std::string& path, const char* text) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const DWORD len = static_cast<DWORD>(std::strlen(text));
    const bool okw = WriteFile(h, text, len, &written, nullptr) && written == len;
    CloseHandle(h);
    return okw;
}

std::string exe_dir() {
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (char* slash = std::strrchr(exe, '\\')) *(slash + 1) = 0;
    return std::string(exe);
}

// 清单格式：id|displayName|tags|hidden|storeId1,storeId2（'#' 开头是注释）
const char kManifest[] =
    "# id|displayName|tags|hidden|storeIds\n"
    "seasonpass|Season Pass|paid;dlc|0|9NBLGGH4ABCD,9NBLGGH4EFGH\n"
    "lang_fr|French|lang|1|\n"
    "bare_feature\n";

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) SetEnvironmentVariableA("XBGTASIX_CONFIG", argv[1]);
    if (argc > 2) {
        // 刻意不使用 argv[2] 当包根：本测试要往里写 features.txt/子目录，
        // 写进真实包目录就是污染。这里造独立夹具。
        xbgtasix::logf("[t23] 忽略 argv[2]=%s（本测试自建夹具目录）", argv[2]);
    }

    // ---- 0) 造夹具 ----
    std::string root = exe_dir() + "t23_pkg2_root";
    if (!ensure_dir(root)) {
        char tmp[MAX_PATH] = {0};
        GetTempPathA(MAX_PATH, tmp);
        root = std::string(tmp) + "t23_pkg2_root";
        (void)ensure_dir(root);
    }
    const bool root_ok = ensure_dir(root);
    ok(root_ok, "自建包根目录", root.c_str());
    if (!root_ok) {
        xbgtasix::logf("[t23] 夹具目录建不出来，后续断言必然失败");
    }
    // DLC 子目录（Content 判定用）
    ok(ensure_dir(root + "\\dlc01"), "自建 dlc01 子目录");
    // 没有 features.txt 的目录（E_NOTIMPL 判定用）
    std::string empty_root = root + "\\no_features";
    (void)ensure_dir(empty_root);
    ok(write_file(root + "\\features.txt", kManifest), "写入 features.txt");

    SetEnvironmentVariableA("XBGTASIX_PACKAGE_ROOT", root.c_str());
    (void)XGameRuntimeInitialize();

    // ---- 1) XPackageGetPackageKind ----
    unsigned kind = 0xFFFFFFFFu;
    HRESULT hr = XPackageGetPackageKind("*", &kind);
    ok(SUCCEEDED(hr) && kind == 0 /* Game */, "GetPackageKind(\"*\") == Game", detail_hr(hr));

    char proc_id[64] = {0};
    const HRESULT id_hr = XPackageGetCurrentProcessPackageIdentifier(sizeof(proc_id), proc_id);
    if (SUCCEEDED(id_hr) && proc_id[0]) {
        kind = 0xFFFFFFFFu;
        hr = XPackageGetPackageKind(proc_id, &kind);
        ok(SUCCEEDED(hr) && kind == 0, "GetPackageKind(进程自身 id) == Game", proc_id);
    } else {
        xbgtasix::logf("[t23] 读不到进程包标识符（无 MicrosoftGame.Config），跳过自身 id 判定");
    }

    kind = 0xFFFFFFFFu;
    hr = XPackageGetPackageKind("dlc01", &kind);
    ok(SUCCEEDED(hr) && kind == 1 /* Content */, "GetPackageKind(子目录) == Content",
       detail_hr(hr));

    kind = 0xFFFFFFFFu;
    hr = XPackageGetPackageKind("no_such_pkg_xyz", &kind);
    ok(hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND), "GetPackageKind(不存在) == E_NOTFOUND",
       detail_hr(hr));

    kind = 0xFFFFFFFFu;
    hr = XPackageGetPackageKind(nullptr, &kind);
    ok(hr == E_INVALIDARG, "GetPackageKind(nullptr id) == E_INVALIDARG", detail_hr(hr));
    hr = XPackageGetPackageKind("*", nullptr);
    ok(hr == E_INVALIDARG, "GetPackageKind(nullptr out) == E_INVALIDARG", detail_hr(hr));

    // ---- 2) XPackageEnumerateFeatures ----
    hr = XPackageEnumerateFeatures("*", nullptr, &on_feature);
    ok(SUCCEEDED(hr), "EnumerateFeatures 成功", detail_hr(hr));
    ok(g_count == 3, "枚举到 3 个 feature（注释行/空 id 被跳过）");
    if (g_count == 3) {
        ok(g_ids[0] == "seasonpass", "feature[0].id", g_ids[0].c_str());
        ok(g_names[0] == "Season Pass", "feature[0].displayName", g_names[0].c_str());
        ok(g_tags[0] == "paid;dlc", "feature[0].tags", g_tags[0].c_str());
        ok(!g_hidden[0], "feature[0].hidden == false");
        ok(g_store_count[0] == 2 && g_store_ptr_ok[0], "feature[0] 有 2 个 storeId 且指针非空");
        ok(g_first_store == "9NBLGGH4ABCD", "feature[0].storeIds[0] 内容正确",
           g_first_store.c_str());
        ok(g_names[1] == "French" && g_hidden[1], "feature[1] hidden == true",
           g_names[1].c_str());
        ok(g_store_count[1] == 0 && g_store_ptr_ok[1], "feature[1] 零 storeId");
        ok(g_names[2] == "bare_feature", "只写 id 时 displayName 回落成 id",
           g_names[2].c_str());
    }

    // 调用方返回 false 必须让枚举立刻停
    g_count = 0;
    g_limit = 1;
    hr = XPackageEnumerateFeatures("*", nullptr, &on_feature);
    g_limit = -1;
    ok(SUCCEEDED(hr) && g_count == 1, "回调返回 false 时停止枚举（只回 1 次）");

    // 没有清单 -> E_NOTIMPL（不许 S_OK + 零回调假装"这个包没有 feature"）
    SetEnvironmentVariableA("XBGTASIX_PACKAGE_ROOT", empty_root.c_str());
    g_count = 0;
    hr = XPackageEnumerateFeatures("*", nullptr, &on_feature);
    ok(hr == E_NOTIMPL && g_count == 0, "无 features.txt == E_NOTIMPL 且零回调", detail_hr(hr));
    SetEnvironmentVariableA("XBGTASIX_PACKAGE_ROOT", root.c_str());

    hr = XPackageEnumerateFeatures("*", nullptr, nullptr);
    ok(hr == E_INVALIDARG, "EnumerateFeatures(nullptr callback) == E_INVALIDARG",
       detail_hr(hr));

    // ---- 3) 包安装通知的注册 / 注销 ----
    T23RegToken token = {0};
    hr = XPackageRegisterPackageInstalled(nullptr, nullptr, &on_installed, &token);
    ok(SUCCEEDED(hr) && token.token != 0, "RegisterPackageInstalled 给出非零 token",
       detail_hr(hr));
    const uint64_t got_token = token.token;

    T23RegToken again = {0};
    hr = XPackageRegisterPackageInstalled(nullptr, nullptr, &on_installed, &again);
    ok(SUCCEEDED(hr) && again.token != 0 && again.token != got_token, "第二个 token 唯一",
       detail_hr(hr));

    ok(XPackageUnregisterPackageInstalled(token, true), "注销已注册 token == true");
    ok(!XPackageUnregisterPackageInstalled(token, false), "重复注销旧 token == false");
    ok(XPackageUnregisterPackageInstalled(again, true), "注销第二个 token == true");

    T23RegToken null_token = {0xDEADBEEFull};
    hr = XPackageRegisterPackageInstalled(nullptr, nullptr, nullptr, &null_token);
    ok(hr == E_INVALIDARG && null_token.token == 0,
       "Register(null callback) == E_INVALIDARG 且 token 被清零", detail_hr(hr));
    hr = XPackageRegisterPackageInstalled(nullptr, nullptr, &on_installed, nullptr);
    ok(hr == E_INVALIDARG, "Register(null token) == E_INVALIDARG", detail_hr(hr));

    // ---- 4) 同步安装：必须明确 E_NOTIMPL，且不留野句柄、不卡住 ----
    void* monitor = reinterpret_cast<void*>(0x1);
    const ULONGLONG t0 = GetTickCount64();
    hr = XPackageInstallChunks("*", 0, nullptr, 1000, false, nullptr, &monitor);
    const ULONGLONG dt = GetTickCount64() - t0;
    ok(hr == E_NOTIMPL, "InstallChunks == E_NOTIMPL（无安装器，不假装成功）", detail_hr(hr));
    ok(monitor == nullptr, "失败时 *installationMonitor 被置空（不留野句柄）");
    ok(dt < 1000, "InstallChunks 耗时 < 1 秒（没有卡住）");

    hr = XPackageInstallChunks("*", 0, nullptr, 0, true, nullptr, nullptr);
    ok(hr == E_INVALIDARG, "InstallChunks(nullptr out) == E_INVALIDARG", detail_hr(hr));

    // ---- 5) 卸载：恒 false，绝不假装删了东西 ----
    ok(!XPackageUninstallPackage("dlc01"), "UninstallPackage(存在的包) == false");
    ok(!XPackageUninstallPackage("no_such_pkg_xyz"), "UninstallPackage(不存在) == false");
    ok(!XPackageUninstallPackage(nullptr), "UninstallPackage(nullptr) == false");
    // 证明我们真的没删东西
    ok(GetFileAttributesA((root + "\\dlc01").c_str()) != INVALID_FILE_ATTRIBUTES,
       "dlc01 目录仍然存在（没有偷偷删）");

    hr = XPackageUninstallUWPInstance("Some.Uwp.Package_1.0.0.0_x64__abc");
    ok(hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND), "UninstallUWPInstance == E_NOTFOUND",
       detail_hr(hr));
    hr = XPackageUninstallUWPInstance(nullptr);
    ok(hr == E_INVALIDARG, "UninstallUWPInstance(nullptr) == E_INVALIDARG", detail_hr(hr));

    xbgtasix::logf("[t23] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
