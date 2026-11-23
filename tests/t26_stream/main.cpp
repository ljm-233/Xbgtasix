// SPDX-License-Identifier: GPL-3.0-or-later
// t26_stream — XGameStreaming / XAppCapture / XNetworking 的"安全降级"冒烟测试
//
// 这三个模块在 PC 上没有对应物，所以测试重点不是"能用"，而是**能诚实地说明没有**：
//   1) 布尔/枚举查询给出 false / 0 / 头文件里的 无·断开 哨兵
//   2) 结构体出参被整块写过（哨兵 0xCC 不残留），失败也有确定内容
//   3) 回调注册一定给出非零 token，注销返回 true
//   4) 每个 *Async 都必须让 XAsyncGetStatus(wait=true) 完成，且远快于 1 秒
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "xbgtasix/trace.h"

#include "XAsync.h"
#include "XGameRuntime.h"
#include "XGameStreaming.h"
#include "XAppCapture.h"
#include "XNetworking.h"

namespace {

int g_fail = 0;
int g_async_done = 0;   // 异步完成回调真的被调用了

void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t26]   PASS  %-46s %s", what, detail);
    } else {
        xbgtasix::logf("[t26]   FAIL  %-46s %s", what, detail);
        ++g_fail;
    }
}

// 出参整块检查：结构体先用 0xCC 灌满，实现必须整块 memset 后才填，残留 0xCC 就是 bug。
bool has_residue(const void* p, size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i)
        if (b[i] == 0xCC) return true;
    return false;
}

// 兜底结构：与 XAsyncBlock 的前两个字段 + 回调布局一致（queue/context/callback/internal）。
// 用真头文件的 XAsyncBlock，另外编译期证明布局一致。
struct Blk {
    void* queue;
    void* context;
    void (*callback)(Blk*);
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};
static_assert(sizeof(Blk) == sizeof(XAsyncBlock), "Blk 与 XAsyncBlock 大小不一致");
static_assert(offsetof(Blk, callback) == offsetof(XAsyncBlock, callback), "callback 偏移不一致");

void CALLBACK on_conn_state(void* context, XGameStreamingClientId client,
                            XGameStreamingConnectionState state) {
    (void)context;
    (void)client;
    (void)state;
}

void CALLBACK on_client_props(void* context, XGameStreamingClientId client,
                              uint32_t updatedPropertiesCount,
                              XGameStreamingClientProperty* updatedProperties) {
    (void)context;
    (void)client;
    (void)updatedPropertiesCount;
    (void)updatedProperties;
}

void CALLBACK on_metadata_purged(void* context) { (void)context; }

void on_connectivity_hint(void* context, const XNetworkingConnectivityHint* hint) {
    (void)context;
    (void)hint;
}

void CALLBACK on_async_complete(XAsyncBlock* async) {
    (void)async;
    ++g_async_done;
}

// 提交一个 *Async，断言它真的完成、不是 E_PENDING、且没空转。
void check_async(const char* what, HRESULT (*submit)(XAsyncBlock*)) {
    XAsyncBlock b{};
    b.callback = &on_async_complete;
    g_async_done = 0;
    const ULONGLONG t0 = GetTickCount64();
    const HRESULT hr = submit(&b);
    const HRESULT st = XAsyncGetStatus(&b, true);
    const ULONGLONG dt = GetTickCount64() - t0;
    xbgtasix::logf("[t26] %s: submit=0x%08lx status=0x%08lx 耗时 %llu ms", what,
                   static_cast<unsigned long>(hr), static_cast<unsigned long>(st),
                   static_cast<unsigned long long>(dt));
    ok(SUCCEEDED(hr), what, "提交成功");
    ok(st != E_PENDING, what, "已完成（不是 E_PENDING）");
    ok(st == E_NOTIMPL, what, "结果是 E_NOTIMPL（明确的\"没有\"）");
    ok(dt < 1000, what, "耗时 < 1000 ms");
    for (int i = 0; i < 500 && g_async_done == 0; ++i) Sleep(1);   // 回调可能比完成位稍晚
    ok(g_async_done == 1, what, "完成回调被调用了一次");
    // 已经完成后再查一次仍应是同一状态，不能变回 E_PENDING
    ok(XAsyncGetStatus(&b, false) == st, what, "重复查询状态稳定");
}

HRESULT submit_udp_port(XAsyncBlock* b) {
    return XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(b);
}
HRESULT submit_sec_info(XAsyncBlock* b) {
    return XNetworkingQuerySecurityInformationForUrlAsync("https://example.com", b);
}
HRESULT submit_sec_info_utf16(XAsyncBlock* b) {
    return XNetworkingQuerySecurityInformationForUrlUtf16Async(L"https://example.com", b);
}

}  // namespace

int main() {
    ok(SUCCEEDED(XGameRuntimeInitialize()), "XGameRuntimeInitialize");

    // ---------------- XGameStreaming ----------------
    ok(XGameStreamingIsStreaming() == false, "IsStreaming == false");
    ok(XGameStreamingGetClientCount() == 0, "GetClientCount == 0");

    XGameStreamingClientId clients[4];
    std::memset(clients, 0xCD, sizeof(clients));
    uint32_t used = 0xFFFF;
    const HRESULT gch = XGameStreamingGetClients(4, clients, &used);
    ok(SUCCEEDED(gch) && used == 0, "GetClients: S_OK 且 clientsUsed == 0");
    ok(clients[0] == 0 && clients[3] == 0, "GetClients: 数组被清零（不是 0xCD 残值）");
    ok(XGameStreamingGetClients(4, nullptr, &used) == E_INVALIDARG,
       "GetClients: clients==null 且 count>0 -> E_INVALIDARG");

    // 头文件 XGameStreaming.h:46：Disconnected == 0 是"没有连接"的语义值
    ok(XGameStreamingGetConnectionState(XGameStreamingNullClientId) ==
           XGameStreamingConnectionState::Disconnected,
       "GetConnectionState(NullClientId) == Disconnected");
    ok(XGameStreamingGetConnectionState(XGameStreamingAllClients) ==
           XGameStreamingConnectionState::Disconnected,
       "GetConnectionState(AllClients) == Disconnected");

    XTaskQueueRegistrationToken tok{0};
    ok(SUCCEEDED(XGameStreamingRegisterConnectionStateChanged(nullptr, nullptr, &on_conn_state, &tok)) &&
           tok.token != 0,
       "RegisterConnectionStateChanged 给出非零 token");
    ok(XGameStreamingUnregisterConnectionStateChanged(tok, false) == true,
       "UnregisterConnectionStateChanged == true");
    tok = XTaskQueueRegistrationToken{0};
    ok(SUCCEEDED(XGameStreamingRegisterClientPropertiesChanged(
           XGameStreamingAllClients, nullptr, nullptr, &on_client_props, &tok)) &&
           tok.token != 0,
       "RegisterClientPropertiesChanged 给出非零 token");
    ok(XGameStreamingUnregisterClientPropertiesChanged(XGameStreamingAllClients, tok, false) == true,
       "UnregisterClientPropertiesChanged == true");
    tok = XTaskQueueRegistrationToken{0};
    ok(XGameStreamingRegisterConnectionStateChanged(nullptr, nullptr, &on_conn_state, nullptr) ==
           E_INVALIDARG,
       "RegisterConnectionStateChanged: token==null -> E_INVALIDARG");

    // 结构体出参：失败路径也要把整块写成 0（不是 0xCC）
    XGameStreamingDisplayDetails dd;
    std::memset(&dd, 0xCC, sizeof(dd));
    const HRESULT gdd = XGameStreamingGetDisplayDetails(
        XGameStreamingAllClients, 3840 * 2160, 21.0f / 9.0f, 4.0f / 3.0f, &dd);
    ok(gdd == E_NOTIMPL, "GetDisplayDetails == E_NOTIMPL");
    ok(!has_residue(&dd, sizeof(dd)), "GetDisplayDetails 整块写过（无 0xCC 残值）");
    ok(dd.preferredWidth == 0 && dd.preferredHeight == 0 && static_cast<uint32_t>(dd.flags) == 0,
       "GetDisplayDetails 全 0 / flags == None(0)");

    uint32_t hmm = 0xFFFF, vmm = 0xFFFF;
    ok(XGameStreamingGetStreamPhysicalDimensions(XGameStreamingNullClientId, &hmm, &vmm) ==
               E_NOTIMPL &&
           hmm == 0 && vmm == 0,
       "GetStreamPhysicalDimensions: E_NOTIMPL + 出参 0");

    bool touch = true;
    ok(SUCCEEDED(XGameStreamingIsTouchInputEnabled(XGameStreamingNullClientId, &touch)) &&
           touch == false,
       "IsTouchInputEnabled: S_OK 且 false");
    ok(XGameStreamingGetServerLocationNameSize() == 0, "GetServerLocationNameSize == 0");

    // ---------------- XAppCapture ----------------
    XAppCaptureVideoCaptureSettings cfg;
    std::memset(&cfg, 0xCC, sizeof(cfg));
    const HRESULT gcfg = XAppCaptureGetVideoCaptureSettings(&cfg);
    ok(SUCCEEDED(gcfg), "GetVideoCaptureSettings 成功返回");
    ok(!has_residue(&cfg, sizeof(cfg)), "GetVideoCaptureSettings 整块清零（无 0xCC 残值）");
    ok(cfg.width == 0 && cfg.height == 0 && cfg.maxRecordTimespanDurationInMs == 0,
       "GetVideoCaptureSettings 尺寸/时长全 0");
    ok(cfg.isCaptureByGamesAllowed == false, "GetVideoCaptureSettings: isCaptureByGamesAllowed == false");
    ok(cfg.encoding == XAppCaptureVideoEncoding::H264 && cfg.colorFormat == XAppCaptureVideoColorFormat::SDR,
       "编码/色彩格式为默认 H264/SDR（无偏好）");

    ok(XAppCaptureEnableRecord() == E_NOTIMPL, "EnableRecord == E_NOTIMPL（没有捕获服务）");
    ok(XAppCaptureDisableRecord() == E_NOTIMPL, "DisableRecord == E_NOTIMPL");

    tok = XTaskQueueRegistrationToken{0};
    ok(SUCCEEDED(XAppCaptureRegisterMetadataPurged(nullptr, nullptr, &on_metadata_purged, &tok)) &&
           tok.token != 0,
       "RegisterMetadataPurged 给出非零 token");
    ok(XAppCaptureUnRegisterMetadataPurged(tok, true) == true, "UnRegisterMetadataPurged == true");

    uint64_t remain = 0xFFFF;
    ok(SUCCEEDED(XAppCaptureMetadataRemainingStorageBytesAvailable(&remain)) && remain == 0,
       "MetadataRemainingStorageBytesAvailable == 0");

    XAppCaptureDiagnosticScreenshotResult shot;
    std::memset(&shot, 0xCC, sizeof(shot));
    ok(XAppCaptureTakeDiagnosticScreenshot(true, XAppCaptureScreenshotFormatFlag::SDR, "x", &shot) ==
               E_NOTIMPL &&
           shot.fileCount == 0 && shot.files[0].path[0] == '\0',
       "TakeDiagnosticScreenshot: E_NOTIMPL + fileCount 0 + 空路径");

    ok(XAppBroadcastIsAppBroadcasting() == false, "BroadcastIsAppBroadcasting == false");
    XAppBroadcastStatus bcast;
    std::memset(&bcast, 0xCC, sizeof(bcast));
    ok(SUCCEEDED(XAppBroadcastGetStatus(nullptr, &bcast)) && !has_residue(&bcast, sizeof(bcast)) &&
           !bcast.canStartBroadcast && !bcast.isAnyAppBroadcasting,
       "BroadcastGetStatus: 全 false 且无残值");

    // ---------------- XNetworking ----------------
    XNetworkingConnectivityHint hint;
    std::memset(&hint, 0xCC, sizeof(hint));
    const HRESULT gh = XNetworkingGetConnectivityHint(&hint);
    ok(SUCCEEDED(gh), "GetConnectivityHint 成功返回");
    ok(!has_residue(&hint, sizeof(hint)), "GetConnectivityHint 整块写过（无 0xCC 残值）");
    // XNetworking.h:107：Level 的"无"值是 None(1)，0 是 Unknown（别想当然用 0）
    ok(hint.connectivityLevel == XNetworkingConnectivityLevelHint::None,
       "connectivityLevel == None(1)");
    // XNetworking.h:115：Cost 没有"无"值，离线时用 Unknown(0) 表示未知
    ok(hint.connectivityCost == XNetworkingConnectivityCostHint::Unknown,
       "connectivityCost == Unknown(0)");
    ok(hint.networkInitialized == false && hint.roaming == false && hint.overDataLimit == false &&
           hint.approachingDataLimit == false,
       "networkInitialized/roaming/数据上限标志全 false");
    ok(XNetworkingGetConnectivityHint(nullptr) == E_INVALIDARG,
       "GetConnectivityHint(nullptr) == E_INVALIDARG");

    tok = XTaskQueueRegistrationToken{0};
    ok(SUCCEEDED(XNetworkingRegisterConnectivityHintChanged(nullptr, nullptr, &on_connectivity_hint,
                                                            &tok)) &&
           tok.token != 0,
       "RegisterConnectivityHintChanged 给出非零 token");
    ok(XNetworkingUnregisterConnectivityHintChanged(tok, false) == true,
       "UnregisterConnectivityHintChanged == true");

    uint16_t port = 0xFFFF;
    ok(SUCCEEDED(XNetworkingQueryPreferredLocalUdpMultiplayerPort(&port)) && port == 0,
       "QueryPreferredLocalUdpMultiplayerPort == 0（无系统保留端口）");
    ok(XNetworkingVerifyServerCertificate(nullptr, nullptr) == E_NOTIMPL,
       "VerifyServerCertificate == E_NOTIMPL");

    XNetworkingStatisticsBuffer stats;
    std::memset(&stats, 0xCC, sizeof(stats));
    ok(XNetworkingQueryStatistics(XNetworkingStatisticsType::TitleTcpQueuedReceivedBufferUsage, &stats) ==
               E_NOTIMPL &&
           !has_residue(&stats, sizeof(stats)),
       "QueryStatistics: E_NOTIMPL + union 整块清零");

    // ---- 所有 *Async：必须让 XAsyncBlock 完成，且远快于 1 秒 ----
    check_async("QueryPreferredLocalUdpMultiplayerPortAsync", &submit_udp_port);
    check_async("QuerySecurityInformationForUrlAsync", &submit_sec_info);
    check_async("QuerySecurityInformationForUrlUtf16Async", &submit_sec_info_utf16);
    // 提交失败路径：null async 必须立刻 E_INVALIDARG，而不是崩
    ok(XNetworkingQuerySecurityInformationForUrlAsync("https://example.com", nullptr) == E_INVALIDARG,
       "Async(nullptr) == E_INVALIDARG");

    // 异步结果函数：明确的 E_NOTIMPL + 出参 0（不能给残值）
    XAsyncBlock b2{};
    ok(SUCCEEDED(XNetworkingQuerySecurityInformationForUrlAsync("https://example.com", &b2)) &&
           XAsyncGetStatus(&b2, true) == E_NOTIMPL,
       "Async 完成后可取结果状态");
    size_t need = 0xFFFF;
    ok(XNetworkingQuerySecurityInformationForUrlAsyncResultSize(&b2, &need) == E_NOTIMPL && need == 0,
       "AsyncResultSize: E_NOTIMPL + 0");
    XNetworkingSecurityInformation* si = reinterpret_cast<XNetworkingSecurityInformation*>(0x1);
    size_t used_bytes = 0xFFFF;
    uint8_t buf[32];
    std::memset(buf, 0xCC, sizeof(buf));
    ok(XNetworkingQuerySecurityInformationForUrlAsyncResult(&b2, sizeof(buf), &used_bytes, buf, &si) ==
               E_NOTIMPL &&
           si == nullptr && used_bytes == 0 && !has_residue(buf, sizeof(buf)),
       "AsyncResult: E_NOTIMPL + 空指针/0/缓冲清零");

    xbgtasix::logf("[t26] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
