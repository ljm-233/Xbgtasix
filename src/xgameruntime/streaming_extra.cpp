// streaming_extra.cpp —— XGameStreaming / XAppCapture / XNetworking 的"安全降级"
//
// 这三个模块在 PC 上都没有对应物。正确的降级不是"假装有"，而是**明确告诉调用方没有**：
//   * bool 查询          -> false
//   * 枚举               -> 头文件里真正的 无/断开/未知 哨兵（不是想当然的 0，见下方逐个注释）
//   * 结构体/出参        -> 一律先整块 memset 再填字段（失败路径没有错误通道，残值会被当数据用）
//   * 回调注册           -> 必须填写 token 出参，返回 S_OK；注销 -> true
//   * *Async             -> 一律走已导出的 XAsyncRun，保证 XAsyncGetStatus 能完成（结果 E_NOTIMPL）
//
// 注意：XGameStreamingGetConnectionState 已在 xgameruntime.cpp:1332 实现，这里不重复定义。
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <ctime>

#include "xbgtasix/trace.h"

#include "XAppCapture.h"
#include "XGameStreaming.h"
#include "XNetworking.h"

// ---------------------------------------------------------------------------
// 铁律 1 的兜底结构：XAsyncBlock 的顺序必须是 queue/context/callback/internal。
// 我们用真头文件里的 XAsyncBlock 调 XAsyncRun，并在编译期证明两者布局一致 ——
// 只要有人改了顺序或大小，这里立刻编译失败，而不是运行时把回调指针读成垃圾。
// ---------------------------------------------------------------------------
namespace {

struct XAsyncBlockFallback
{
    void* queue;
    void* context;
    void (*callback)(XAsyncBlockFallback*);
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};

static_assert(sizeof(XAsyncBlockFallback) == sizeof(XAsyncBlock),
              "XAsyncBlock 布局与兜底结构不一致（queue/context/callback/internal）");
static_assert(offsetof(XAsyncBlockFallback, queue) == offsetof(XAsyncBlock, queue), "queue 偏移漂移");
static_assert(offsetof(XAsyncBlockFallback, context) == offsetof(XAsyncBlock, context),
              "context 偏移漂移");
static_assert(offsetof(XAsyncBlockFallback, callback) == offsetof(XAsyncBlock, callback),
              "callback 偏移漂移（*Async 完成回调会踩空）");

std::atomic<uint64_t> g_next_registration_token{1};

// 回调注册的共同收尾：token 一定要写。调用方会拿着它去 Unregister，
// 留成栈垃圾会让它去注销一个不存在的 id（甚至我们自己的注册表被误删）。
void fill_registration_token(XTaskQueueRegistrationToken* token)
{
    if (token) token->token = g_next_registration_token.fetch_add(1, std::memory_order_relaxed);
}

template <typename T>
void zero_out(T* p)
{
    if (p) std::memset(static_cast<void*>(p), 0, sizeof(T));
}

// 所有 *Async 的 work：立刻以 E_NOTIMPL 完成。
// XAsyncRun（xgameruntime.cpp:594）会在工作线程里执行它并把 HRESULT 记进完成表，
// 所以 XAsyncGetStatus(wait=true) 会拿到 E_NOTIMPL，而不是空转到 E_PENDING。
HRESULT CALLBACK not_implemented_work(XAsyncBlock* async) noexcept
{
    (void)async;
    return E_NOTIMPL;
}

HRESULT begin_async_not_implemented(XAsyncBlock* async, const char* api)
{
    if (!async) return E_INVALIDARG;
    xbgtasix::logf("[stream] %s -> XAsyncRun(E_NOTIMPL)", api);
    const HRESULT hr = XAsyncRun(async, &not_implemented_work);
    return FAILED(hr) ? hr : S_OK;
}

}  // namespace

extern "C" {

// ===========================================================================
// XGameStreaming
// ===========================================================================

STDAPI XGameStreamingInitialize() noexcept
{
    xbgtasix::logf("[stream] XGameStreamingInitialize（PC 上无串流栈，惰性初始化）");
    return S_OK;
}

STDAPI_(void) XGameStreamingUninitialize() noexcept
{
    // 没有真实资源要释放；注册表是进程级的，注销由调用方逐个做。
}

STDAPI_(bool) XGameStreamingIsStreaming() noexcept
{
    return false;   // 本机没有 xCloud / 远程串流客户端
}

STDAPI_(uint32_t) XGameStreamingGetClientCount() noexcept
{
    return 0;
}

STDAPI XGameStreamingGetClients(
    uint32_t clientCount,
    XGameStreamingClientId* clients,
    uint32_t* clientsUsed) noexcept
{
    if (!clientsUsed) return E_INVALIDARG;   // 唯一的错误通道，先挡 null
    *clientsUsed = 0;                        // "一个客户端都没有"
    if (clientCount != 0 && !clients) return E_INVALIDARG;
    if (clients) std::memset(clients, 0, static_cast<size_t>(clientCount) * sizeof(clients[0]));
    return S_OK;
}

STDAPI XGameStreamingRegisterConnectionStateChanged(
    XTaskQueueHandle queue,
    void* context,
    XGameStreamingConnectionStateChangedCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept
{
    if (!token) return E_INVALIDARG;
    fill_registration_token(token);
    xbgtasix::logf("[stream] RegisterConnectionStateChanged(queue=%p ctx=%p cb=%p) token=%llu",
                   static_cast<void*>(queue), context, reinterpret_cast<void*>(callback),
                   static_cast<unsigned long long>(token->token));
    // 不会有人串流进来，所以永远不会触发；但调用方拿到的 token 是有效可注销的。
    return S_OK;
}

STDAPI_(bool) XGameStreamingUnregisterConnectionStateChanged(
    XTaskQueueRegistrationToken token,
    bool wait) noexcept
{
    (void)token;
    (void)wait;
    return true;   // 契约：注销成功（幂等，本来就不会有回调）
}

STDAPI XGameStreamingRegisterClientPropertiesChanged(
    XGameStreamingClientId client,
    XTaskQueueHandle queue,
    void* context,
    XGameStreamingClientPropertiesChangedCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept
{
    if (!token) return E_INVALIDARG;
    fill_registration_token(token);
    xbgtasix::logf("[stream] RegisterClientPropertiesChanged(client=%llu queue=%p ctx=%p cb=%p) token=%llu",
                   static_cast<unsigned long long>(client), static_cast<void*>(queue), context,
                   reinterpret_cast<void*>(callback),
                   static_cast<unsigned long long>(token->token));
    return S_OK;
}

STDAPI_(bool) XGameStreamingUnregisterClientPropertiesChanged(
    XGameStreamingClientId client,
    XTaskQueueRegistrationToken token,
    bool wait) noexcept
{
    (void)client;
    (void)token;
    (void)wait;
    return true;
}

STDAPI XGameStreamingGetStreamPhysicalDimensions(
    XGameStreamingClientId client,
    uint32_t* horizontalMm,
    uint32_t* verticalMm) noexcept
{
    (void)client;
    if (!horizontalMm || !verticalMm) return E_INVALIDARG;
    *horizontalMm = 0;   // 没有物理屏幕可报
    *verticalMm = 0;
    return E_NOTIMPL;
}

STDAPI XGameStreamingGetStreamAddedLatency(
    XGameStreamingClientId client,
    uint32_t* averageInputLatencyUs,
    uint32_t* averageOutputLatencyUs,
    uint32_t* standardDeviationUs) noexcept
{
    (void)client;
    if (!averageInputLatencyUs || !averageOutputLatencyUs || !standardDeviationUs) return E_INVALIDARG;
    *averageInputLatencyUs = 0;
    *averageOutputLatencyUs = 0;
    *standardDeviationUs = 0;
    return E_NOTIMPL;
}

STDAPI_(size_t) XGameStreamingGetServerLocationNameSize() noexcept
{
    return 0;   // 没有串流服务器
}

STDAPI XGameStreamingGetServerLocationName(
    size_t serverLocationNameSize,
    char* serverLocationName) noexcept
{
    // _Out_writes_z_：即使失败也要留一个合法空串，否则调用方一读就飞。
    if (serverLocationName && serverLocationNameSize != 0) serverLocationName[0] = '\0';
    return E_NOTIMPL;
}

STDAPI XGameStreamingIsTouchInputEnabled(
    XGameStreamingClientId client,
    bool* touchInputEnabled) noexcept
{
    (void)client;
    if (!touchInputEnabled) return E_INVALIDARG;
    *touchInputEnabled = false;   // 没有客户端 -> 没有触摸输入
    return S_OK;
}

STDAPI XGameStreamingGetGamepadPhysicality(
    IGameInputReading* gamepadReading,
    XGameStreamingGamepadPhysicality* gamepadPhysicality) noexcept
{
    (void)gamepadReading;
    if (!gamepadPhysicality) return E_INVALIDARG;
    // XGameStreamingGamepadPhysicality::None == 0x0（XGameStreaming.h:165）
    zero_out(gamepadPhysicality);
    return E_NOTIMPL;
}

// ---- Touch controls：纯 UI 指令，降级成空操作（void，没有错误通道） ----

STDAPI_(void) XGameStreamingHideTouchControls() noexcept {}

STDAPI_(void) XGameStreamingHideTouchControlsOnClient(XGameStreamingClientId client) noexcept
{
    (void)client;
}

STDAPI_(void) XGameStreamingShowTouchControlLayout(const char* layout) noexcept
{
    (void)layout;
}

STDAPI_(void) XGameStreamingShowTouchControlLayoutOnClient(
    XGameStreamingClientId client,
    const char* layout) noexcept
{
    (void)client;
    (void)layout;
}

STDAPI XGameStreamingUpdateTouchControlsState(
    size_t operationCount,
    const XGameStreamingTouchControlsStateOperation* operations) noexcept
{
    (void)operationCount;
    (void)operations;
    return S_OK;   // 没有布局在显示，状态更新就地丢弃
}

STDAPI XGameStreamingUpdateTouchControlsStateOnClient(
    XGameStreamingClientId client,
    size_t operationCount,
    const XGameStreamingTouchControlsStateOperation* operations) noexcept
{
    (void)client;
    (void)operationCount;
    (void)operations;
    return S_OK;
}

STDAPI XGameStreamingShowTouchControlsWithStateUpdate(
    const char* layout,
    size_t operationCount,
    const XGameStreamingTouchControlsStateOperation* operations) noexcept
{
    (void)layout;
    (void)operationCount;
    (void)operations;
    return S_OK;
}

STDAPI XGameStreamingShowTouchControlsWithStateUpdateOnClient(
    XGameStreamingClientId client,
    const char* layout,
    size_t operationCount,
    const XGameStreamingTouchControlsStateOperation* operations) noexcept
{
    (void)client;
    (void)layout;
    (void)operationCount;
    (void)operations;
    return S_OK;
}

STDAPI_(size_t) XGameStreamingGetTouchBundleVersionNameSize(
    XGameStreamingClientId client) noexcept
{
    (void)client;
    return 0;   // 没有触控适配包
}

STDAPI XGameStreamingGetTouchBundleVersion(
    XGameStreamingClientId client,
    XVersion* version,
    size_t versionNameSize,
    char* versionName) noexcept
{
    (void)client;
    if (version) *version = XVersion{};          // 0.0.0.0 = 没有版本
    if (versionName && versionNameSize) versionName[0] = '\0';
    return E_NOTIMPL;
}

STDAPI XGameStreamingGetSessionId(
    XGameStreamingClientId client,
    size_t sessionIdSize,
    char* sessionId,
    size_t* sessionIdUsed) noexcept
{
    (void)client;
    if (!sessionId) return E_INVALIDARG;
    if (sessionIdSize != 0) sessionId[0] = '\0';
    if (sessionIdUsed) *sessionIdUsed = 0;
    return E_NOTIMPL;
}

STDAPI XGameStreamingGetDisplayDetails(
    XGameStreamingClientId client,
    uint32_t maxSupportedPixels,
    float widestSupportedAspectRatio,
    float tallestSupportedAspectRatio,
    XGameStreamingDisplayDetails* displayDetails) noexcept
{
    (void)client;
    (void)maxSupportedPixels;
    (void)widestSupportedAspectRatio;
    (void)tallestSupportedAspectRatio;
    if (!displayDetails) return E_INVALIDARG;
    zero_out(displayDetails);   // safeArea 全 0、flags = XGameStreamingVideoFlags::None
    return E_NOTIMPL;
}

STDAPI XGameStreamingSetResolution(uint32_t width, uint32_t height) noexcept
{
    (void)width;
    (void)height;
    return E_NOTIMPL;   // 本机直接出图，没有"流分辨率"可设
}

// ===========================================================================
// XAppCapture
// ===========================================================================

// ---- 广播：PC 上没有广播管线 ----

STDAPI XAppBroadcastShowUI(XUserHandle requestingUser) noexcept
{
    (void)requestingUser;
    return E_NOTIMPL;
}

STDAPI XAppBroadcastGetStatus(XUserHandle requestingUser, XAppBroadcastStatus* appBroadcastStatus) noexcept
{
    (void)requestingUser;
    if (!appBroadcastStatus) return E_INVALIDARG;
    zero_out(appBroadcastStatus);   // 9 个 bool 全 false == 不能播/没在播
    return S_OK;
}

STDAPI_(bool) XAppBroadcastIsAppBroadcasting() noexcept
{
    return false;
}

STDAPI XAppBroadcastRegisterIsAppBroadcastingChanged(
    XTaskQueueHandle queue,
    void* context,
    XAppBroadcastMonitorCallback* appBroadcastMonitorCallback,
    XTaskQueueRegistrationToken* token) noexcept
{
    if (!token) return E_INVALIDARG;
    fill_registration_token(token);
    xbgtasix::logf("[capture] BroadcastRegister(queue=%p ctx=%p cb=%p) token=%llu",
                   static_cast<void*>(queue), context,
                   reinterpret_cast<void*>(appBroadcastMonitorCallback),
                   static_cast<unsigned long long>(token->token));
    return S_OK;
}

STDAPI_(bool) XAppBroadcastUnregisterIsAppBroadcastingChanged(
    XTaskQueueRegistrationToken token,
    bool wait) noexcept
{
    (void)token;
    (void)wait;
    return true;
}

// ---- 元数据：没有捕获管线，全部丢弃（HRESULT 返回 E_NOTIMPL） ----

STDAPI XAppCaptureMetadataAddStringEvent(const char* name, const char* value,
                                         XAppCaptureMetadataPriority priority) noexcept
{
    (void)name;
    (void)value;
    (void)priority;
    return E_NOTIMPL;
}

STDAPI XAppCaptureMetadataAddInt32Event(const char* name, int32_t value,
                                        XAppCaptureMetadataPriority priority) noexcept
{
    (void)name;
    (void)value;
    (void)priority;
    return E_NOTIMPL;
}

STDAPI XAppCaptureMetadataAddDoubleEvent(const char* name, double value,
                                         XAppCaptureMetadataPriority priority) noexcept
{
    (void)name;
    (void)value;
    (void)priority;
    return E_NOTIMPL;
}

STDAPI XAppCaptureMetadataStartStringState(const char* name, const char* value,
                                           XAppCaptureMetadataPriority priority) noexcept
{
    (void)name;
    (void)value;
    (void)priority;
    return E_NOTIMPL;
}

STDAPI XAppCaptureMetadataStartInt32State(const char* name, int32_t value,
                                          XAppCaptureMetadataPriority priority) noexcept
{
    (void)name;
    (void)value;
    (void)priority;
    return E_NOTIMPL;
}

STDAPI XAppCaptureMetadataStartDoubleState(const char* name, double value,
                                           XAppCaptureMetadataPriority priority) noexcept
{
    (void)name;
    (void)value;
    (void)priority;
    return E_NOTIMPL;
}

STDAPI XAppCaptureMetadataStopState(const char* name) noexcept
{
    (void)name;
    return E_NOTIMPL;
}

STDAPI XAppCaptureMetadataStopAllStates() noexcept
{
    return E_NOTIMPL;
}

STDAPI XAppCaptureMetadataRemainingStorageBytesAvailable(uint64_t* value) noexcept
{
    if (!value) return E_INVALIDARG;
    *value = 0;   // 没有给本作的捕获配额
    return S_OK;
}

STDAPI XAppCaptureRegisterMetadataPurged(
    XTaskQueueHandle queue,
    void* context,
    XAppCaptureMetadataPurgedCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept
{
    if (!token) return E_INVALIDARG;
    fill_registration_token(token);
    xbgtasix::logf("[capture] RegisterMetadataPurged(queue=%p ctx=%p cb=%p) token=%llu",
                   static_cast<void*>(queue), context, reinterpret_cast<void*>(callback),
                   static_cast<unsigned long long>(token->token));
    return S_OK;   // 永远不会 purge，但 token 可注销
}

STDAPI_(bool) XAppCaptureUnRegisterMetadataPurged(XTaskQueueRegistrationToken token,
                                                  bool wait) noexcept
{
    (void)token;
    (void)wait;
    return true;
}

// ---- 诊断截图/剪辑 ----

STDAPI XAppCaptureTakeDiagnosticScreenshot(
    bool gamescreenOnly,
    XAppCaptureScreenshotFormatFlag captureFlags,
    const char* filenamePrefix,
    XAppCaptureDiagnosticScreenshotResult* result) noexcept
{
    (void)gamescreenOnly;
    (void)captureFlags;
    (void)filenamePrefix;
    if (!result) return E_INVALIDARG;
    zero_out(result);   // fileCount = 0，path[]/fileSize/width/height 全 0
    return E_NOTIMPL;
}

STDAPI XAppCaptureRecordDiagnosticClip(
    time_t startTime,
    uint32_t durationInMs,
    const char* filenamePrefix,
    XAppCaptureRecordClipResult* result) noexcept
{
    (void)startTime;
    (void)durationInMs;
    (void)filenamePrefix;
    if (!result) return E_INVALIDARG;
    zero_out(result);   // path[] 空串，encoding = H264(0)
    return E_NOTIMPL;
}

// ---- 本地捕获 ----

STDAPI XAppCaptureGetVideoCaptureSettings(XAppCaptureVideoCaptureSettings* userCaptureSettings) noexcept
{
    if (!userCaptureSettings) return E_INVALIDARG;
    zero_out(userCaptureSettings);
    // 显式写一遍"没有"的语义（数值都是 0，但读代码的人不该去猜）：
    userCaptureSettings->width = 0;
    userCaptureSettings->height = 0;
    userCaptureSettings->maxRecordTimespanDurationInMs = 0;
    userCaptureSettings->encoding = XAppCaptureVideoEncoding::H264;        // 无偏好
    userCaptureSettings->colorFormat = XAppCaptureVideoColorFormat::SDR;   // 无偏好
    userCaptureSettings->isCaptureByGamesAllowed = false;                 // 关键：不允许游戏捕获
    xbgtasix::logf("[capture] GetVideoCaptureSettings -> 0x0, isCaptureByGamesAllowed=false");
    return S_OK;   // 成功返回"没有任何可用的捕获设置"
}

STDAPI XAppCaptureRecordTimespan(
    const SYSTEMTIME* startTimestamp,
    uint64_t durationInMilliseconds,
    XAppCaptureLocalResult* result) noexcept
{
    (void)startTimestamp;
    (void)durationInMilliseconds;
    if (!result) return E_INVALIDARG;
    zero_out(result);   // clipHandle = nullptr（不存在的流句柄）
    return E_NOTIMPL;
}

STDAPI XAppCaptureReadLocalStream(
    XAppCaptureLocalStreamHandle handle,
    size_t startPosition,
    uint32_t bytesToRead,
    uint8_t* buffer,
    uint32_t* bytesWritten) noexcept
{
    (void)handle;
    (void)startPosition;
    if (!bytesWritten) return E_INVALIDARG;
    *bytesWritten = 0;
    if (buffer && bytesToRead != 0) std::memset(buffer, 0, bytesToRead);
    return E_NOTIMPL;
}

STDAPI XAppCaptureCloseLocalStream(XAppCaptureLocalStreamHandle handle) noexcept
{
    if (!handle) return E_INVALIDARG;
    return E_NOTIMPL;
}

STDAPI XAppCaptureStartUserRecord(
    XUserHandle requestingUser,
    uint32_t localIdBufferLength,
    char* localIdBuffer) noexcept
{
    (void)requestingUser;
    if (localIdBufferLength != 0 && !localIdBuffer) return E_INVALIDARG;
    if (localIdBuffer && localIdBufferLength != 0)
        std::memset(localIdBuffer, 0, localIdBufferLength);   // 空 localId，不是残值字符串
    return E_NOTIMPL;
}

STDAPI XAppCaptureStopUserRecord(
    const char* localId,
    XAppCaptureUserRecordingResult* result) noexcept
{
    (void)localId;
    if (!result) return E_INVALIDARG;
    zero_out(result);
    return E_NOTIMPL;
}

STDAPI XAppCaptureCancelUserRecord(const char* localId) noexcept
{
    (void)localId;
    return E_NOTIMPL;
}

// ---- 截图流 ----

STDAPI XAppCaptureTakeScreenshot(
    XUserHandle requestingUser,
    XAppCaptureTakeScreenshotResult* result) noexcept
{
    (void)requestingUser;
    if (!result) return E_INVALIDARG;
    zero_out(result);   // localId = ""，availableScreenshotFormats = 无格式
    return E_NOTIMPL;
}

STDAPI XAppCaptureOpenScreenshotStream(
    const char* localId,
    XAppCaptureScreenshotFormatFlag screenshotFormat,
    XAppCaptureScreenshotStreamHandle* handle,
    uint64_t* totalBytes) noexcept
{
    (void)localId;
    (void)screenshotFormat;
    if (!handle) return E_INVALIDARG;
    *handle = nullptr;                                // 明确的"没有句柄"
    if (totalBytes) *totalBytes = 0;
    return E_NOTIMPL;
}

STDAPI XAppCaptureReadScreenshotStream(
    XAppCaptureScreenshotStreamHandle handle,
    uint64_t startPosition,
    uint32_t bytesToRead,
    uint8_t* buffer,
    uint32_t* bytesWritten) noexcept
{
    (void)handle;
    (void)startPosition;
    if (!bytesWritten) return E_INVALIDARG;
    *bytesWritten = 0;
    if (buffer && bytesToRead != 0) std::memset(buffer, 0, bytesToRead);
    return E_NOTIMPL;
}

STDAPI XAppCaptureCloseScreenshotStream(XAppCaptureScreenshotStreamHandle handle) noexcept
{
    if (!handle) return E_INVALIDARG;
    return E_NOTIMPL;
}

// ---- 设置 ----

STDAPI XAppCaptureEnableRecord() noexcept
{
    xbgtasix::logf("[capture] EnableRecord -> E_NOTIMPL（PC 上没有系统捕获服务）");
    return E_NOTIMPL;
}

STDAPI XAppCaptureDisableRecord() noexcept
{
    return E_NOTIMPL;   // 本来就没开，但也不假装成功
}

// ===========================================================================
// XNetworking
// ===========================================================================

STDAPI XNetworkingQueryPreferredLocalUdpMultiplayerPort(
    uint16_t* preferredLocalUdpMultiplayerPort) noexcept
{
    if (!preferredLocalUdpMultiplayerPort) return E_INVALIDARG;
    *preferredLocalUdpMultiplayerPort = 0;   // 0 = 没有系统保留端口，调用方自选
    return S_OK;
}

STDAPI XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync(XAsyncBlock* asyncBlock) noexcept
{
    return begin_async_not_implemented(asyncBlock, "QueryPreferredLocalUdpMultiplayerPortAsync");
}

STDAPI XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult(
    XAsyncBlock* asyncBlock,
    uint16_t* preferredLocalUdpMultiplayerPort) noexcept
{
    (void)asyncBlock;
    if (!preferredLocalUdpMultiplayerPort) return E_INVALIDARG;
    *preferredLocalUdpMultiplayerPort = 0;
    return E_NOTIMPL;   // 异步本身已以 E_NOTIMPL 完成
}

STDAPI XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged(
    XTaskQueueHandle queue,
    void* context,
    XNetworkingPreferredLocalUdpMultiplayerPortChangedCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept
{
    if (!token) return E_INVALIDARG;
    fill_registration_token(token);
    xbgtasix::logf("[net] RegisterPreferredLocalUdpMultiplayerPortChanged(queue=%p ctx=%p cb=%p) token=%llu",
                   static_cast<void*>(queue), context, reinterpret_cast<void*>(callback),
                   static_cast<unsigned long long>(token->token));
    return S_OK;
}

STDAPI_(bool) XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged(
    XTaskQueueRegistrationToken token,
    bool wait) noexcept
{
    (void)token;
    (void)wait;
    return true;
}

// ---- 安全信息：没有平台证书/thumbprint 供应，异步明确失败 ----

STDAPI XNetworkingQuerySecurityInformationForUrlAsync(
    const char* url,
    XAsyncBlock* asyncBlock) noexcept
{
    (void)url;
    return begin_async_not_implemented(asyncBlock, "QuerySecurityInformationForUrlAsync");
}

STDAPI XNetworkingQuerySecurityInformationForUrlAsyncResultSize(
    XAsyncBlock* asyncBlock,
    size_t* securityInformationBufferByteCount) noexcept
{
    (void)asyncBlock;
    if (!securityInformationBufferByteCount) return E_INVALIDARG;
    *securityInformationBufferByteCount = 0;
    return E_NOTIMPL;
}

STDAPI XNetworkingQuerySecurityInformationForUrlAsyncResult(
    XAsyncBlock* asyncBlock,
    size_t securityInformationBufferByteCount,
    size_t* securityInformationBufferByteCountUsed,
    uint8_t* securityInformationBuffer,
    XNetworkingSecurityInformation** securityInformation) noexcept
{
    (void)asyncBlock;
    if (!securityInformation) return E_INVALIDARG;
    *securityInformation = nullptr;
    if (securityInformationBufferByteCountUsed) *securityInformationBufferByteCountUsed = 0;
    if (securityInformationBuffer && securityInformationBufferByteCount != 0)
        std::memset(securityInformationBuffer, 0, securityInformationBufferByteCount);
    return E_NOTIMPL;
}

STDAPI XNetworkingQuerySecurityInformationForUrlUtf16Async(
    const wchar_t* url,
    XAsyncBlock* asyncBlock) noexcept
{
    (void)url;
    return begin_async_not_implemented(asyncBlock, "QuerySecurityInformationForUrlUtf16Async");
}

STDAPI XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize(
    XAsyncBlock* asyncBlock,
    size_t* securityInformationBufferByteCount) noexcept
{
    (void)asyncBlock;
    if (!securityInformationBufferByteCount) return E_INVALIDARG;
    *securityInformationBufferByteCount = 0;
    return E_NOTIMPL;
}

STDAPI XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult(
    XAsyncBlock* asyncBlock,
    size_t securityInformationBufferByteCount,
    size_t* securityInformationBufferByteCountUsed,
    uint8_t* securityInformationBuffer,
    XNetworkingSecurityInformation** securityInformation) noexcept
{
    (void)asyncBlock;
    if (!securityInformation) return E_INVALIDARG;
    *securityInformation = nullptr;
    if (securityInformationBufferByteCountUsed) *securityInformationBufferByteCountUsed = 0;
    if (securityInformationBuffer && securityInformationBufferByteCount != 0)
        std::memset(securityInformationBuffer, 0, securityInformationBufferByteCount);
    return E_NOTIMPL;
}

STDAPI XNetworkingVerifyServerCertificate(
    void* requestHandle,
    const XNetworkingSecurityInformation* securityInformation) noexcept
{
    (void)requestHandle;
    (void)securityInformation;
    return E_NOTIMPL;   // 没有平台证书链可校验
}

// ---- 连通性提示 ----

STDAPI XNetworkingGetConnectivityHint(XNetworkingConnectivityHint* connectivityHint) noexcept
{
    if (!connectivityHint) return E_INVALIDARG;
    zero_out(connectivityHint);
    // XNetworking.h:107 —— 枚举的"无"值是 None(1)，不是 0（0 是 Unknown，语义是"还不知道"）
    connectivityHint->connectivityLevel = XNetworkingConnectivityLevelHint::None;
    // XNetworking.h:115 —— 成本枚举没有"无"值，离线时成本无意义 -> 用 Unknown(0) 的"未知"
    connectivityHint->connectivityCost = XNetworkingConnectivityCostHint::Unknown;
    connectivityHint->ianaInterfaceType = 0;        // 没有接口（IF_TYPE_OTHER 才是 1，这里表示未设置）
    connectivityHint->networkInitialized = false;   // 我们的 shim 不接管系统网络栈，false 才是诚实的"没有"
    connectivityHint->approachingDataLimit = false;
    connectivityHint->overDataLimit = false;
    connectivityHint->roaming = false;
    return S_OK;
}

STDAPI XNetworkingRegisterConnectivityHintChanged(
    XTaskQueueHandle queue,
    void* context,
    XNetworkingConnectivityHintChangedCallback* callback,
    XTaskQueueRegistrationToken* token) noexcept
{
    if (!token) return E_INVALIDARG;
    fill_registration_token(token);
    xbgtasix::logf("[net] RegisterConnectivityHintChanged(queue=%p ctx=%p cb=%p) token=%llu",
                   static_cast<void*>(queue), context, reinterpret_cast<void*>(callback),
                   static_cast<unsigned long long>(token->token));
    return S_OK;   // 没有网络事件，但 token 可以注销
}

STDAPI_(bool) XNetworkingUnregisterConnectivityHintChanged(
    XTaskQueueRegistrationToken token,
    bool wait) noexcept
{
    (void)token;
    (void)wait;
    return true;
}

// ---- 配置与统计 ----

STDAPI XNetworkingQueryConfigurationSetting(
    XNetworkingConfigurationSetting configurationSetting,
    uint64_t* value) noexcept
{
    (void)configurationSetting;
    if (!value) return E_INVALIDARG;
    *value = 0;
    return E_NOTIMPL;
}

STDAPI XNetworkingSetConfigurationSetting(
    XNetworkingConfigurationSetting configurationSetting,
    uint64_t value) noexcept
{
    (void)configurationSetting;
    (void)value;
    return E_NOTIMPL;   // 不接管 TCP 缓冲，设置也无处生效
}

STDAPI XNetworkingQueryStatistics(
    XNetworkingStatisticsType statisticsType,
    XNetworkingStatisticsBuffer* statisticsBuffer) noexcept
{
    (void)statisticsType;
    if (!statisticsBuffer) return E_INVALIDARG;
    zero_out(statisticsBuffer);   // union 整块清零，5 个 uint64 全是 0 而不是残值
    return E_NOTIMPL;
}

}  // extern "C"
