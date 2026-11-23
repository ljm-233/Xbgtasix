// SPDX-License-Identifier: GPL-3.0-or-later
// package_more.cpp —— XPackage 剩余接口（第二批：包种类 / 特性枚举 / 安装注册 / 卸载）
//
// 原则与 package_extra.cpp 完全一致：
//   * 签名逐字抄自 gdk/include_clean/XPackage.h，ABI 交给编译器，不手搓结构体；
//   * 能从包根目录（XBGTASIX_PACKAGE_ROOT）读出来的就真读；
//   * 读不出来 / 没有能力的明确失败（E_NOTFOUND / E_NOTIMPL），绝不 S_OK 假装成功。
//
// 本文件实现的 7 个接口全是同步 API（一个 *Async 都没有），所以不存在
// "XAsyncBlock 永远不完成"的坑；反过来也有一条硬约束：
//
//   ★ 绝不在本 TU 里 new XPackageInstallationMonitor ★
//   XPackageInstallationMonitor 的字段布局是 package_extra.cpp 的私有实现
//   （见那边 172-187 行）。本 TU 若自己再定义一份同名 struct，链接后两个 TU
//   会各按自己的布局访问同一块堆内存 —— XPackageGetInstallationProgress
//   会读到错位的 completed/installedBytes，是"看起来能跑、偶发乱码"的那种 AV。
//   所以 XPackageInstallChunks（同步版）只把 *installationMonitor 置空并明确
//   返回 E_NOTIMPL，绝不交出一个"我们以为对、别人按另一个布局读"的句柄。
//
// 未实现（且本文件刻意不导出，理由见文件末尾注释）：
//   XPackageGetIdentifier —— 只出现在 gdk/lib/xgameruntime.lib 的导出表里，
//   gdk/include_clean/XPackage.h 没有它的声明，签名无从逐字核对，故不猜。
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "XPackage.h"
#include "XTaskQueue.h"

#include "xbgtasix/trace.h"
#include "trace_entry.h"   // 统一入口追踪（XB_TRACE_ENTRY）

// 精简头文件没带这个宏；GDK 的语义就是 ERROR_NOT_FOUND（与 xgameruntime.cpp 的
// E_NOTFOUND / package_extra.cpp:34-36 同值，0x80070490）。
#ifndef E_NOTFOUND
#define E_NOTFOUND HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
#endif

namespace {

// ---------------- 包根目录与文件系统helpers ----------------

// 包目录来自环境变量 XBGTASIX_PACKAGE_ROOT（与 XPackageMount 的 package_root 同源）。
// 注意：这里每次都重新读环境变量（不做静态缓存），因为调用方可能在运行期改它。
bool package_root(std::string* out)
{
    char buf[32768] = {};
    const DWORD n = ::GetEnvironmentVariableA("XBGTASIX_PACKAGE_ROOT", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return false;
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    if (s.empty()) return false;
    *out = s;
    return true;
}

// base + 分隔符 + leaf：base 末尾已经有分隔符时不重复加（否则 "D:\x\\y" 仍能打开，
// 但 FindFirstFile/GetFileAttributes 对 "\\" 的容忍度不值得赌）
std::string join_path(const std::string& base, const char* leaf)
{
    std::string r = base;
    if (!r.empty() && r.back() != '\\' && r.back() != '/') r += '\\';
    r += leaf;
    return r;
}

bool dir_exists(const std::string& path)
{
    const DWORD attr = ::GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool read_text_file(const std::string& path, std::string* out)
{
    HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    bool all = true;
    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(h, buf, static_cast<DWORD>(sizeof(buf)), &got, nullptr)) {
            all = false; // 读一半失败也要把已读到的留给调用方判断
            break;
        }
        if (got == 0) break;
        out->append(buf, got);
    }
    ::CloseHandle(h);
    return all;
}

std::string trim(const std::string& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

void split_char(const std::string& s, char sep, std::vector<std::string>* out)
{
    size_t start = 0;
    for (;;) {
        const size_t pos = s.find(sep, start);
        if (pos == std::string::npos) {
            out->push_back(s.substr(start));
            return;
        }
        out->push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
}

// ---------------- 包标识符 ----------------

// 进程自己的包标识符（读不到就返回空串，调用方按 "*" 处理）。
// XPACKAGE_IDENTIFIER_MAX_LENGTH 是"含结尾 0"的上限（33），所以缓冲正好开这么大。
std::string process_package_id()
{
    char buf[XPACKAGE_IDENTIFIER_MAX_LENGTH] = {};
    if (SUCCEEDED(XPackageGetCurrentProcessPackageIdentifier(sizeof(buf), buf)))
        return std::string(buf);
    return std::string();
}

// 标识符里出现路径分隔符 / ".." 的话，join_path 会拼出包根之外的目录 —— 直接拒绝。
bool identifier_is_safe(const std::string& id)
{
    if (id.find('\\') != std::string::npos) return false;
    if (id.find('/') != std::string::npos) return false;
    if (id.find("..") != std::string::npos) return false;
    return true;
}

// 包标识符 -> 真实目录。
//   "" / "*" / 进程自身 id  -> 包根目录本身（= 游戏本体）
//   包根目录下同名子目录    -> 那个子目录（= DLC / 内容包）
//   其它                    -> E_NOTFOUND（不猜、不回落，免得把 DLC 当成主包）
HRESULT resolve_package_dir(const char* packageIdentifier, std::string* dir)
{
    // 头文件里这三个包标识符参数都是 _In_z_（非空必填）。空指针是调用方 bug，
    // 明确 E_INVALIDARG —— 与 package_extra.cpp:363 对 CreateInstallationMonitor 的处理一致，
    // 免得把 nullptr 悄悄当成"本进程自己的包"，让游戏的错误被掩盖。
    if (!packageIdentifier) return E_INVALIDARG;

    std::string root;
    if (!package_root(&root)) return E_NOTFOUND;

    const std::string id = packageIdentifier;
    if (id.empty() || id == "*") {
        *dir = root;
        return S_OK;
    }
    if (!identifier_is_safe(id)) return E_INVALIDARG;
    if (id == process_package_id()) {
        *dir = root;
        return S_OK;
    }
    const std::string sub = join_path(root, id.c_str());
    if (dir_exists(sub)) {
        *dir = sub;
        return S_OK;
    }
    return E_NOTFOUND;
}

}  // namespace

// ================ 1. 包种类 ================

// 签名：XPackage.h:228
STDAPI XPackageGetPackageKind(
    const char* packageIdentifier,
    XPackageKind* kind
    ) noexcept
{
    XB_TRACE_ENTRY("XPackageGetPackageKind", packageIdentifier, kind, 0ULL, 0ULL);
    if (!kind) return E_INVALIDARG;
    // 先写一个合法值：失败路径上调用方也可能读 *kind，不能留栈垃圾
    *kind = XPackageKind::Game;

    std::string dir;
    const HRESULT hr = resolve_package_dir(packageIdentifier, &dir);
    if (FAILED(hr)) {
        xbgtasix::logf("[xgamert] GetPackageKind(%s)：包不存在 -> 0x%08lx",
                       packageIdentifier ? packageIdentifier : "*",
                       static_cast<unsigned long>(hr));
        return hr;
    }

    std::string root;
    (void)package_root(&root);
    // 包根目录就是"本进程装的游戏"；包根下的子目录是本进程能看见的内容包。
    // 头文件里的第三档 PublisherContent 需要发布者信息，本层没有来源，故从不返回。
    *kind = (dir == root) ? XPackageKind::Game : XPackageKind::Content;

    xbgtasix::logf("[xgamert] GetPackageKind(%s) -> %s (%s)",
                   packageIdentifier ? packageIdentifier : "*",
                   *kind == XPackageKind::Game ? "Game" : "Content", dir.c_str());
    return S_OK;
}

// ================ 2. 特性（feature）枚举 ================

// 特性清单：<包目录>\features.txt，UTF-8，一行一个 feature，'#' 开头是注释。
//
//   id|displayName|tags|hidden|storeId1,storeId2
//
// 只有 id 必填，后面四列可以省略；displayName 缺省 = id，hidden 接受 1/true/TRUE。
// 为什么走清单而不是像 chunk 那样直接拿子目录当 feature：
//   chunk 目录名天然是 chunk 身份，而 feature（语言包/模式/季票）在文件系统上
//   没有必然痕迹，硬造出来会让游戏"枚举到一堆不存在的 feature"。宁可 E_NOTIMPL。
namespace {
const char kFeaturesManifest[] = "features.txt";
}  // namespace

// 签名：XPackage.h:245
STDAPI XPackageEnumerateFeatures(
    const char* packageIdentifier,
    void* context,
    XPackageFeatureEnumerationCallback* callback
    ) noexcept
{
    XB_TRACE_ENTRY("XPackageEnumerateFeatures", packageIdentifier, context, callback, 0ULL);
    if (!callback) return E_INVALIDARG;

    std::string dir;
    const HRESULT hr = resolve_package_dir(packageIdentifier, &dir);
    if (FAILED(hr)) {
        xbgtasix::logf("[xgamert] EnumerateFeatures：包目录解析失败 -> 0x%08lx",
                       static_cast<unsigned long>(hr));
        return hr;
    }

    const std::string manifest = join_path(dir, kFeaturesManifest);
    std::string text;
    if (!read_text_file(manifest, &text)) {
        // 没有清单 = 本层没有 feature 的元数据来源。返回 S_OK + 零回调等于
        // 告诉游戏"这个包没有任何 feature"，那是编造；E_NOTIMPL 才是真话。
        xbgtasix::logf("[xgamert] EnumerateFeatures：没有清单 %s -> E_NOTIMPL",
                       manifest.c_str());
        return E_NOTIMPL;
    }

    // ---- 第一阶段：把所有字符串解析完（vector 会 realloc，此时绝不能取 c_str） ----
    struct Rec
    {
        std::string id;
        std::string display_name;
        std::string tags;
        bool hidden = false;
        std::vector<std::string> store_ids;
    };
    std::vector<Rec> recs;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line[0] == '#') continue;

        std::vector<std::string> fields;
        split_char(line, '|', &fields);
        Rec r;
        r.id = trim(fields[0]);
        if (r.id.empty()) continue; // 没有 id 的 feature 无法被游戏查询，丢掉
        if (fields.size() > 1) r.display_name = trim(fields[1]);
        if (r.display_name.empty()) r.display_name = r.id;
        if (fields.size() > 2) r.tags = trim(fields[2]);
        if (fields.size() > 3) {
            const std::string h = trim(fields[3]);
            r.hidden = (h == "1" || h == "true" || h == "TRUE");
        }
        if (fields.size() > 4) {
            std::vector<std::string> ids;
            split_char(fields[4], ',', &ids);
            for (const std::string& s : ids) {
                const std::string t = trim(s);
                if (!t.empty()) r.store_ids.push_back(t);
            }
        }
        recs.push_back(std::move(r));
    }

    // ---- 第二阶段：字符串都定型了，再取指针（这是 chunk 枚举同款陷阱） ----
    std::vector<std::vector<const char*>> store_ptrs(recs.size());
    std::vector<XPackageFeature> features(recs.size());
    for (size_t i = 0; i < recs.size(); ++i) {
        // 先 reserve 到最终大小再 push_back：容量不再增长，data() 全程稳定
        store_ptrs[i].reserve(recs[i].store_ids.size());
        for (const std::string& s : recs[i].store_ids) store_ptrs[i].push_back(s.c_str());

        XPackageFeature f{};
        f.id = recs[i].id.c_str();
        f.displayName = recs[i].display_name.c_str();
        f.tags = recs[i].tags.c_str();
        f.hidden = recs[i].hidden;
        f.storeIdCount = static_cast<uint32_t>(recs[i].store_ids.size());
        f.storeIds = store_ptrs[i].empty() ? nullptr : store_ptrs[i].data();
        features[i] = f;
    }

    // ---- 第三阶段：回调。返回 false = 调用方不想继续，立刻停 ----
    uint32_t sent = 0;
    for (size_t i = 0; i < features.size(); ++i) {
        ++sent;
        if (!callback(context, &features[i])) break;
    }

    xbgtasix::logf("[xgamert] EnumerateFeatures(%s)：清单 %s 解析出 %u 个 feature，回调 %u 次",
                   packageIdentifier ? packageIdentifier : "*", manifest.c_str(),
                   static_cast<unsigned>(features.size()), static_cast<unsigned>(sent));
    return S_OK;
}

// ================ 3. "包已安装"通知的注册表 ================

// 注册表是真的（token 唯一、可注销、线程安全），但本层没有安装器，所以这些回调
// 不会被自发触发。这里不伪造"刚装好一个包"的事件 —— 那条假事件会让游戏去
// XPackageMount 一个根本不存在的包。
namespace {

struct InstalledReg
{
    uint64_t token = 0;
    XTaskQueueHandle queue = nullptr;
    void* context = nullptr;
    XPackageInstalledCallback* callback = nullptr;
};

std::mutex g_reg_mu;
std::vector<InstalledReg> g_regs;
std::atomic<uint64_t> g_next_token{1};

}  // namespace

// 签名：XPackage.h:233
STDAPI XPackageRegisterPackageInstalled(
    XTaskQueueHandle queue,
    void* context,
    XPackageInstalledCallback* callback,
    XTaskQueueRegistrationToken* token
    ) noexcept
{
    XB_TRACE_ENTRY("XPackageRegisterPackageInstalled", queue, context, callback, token);
    if (!token) return E_INVALIDARG;
    // token 是 _Out_：必须在任何失败返回之前就写掉，否则调用方（或调用方上层的
    // cleanup 路径）拿着栈垃圾/旧值去 XPackageUnregisterPackageInstalled。
    // 这个顺序是被 t23 抓出来的：先校验 callback 再清零会漏掉 null callback 那条路。
    token->token = 0;
    if (!callback) return E_INVALIDARG;

    InstalledReg reg;
    reg.token = g_next_token.fetch_add(1);
    reg.queue = queue;
    reg.context = context;
    reg.callback = callback;
    {
        std::lock_guard<std::mutex> lock(g_reg_mu);
        g_regs.push_back(reg);
    }

    token->token = reg.token;
    xbgtasix::logf("[xgamert] RegisterPackageInstalled(queue=%p) -> token=%llu"
                   "（注册表真实存在，但没有安装器故不会自发回调）",
                   static_cast<void*>(queue),
                   static_cast<unsigned long long>(reg.token));
    return S_OK;
}

// 签名：XPackage.h:240
STDAPI_(bool) XPackageUnregisterPackageInstalled(
    XTaskQueueRegistrationToken token,
    bool wait
    ) noexcept
{
    XB_TRACE_ENTRY("XPackageUnregisterPackageInstalled", token, wait, 0ULL, 0ULL);
    (void)wait; // 本层回调从不入队，没有"在途回调"需要等
    {
        std::lock_guard<std::mutex> lock(g_reg_mu);
        for (auto it = g_regs.begin(); it != g_regs.end(); ++it) {
            if (it->token == token.token) {
                g_regs.erase(it);
                xbgtasix::logf("[xgamert] UnregisterPackageInstalled(token=%llu) -> true",
                               static_cast<unsigned long long>(token.token));
                return true;
            }
        }
    }
    // 注销一个没注册过的 token：明确告诉调用方"这条没找到"，而不是假装删掉了
    xbgtasix::logf("[xgamert] UnregisterPackageInstalled(token=%llu)：未注册过 -> false",
                   static_cast<unsigned long long>(token.token));
    return false;
}

// ================ 4. 同步安装（明确不支持） ================

// 签名：XPackage.h:183
//
// 为什么不是"chunk 目录已经在就直接 S_OK"：这条 API 的成功路径必须交出一个
// XPackageInstallationMonitorHandle 供后续查进度/注册回调。那个结构体的布局在
// package_extra.cpp 内部（见文件头 ★ 那段），本 TU 造不出来。交出 nullptr 让
// 调用方去 deref 比明确失败更糟，所以这里 E_NOTIMPL。
STDAPI XPackageInstallChunks(
    const char* packageIdentifier,
    uint32_t selectorCount,
    XPackageChunkSelector* selectors,
    uint32_t minimumUpdateIntervalMs,
    bool suppressUserConfirmation,
    XTaskQueueHandle queue,
    XPackageInstallationMonitorHandle* installationMonitor
    ) noexcept
{
    XB_TRACE_ENTRY("XPackageInstallChunks", packageIdentifier, selectorCount, selectors, minimumUpdateIntervalMs);
    if (!installationMonitor) return E_INVALIDARG;
    *installationMonitor = nullptr; // 失败绝不留野句柄
    (void)selectors;
    (void)queue;
    xbgtasix::logf("[xgamert] InstallChunks(%s, %u selectors, %u ms, suppressUi=%d)："
                   "本层无安装器 -> E_NOTIMPL（不交出 monitor）",
                   packageIdentifier ? packageIdentifier : "(null)", selectorCount,
                   minimumUpdateIntervalMs, suppressUserConfirmation ? 1 : 0);
    return E_NOTIMPL;
}

// ================ 5. 卸载（明确失败，绝不假装删了东西） ================

// 签名：XPackage.h:290
//
// 返回 bool：false = 没卸载成功。包目录是用户的数据（也可能是别人的安装目录），
// 这套兼容层没有任何权限/回滚能力去真删，所以恒返回 false。日志里区分
// "包根本不存在" 和 "存在但我们不动它"，方便定位游戏为什么重试。
STDAPI_(bool) XPackageUninstallPackage(
    const char* packageIdentifier
    ) noexcept
{
    XB_TRACE_ENTRY("XPackageUninstallPackage", packageIdentifier, 0ULL, 0ULL, 0ULL);
    std::string dir;
    const HRESULT hr = resolve_package_dir(packageIdentifier, &dir);
    xbgtasix::logf("[xgamert] UninstallPackage(%s)：%s，不删除包目录 -> false",
                   packageIdentifier ? packageIdentifier : "(null)",
                   SUCCEEDED(hr) ? "包存在" : "包不存在");
    return false;
}

// 签名：XPackage.h:286
//
// 这条是 UWP/MSIX 实例的卸载。我们是 Win32 兼容层，没有注册任何 UWP 实例，
// "找不到那个实例"就是如实的失败原因（ERROR_NOT_FOUND = 0x80070490）。
STDAPI XPackageUninstallUWPInstance(
    const char* packageName
    ) noexcept
{
    XB_TRACE_ENTRY("XPackageUninstallUWPInstance", packageName, 0ULL, 0ULL, 0ULL);
    if (!packageName || !packageName[0]) return E_INVALIDARG;
    xbgtasix::logf("[xgamert] UninstallUWPInstance(%s)：本进程无 UWP 实例 -> E_NOTFOUND",
                   packageName);
    return E_NOTFOUND;
}

// ---------------- 本文件刻意不实现的 ----------------
//
// XPackageGetIdentifier：
//   gdk/lib/xgameruntime.lib 的导出表里有它（见 src/generated/xgameruntime_exports.inc:140），
//   但 gdk/include_clean/XPackage.h 通篇没有声明。上一个代理的教训就是"凭记忆补签名"，
//   这里不猜，继续留在生成的桩上（调用即 XB_UNIMPL 明确报错）。
//   名字也不写进 exports_pkg2.txt，免得把桩顶掉又给不出实现。
