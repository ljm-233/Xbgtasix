// SPDX-License-Identifier: GPL-3.0-or-later
// xapu.cpp —— xapu.dll：Xbox 主机 APU 音频解码命令队列的 PC 端移植
//
// ============================ 移植来源 ============================
// 仓库：omonkeyz/xbox-emulator       （GPL-3.0，与本项目同许可）
//   crates/compat/xapu/src/rec.rs     导出壳（#[no_mangle] extern "system"）
//   crates/compat/xapu/src/lib.rs     队列 / 句柄 / 结果环 / 连接参数校验
//   crates/compat/xapu/src/engine.rs  解码与重采样引擎（Opus + HSRC）—— 本移植不实现
//   crates/compat/xapu/Cargo.toml     依赖 opusdec（参考实现自带的 Opus 解码器）
//   docs/LIBRARY_MAP.md  "xapu.dll: 5 functions; implemented 5" 一节
//
// ============================ 签名依据（重点） ============================
// 全部来自 rec.rs 的 extern "system" 定义：
//   XApuConnect(params: *const u32, region: *mut u8, handle: *mut usize) -> i32      rec.rs:130
//   XApuDisconnect(h: usize) -> i32                                                  rec.rs:144
//   XApuEnqueueCommand(h: usize, cmd: *const u8, out_seq: *mut u32) -> i32            rec.rs:149
//   XApuDequeueResult(h: usize, out: *mut u8) -> i32                                 rec.rs:175
//   XApuGetResultCount(h: usize, dequeued_out: *mut u32) -> u32                      rec.rs:191
//
// ★ 与旧桩的差异：XApuDisconnect 1 个参数（旧 2）、XApuDequeueResult 2 个（旧 3）。
//   旧桩的 (void*, void*, void*) 会把返回值塞在 rax 之外还多写参数——这里按真签名来。
//
// ============================ 关键 ABI 事实（都是参考实现里读出来的） ============================
// 1) 句柄是 **取反的指针**：`*handle = !(apu as usize)`（lib.rs:329）。
//    即 handle == 0 / usize::MAX 都不可能是有效句柄（obj() 里显式挡掉这两种，
//    lib.rs:254-259），这也是为什么 0 能被当成"空句柄"。
// 2) region 是一块 **16 字节** 的调用方缓冲：+0x0 写 APU 内存宿主指针，
//    +0x8 写结果事件句柄（lib.rs:327-328）。连接失败时这两处被清零。
// 3) params 是 **6 个 u32**（lib.rs:284-285）：
//      [0] codec      1=DecodeOpus 2=DecodeConvertOpus 3=ConvertPCM（engine.rs:5-7）
//      [1] streams    1..=500（TOTAL_STREAMS=500）
//      [2] depth      1..=16（每个流的命令队列深度）
//      [3] mem_size   <= 0x20000000
//      [4] frame      仅 codec 2/3 校验：0x80..=0x400 且为偶数
//      [5] options    只有低 4 位合法：2=DeinterleavedInput 4=DeinterleavedOutput
//                     8=EnableResultReadyEvent（xapu.rs:290, engine.rs:9-10）
// 4) 命令 0x40 字节、结果 0x50 字节（xapu.rs:42-43），命令类型 1=Start 2=Stop 3=Data 5=Reset。
//
// ============================ 为什么这一层只做"诚实失败" ============================
// xapu.dll 的全部价值就是"把 Opus 包解码/重采样成 PCM"。参考实现为此依赖
// 自带的 Opus 解码器（Cargo.toml 的 opusdec），并实现了带 5 帧群延迟的 HSRC
// 重采样器（engine.rs）。PC 上不存在主机 APU，也没有可以直接转发的系统解码器。
//
// 本移植**不**伪造一个"连上了"的句柄：如果 XApuConnect 返回 S_OK，游戏会建立
// 最多 500 个流并开始投递 Opus 包，而我们只能对每个包返回失败或给出静音数据；
// 那就是需求里明确禁止的"假成功让游戏拿空数据继续跑，崩得更晚更难查"。
// 因此：连接请求在**参数校验通过之后**返回 E_NOTIMPL（0x80004001），并且只记一次
// 日志。参数不合法时仍按控制台的规则返回 E_INVALIDARG / E_POINTER，
// 这样"游戏传错参数"和"我们没有解码器"在日志里能区分开。
//
// 要把这层打通，需要：① 一个可用的 Opus 解码器（libopus 或参考实现的 opusdec）；
// ② 移植 engine.rs 的重采样器；③ 按 lib.rs:363-441 实现每个流的命令队列与结果环。

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "xbgtasix/trace.h"

namespace {

// ============================== 返回码 ==============================
// 照抄 xapu.rs:14-27。
constexpr int32_t kS_OK = 0;
constexpr int32_t kE_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t kE_POINTER = static_cast<int32_t>(0x80004003u);
constexpr int32_t kE_OUTOFMEMORY = static_cast<int32_t>(0x8007000Eu);
constexpr int32_t kE_NOTIMPL = static_cast<int32_t>(0x80004001u);
constexpr int32_t kXAPU_E_NO_RESULT = static_cast<int32_t>(0x8AC81001u);

// ============================== 控制台常量 ==============================
constexpr uint32_t kTotalStreams = 500;         // 全部会话共享的流上限
constexpr uint32_t kMaxBaseData = 0x20000000;   // mem_size 上限
constexpr uint32_t kResultSize = 0x50;          // 结果结构大小
constexpr uint32_t kCmdSize = 0x40;             // 命令结构大小

constexpr uint32_t kPtDecodeOpus = 1;
constexpr uint32_t kPtConvertPcm = 3;

// 命令类型（xapu.rs:35-38）与结果事件选项
constexpr uint32_t kCmdStart = 1;
constexpr uint32_t kCmdStop = 2;
constexpr uint32_t kCmdData = 3;
constexpr uint32_t kCmdReset = 5;
constexpr uint32_t kOptResultEvent = 8;

// ============================== 只记一次 ==============================
struct OnceTable {
    CRITICAL_SECTION cs{};
    const char* seen[64]{};
    int n = 0;
    OnceTable() { InitializeCriticalSection(&cs); }
};
OnceTable& once_table() {
    static OnceTable* t = new OnceTable();  // 故意不析构：卸载期可能有线程仍在调
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
            ::xbgtasix::logf("[xapu][trace] " api " " __VA_ARGS__);         \
    } while (0)

uint32_t rd32(const void* base, size_t off) {
    uint32_t v = 0;
    std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(v));
    return v;
}

// 句柄有效性：0 与 SIZE_MAX 永远不是句柄（lib.rs:254-259）。
// 本移植不会产生任何有效句柄（连接一律失败），所以这里恒为 false —— 但保留这个判断
// 是为了让错误码与主机一致（无效句柄 -> E_INVALIDARG，而不是"随便返回个失败"）。
bool handle_valid(size_t h) {
    return h != 0 && h != static_cast<size_t>(~static_cast<size_t>(0));
}

void log_no_engine() {
    xbgtasix::logf("[xapu] 没有 APU 解码引擎：Opus 解码（参考实现依赖 opusdec）与 HSRC 重采样器"
                   "（engine.rs）都未移植，PC 上也没有可转发的系统实现。"
                   "为避免游戏拿着空/静音 PCM 继续跑，XApuConnect 在参数校验通过后返回 E_NOTIMPL。");
}

}  // namespace

// ============================================================================
//                              导出的 5 个符号
// ============================================================================
extern "C" {

// ============================== XApuConnect ==============================
// 参考：rec.rs:130 / lib.rs:273-333。3 个参数。
// 执行顺序严格照参考实现，这样日志里能区分"参数错"和"没引擎"：
//   1) region/handle 为空 -> E_POINTER
//   2) 先把 *handle 清 0、region 前 16 字节清零（失败时游戏看到的是干净状态）
//   3) params 为空 -> E_INVALIDARG
//   4) 6 个参数按控制台规则校验 -> 不合法 E_INVALIDARG
//   5) mem_size 超上限 -> E_OUTOFMEMORY
//   6) 本移植到此为止：没有解码引擎 -> E_NOTIMPL
__declspec(dllexport) int32_t XApuConnect(const uint32_t* params, uint8_t* region, size_t* handle) {
    XBTRACE("XApuConnect", "params=%p region=%p handle=%p", static_cast<const void*>(params),
            static_cast<void*>(region), static_cast<void*>(handle));
    if (!region || !handle) return kE_POINTER;

    *handle = 0;
    std::memset(region, 0, 16);  // 见文件头事实 2：+0=内存指针，+8=事件句柄

    if (!params) return kE_INVALIDARG;

    const uint32_t codec = rd32(params, 0);
    const uint32_t streams = rd32(params, 4);
    const uint32_t depth = rd32(params, 8);
    const uint32_t mem_size = rd32(params, 0xc);
    const uint32_t frame = rd32(params, 0x10);
    const uint32_t options = rd32(params, 0x14);

    const bool codec_ok = codec >= kPtDecodeOpus && codec <= kPtConvertPcm;
    const bool streams_ok = streams >= 1 && streams <= kTotalStreams;
    const bool depth_ok = depth >= 1 && depth <= 16;
    // frame 只对 codec 2/3 有约束（lib.rs:289）
    const bool frame_ok = (codec == kPtDecodeOpus) || (frame >= 0x80 && frame <= 0x400 && (frame % 2) == 0);
    const bool options_ok = (options & ~0xFu) == 0;

    if (!(codec_ok && streams_ok && depth_ok && frame_ok && options_ok)) {
        static std::atomic<int> logged{0};
        if (first_time("XApuConnect") || logged.fetch_add(1) < 8) {
            xbgtasix::logf("[xapu] XApuConnect 参数不合法：codec=%u streams=%u depth=%u "
                           "mem=0x%X frame=%u options=0x%X -> E_INVALIDARG",
                           codec, streams, depth, mem_size, frame, options);
        }
        return kE_INVALIDARG;
    }
    if (mem_size > kMaxBaseData) {
        static std::atomic<int> logged{0};
        if (first_time("XApuConnect") || logged.fetch_add(1) < 8) {
            xbgtasix::logf("[xapu] XApuConnect mem_size=0x%X 超过上限 0x%X -> E_OUTOFMEMORY",
                           mem_size, kMaxBaseData);
        }
        return kE_OUTOFMEMORY;
    }

    // 参数完全合法 —— 如果主机在这里就会建流、分配 mem_size 的内存、按 options 建事件。
    // 我们没有解码引擎，所以在这里诚实失败（并保留上面所有校验，让错误可区分）。
    static std::atomic<int> logged{0};
    if (first_time("XApuConnect") || logged.fetch_add(1) < 4) {
        xbgtasix::logf("[xapu] XApuConnect(codec=%u, streams=%u, depth=%u, mem=0x%X, frame=%u, "
                       "options=0x%X)：参数合法，但没有解码引擎 -> E_NOTIMPL"
                       "（控制台命令结构 0x%X 字节 / 结果结构 0x%X 字节）",
                       codec, streams, depth, mem_size, frame, options, kCmdSize, kResultSize);
        log_no_engine();
    }
    return kE_NOTIMPL;
}

// ============================== XApuDisconnect ==============================
// 参考：rec.rs:144 / lib.rs:335-353。1 个参数。
// 无损路径：本移植里不存在有效句柄（连接必然失败），所以任何句柄都按
// 参考实现的 obj(h) == None 分支处理 -> E_INVALIDARG。
// 注意：参考实现里"还有未取走的结果"时返回 XAPU_E_NO_RESULT 而不是 S_OK
// （lib.rs:338,348-352），这条约定写在这里，将来实现队列时要照做。
__declspec(dllexport) int32_t XApuDisconnect(size_t h) {
    XBTRACE("XApuDisconnect", "h=0x%llX", static_cast<unsigned long long>(h));
    if (!handle_valid(h)) {
        return kE_INVALIDARG;  // 0 / SIZE_MAX：控制台也当无效句柄
    }
    static std::atomic<int> logged{0};
    if (first_time("XApuDisconnect") || logged.fetch_add(1) < 4) {
        xbgtasix::logf("[xapu] XApuDisconnect(0x%llX)：句柄不是本模块发出去的"
                       "（连接一律失败）-> E_INVALIDARG",
                       static_cast<unsigned long long>(h));
    }
    return kE_INVALIDARG;
}

// ============================== XApuEnqueueCommand ==============================
// 参考：rec.rs:149 / lib.rs:363-441。3 个参数。
// 命令结构 0x40 字节，+0x0 = 类型（1 Start / 2 Stop / 3 Data / 5 Reset），+4 = 流号。
// 参考实现的校验顺序（将来实现时要照做）：句柄 -> 命令指针 -> 类型 -> 流号范围
//   -> 每流状态（Start 时已 active -> XAPU_E_STREAM_ACTIVE；非 Start 时未 active
//      -> XAPU_E_BAD_STREAM）-> 队列深度（XAPU_E_QUEUE_FULL）-> 结果序号写回 out_seq。
// 没有有效句柄时，按 obj(h)==None -> E_INVALIDARG。
__declspec(dllexport) int32_t XApuEnqueueCommand(size_t h, const uint8_t* cmd, uint32_t* out_seq) {
    XBTRACE("XApuEnqueueCommand", "h=0x%llX cmd=%p", static_cast<unsigned long long>(h),
            static_cast<const void*>(cmd));
    // 参考实现只在成功时写结果序号；这里先清 0，避免调用方拿到栈上残值当序号用。
    if (out_seq) *out_seq = 0;
    if (!handle_valid(h)) return kE_INVALIDARG;
    if (!cmd) return kE_INVALIDARG;
    static std::atomic<int> logged{0};
    if (first_time("XApuEnqueueCommand") || logged.fetch_add(1) < 4) {
        xbgtasix::logf("[xapu] XApuEnqueueCommand(句柄 0x%llX, cmd=%p)：没有有效连接 -> E_INVALIDARG",
                       static_cast<unsigned long long>(h), static_cast<const void*>(cmd));
    }
    return kE_INVALIDARG;
}

// ============================== XApuDequeueResult ==============================
// 参考：rec.rs:175 / lib.rs:443-469。2 个参数。
// 结果结构 0x50 字节；成功时把结果整块拷给调用方，并把该流的 queued 减一。
// 参考实现的顺序是"先判句柄，再判 out 空 -> E_POINTER"，所以句柄无效时返回
// E_INVALIDARG 而不是 E_POINTER。队列空时返回 XAPU_E_NO_RESULT（0x8AC81001）。
__declspec(dllexport) int32_t XApuDequeueResult(size_t h, uint8_t* out) {
    XBTRACE("XApuDequeueResult", "h=0x%llX out=%p", static_cast<unsigned long long>(h),
            static_cast<void*>(out));
    if (!handle_valid(h)) return kE_INVALIDARG;
    if (!out) return kE_POINTER;
    static std::atomic<int> logged{0};
    if (first_time("XApuDequeueResult") || logged.fetch_add(1) < 4) {
        xbgtasix::logf("[xapu] XApuDequeueResult(句柄 0x%llX)：没有有效连接 -> E_INVALIDARG",
                       static_cast<unsigned long long>(h));
    }
    return kE_INVALIDARG;
}

// ============================== XApuGetResultCount ==============================
// 参考：rec.rs:191 / lib.rs:471-478。2 个参数，返回 u32（已提交、未取走的结果数）。
// 句柄无效时参考实现直接返回 0（不写 dequeued_out）；这里额外把 dequeued_out 清 0，
// 避免调用方读到栈上残值。
__declspec(dllexport) uint32_t XApuGetResultCount(size_t h, uint32_t* dequeued_out) {
    XBTRACE("XApuGetResultCount", "h=0x%llX", static_cast<unsigned long long>(h));
    if (dequeued_out) *dequeued_out = 0;
    if (!handle_valid(h)) return 0;
    static std::atomic<int> logged{0};
    if (first_time("XApuGetResultCount") || logged.fetch_add(1) < 4) {
        xbgtasix::logf("[xapu] XApuGetResultCount(句柄 0x%llX)：没有有效连接 -> 0",
                       static_cast<unsigned long long>(h));
    }
    return 0;
}

}  // extern "C"
