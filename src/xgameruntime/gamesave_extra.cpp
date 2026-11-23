// SPDX-License-Identifier: GPL-3.0-or-later
// gamesave_extra.cpp —— XGameSave 的"容器式"接口（provider / container / blob）
//
// 已有的目录式存档（XGameSaveFiles，xgameruntime.cpp:932-999）是"给游戏一个目录，游戏自己
// 读写文件"。这里补的是主机上真正用的那套：游戏不碰文件，只跟 provider / container / blob 打交道，
// 由运行时负责落盘。PC 上最自然的映射：
//
//     PLS 根目录（XBGTASIX_PLS_ROOT，否则 %LOCALAPPDATA%\Xbgtasix\<TitleId>）
//       └─ Containers\<sanitize(configurationId)>          = provider 的命名空间
//            └─ <sanitize(containerName)>                  = 容器 = 一个子目录
//                 └─ <sanitize(blobName)>                  = blob = 目录里的一个文件
//
// 为什么多一层 Containers：目录式的 XGameSaveFiles 已经占用了 PLS\SaveData\<configurationId>
// （xgameruntime.cpp:951），容器式再往那儿塞子目录会让两套 API 的枚举互相看见；
// 分开命名空间后，谁也不会把对方的目录当成自己的容器/blob。
//
// 签名一律逐字来自 gdk/include_clean/XGameSave.h（149 行，已逐条核对）。
// 头文件里【没有】的接口（XGameSaveOpenContainer / WriteBlob / ReadBlob / SubmitContainerDelete）
// 一律不臆造 —— 见本文件末尾 "头文件里没有的接口" 注释。
//
// 关于异步：本项目的 XAsyncGetStatus 只认 xgameruntime.cpp 的内部完成表（g_async），
// 而那张表只有已导出的 XAsyncRun（exports.txt:24，实现 xgameruntime.cpp:594）能写入。
// 所以所有 *Async 都走 XAsyncRun；下面的 Blk 是 XAsyncRun 同步失败时的兜底回调路径。
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "XAsync.h"
#include "XGame.h"
#include "XGameSave.h"
#include "XTaskQueue.h"
#include "XUser.h"

#include "xbgtasix/trace.h"
#include "trace_entry.h"   // 统一入口追踪（XB_TRACE_ENTRY）

// 精简过的 GDK 头文件没带这个宏；数值与真 GDK / xgameruntime.cpp 一致。
#ifndef E_NOTFOUND
#define E_NOTFOUND HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
#endif

// ---------------- 句柄（typedef struct XGameSaveProvider* 等，必须是全局作用域） ----------------

struct XGameSaveProvider
{
    std::string root;      // <PLS>\Containers\<configurationId>
    std::string config_id; // configurationId，日志用
};

struct XGameSaveContainer
{
    XGameSaveProviderHandle provider = nullptr;
    std::string dir;  // 容器目录（= provider->root\容器名）
    std::string name; // 游戏给的容器名（未脱敏，回填 info.name 用）
};

struct XGameSaveUpdate
{
    XGameSaveContainerHandle container = nullptr;
    std::string display_name;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> writes;
    std::vector<std::string> deletes;
};

// ---------------- XAsyncBlock 完成（ABI 陷阱集中处） ----------------

// 字段顺序 queue/context/callback/internal —— 顺序错一位，回调就是野指针。
// 与 gdk/include_clean/XAsync.h:46 的 struct XAsyncBlock 逐字段对齐。
struct Blk
{
    void* queue;
    void* context;
    void (*callback)(Blk*);
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};

// 只在 XAsyncRun 同步失败时兜底用（至少让回调打出去一次，别让引擎空等）。
static void blk_finish(Blk* b, HRESULT hr)
{
    (void)hr;
    if (b && b->callback) b->callback(b);
}

namespace {

static_assert(sizeof(Blk) == sizeof(XAsyncBlock), "Blk 必须与 XAsyncBlock 同宽");
static_assert(offsetof(Blk, queue) == offsetof(XAsyncBlock, queue), "Blk.queue 偏移不符");
static_assert(offsetof(Blk, context) == offsetof(XAsyncBlock, context), "Blk.context 偏移不符");
static_assert(offsetof(Blk, callback) == offsetof(XAsyncBlock, callback), "Blk.callback 偏移不符");
static_assert(offsetof(Blk, internal) == offsetof(XAsyncBlock, internal), "Blk.internal 偏移不符");

// ---------------- 路径：与 xgameruntime.cpp:208 ensure_pls_path() 同一规则 ----------------
//
// 那个函数在匿名命名空间里，本文件拿不到，所以照抄一遍规则：
//   XBGTASIX_PLS_ROOT（便携安装 / 受限沙箱 / 测试隔离）→ 否则 %LOCALAPPDATA%\Xbgtasix
//   再拼 "\<TitleId:%08lX>"。TitleId 同样用已导出的 XGameGetXboxTitleId 取。
// 逐级 CreateDirectoryA，建完必须 GetFileAttributes 验证（docs/env-gotchas.md 第 2 条：
// 不验证就会出现"日志说成功、实际目录根本不存在"）。

std::string pls_root()
{
    char base[MAX_PATH] = {0};
    if (::GetEnvironmentVariableA("XBGTASIX_PLS_ROOT", base, MAX_PATH) == 0 || base[0] == 0) {
        if (::GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH) == 0 || base[0] == 0) {
            xbgtasix::logf("[save2] PLS 根目录计算失败：XBGTASIX_PLS_ROOT 与 LOCALAPPDATA 都没读到");
            return {};
        }
        const size_t n = std::strlen(base);
        _snprintf(base + n, MAX_PATH - n, "\\Xbgtasix");
        base[MAX_PATH - 1] = 0;
    }
    uint32_t title_id = 0;
    if (FAILED(XGameGetXboxTitleId(&title_id))) title_id = 0;
    char buf[MAX_PATH] = {0};
    _snprintf(buf, MAX_PATH, "%s\\%08lX", base, static_cast<unsigned long>(title_id));
    buf[MAX_PATH - 1] = 0;
    return std::string(buf);
}

// 逐级建目录 + 事后验证。返回 false 时已带 GetLastError 记日志。
bool ensure_dir_tree(const std::string& path)
{
    if (path.size() < 4) return false;
    std::string cur;
    size_t i = 0;
    if (path.size() > 1 && path[1] == ':') { // 盘符前缀 "C:"
        cur = path.substr(0, 2);
        i = 2;
    }
    for (; i < path.size(); ++i) {
        const char c = path[i];
        if (c == '\\' || c == '/') {
            const size_t floor = (cur.size() > 1 && cur[1] == ':') ? 2u : 0u;
            if (cur.size() > floor) ::CreateDirectoryA(cur.c_str(), nullptr);
        }
        cur.push_back(c);
    }
    if (!cur.empty()) ::CreateDirectoryA(cur.c_str(), nullptr);

    const DWORD attr = ::GetFileAttributesA(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        xbgtasix::logf("[save2] 目录创建失败（GetLastError=%lu）: %s",
                       static_cast<unsigned long>(::GetLastError()), path.c_str());
        return false;
    }
    return true;
}

// 脱敏：GDK 的名字是自由字符串，这里当文件名用，非法字符替换成 '_'。
// 替换掉 '\\' '/' 之后已不可能带分隔符，".." 之类的整段名再单独挡掉。
std::string sanitize(const std::string& in, size_t max_len)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        const bool bad = c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' ||
                         c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 0x20;
        out.push_back(bad ? '_' : c);
    }
    if (out.size() > max_len) out.resize(max_len);
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.empty() || out == "." || out == "..") out = "default";
    return out;
}

// 防路径穿越：path 必须是 root 的【真子目录】，且相对段里不许出现 ".."、空段。
// 删除类操作（递归删目录）之前必须过这一关。
bool is_under(const std::string& root, const std::string& path)
{
    if (root.empty() || path.size() <= root.size() + 1) return false;
    if (path.compare(0, root.size(), root) != 0) return false;
    if (path[root.size()] != '\\') return false;
    const std::string rel = path.substr(root.size() + 1);
    size_t pos = 0;
    while (pos <= rel.size()) {
        const size_t nx = rel.find('\\', pos);
        const std::string seg =
            rel.substr(pos, nx == std::string::npos ? std::string::npos : nx - pos);
        if (seg.empty() || seg == "..") return false;
        if (nx == std::string::npos) break;
        pos = nx + 1;
    }
    return true;
}

// ---------------- 名字驻留池 ----------------
//
// XGameSaveBlobInfo::name / XGameSaveContainerInfo::name 是 const char*，指向的字符串必须在
// 调用方用完之后仍然有效。deque 的 push_back 不会让已有元素的地址失效（vector 会！），
// 所以拿它当存储；再配一张 map 去重，避免长跑游戏里无限增长。
std::mutex g_pool_mu;
std::deque<std::string> g_name_pool;
std::unordered_map<std::string, const char*> g_name_cache;

const char* intern(const std::string& s)
{
    std::lock_guard<std::mutex> lock(g_pool_mu);
    auto it = g_name_cache.find(s);
    if (it != g_name_cache.end()) return it->second;
    g_name_pool.push_back(s);
    const char* p = g_name_pool.back().c_str();
    g_name_cache.emplace(s, p);
    return p;
}

// ---------------- 目录枚举 / 大小 / 时间 ----------------

struct DirInfo
{
    uint32_t file_count = 0;
    uint64_t total_size = 0;
    time_t last_modified = 0;
    bool exists = false;
};

bool list_files(const std::string& dir, std::vector<std::string>* names, DirInfo* info)
{
    WIN32_FIND_DATAA fd{};
    const std::string pattern = dir + "\\*";
    HANDLE h = ::FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (names) names->push_back(fd.cFileName);
        if (info) {
            LARGE_INTEGER sz{};
            sz.LowPart = fd.nFileSizeLow;
            sz.HighPart = static_cast<LONG>(fd.nFileSizeHigh);
            ++info->file_count;
            info->total_size += static_cast<uint64_t>(sz.QuadPart);
        }
    } while (::FindNextFileA(h, &fd));
    ::FindClose(h);
    return true;
}

bool list_dirs(const std::string& dir, std::vector<std::string>* names)
{
    WIN32_FIND_DATAA fd{};
    const std::string pattern = dir + "\\*";
    HANDLE h = ::FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0) continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (names) names->push_back(fd.cFileName);
    } while (::FindNextFileA(h, &fd));
    ::FindClose(h);
    return true;
}

time_t ft_to_time(const FILETIME& ft)
{
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    if (u.QuadPart <= 116444736000000000ULL) return 0;
    return static_cast<time_t>((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

DirInfo dir_info(const std::string& dir)
{
    DirInfo info;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExA(dir.c_str(), GetFileExInfoStandard, &fad)) {
        if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            info.exists = true;
            info.last_modified = ft_to_time(fad.ftLastWriteTime);
        }
    }
    if (!info.exists) return info;
    list_files(dir, nullptr, &info);
    return info;
}

// 递归删目录（先确认在根目录下，见 is_under 的调用点）。
bool remove_tree(const std::string& dir)
{
    WIN32_FIND_DATAA fd{};
    const std::string pattern = dir + "\\*";
    HANDLE h = ::FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (std::strcmp(fd.cFileName, ".") == 0 || std::strcmp(fd.cFileName, "..") == 0) continue;
            const std::string child = dir + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_tree(child);
            } else {
                ::SetFileAttributesA(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!::DeleteFileA(child.c_str()))
                    xbgtasix::logf("[save2] DeleteFileA 失败（%lu）: %s",
                                   static_cast<unsigned long>(::GetLastError()), child.c_str());
            }
        } while (::FindNextFileA(h, &fd));
        ::FindClose(h);
    }
    if (!::RemoveDirectoryA(dir.c_str())) {
        xbgtasix::logf("[save2] RemoveDirectoryA 失败（%lu）: %s",
                       static_cast<unsigned long>(::GetLastError()), dir.c_str());
        return false;
    }
    return true;
}

// ---------------- blob 文件读写 ----------------

HRESULT write_blob_file(const std::string& path, const uint8_t* data, size_t n)
{
    HANDLE h = ::CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        xbgtasix::logf("[save2] 建 blob 文件失败（%lu）: %s", static_cast<unsigned long>(err),
                       path.c_str());
        if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) return E_GS_OUT_OF_LOCAL_STORAGE;
        return HRESULT_FROM_WIN32(err);
    }
    size_t done = 0;
    HRESULT hr = S_OK;
    while (done < n) {
        const DWORD chunk = static_cast<DWORD>(n - done > 0x10000000u ? 0x10000000u : n - done);
        DWORD wrote = 0;
        if (!::WriteFile(h, data + done, chunk, &wrote, nullptr) || wrote == 0) {
            const DWORD err = ::GetLastError();
            xbgtasix::logf("[save2] WriteFile 失败（%lu）: %s", static_cast<unsigned long>(err),
                           path.c_str());
            hr = (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL)
                     ? E_GS_OUT_OF_LOCAL_STORAGE
                     : HRESULT_FROM_WIN32(err);
            break;
        }
        done += wrote;
    }
    if (SUCCEEDED(hr)) ::FlushFileBuffers(h); // 玩家会硬断电：真落盘而不是只进缓存
    ::CloseHandle(h);
    return hr;
}

// 读 blob：用 CreateFileA 判断存在性（头文件语义：不存在就是 ERROR_NOT_FOUND）。
HRESULT read_blob_file(const std::string& path, std::vector<uint8_t>* out)
{
    HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        return HRESULT_FROM_WIN32(err);
    }
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (LONGLONG)GS_MAX_BLOB_SIZE) {
        ::CloseHandle(h);
        return E_GS_UPDATE_TOO_BIG;
    }
    out->assign(static_cast<size_t>(sz.QuadPart), 0);
    size_t done = 0;
    while (done < out->size()) {
        DWORD got = 0;
        if (!::ReadFile(h, out->data() + done, static_cast<DWORD>(out->size() - done), &got, nullptr) ||
            got == 0) {
            const DWORD err = ::GetLastError();
            ::CloseHandle(h);
            xbgtasix::logf("[save2] ReadFile 失败（%lu）: %s", static_cast<unsigned long>(err),
                           path.c_str());
            return err ? HRESULT_FROM_WIN32(err) : E_FAIL;
        }
        done += got;
    }
    ::CloseHandle(h);
    return S_OK;
}

// ============================================================================
// 异步完成：所有 *Async 都通过已导出的 XAsyncRun 走到"完成"
// ============================================================================

struct PendingBlob
{
    std::string name;
    std::vector<uint8_t> bytes;
};

struct Outcome
{
    HRESULT hr = E_UNEXPECTED;
    XGameSaveProviderHandle provider = nullptr; // InitializeProviderAsync 的结果
    int64_t quota = 0;                          // GetRemainingQuotaAsync 的结果
    std::vector<PendingBlob> blobs;             // ReadBlobDataAsync 的结果
};

std::mutex g_out_mu;
std::unordered_map<XAsyncBlock*, Outcome> g_outcome;

// XAsyncRun 的 work：把登记好的 HRESULT 原样当作异步结果返回。
HRESULT CALLBACK gs_work(XAsyncBlock* async) noexcept
{
    std::lock_guard<std::mutex> lock(g_out_mu);
    auto it = g_outcome.find(async);
    if (it == g_outcome.end()) return E_UNEXPECTED;
    return it->second.hr;
}

// 登记结果 + 让 block 真正完成（宿主 g_async 表由 XAsyncRun 写入）。
//
// 后面那段自旋不是装饰：XAsyncRun 起的是 detach 线程（xgameruntime.cpp:600），
// 如果调用方拿到返回值就把栈上的 block 丢了，worker 线程还会回头写它（UAF）。
// 这里等到"状态已登记"（≤200ms，实际 <1ms）再返回，与 ui_extra.cpp:148 的结论一致。
void gs_complete(XAsyncBlock* async, HRESULT hr)
{
    if (!async) return;
    {
        std::lock_guard<std::mutex> lock(g_out_mu);
        g_outcome[async].hr = hr;
    }
    if (FAILED(XAsyncRun(async, &gs_work))) {
        xbgtasix::logf("[save2] XAsyncRun 失败，退化到直接回调 block=%p", static_cast<void*>(async));
        blk_finish(reinterpret_cast<Blk*>(async), hr);
        return;
    }
    for (int i = 0; i < 200; ++i) {
        if (XAsyncGetStatus(async, false) != E_PENDING) return;
        ::Sleep(1);
    }
    xbgtasix::logf("[save2] 异步完成登记超时 block=%p hr=0x%08lx", static_cast<void*>(async),
                   static_cast<unsigned long>(hr));
}

// 取回并清除登记项（Result 用）。没登记过 = 调用方在瞎调。
bool take_outcome(XAsyncBlock* async, Outcome* out)
{
    std::lock_guard<std::mutex> lock(g_out_mu);
    auto it = g_outcome.find(async);
    if (it == g_outcome.end()) return false;
    if (out) *out = std::move(it->second);
    g_outcome.erase(it);
    return true;
}

// 查登记项但不删（FillBlobBuffer 之类的"还要重试"路径用）。
HRESULT peek_hr(XAsyncBlock* async)
{
    std::lock_guard<std::mutex> lock(g_out_mu);
    auto it = g_outcome.find(async);
    return it == g_outcome.end() ? E_PENDING : it->second.hr;
}

// ---------------- 容器目录 / 删除（带穿越检查） ----------------

// 容器名必须"本来就合法"。以前直接用 sanitize()：它会把 ".." 洗成 "default"，
// 于是 DeleteContainer("..") 返回 S_OK（测试 t24 抓到：这等于放行了路径穿越企图）。
bool valid_container_name(const char* name)
{
    if (!name || !*name) return false;
    const size_t len = strlen(name);
    if (len >= GS_MAX_CONTAINER_NAME_SIZE) return false;
    if (name[0] == '.' && (len == 1 || (name[1] == '.' && len == 2))) return false;  // "." / ".."
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x20) return false;
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            return false;
        }
    }
    return !(name[len - 1] == '.' || name[len - 1] == ' ');
}

std::string container_dir(XGameSaveProviderHandle provider, const char* containerName)
{
    if (!provider || !containerName || !containerName[0]) return {};
    if (!valid_container_name(containerName)) return {};   // 非法名字直接判无效，不做"净化"
    const std::string name = containerName;
    const std::string dir = provider->root + "\\" + name;
    if (!is_under(provider->root, dir)) {
        xbgtasix::logf("[save2] 容器名疑似路径穿越（已拒绝）: %s", containerName);
        return {};
    }
    return dir;
}

// provider 的命名空间：<PLS>\Containers\<configurationId>
std::string provider_root(const char* configurationId)
{
    const std::string pls = pls_root();
    if (pls.empty()) return {};
    const std::string cfg = sanitize(configurationId ? configurationId : "default",
                                     GS_MAX_CONFIGURATION_ID_SIZE - 1);
    return pls + "\\Containers\\" + cfg;
}

// 同步删除实现（异步版也走这里，逻辑只有一份）
HRESULT delete_container_impl(XGameSaveProviderHandle provider, const char* containerName)
{
    if (!provider || !containerName) return E_INVALIDARG;
    const std::string dir = container_dir(provider, containerName);
    if (dir.empty()) return E_GS_INVALID_CONTAINER_NAME;
    // 铁律：动手前再确认一次"路径在根目录下"，防穿越
    if (!is_under(provider->root, dir)) return E_GS_NO_ACCESS;

    const DWORD attr = ::GetFileAttributesA(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        // 已不存在：删除是幂等的（GDK 语义），但留下日志好排查
        xbgtasix::logf("[save2] DeleteContainer(%s)：目录本就不存在", containerName);
        return S_OK;
    }
    if (!remove_tree(dir)) return E_FAIL;
    const DWORD after = ::GetFileAttributesA(dir.c_str());
    if (after != INVALID_FILE_ATTRIBUTES) {
        xbgtasix::logf("[save2] DeleteContainer(%s)：删完目录仍在！", containerName);
        return E_FAIL;
    }
    xbgtasix::logf("[save2] DeleteContainer(%s) 已删除目录 %s", containerName, dir.c_str());
    return S_OK;
}

// ---------------- 读 blob 的公共实现（同步 / 异步 Result 共用） ----------------

uint64_t blob_bytes_used(const std::vector<PendingBlob>& blobs)
{
    uint64_t total = 0;
    for (const auto& b : blobs) total += b.bytes.size();
    return total;
}

// 把 [name,data] 按 GDK 的布局写进调用方的 buffer：
//   blobData[0..n) 是结构体数组，payload 紧跟在数组之后，blobData[i].data 指向自己的 payload。
// buffer 不够时回 E_GS_PROVIDED_BUFFER_TOO_SMALL 并把 *countOfBlobs 写成需要的个数。
HRESULT fill_blob_buffer(const std::vector<PendingBlob>& blobs, size_t blobsSize,
                         XGameSaveBlob* blobData, uint32_t* countOfBlobs)
{
    const uint32_t n = static_cast<uint32_t>(blobs.size());
    const uint64_t need_structs = static_cast<uint64_t>(n) * sizeof(XGameSaveBlob);
    const uint64_t need = need_structs + blob_bytes_used(blobs);
    if (n == 0) { // 空集合：不需要 buffer，也别误报"太小"
        *countOfBlobs = 0;
        return S_OK;
    }
    if (!blobData || blobsSize < need) {
        *countOfBlobs = n;
        return E_GS_PROVIDED_BUFFER_TOO_SMALL;
    }
    uint8_t* payload = reinterpret_cast<uint8_t*>(blobData) + need_structs;
    for (uint32_t i = 0; i < n; ++i) {
        blobData[i].info.name = intern(blobs[i].name);
        blobData[i].info.size = static_cast<uint32_t>(blobs[i].bytes.size());
        blobData[i].data = payload;
        if (!blobs[i].bytes.empty())
            std::memcpy(payload, blobs[i].bytes.data(), blobs[i].bytes.size());
        payload += blobs[i].bytes.size();
    }
    *countOfBlobs = n;
    return S_OK;
}

} // namespace

extern "C" {

// ===========================================================================
// provider
// ===========================================================================

STDAPI XGameSaveInitializeProvider(XUserHandle requestingUser, const char* configurationId,
                                   bool syncOnDemand,
                                   XGameSaveProviderHandle* provider) noexcept
{
    XB_TRACE_ENTRY("XGameSaveInitializeProvider", requestingUser, configurationId, syncOnDemand, provider);
    (void)requestingUser; // 本地模拟用户，不校验（与 XGameSaveFiles 一致）
    (void)syncOnDemand;   // 没有云，需求式同步退化为"本地直读"
    if (!provider) return E_INVALIDARG;
    *provider = nullptr;
    if (!configurationId || !configurationId[0]) return E_INVALIDARG;

    const std::string root = provider_root(configurationId);
    if (root.empty()) return E_FAIL;
    if (!ensure_dir_tree(root)) return E_FAIL;

    auto* p = new (std::nothrow) XGameSaveProvider();
    if (!p) return E_OUTOFMEMORY;
    p->root = root;
    p->config_id = configurationId;
    *provider = p;
    xbgtasix::logf("[save2] InitializeProvider(%s) -> %s", configurationId, root.c_str());
    return S_OK;
}

STDAPI XGameSaveInitializeProviderAsync(XUserHandle requestingUser, const char* configurationId,
                                        bool syncOnDemand, XAsyncBlock* async) noexcept
{
    XB_TRACE_ENTRY("XGameSaveInitializeProviderAsync", requestingUser, configurationId, syncOnDemand, async);
    if (!async) return E_INVALIDARG;
    XGameSaveProviderHandle provider = nullptr;
    const HRESULT hr =
        XGameSaveInitializeProvider(requestingUser, configurationId, syncOnDemand, &provider);
    {
        std::lock_guard<std::mutex> lock(g_out_mu);
        Outcome& o = g_outcome[async];
        o.hr = hr;
        o.provider = provider;
    }
    gs_complete(async, hr);
    // GDK 语义：*Async 的返回值只表示"请求是否被受理"，操作结果走 XAsyncGetStatus。
    return S_OK;
}

STDAPI XGameSaveInitializeProviderResult(XAsyncBlock* async,
                                         XGameSaveProviderHandle* provider) noexcept
{
    XB_TRACE_ENTRY("XGameSaveInitializeProviderResult", async, provider, 0ULL, 0ULL);
    if (!async || !provider) return E_INVALIDARG;
    Outcome o;
    if (!take_outcome(async, &o)) return E_UNEXPECTED;
    if (FAILED(o.hr)) {
        *provider = nullptr;
        return o.hr;
    }
    *provider = o.provider;
    return S_OK;
}

STDAPI_(void) XGameSaveCloseProvider(XGameSaveProviderHandle provider) noexcept
{
    XB_TRACE_ENTRY("XGameSaveCloseProvider", provider, 0ULL, 0ULL, 0ULL);
    if (!provider) return;
    xbgtasix::logf("[save2] CloseProvider %s", provider->root.c_str());
    delete provider;
}

// ===========================================================================
// 配额（provider 级）
// ===========================================================================

namespace {

// 没读 MicrosoftGame.Config 时的保守默认（与 xgameruntime.cpp:151 同值）
const uint64_t kDefaultQuota = 256ull * 1024ull * 1024ull;

uint64_t provider_used_bytes(XGameSaveProviderHandle provider)
{
    if (!provider) return 0;
    std::vector<std::string> dirs;
    uint64_t total = 0;
    DirInfo root_info = dir_info(provider->root);
    if (!root_info.exists) return 0;
    total += root_info.total_size; // provider 根下的散文件（正常不存在）
    list_dirs(provider->root, &dirs);
    for (const auto& d : dirs) {
        DirInfo di = dir_info(provider->root + "\\" + d);
        if (di.exists) total += di.total_size;
    }
    return total;
}

HRESULT quota_impl(XGameSaveProviderHandle provider, int64_t* remainingQuota)
{
    if (!provider || !remainingQuota) return E_INVALIDARG;
    const uint64_t used = provider_used_bytes(provider);
    *remainingQuota = used >= kDefaultQuota ? 0 : static_cast<int64_t>(kDefaultQuota - used);
    return S_OK;
}

} // namespace

STDAPI XGameSaveGetRemainingQuota(XGameSaveProviderHandle provider,
                                  int64_t* remainingQuota) noexcept
{
    XB_TRACE_ENTRY("XGameSaveGetRemainingQuota", provider, remainingQuota, 0ULL, 0ULL);
    return quota_impl(provider, remainingQuota);
}

STDAPI XGameSaveGetRemainingQuotaAsync(XGameSaveProviderHandle provider,
                                       XAsyncBlock* async) noexcept
{
    XB_TRACE_ENTRY("XGameSaveGetRemainingQuotaAsync", provider, async, 0ULL, 0ULL);
    if (!async) return E_INVALIDARG;
    int64_t quota = 0;
    const HRESULT hr = quota_impl(provider, &quota);
    {
        std::lock_guard<std::mutex> lock(g_out_mu);
        Outcome& o = g_outcome[async];
        o.hr = hr;
        o.quota = quota;
    }
    gs_complete(async, hr);
    return S_OK;
}

STDAPI XGameSaveGetRemainingQuotaResult(XAsyncBlock* async, int64_t* remainingQuota) noexcept
{
    XB_TRACE_ENTRY("XGameSaveGetRemainingQuotaResult", async, remainingQuota, 0ULL, 0ULL);
    if (!async || !remainingQuota) return E_INVALIDARG;
    const HRESULT hr = peek_hr(async);
    if (hr == E_PENDING) return E_PENDING;
    Outcome o;
    if (!take_outcome(async, &o)) return E_UNEXPECTED;
    if (FAILED(o.hr)) return o.hr;
    *remainingQuota = o.quota;
    return S_OK;
}

// ===========================================================================
// 容器管理
// ===========================================================================

STDAPI XGameSaveCreateContainer(XGameSaveProviderHandle provider, const char* containerName,
                                XGameSaveContainerHandle* containerContext) noexcept
{
    XB_TRACE_ENTRY("XGameSaveCreateContainer", provider, containerName, containerContext, 0ULL);
    if (!provider || !containerName || !containerContext) return E_INVALIDARG;
    *containerContext = nullptr;
    const std::string dir = container_dir(provider, containerName);
    if (dir.empty()) return E_GS_INVALID_CONTAINER_NAME;
    // 同名容器 = 打开：CreateDirectoryA 对已存在的目录返回 ERROR_ALREADY_EXISTS，
    // 但 ensure_dir_tree 只做"最终 GetFileAttributes 验证"，所以这里天然是"建或复用"。
    // （头文件里没有 XGameSaveOpenContainer，GDK 的打开语义就落在这里。）
    if (!ensure_dir_tree(dir)) return E_FAIL;

    auto* c = new (std::nothrow) XGameSaveContainer();
    if (!c) return E_OUTOFMEMORY;
    c->provider = provider;
    c->dir = dir;
    c->name = containerName;
    *containerContext = c;
    xbgtasix::logf("[save2] CreateContainer(%s) -> %s", containerName, dir.c_str());
    return S_OK;
}

STDAPI_(void) XGameSaveCloseContainer(XGameSaveContainerHandle context) noexcept
{
    XB_TRACE_ENTRY("XGameSaveCloseContainer", context, 0ULL, 0ULL, 0ULL);
    if (!context) return;
    xbgtasix::logf("[save2] CloseContainer(%s)", context->name.c_str());
    delete context;
}

STDAPI XGameSaveDeleteContainer(XGameSaveProviderHandle provider,
                                const char* containerName) noexcept
{
    XB_TRACE_ENTRY("XGameSaveDeleteContainer", provider, containerName, 0ULL, 0ULL);
    return delete_container_impl(provider, containerName);
}

STDAPI XGameSaveDeleteContainerAsync(XGameSaveProviderHandle provider, const char* containerName,
                                     XAsyncBlock* async) noexcept
{
    XB_TRACE_ENTRY("XGameSaveDeleteContainerAsync", provider, containerName, async, 0ULL);
    if (!async) return E_INVALIDARG;
    gs_complete(async, delete_container_impl(provider, containerName));
    return S_OK;
}

STDAPI XGameSaveDeleteContainerResult(XAsyncBlock* async) noexcept
{
    XB_TRACE_ENTRY("XGameSaveDeleteContainerResult", async, 0ULL, 0ULL, 0ULL);
    if (!async) return E_INVALIDARG;
    const HRESULT hr = peek_hr(async);
    if (hr == E_PENDING) return E_PENDING;
    Outcome o;
    if (!take_outcome(async, &o)) return E_UNEXPECTED;
    return o.hr;
}

STDAPI XGameSaveGetContainerInfo(XGameSaveProviderHandle provider, const char* containerName,
                                 void* context,
                                 XGameSaveContainerInfoCallback* callback) noexcept
{
    XB_TRACE_ENTRY("XGameSaveGetContainerInfo", provider, containerName, context, callback);
    if (!provider || !containerName || !callback) return E_INVALIDARG;
    const std::string dir = container_dir(provider, containerName);
    if (dir.empty()) return E_GS_INVALID_CONTAINER_NAME;
    const DirInfo di = dir_info(dir);
    if (!di.exists) {
        // 容器不存在。真实 GDK 会去云上确认后回 E_NOTFOUND，这里直接回同一个值。
        return E_NOTFOUND;
    }
    XGameSaveContainerInfo info{};
    info.name = intern(containerName);
    info.displayName = intern(containerName);
    info.blobCount = di.file_count;
    info.totalSize = di.total_size;
    info.lastModifiedTime = di.last_modified;
    info.needsSync = false; // 本地只有一个"副本"，永远算已同步
    callback(&info, context);
    return S_OK;
}

namespace {

HRESULT enumerate_containers_impl(XGameSaveProviderHandle provider, const char* prefix,
                                  void* context, XGameSaveContainerInfoCallback* callback)
{
    if (!provider || !callback) return E_INVALIDARG;
    if (!dir_info(provider->root).exists) return E_NOTFOUND;
    std::vector<std::string> dirs;
    if (!list_dirs(provider->root, &dirs)) return E_FAIL;
    const std::string pre = prefix ? prefix : "";
    for (const auto& d : dirs) {
        if (!pre.empty() && d.compare(0, pre.size(), pre) != 0) continue;
        const std::string full = provider->root + "\\" + d;
        if (!is_under(provider->root, full)) continue; // 只认根下的直接子目录
        const DirInfo di = dir_info(full);
        XGameSaveContainerInfo info{};
        info.name = intern(d);
        info.displayName = intern(d);
        info.blobCount = di.file_count;
        info.totalSize = di.total_size;
        info.lastModifiedTime = di.last_modified;
        info.needsSync = false;
        if (!callback(&info, context)) break; // 回调返回 false = 停止枚举
    }
    return S_OK;
}

} // namespace

STDAPI XGameSaveEnumerateContainerInfo(XGameSaveProviderHandle provider, void* context,
                                       XGameSaveContainerInfoCallback* callback) noexcept
{
    XB_TRACE_ENTRY("XGameSaveEnumerateContainerInfo", provider, context, callback, 0ULL);
    return enumerate_containers_impl(provider, nullptr, context, callback);
}

STDAPI XGameSaveEnumerateContainerInfoByName(XGameSaveProviderHandle provider,
                                             const char* containerNamePrefix, void* context,
                                             XGameSaveContainerInfoCallback* callback) noexcept
{
    XB_TRACE_ENTRY("XGameSaveEnumerateContainerInfoByName", provider, containerNamePrefix, context, callback);
    return enumerate_containers_impl(provider, containerNamePrefix, context, callback);
}

// ===========================================================================
// blob 枚举
// ===========================================================================

namespace {

HRESULT enumerate_blobs_impl(XGameSaveContainerHandle container, const char* prefix, void* context,
                             XGameSaveBlobInfoCallback* callback)
{
    if (!container || !callback) return E_INVALIDARG;
    std::vector<std::string> files;
    if (!list_files(container->dir, &files, nullptr)) return E_NOTFOUND;
    std::sort(files.begin(), files.end()); // 顺序稳定，便于测试与去重
    const std::string pre = prefix ? prefix : "";
    for (const auto& f : files) {
        if (!pre.empty() && f.compare(0, pre.size(), pre) != 0) continue;
        DirInfo one;
        const std::string full = container->dir + "\\" + f;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!::GetFileAttributesExA(full.c_str(), GetFileExInfoStandard, &fad)) continue;
        one.total_size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        XGameSaveBlobInfo info{};
        info.name = intern(f);
        info.size = static_cast<uint32_t>(one.total_size);
        if (!callback(&info, context)) break;
    }
    return S_OK;
}

} // namespace

STDAPI XGameSaveEnumerateBlobInfo(XGameSaveContainerHandle container, void* context,
                                  XGameSaveBlobInfoCallback* callback) noexcept
{
    XB_TRACE_ENTRY("XGameSaveEnumerateBlobInfo", container, context, callback, 0ULL);
    return enumerate_blobs_impl(container, nullptr, context, callback);
}

STDAPI XGameSaveEnumerateBlobInfoByName(XGameSaveContainerHandle container,
                                        const char* blobNamePrefix, void* context,
                                        XGameSaveBlobInfoCallback* callback) noexcept
{
    XB_TRACE_ENTRY("XGameSaveEnumerateBlobInfoByName", container, blobNamePrefix, context, callback);
    return enumerate_blobs_impl(container, blobNamePrefix, context, callback);
}

// ===========================================================================
// 读 blob
// ===========================================================================

namespace {

// blobNames == nullptr → 容器里全部 blob；否则按名字逐个读。
// 任何一个不存在 → 整次失败并回 E_NOTFOUND（原子读：不给"读了一半"）。
HRESULT collect_blobs(XGameSaveContainerHandle container, const char** blobNames,
                      uint32_t* countOfBlobs, std::vector<PendingBlob>* out)
{
    if (!container || !countOfBlobs) return E_INVALIDARG;
    std::vector<std::string> names;
    if (blobNames) {
        const uint32_t requested = *countOfBlobs;
        for (uint32_t i = 0; i < requested; ++i) {
            if (!blobNames[i]) return E_INVALIDARG;
            names.emplace_back(blobNames[i]);
        }
    } else {
        if (!list_files(container->dir, &names, nullptr)) return E_NOTFOUND;
        std::sort(names.begin(), names.end());
    }
    for (const auto& raw : names) {
        PendingBlob pb;
        pb.name = raw;
        const std::string file = container->dir + "\\" + sanitize(raw, GS_MAX_BLOB_NAME_SIZE - 1);
        const HRESULT hr = read_blob_file(file, &pb.bytes);
        if (FAILED(hr)) {
            xbgtasix::logf("[save2] 读 blob 失败 hr=0x%08lx name=%s",
                           static_cast<unsigned long>(hr), raw.c_str());
            return hr;
        }
        out->push_back(std::move(pb));
    }
    return S_OK;
}

} // namespace

STDAPI XGameSaveReadBlobData(XGameSaveContainerHandle container, const char** blobNames,
                             uint32_t* countOfBlobs, size_t blobsSize,
                             XGameSaveBlob* blobData) noexcept
{
    XB_TRACE_ENTRY("XGameSaveReadBlobData", container, blobNames, countOfBlobs, blobsSize);
    if (!container || !countOfBlobs) return E_INVALIDARG;
    std::vector<PendingBlob> blobs;
    const HRESULT hr = collect_blobs(container, blobNames, countOfBlobs, &blobs);
    if (FAILED(hr)) return hr;
    return fill_blob_buffer(blobs, blobsSize, blobData, countOfBlobs);
}

STDAPI XGameSaveReadBlobDataAsync(XGameSaveContainerHandle container, const char** blobNames,
                                  uint32_t countOfBlobs, XAsyncBlock* async) noexcept
{
    XB_TRACE_ENTRY("XGameSaveReadBlobDataAsync", container, blobNames, countOfBlobs, async);
    if (!container || !async) return E_INVALIDARG;
    uint32_t n = countOfBlobs;
    std::vector<PendingBlob> blobs;
    const HRESULT hr = collect_blobs(container, blobNames, &n, &blobs);
    {
        std::lock_guard<std::mutex> lock(g_out_mu);
        Outcome& o = g_outcome[async];
        o.hr = hr;
        if (SUCCEEDED(hr)) o.blobs = std::move(blobs);
    }
    gs_complete(async, hr);
    return S_OK;
}

STDAPI XGameSaveReadBlobDataResult(XAsyncBlock* async, size_t blobsSize,
                                   XGameSaveBlob* blobData, uint32_t* countOfBlobs) noexcept
{
    XB_TRACE_ENTRY("XGameSaveReadBlobDataResult", async, blobsSize, blobData, countOfBlobs);
    if (!async || !countOfBlobs) return E_INVALIDARG;
    const HRESULT hr = peek_hr(async);
    if (hr == E_PENDING) return E_PENDING;
    Outcome o;
    if (!take_outcome(async, &o)) return E_UNEXPECTED;
    if (FAILED(o.hr)) return o.hr;
    const HRESULT fill = fill_blob_buffer(o.blobs, blobsSize, blobData, countOfBlobs);
    if (fill == E_GS_PROVIDED_BUFFER_TOO_SMALL) {
        // 让调用方拿正确 size 再试一次：把数据放回去
        std::lock_guard<std::mutex> lock(g_out_mu);
        g_outcome[async] = std::move(o);
    }
    return fill;
}

// ===========================================================================
// update / 写 blob
// ===========================================================================

STDAPI XGameSaveCreateUpdate(XGameSaveContainerHandle container, const char* containerDisplayName,
                             XGameSaveUpdateHandle* updateContext) noexcept
{
    XB_TRACE_ENTRY("XGameSaveCreateUpdate", container, containerDisplayName, updateContext, 0ULL);
    if (!container || !containerDisplayName || !updateContext) return E_INVALIDARG;
    *updateContext = nullptr;
    if (std::strlen(containerDisplayName) >= GS_MAX_CONTAINER_DISPLAY_NAME_SIZE) {
        xbgtasix::logf("[save2] CreateUpdate：displayName 超长（上限 %u）",
                       static_cast<unsigned>(GS_MAX_CONTAINER_DISPLAY_NAME_SIZE));
        return E_GS_INVALID_CONTAINER_NAME;
    }
    auto* u = new (std::nothrow) XGameSaveUpdate();
    if (!u) return E_OUTOFMEMORY;
    u->container = container;
    u->display_name = containerDisplayName;
    *updateContext = u;
    return S_OK;
}

STDAPI_(void) XGameSaveCloseUpdate(XGameSaveUpdateHandle context) noexcept
{
    XB_TRACE_ENTRY("XGameSaveCloseUpdate", context, 0ULL, 0ULL, 0ULL);
    if (!context) return;
    delete context;
}

STDAPI XGameSaveSubmitBlobWrite(XGameSaveUpdateHandle updateContext, const char* blobName,
                                const uint8_t* data, size_t byteCount) noexcept
{
    XB_TRACE_ENTRY("XGameSaveSubmitBlobWrite", updateContext, blobName, data, byteCount);
    if (!updateContext || !blobName || !blobName[0]) return E_INVALIDARG;
    if (byteCount > 0 && !data) return E_INVALIDARG;
    if (std::strlen(blobName) >= GS_MAX_BLOB_NAME_SIZE) {
        xbgtasix::logf("[save2] SubmitBlobWrite：blob 名超长 %s", blobName);
        return E_GS_INVALID_CONTAINER_NAME;
    }
    if (byteCount > GS_MAX_BLOB_SIZE) return E_GS_UPDATE_TOO_BIG;
    // 先囤在 update 里：GDK 语义是"全部改动一起提交"，不是写一个落一个
    auto& writes = updateContext->writes;
    for (auto& w : writes) {
        if (w.first == blobName) { // 同名重复写：后写覆盖先写
            w.second.assign(data, data + byteCount);
            return S_OK;
        }
    }
    writes.emplace_back(std::string(blobName), std::vector<uint8_t>(data, data + byteCount));
    return S_OK;
}

STDAPI XGameSaveSubmitBlobDelete(XGameSaveUpdateHandle updateContext, const char* blobName) noexcept
{
    XB_TRACE_ENTRY("XGameSaveSubmitBlobDelete", updateContext, blobName, 0ULL, 0ULL);
    if (!updateContext || !blobName || !blobName[0]) return E_INVALIDARG;
    updateContext->deletes.emplace_back(blobName);
    return S_OK;
}

namespace {

HRESULT submit_update_impl(XGameSaveUpdateHandle u)
{
    if (!u || !u->container) return E_INVALIDARG;
    const std::string dir = u->container->dir;
    HRESULT first_error = S_OK;
    for (const auto& w : u->writes) {
        const std::string file = dir + "\\" + sanitize(w.first, GS_MAX_BLOB_NAME_SIZE - 1);
        const HRESULT hr = write_blob_file(file, w.second.data(), w.second.size());
        if (FAILED(hr)) {
            first_error = hr;
            break; // 原子提交：一处失败整批失败
        }
    }
    if (SUCCEEDED(first_error)) {
        for (const auto& d : u->deletes) {
            const std::string file = dir + "\\" + sanitize(d, GS_MAX_BLOB_NAME_SIZE - 1);
            if (!::DeleteFileA(file.c_str())) {
                const DWORD err = ::GetLastError();
                if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
                    xbgtasix::logf("[save2] 删除 blob 失败（%lu）: %s",
                                   static_cast<unsigned long>(err), file.c_str());
                    first_error = HRESULT_FROM_WIN32(err);
                    break;
                }
            }
        }
    }
    if (SUCCEEDED(first_error)) {
        // 提交成功后清空 update，避免重复提交导致重复写
        u->writes.clear();
        u->deletes.clear();
        xbgtasix::logf("[save2] SubmitUpdate(%s) 完成", dir.c_str());
    }
    return first_error;
}

} // namespace

STDAPI XGameSaveSubmitUpdate(XGameSaveUpdateHandle updateContext) noexcept
{
    XB_TRACE_ENTRY("XGameSaveSubmitUpdate", updateContext, 0ULL, 0ULL, 0ULL);
    return submit_update_impl(updateContext);
}

STDAPI XGameSaveSubmitUpdateAsync(XGameSaveUpdateHandle updateContext,
                                  XAsyncBlock* async) noexcept
{
    XB_TRACE_ENTRY("XGameSaveSubmitUpdateAsync", updateContext, async, 0ULL, 0ULL);
    if (!async) return E_INVALIDARG;
    gs_complete(async, submit_update_impl(updateContext));
    return S_OK;
}

STDAPI XGameSaveSubmitUpdateResult(XAsyncBlock* async) noexcept
{
    XB_TRACE_ENTRY("XGameSaveSubmitUpdateResult", async, 0ULL, 0ULL, 0ULL);
    if (!async) return E_INVALIDARG;
    const HRESULT hr = peek_hr(async);
    if (hr == E_PENDING) return E_PENDING;
    Outcome o;
    if (!take_outcome(async, &o)) return E_UNEXPECTED;
    return o.hr;
}

} // extern "C"

// ===========================================================================
// 头文件里没有的接口（按任务要求不臆造，故不实现）：
//
//   XGameSaveOpenContainer       —— XGameSave.h:127 只有 CreateContainer；
//                                   打开已存在的容器由 CreateContainer 的"建或复用"承担。
//   XGameSaveWriteBlob / ReadBlob—— 真实名是 SubmitBlobWrite（经 CreateUpdate + SubmitUpdate）
//                                   与 ReadBlobData / ReadBlobDataAsync（:135-137）。
//   XGameSaveSubmitContainerDelete —— 头文件没有；容器删除是 DeleteContainer /
//                                   DeleteContainerAsync（:119-121）。
//   XGameSaveSubmitAsync         —— 头文件里对应的是 SubmitUpdateAsync（:146）。
// ===========================================================================
