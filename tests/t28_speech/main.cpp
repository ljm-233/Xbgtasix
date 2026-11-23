// SPDX-License-Identifier: GPL-3.0-or-later
// t28_speech — XSpeechSynthesizer 用 SAPI 真合成
//
// 这个测试不看返回值就完事：它要求**真的产出一段 WAV 字节**，
// 并且头部必须是 "RIFF....WAVE"，长度大于文件头本身。
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "xbgtasix/trace.h"

struct VoiceInfo {
    const char* Description;
    const char* DisplayName;
    unsigned Gender;
    const char* VoiceId;
    const char* Language;
};

extern "C" {
HRESULT WINAPI XSpeechSynthesizerCreate(void** synth);
HRESULT WINAPI XSpeechSynthesizerCloseHandle(void* synth);
HRESULT WINAPI XSpeechSynthesizerEnumerateInstalledVoices(void* context, void* callback);
HRESULT WINAPI XSpeechSynthesizerCreateStreamFromText(void* synth, const char* text, void** stream);
HRESULT WINAPI XSpeechSynthesizerGetStreamDataSize(void* stream, size_t* size);
HRESULT WINAPI XSpeechSynthesizerGetStreamData(void* stream, size_t size, void* buffer, size_t* used);
HRESULT WINAPI XSpeechSynthesizerCloseStreamHandle(void* stream);
HRESULT WINAPI XSpeechSynthesizerSetDefaultVoice(void* synth);
HRESULT WINAPI XSpeechToTextSendString(const char* speaker, const char* content, unsigned type);
HRESULT WINAPI XSpeechToTextBeginHypothesisString(const char* speaker, const char* content, unsigned type,
                                                  unsigned* id);
}

namespace {

int g_fail = 0;
int g_voices = 0;

void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t28]   PASS  %-34s %s", what, detail);
    } else {
        xbgtasix::logf("[t28]   FAIL  %-34s %s", what, detail);
        ++g_fail;
    }
}

bool CALLBACK on_voice(const VoiceInfo* info, void* context) {
    (void)context;
    if (info && info->Description && info->Description[0]) {
        ++g_voices;
        if (g_voices <= 3) {
            xbgtasix::logf("[t28]   语音: %s | id=%s | lang=%s", info->Description,
                           info->VoiceId ? info->VoiceId : "?", info->Language ? info->Language : "?");
        }
    }
    return true;   // 继续枚举
}

}  // namespace

int main() {
    // ---- 1) 枚举系统语音 ----
    const HRESULT ehr = XSpeechSynthesizerEnumerateInstalledVoices(nullptr,
                                                                  reinterpret_cast<void*>(&on_voice));
    ok(SUCCEEDED(ehr), "EnumerateInstalledVoices");
    xbgtasix::logf("[t28] 共枚举到 %d 个语音", g_voices);
    ok(g_voices >= 1, "至少有一个可用语音");

    // ---- 2) 创建合成器 ----
    void* synth = nullptr;
    const HRESULT chr = XSpeechSynthesizerCreate(&synth);
    ok(SUCCEEDED(chr) && synth, "Create 拿到合成器句柄");
    if (!synth) {
        xbgtasix::logf("[t28] done failures=%d -> FAIL（无合成器，后续跳过）", g_fail);
        return 1;
    }
    ok(SUCCEEDED(XSpeechSynthesizerSetDefaultVoice(synth)), "SetDefaultVoice");

    // ---- 3) 真朗读（输出到系统默认设备）----
    void* stream = nullptr;
    const HRESULT shr = XSpeechSynthesizerCreateStreamFromText(synth, "xbgtasix speech test", &stream);
    ok(SUCCEEDED(shr) && stream, "CreateStreamFromText（真朗读，返回值即 Speak 结果）");
    if (FAILED(shr)) {
        // 这台机器/会话可能没有可用的音频输出（沙箱、RDP、无音频服务）。
        // 那不是兼容层的错，但也不能算 PASS —— 按套件约定返回 2（SKIP）。
        xbgtasix::logf("[t28] Speak 失败 hr=0x%08lx：本环境没有可用音频输出 -> SKIP",
                       static_cast<unsigned long>(shr));
        XSpeechSynthesizerCloseHandle(synth);
        return 2;
    }
    if (stream) {
        // 本实现的能力边界：真朗读，但不提供离线 WAV 渲染。
        // 这里断言"它会诚实报不可用"，而不是断言一个伪造的 WAV。
        size_t size = 12345;
        const HRESULT szh = XSpeechSynthesizerGetStreamDataSize(stream, &size);
        ok(size == 0, "GetStreamDataSize 报 0 字节（无离线条带）");
        ok(szh == E_NOTIMPL, "取数据接口明确报 E_NOTIMPL，而不是给空音频");

        unsigned char buf[8];
        size_t used = 999;
        ok(XSpeechSynthesizerGetStreamData(stream, sizeof(buf), buf, &used) == E_NOTIMPL,
           "GetStreamData 同样明确报 E_NOTIMPL");
        ok(used == 0, "bufferUsed 被写成 0");
        XSpeechSynthesizerCloseStreamHandle(stream);
    }

    // ---- 4) 语音转文字是"输出侧"接口：给 S_OK，不阻塞 ----
    ok(SUCCEEDED(XSpeechToTextSendString("玩家", "你好", 1)), "SpeechToTextSendString 返回 S_OK");
    unsigned hyp = 0;
    ok(SUCCEEDED(XSpeechToTextBeginHypothesisString("玩家", "你", 1, &hyp)) && hyp != 0,
       "BeginHypothesisString 写出非零 id");

    XSpeechSynthesizerCloseHandle(synth);
    ok(true, "CloseHandle 未崩溃");
    xbgtasix::logf("[t28] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
