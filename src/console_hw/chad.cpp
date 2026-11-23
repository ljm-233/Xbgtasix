// SPDX-License-Identifier: GPL-3.0-or-later
// chad.cpp —— chad.dll：Xbox 主机音频 DSP（卷积 / FFT）的 PC 端移植
//
// ============================ 移植来源 ============================
// 仓库：omonkeyz/xbox-emulator       （GPL-3.0，与本项目同许可）
//   crates/compat/chad/src/lib.rs   全部导出 + 客户端/上下文/卷积/FFT 实现
//   docs/LIBRARY_MAP.md  "chad.dll: 5 functions; implemented 5" 一节
//
// ============================ 签名依据（重点） ============================
// 全部来自 chad.rs 的 extern "system" 定义：
//   XDspConnectWithMaximumStreamLimit(mem: *mut c_void, mem_size: u32,
//       max_activations: u32, max_streams: u32, priority: u32,
//       out: *mut *mut Client) -> i32                                    chad.rs:317
//   XDspConnect(mem: *mut c_void, mem_size: u32, max_activations: u32,
//       out: *mut *mut Client) -> i32                                    chad.rs:309
//       ↑ 控制台确实有它（只是把 max_streams=0x100、priority=2 写死的薄封装），
//         但**目标 exe 没有导入这个名字**，所以本文件不导出它 —— 导出表要与
//         "游戏实际导入的 5 个符号"严格一致，多一个只会给导出数对账添噪音。
//   XDspDisconnect(client: *mut Client) -> i32                           chad.rs:341
//   XDspActivate(client: *mut Client, desc: *const u8,
//       status_out: *mut *mut u32, ctx_out: *mut *mut Context) -> i32    chad.rs:358
//   XDspSubmitCommand(ctx: *mut Context, cmd: *const u8,
//       out_seq: *mut u32) -> i32                                        chad.rs:425
//   （XDspDeactivate(ctx: *mut Context) -> i32                           chad.rs:496）
//
// ★ 与旧桩的差异：XDspConnectWithMaximumStreamLimit **6 个参数**（旧桩 4 个）、
//   XDspActivate **4 个**（旧桩 3 个）、XDspDisconnect 1 个（旧 2）、
//   XDspDeactivate 1 个（旧 2）。旧桩的参数少写两个，游戏读到的
//   status_out / ctx_out 就永远不会被填 —— 那是"看起来成功但对象为空"的典型崩法。
//
// ============================ 关键 ABI 事实（从参考实现读出） ============================
// 1) XDspConnect 就是"最大流数/优先级取默认值"的薄封装（chad.rs:309-314）：
//      max_streams = 0x100, priority = 2。
// 2) 连接参数校验（chad.rs:318）：
//      mem 非空；mem_size 在 [0x10000, 0x1F400000] 内；
//      max_activations <= 0x80；max_streams <= 0x100；priority <= 2。
//    全局上限：最多 8 个 client（MAX_CLIENTS）、所有 client 的 max_activations
//    总和最多 0x80。
// 3) 激活描述符（desc）布局 —— chad.rs:369-371：
//      +0x00 u32 processType   1=Convolution 2=FFT(正向) 3=IFFT(反向)
//      +0x04 u32 flags         2=Deinterleaved 4=EnablePolarFormat 8=StereoImpulseResponse
//      +0x08 u32 blockFrameCount  卷积只允许 0x200/0x400；FFT/IFFT 允许 0x200/0x400/0x800
//      +0x0c u32 channelCount  卷积 1..=2；FFT/IFFT 只能 1（单声道）
//      +0x10 u32 impulseResponseLength（元素个数，不是字节）
//      +0x14 u64 impulseResponse（IR 数据指针；FFT/IFFT 不读它）
//    控制台检查（chad.dll 0x1efc，chad.rs:372-406）：
//      · flags & 0xA 非 0（消交织 / 立体声 IR）时 channelCount 必须为 2；
//      · flags & 4（极坐标格式）在卷积下直接 E_INVALIDARG；
//      · 卷积：partitions = len/2/block，fdl = (partitions+7)&~7，
//        partitions < 9 或 fdl > 0x800 -> XDSP_E_IR_SIZE；IR 指针为空 -> E_INVALIDARG。
// 4) 提交命令（cmd）布局 —— chad.rs:446-450：
//      +0x00 f32 gainIn  +0x04 f32 gainOut  +0x08 ptr inputBlock
//      +0x10 ptr outputBlock  +0x18 ptr blockBufferMultiplier（仅 FFT 读）
//    输入/输出（以及 FFT 的非空 multiplier）必须落在 XDspConnect 给的 mem 区间内，
//    否则 E_INVALIDARG（控制台 0x254c）。结果序号写到 out_seq。
// 5) 错误码（chad.rs:8-16）：XDSP_E_NO_SLOT 0x8AC810A0 / XDSP_E_BUSY 0x8AC810A1 /
//    XDSP_E_TOO_MANY_CONTEXTS 0x8AC810A2 / XDSP_E_CONTEXTS_ACTIVE 0x8AC810A4 /
//    XDSP_E_TOO_MANY_CLIENTS 0x8AC810A5 / XDSP_E_TOO_MANY_ACTIVATIONS 0x8AC810A6 /
//    XDSP_E_IR_SIZE 0x8AC810A7。
//
// ============================ 分层：哪部分真、哪部分诚实失败 ============================
// 【真实现】XDspConnect / XDspConnectWithMaximumStreamLimit / XDspDisconnect
//   —— 这三个是**纯记账**：校验参数、登记 client、维护 8/0x80 两个全局额度。
//      不涉及任何信号处理，也不返回需要游戏自己解引用的回调表（client 是不透明指针，
//      只由本模块的导出函数接收），所以可以完整移植、没有 ABI 猜测风险。
// 【诚实失败】XDspActivate / XDspDeactivate / XDspSubmitCommand
//   —— 主机上这三步背后是 DSP 固件里的卷积/FFT 程序。**注意一件重要的事**：
//      参考实现的 chad **没有任何外部依赖**（Cargo.toml 只有 compat-common），
//      也就是说它的卷积（分块 overlap-save + FFT）与 FFT/IFFT 全是纯 C++ 可以做的数学，
//      并不是"PC 上没有的硬件"。本移植没做它，是**这次的范围与优先级选择**，
//      不是能力边界 —— 换句话说这是三个模块里最值得下一步补的一块。
//      在此之前，激活请求在**描述符校验通过之后**返回 E_NOTIMPL，绝不假装激活成功：
//      假成功会让游戏以为上下文档位已就绪，把音频块提交进来却发现没人处理。
//
// 补 DSP 的下一步（对应参考实现）：
//   1) Context 结构：status[4]、block、partitions、irs、chans（每通道 hist/fdl）、pos、
//      fft、work、acc、envelope_count、envelope、group（chad.rs:150-180）；
//   2) Fft::new/run（chad.rs:53-99）+ pack_forward / unpack_inverse（chad.rs:101-125）；
//   3) 卷积 process（chad.rs:284-306）：历史块前移、正变换、按 slot 做谱乘加、逆变换取后半；
//   4) transform（chad.rs:237-262）：FFT 输出 N/2 个 (re,im) 对，IFFT 输入同样布局，
//      正向不缩放、反向 1/N，polar 选项在 (magnitude, phase) 与 (re,im) 之间转换；
//   5) submit 的状态机（chad.rs:434-493）：status[0] 递增/status[1] 提交位、
//      8 条命令一组、envelope 只在组首命令更新。

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>

#include "xbgtasix/trace.h"

namespace {

// ============================== 返回码 ==============================
constexpr int32_t kS_OK = 0;
constexpr int32_t kE_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t kE_POINTER = static_cast<int32_t>(0x80004003u);
constexpr int32_t kE_NOTIMPL = static_cast<int32_t>(0x80004001u);
constexpr int32_t kXDSP_E_NO_SLOT = static_cast<int32_t>(0x8AC810A0u);
constexpr int32_t kXDSP_E_BUSY = static_cast<int32_t>(0x8AC810A1u);
constexpr int32_t kXDSP_E_TOO_MANY_CONTEXTS = static_cast<int32_t>(0x8AC810A2u);
constexpr int32_t kXDSP_E_CONTEXTS_ACTIVE = static_cast<int32_t>(0x8AC810A4u);
constexpr int32_t kXDSP_E_TOO_MANY_CLIENTS = static_cast<int32_t>(0x8AC810A5u);
constexpr int32_t kXDSP_E_TOO_MANY_ACTIVATIONS = static_cast<int32_t>(0x8AC810A6u);
constexpr int32_t kXDSP_E_IR_SIZE = static_cast<int32_t>(0x8AC810A7u);

// ============================== 控制台常量 ==============================
constexpr uint32_t kMaxClients = 8;
constexpr uint32_t kMaxActivations = 0x80;   // 所有 client 的 max_activations 总和
constexpr uint32_t kMaxContexts = 0x100;     // 单个 client 的活跃上下文上限
constexpr uint32_t kMaxStreams = 0x100;      // XDspConnect 的默认 max_streams
constexpr uint32_t kMaxPriority = 2;
constexpr uint32_t kMinMemSize = 0x10000;    // mem_size 下界
constexpr uint32_t kMaxMemSize = 0x1F400000; // mem_size 上界

// 进程类型 / 选项位 / 块大小
constexpr uint32_t kKindConvolution = 1;
constexpr uint32_t kKindFft = 2;
constexpr uint32_t kKindIfft = 3;
constexpr uint32_t kFlagDeinterleaved = 2;
constexpr uint32_t kFlagPolar = 4;
constexpr uint32_t kFlagStereoIr = 8;

// 描述符各字段偏移（见文件头事实 3）
constexpr size_t kDescKind = 0x00;
constexpr size_t kDescFlags = 0x04;
constexpr size_t kDescBlock = 0x08;
constexpr size_t kDescChannels = 0x0c;
constexpr size_t kDescIrLen = 0x10;
constexpr size_t kDescIrPtr = 0x14;

// ============================== 只记一次 ==============================
struct OnceTable {
    CRITICAL_SECTION cs{};
    const char* seen[64]{};
    int n = 0;
    OnceTable() { InitializeCriticalSection(&cs); }
};
OnceTable& once_table() {
    static OnceTable* t = new OnceTable();  // 故意不析构
    return *t;
}
bool first_time(const char* api) {
    OnceTable& t = once_table();
    EnterCriticalSection(&t.cs);
    bool first = true;
    for (int i = 0; i < t.n; ++i) {
        if (std::strcmp(t.seen[i], api) == 0) {
            first = false;
            break;
        }
    }
    if (first && t.n < static_cast<int>(sizeof(t.seen) / sizeof(t.seen[0]))) t.seen[t.n++] = api;
    LeaveCriticalSection(&t.cs);
    return first;
}

bool trace_on() {
    static const bool on = [] {
        char b[8] = {0};
        return GetEnvironmentVariableA("XBGTASIX_TRACE", b, sizeof(b)) > 0 && b[0] == '1';
    }();
    return on;
}

#define XBTRACE(api, ...)                                                  \
    do {                                                                   \
        static std::atomic<int> n_{0};                                     \
        if (trace_on() && n_.fetch_add(1) < 400)                           \
            ::xbgtasix::logf("[chad][trace] " api " " __VA_ARGS__);         \
    } while (0)

uint32_t rd32(const void* base, size_t off) {
    uint32_t v = 0;
    std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(v));
    return v;
}
uint64_t rd64(const void* base, size_t off) {
    uint64_t v = 0;
    std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(v));
    return v;
}

// ============================== client 对象 ==============================
// 游戏从不解引用它（只用指针回传给本模块的导出函数），所以布局可以自己定。
// magic 用来挡"明显不是我们发出去的 client"：stale 指针、别的模块的对象、
// 传错层级的句柄 —— 只要那块内存还能读，magic 对不上就返回 E_INVALIDARG，
// 而不是像旧桩那样把任意非空指针当 client，读到什么 active 就减什么，
// 直接把全局额度减成负数。（真正野到不可读的指针只能靠调用方守规矩，
// 主机 DLL 同样会 AV，这里不额外做 IsBadReadPtr 那类不可靠探测。）
constexpr uint32_t kClientMagic = 0x43445350;  // 'CDSP'
struct Client {
    uint32_t magic;
    uint32_t max_act;
    uint32_t active;
    uint32_t max_streams;
    uint32_t priority;
    bool dead;
    uintptr_t mem;
    uintptr_t mem_size;
};

struct GlobalCounters {
    uint32_t clients = 0;
    uint32_t activations = 0;
};
GlobalCounters& counters() {
    static GlobalCounters* g = new GlobalCounters();
    return *g;
}
std::mutex& counters_mutex() {
    static std::mutex* m = new std::mutex();
    return *m;
}

// 校验并取回 client（不合法返回空）。不抛异常、不 deref 未验证的指针。
Client* checked_client(void* p) {
    if (!p) return nullptr;
    Client* c = static_cast<Client*>(p);
    if (c->magic != kClientMagic || c->dead) return nullptr;
    return c;
}

// ============================== 描述符解析与校验 ==============================
// 完整移植控制台 chad.dll 0x1efc 的检查（chad.rs:369-406）。
// 返回值：kS_OK 表示描述符合法（可以激活），否则是应该返回给游戏的错误码。
int32_t validate_descriptor(const uint8_t* desc, uint32_t* out_kind, uint32_t* out_flags,
                            uint32_t* out_block, uint32_t* out_channels, uint32_t* out_partitions) {
    const uint32_t kind = rd32(desc, kDescKind);
    const uint32_t flags = rd32(desc, kDescFlags);
    const uint32_t block = rd32(desc, kDescBlock);
    const uint32_t channels = rd32(desc, kDescChannels);
    const uint32_t len = rd32(desc, kDescIrLen);
    const uint64_t ir = rd64(desc, kDescIrPtr);  // 只读不用于卷积以外的类型

    if (out_kind) *out_kind = kind;
    if (out_flags) *out_flags = flags;
    if (out_block) *out_block = block;
    if (out_channels) *out_channels = channels;

    if (kind < 1 || kind > 3) return kE_INVALIDARG;
    if (channels < 1 || channels > 2) return kE_INVALIDARG;
    // 消交织 / 立体声 IR 只对立体声有意义
    if ((flags & (kFlagDeinterleaved | kFlagStereoIr)) != 0 && channels != 2) return kE_INVALIDARG;
    // 极坐标格式不被卷积接受
    if ((flags & kFlagPolar) != 0 && kind == kKindConvolution) return kE_INVALIDARG;

    bool block_ok;
    if (kind == kKindConvolution) {
        block_ok = (block == 0x200 || block == 0x400);
    } else {
        // FFT/IFFT 单声道，且多允许 0x800 一档
        block_ok = (block == 0x200 || block == 0x400 || block == 0x800) && channels == 1;
    }
    if (!block_ok) return kE_INVALIDARG;

    if (kind != kKindConvolution) {
        if (out_partitions) *out_partitions = 0;
        return kS_OK;
    }

    // 卷积：IR 分区数与 FDL 上限
    const uint32_t partitions = (len / 2) / block;
    const uint32_t fdl = (partitions + 7) & ~7u;
    if (out_partitions) *out_partitions = partitions;
    if (partitions < 9 || fdl > 0x800) return kXDSP_E_IR_SIZE;
    if (ir == 0) return kE_INVALIDARG;
    return kS_OK;
}

void log_no_engine() {
    xbgtasix::logf("[chad] 没有 DSP 引擎：卷积/FFT 的运算本身是纯软件可实现的"
                   "（参考实现无任何外部依赖），但本次没有移植 chad.rs 的 Fft/Context/process，"
                   "所以 XDspActivate 在描述符校验通过后返回 E_NOTIMPL，"
                   "而不是给游戏一个【已激活】但永远不会被处理的上下文。");
}

}  // namespace

// ============================================================================
//                              导出的 5 个符号
// ============================================================================
extern "C" {

// ============================== XDspConnectWithMaximumStreamLimit ==============================
// 参考：chad.rs:317-338。6 个参数。
// 纯记账：参数范围 -> out 指针 -> 两个全局额度 -> 建 client。
__declspec(dllexport) int32_t XDspConnectWithMaximumStreamLimit(void* mem, uint32_t mem_size,
                                                                uint32_t max_activations,
                                                                uint32_t max_streams,
                                                                uint32_t priority, void** out) {
    XBTRACE("XDspConnectWithMaximumStreamLimit", "mem=%p size=0x%X maxAct=%u maxStreams=%u prio=%u",
            mem, mem_size, max_activations, max_streams, priority);
    if (!mem || mem_size < kMinMemSize || mem_size > kMaxMemSize || max_activations > kMaxActivations ||
        max_streams > kMaxStreams || priority > kMaxPriority) {
        static std::atomic<int> logged{0};
        if (first_time("XDspConnectWithMaximumStreamLimit") || logged.fetch_add(1) < 8) {
            xbgtasix::logf("[chad] XDspConnectWithMaximumStreamLimit 参数不合法：mem=%p size=0x%X "
                           "maxAct=%u maxStreams=%u prio=%u -> E_INVALIDARG",
                           mem, mem_size, max_activations, max_streams, priority);
        }
        return kE_INVALIDARG;
    }
    if (!out) return kE_POINTER;
    *out = nullptr;

    std::lock_guard<std::mutex> lock(counters_mutex());
    GlobalCounters& g = counters();
    if (g.activations + max_activations > kMaxActivations) return kXDSP_E_TOO_MANY_ACTIVATIONS;
    if (g.clients >= kMaxClients) return kXDSP_E_TOO_MANY_CLIENTS;

    Client* c = new (std::nothrow) Client{};
    if (!c) return kE_INVALIDARG;
    c->magic = kClientMagic;
    c->max_act = max_activations;
    c->active = 0;
    c->max_streams = max_streams;
    c->priority = priority;
    c->dead = false;
    c->mem = reinterpret_cast<uintptr_t>(mem);
    c->mem_size = mem_size;
    g.clients += 1;
    g.activations += max_activations;
    *out = c;

    xbgtasix::logf("[chad] XDspConnectWithMaximumStreamLimit(mem=%p, size=0x%X, maxAct=%u, "
                   "maxStreams=%u, prio=%u) -> S_OK client=%p（client 只是记账，"
                   "激活上下文仍需 DSP 引擎）",
                   mem, mem_size, max_activations, max_streams, priority,
                   static_cast<void*>(c));
    return kS_OK;
}

// ============================== XDspDisconnect ==============================
// 参考：chad.rs:341-355。1 个参数。
// 还有活跃上下文时返回 XDSP_E_CONTEXTS_ACTIVE（不是 S_OK）。本移植不会有活跃上下文
// （激活一律失败），所以正常路径就是 S_OK。
// 注意：这里**故意不 free** client 对象，只标 dead —— 游戏若在 disconnect 之后又拿旧指针
// 调一次（错乱的清理路径），读的是仍然有效内存 + magic 校验，返回 E_INVALIDARG；
// 若真 free 了就是 use-after-free。几十字节的泄漏换掉一类崩溃，值得。
__declspec(dllexport) int32_t XDspDisconnect(void* client) {
    XBTRACE("XDspDisconnect", "client=%p", client);
    if (!client) return kE_INVALIDARG;
    std::lock_guard<std::mutex> lock(counters_mutex());
    Client* c = checked_client(client);
    if (!c) {
        static std::atomic<int> logged{0};
        if (first_time("XDspDisconnect") || logged.fetch_add(1) < 8) {
            xbgtasix::logf("[chad] XDspDisconnect(%p)：不是本模块发出的 client（或已断开）-> E_INVALIDARG",
                           client);
        }
        return kE_INVALIDARG;
    }
    if (c->active != 0) return kXDSP_E_CONTEXTS_ACTIVE;

    GlobalCounters& g = counters();
    if (g.clients > 0) g.clients -= 1;
    if (g.activations >= c->max_act) g.activations -= c->max_act;
    else g.activations = 0;
    c->dead = true;  // 不 free，见函数头注释
    xbgtasix::logf("[chad] XDspDisconnect -> S_OK（剩余 client=%u，已登记 activations=%u）", g.clients,
                   g.activations);
    return kS_OK;
}

// ============================== XDspActivate ==============================
// 参考：chad.rs:358-422。4 个参数（旧桩只写了 3 个，status_out/ctx_out 永远不填）。
// 顺序：出参指针 -> 清出参 -> client/desc 空 -> 描述符校验 -> 上下文额度 -> 引擎。
// 引擎不存在，所以最后一步返回 E_NOTIMPL；前面所有校验都照控制台做，
// 这样"游戏传错描述符"和"我们没有 DSP"在日志里可区分，游戏也会拿到正确的错误码。
__declspec(dllexport) int32_t XDspActivate(void* client, const uint8_t* desc, uint32_t** status_out,
                                           void** ctx_out) {
    XBTRACE("XDspActivate", "client=%p desc=%p", client, static_cast<const void*>(desc));
    if (!status_out || !ctx_out) return kE_POINTER;
    *status_out = nullptr;
    *ctx_out = nullptr;
    if (!client || !desc) return kE_INVALIDARG;

    uint32_t kind = 0, flags = 0, block = 0, channels = 0, partitions = 0;
    int32_t hr;
    {
        std::lock_guard<std::mutex> lock(counters_mutex());
        Client* c = checked_client(client);
        if (!c) {
            static std::atomic<int> logged{0};
            if (first_time("XDspActivate") || logged.fetch_add(1) < 8) {
                xbgtasix::logf("[chad] XDspActivate(%p)：不是本模块发出的 client -> E_INVALIDARG",
                               client);
            }
            return kE_INVALIDARG;
        }
        hr = validate_descriptor(desc, &kind, &flags, &block, &channels, &partitions);
        if (hr != kS_OK) {
            static std::atomic<int> logged{0};
            if (first_time("XDspActivate") || logged.fetch_add(1) < 8) {
                xbgtasix::logf("[chad] XDspActivate：描述符不合法 kind=%u flags=0x%X block=0x%X "
                               "channels=%u -> 0x%08X",
                               kind, flags, block, channels, static_cast<uint32_t>(hr));
            }
            return hr;
        }
        if (c->active >= c->max_act || c->active >= kMaxContexts) return kXDSP_E_TOO_MANY_CONTEXTS;
    }

    // 描述符完全合法：主机在这里会建 Context 并加载 IR。
    static std::atomic<int> logged{0};
    if (first_time("XDspActivate") || logged.fetch_add(1) < 4) {
        xbgtasix::logf("[chad] XDspActivate：描述符合法 processType=%u flags=0x%X block=0x%X "
                       "channels=%u partitions=%u -> E_NOTIMPL（没有 DSP 引擎，未建上下文）",
                       kind, flags, block, channels, partitions);
        log_no_engine();
    }
    return kE_NOTIMPL;
}

// ============================== XDspDeactivate ==============================
// 参考：chad.rs:496-507。1 个参数。
// 还有未提交的命令（status[0] != status[1]）时返回 XDSP_E_BUSY。
// 本移植里不可能存在有效上下文（XDspActivate 一律失败），所以任何指针都是无效的。
// 不 deref：避免拿一个野指针去读 status 字段。
__declspec(dllexport) int32_t XDspDeactivate(void* ctx) {
    XBTRACE("XDspDeactivate", "ctx=%p", ctx);
    if (!ctx) return kE_INVALIDARG;
    static std::atomic<int> logged{0};
    if (first_time("XDspDeactivate") || logged.fetch_add(1) < 4) {
        xbgtasix::logf("[chad] XDspDeactivate(%p)：本模块从未成功激活过上下文 -> E_INVALIDARG", ctx);
    }
    return kE_INVALIDARG;
}

// ============================== XDspSubmitCommand ==============================
// 参考：chad.rs:425-493（其中 submit() 在 434-493）。3 个参数。
// 顺序：ctx/cmd 空 -> 上下文错误 -> 8 条命令一组的状态检查(XDSP_E_NO_SLOT) ->
//       输入/输出（FFT 还有 multiplier）必须落在 connect 时的 mem 区间内(E_INVALIDARG) ->
//       执行 -> status 递增 -> out_seq。
// 没有有效上下文，这里给 E_INVALIDARG。
__declspec(dllexport) int32_t XDspSubmitCommand(void* ctx, const uint8_t* cmd, uint32_t* out_seq) {
    XBTRACE("XDspSubmitCommand", "ctx=%p cmd=%p", ctx, static_cast<const void*>(cmd));
    // 参考实现只在成功时写 out_seq；这里先清 0，避免调用方把栈上残值当序号。
    if (out_seq) *out_seq = 0;
    if (!ctx || !cmd) return kE_INVALIDARG;
    static std::atomic<int> logged{0};
    if (first_time("XDspSubmitCommand") || logged.fetch_add(1) < 4) {
        xbgtasix::logf("[chad] XDspSubmitCommand(ctx=%p, cmd=%p)：没有有效上下文"
                       "（XDspActivate 一律 E_NOTIMPL）-> E_INVALIDARG",
                       ctx, static_cast<const void*>(cmd));
    }
    return kE_INVALIDARG;
}

}  // extern "C"
