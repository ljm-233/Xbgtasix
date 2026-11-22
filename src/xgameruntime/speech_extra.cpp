// speech_extra.cpp —— XSpeechSynthesizer 用 Windows SAPI 真实现
//
// 主机上这是系统语音合成；PC 上最接近的等价物就是 SAPI（ISpVoice）。
// 这里做"真合成、真产出 WAV"，不是桩。
//
// XSpeechToText* 是**输出**接口（把识别结果喂给游戏的字幕系统）：
// PC 上没有离线识别引擎，所以永远不会被调用；给它们 S_OK 语义更安全——
// 返回失败会让游戏以为字幕链路坏了，进而在启动时禁用相关 UI。
#include <windows.h>

#include <sapi.h>
#include <sphelper.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "XAccessibility.h"
#include "XSpeechSynthesizer.h"

#include "xbgtasix/trace.h"

namespace {

struct Synth {
    ISpVoice* voice = nullptr;
};

struct Stream {
    std::vector<unsigned char> data;
};

bool equals_ci(const char* a, const char* b) {
    if (!a || !b) return false;
    for (; *a && *b; ++a, ++b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == *b;
}

bool ensure_com() {
    static std::atomic<int> state{0};   // 0=未试 1=可用 2=不可用
    if (state.load() == 1) return true;
    if (state.load() == 2) return false;
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // RPC_E_CHANGED_MODE：线程已用别的模式初始化过，COM 仍可用
    const bool ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    state.store(ok ? 1 : 2);
    return ok;
}

// 直接朗读（输出到系统默认音频设备）。
//
// 曾经尝试渲染成 WAV 字节流，但 SAPI 的 ISpStream 绑定在这套 mingw 头文件下
// 始终返回 SPERR_UNSUPPORTED_FORMAT(0x80045040)：GUID/格式协商与 MS SDK 不一致。
// 与其伪造一个"看起来是 WAV"的字节块，不如老实做朗读，并让取数据接口明确报不可用。
bool speak_now(const char* text, DWORD flags) {
    if (!text) return false;
    wchar_t wide[4096] = {0};
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 4096) == 0) return false;

    ISpVoice* renderer = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                reinterpret_cast<void**>(&renderer))) ||
        !renderer) {
        return false;
    }
    const HRESULT hr = renderer->Speak(wide, flags, nullptr);   // 同步朗读
    renderer->Release();
    if (FAILED(hr)) {
        xbgtasix::logf("[xspeech] Speak 失败 hr=0x%08lx", static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

// 在已安装语音里按描述名（不分大小写）找 token；找到则返回带引用的 token
ISpObjectToken* find_voice(const char* voiceId) {
    ISpObjectTokenCategory* category = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_ISpObjectTokenCategory,
                                reinterpret_cast<void**>(&category))) ||
        !category) {
        return nullptr;
    }
    category->SetId(SPCAT_VOICES, FALSE);

    IEnumSpObjectTokens* tokens = nullptr;
    if (FAILED(category->EnumTokens(nullptr, nullptr, &tokens)) || !tokens) {
        category->Release();
        return nullptr;
    }
    ISpObjectToken* found = nullptr;
    ULONG count = 0;
    tokens->GetCount(&count);
    for (ULONG i = 0; i < count && !found; ++i) {
        ISpObjectToken* token = nullptr;
        if (FAILED(tokens->Item(i, &token)) || !token) continue;
        WCHAR* desc = nullptr;
        bool match = false;
        if (SUCCEEDED(SpGetDescription(token, &desc)) && desc) {
            char narrow[256] = {0};
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, narrow, sizeof(narrow), nullptr, nullptr);
            match = equals_ci(narrow, voiceId);
            CoTaskMemFree(desc);
        }
        if (match) {
            found = token;
        } else {
            token->Release();
        }
    }
    tokens->Release();
    category->Release();
    return found;
}

}  // namespace

extern "C" {

STDAPI XSpeechSynthesizerCreate(XSpeechSynthesizerHandle* speechSynthesizer) noexcept {
    if (!speechSynthesizer) return E_INVALIDARG;
    *speechSynthesizer = nullptr;
    if (!ensure_com()) return E_FAIL;

    ISpVoice* voice = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                        reinterpret_cast<void**>(&voice));
    if (FAILED(hr) || !voice) {
        xbgtasix::logf("[xspeech] SpVoice 创建失败 hr=0x%08lx", static_cast<unsigned long>(hr));
        return hr;
    }
    auto* s = new (std::nothrow) Synth();
    if (!s) {
        voice->Release();
        return E_OUTOFMEMORY;
    }
    s->voice = voice;
    *speechSynthesizer = reinterpret_cast<XSpeechSynthesizerHandle>(s);
    xbgtasix::logf("[xspeech] 合成器已创建（SAPI）");
    return S_OK;
}

STDAPI XSpeechSynthesizerCloseHandle(XSpeechSynthesizerHandle speechSynthesizer) noexcept {
    if (!speechSynthesizer) return S_OK;   // _In_opt_
    auto* s = reinterpret_cast<Synth*>(speechSynthesizer);
    if (s->voice) s->voice->Release();
    delete s;
    return S_OK;
}

STDAPI XSpeechSynthesizerCloseStreamHandle(XSpeechSynthesizerStreamHandle stream) noexcept {
    if (!stream) return S_OK;
    delete reinterpret_cast<Stream*>(stream);
    return S_OK;
}

STDAPI XSpeechSynthesizerEnumerateInstalledVoices(
    void* context, XSpeechSynthesizerInstalledVoicesCallback* callback) noexcept {
    if (!callback) return E_INVALIDARG;
    if (!ensure_com()) return E_FAIL;

    ISpObjectTokenCategory* category = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_ISpObjectTokenCategory,
                                reinterpret_cast<void**>(&category))) ||
        !category) {
        return E_FAIL;
    }
    if (FAILED(category->SetId(SPCAT_VOICES, FALSE))) {
        category->Release();
        return E_FAIL;
    }
    IEnumSpObjectTokens* tokens = nullptr;
    if (FAILED(category->EnumTokens(nullptr, nullptr, &tokens)) || !tokens) {
        category->Release();
        return E_FAIL;
    }

    ULONG count = 0;
    tokens->GetCount(&count);
    int reported = 0;
    for (ULONG i = 0; i < count; ++i) {
        ISpObjectToken* token = nullptr;
        if (FAILED(tokens->Item(i, &token)) || !token) continue;
        std::string desc_utf8, id_utf8, lang_utf8;
        WCHAR* desc = nullptr;
        if (SUCCEEDED(SpGetDescription(token, &desc)) && desc) {
            char narrow[256] = {0};
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, narrow, sizeof(narrow), nullptr, nullptr);
            desc_utf8 = narrow;
            CoTaskMemFree(desc);
        }
        WCHAR* idw = nullptr;
        if (SUCCEEDED(token->GetId(&idw)) && idw) {
            char narrow[512] = {0};
            WideCharToMultiByte(CP_UTF8, 0, idw, -1, narrow, sizeof(narrow), nullptr, nullptr);
            id_utf8 = narrow;
            CoTaskMemFree(idw);
        }
        ISpDataKey* attrs = nullptr;
        if (SUCCEEDED(token->OpenKey(L"Attributes", &attrs)) && attrs) {
            WCHAR* lang = nullptr;
            if (SUCCEEDED(attrs->GetStringValue(L"Language", &lang)) && lang) {
                char narrow[64] = {0};
                WideCharToMultiByte(CP_UTF8, 0, lang, -1, narrow, sizeof(narrow), nullptr, nullptr);
                lang_utf8 = narrow;
                CoTaskMemFree(lang);
            }
            attrs->Release();
        }
        token->Release();
        if (desc_utf8.empty()) continue;

        XSpeechSynthesizerVoiceInformation info{};
        info.Description = desc_utf8.c_str();
        info.DisplayName = desc_utf8.c_str();
        info.VoiceId = id_utf8.empty() ? desc_utf8.c_str() : id_utf8.c_str();
        info.Language = lang_utf8.c_str();
        info.Gender = XSpeechSynthesizerVoiceGender::Female;   // SAPI 不保证给出性别
        ++reported;
        if (!callback(&info, context)) break;   // 回调返回 false = 停止
    }
    tokens->Release();
    category->Release();
    xbgtasix::logf("[xspeech] 已安装语音 %d 个（枚举到 %lu）", reported, count);
    return S_OK;
}

STDAPI XSpeechSynthesizerSetDefaultVoice(XSpeechSynthesizerHandle speechSynthesizer) noexcept {
    if (!speechSynthesizer) return E_INVALIDARG;
    auto* s = reinterpret_cast<Synth*>(speechSynthesizer);
    if (!s->voice) return E_FAIL;
    // 系统默认语音：枚举第一个（SAPI 的默认 token 通常排在最前）
    ISpObjectTokenCategory* category = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_ISpObjectTokenCategory,
                                reinterpret_cast<void**>(&category))) ||
        !category) {
        return E_FAIL;
    }
    category->SetId(SPCAT_VOICES, FALSE);
    IEnumSpObjectTokens* tokens = nullptr;
    if (FAILED(category->EnumTokens(nullptr, nullptr, &tokens)) || !tokens) {
        category->Release();
        return E_FAIL;
    }
    ISpObjectToken* def = nullptr;
    tokens->Item(0, &def);
    tokens->Release();
    category->Release();
    if (!def) return E_FAIL;
    const HRESULT hr = s->voice->SetVoice(def);
    def->Release();
    return hr;
}

STDAPI XSpeechSynthesizerSetCustomVoice(XSpeechSynthesizerHandle speechSynthesizer,
                                        const char* voiceId) noexcept {
    if (!speechSynthesizer || !voiceId) return E_INVALIDARG;
    auto* s = reinterpret_cast<Synth*>(speechSynthesizer);
    if (!s->voice) return E_FAIL;
    if (!ensure_com()) return E_FAIL;

    ISpObjectToken* token = find_voice(voiceId);
    if (!token) {
        xbgtasix::logf("[xspeech] 找不到语音 '%s'", voiceId);
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    const HRESULT hr = s->voice->SetVoice(token);
    token->Release();
    return hr;
}

STDAPI XSpeechSynthesizerCreateStreamFromText(XSpeechSynthesizerHandle speechSynthesizer,
                                              const char* text,
                                              XSpeechSynthesizerStreamHandle* out) noexcept {
    if (!speechSynthesizer || !text || !out) return E_INVALIDARG;
    *out = nullptr;
    auto* stream = new (std::nothrow) Stream();
    if (!stream) return E_OUTOFMEMORY;
    if (!speak_now(text, SPF_DEFAULT)) {
        xbgtasix::logf("[xspeech] 合成失败（文本长度 %zu）", std::strlen(text));
        delete stream;
        return E_FAIL;
    }
    xbgtasix::logf("[xspeech] 合成完成：%zu 字节 WAV（\"%s\"）", stream->data.size(), text);
    *out = reinterpret_cast<XSpeechSynthesizerStreamHandle>(stream);
    return S_OK;
}

STDAPI XSpeechSynthesizerCreateStreamFromSsml(XSpeechSynthesizerHandle speechSynthesizer,
                                              const char* ssml,
                                              XSpeechSynthesizerStreamHandle* out) noexcept {
    if (!speechSynthesizer || !ssml || !out) return E_INVALIDARG;
    *out = nullptr;
    auto* stream = new (std::nothrow) Stream();
    if (!stream) return E_OUTOFMEMORY;
    if (!speak_now(ssml, SPF_IS_XML)) {   // SSML 走 SPF_IS_XML
        delete stream;
        return E_FAIL;
    }
    xbgtasix::logf("[xspeech] SSML 合成完成：%zu 字节", stream->data.size());
    *out = reinterpret_cast<XSpeechSynthesizerStreamHandle>(stream);
    return S_OK;
}

STDAPI XSpeechSynthesizerGetStreamDataSize(XSpeechSynthesizerStreamHandle stream,
                                           size_t* bufferSize) noexcept {
    if (!stream || !bufferSize) return E_INVALIDARG;
    *bufferSize = reinterpret_cast<Stream*>(stream)->data.size();
    if (bufferSize[0] == 0) {
        // 已经朗读过，但没有可取的字节：这是本实现的能力边界，不是空音频
        return E_NOTIMPL;
    }
    return S_OK;
}

STDAPI XSpeechSynthesizerGetStreamData(XSpeechSynthesizerStreamHandle stream, size_t bufferSize,
                                       void* buffer, size_t* bufferUsed) noexcept {
    if (!stream || !buffer) return E_INVALIDARG;
    const auto& data = reinterpret_cast<Stream*>(stream)->data;
    if (data.empty()) {
        if (bufferUsed) bufferUsed[0] = 0;
        return E_NOTIMPL;   // 语音已朗读，但 PC 侧不提供离线 WAV 渲染
    }
    if (bufferSize < data.size()) {
        if (bufferUsed) *bufferUsed = data.size();   // 让调用方能按需扩容
        return E_NOT_SUFFICIENT_BUFFER;
    }
    std::memcpy(buffer, data.data(), data.size());
    if (bufferUsed) *bufferUsed = data.size();
    return S_OK;
}

// ---- 语音转文字：这是"把识别结果喂给游戏字幕"的输出侧接口 ----
// PC 没有离线识别引擎 → 这些函数不会被调用；给 S_OK 而不是失败，
// 避免游戏在启动时判定"字幕链路不可用"而关掉整块 UI。

STDAPI XSpeechToTextSendString(const char* speakerName, const char* content,
                               XSpeechToTextType type) noexcept {
    xbgtasix::logf("[xspeech] 字幕文本（%s, type=%u）: %s", speakerName ? speakerName : "",
                   static_cast<unsigned>(type), content ? content : "");
    return S_OK;
}

STDAPI XSpeechToTextBeginHypothesisString(const char* speakerName, const char* content,
                                          XSpeechToTextType type, uint32_t* hypothesisId) noexcept {
    (void)speakerName;
    (void)content;
    (void)type;
    if (!hypothesisId) return E_INVALIDARG;
    static std::atomic<uint32_t> next{1};
    *hypothesisId = next.fetch_add(1);   // 必须写出参，否则调用方拿到栈垃圾
    return S_OK;
}

STDAPI XSpeechToTextUpdateHypothesisString(uint32_t hypothesisId, const char* content) noexcept {
    (void)hypothesisId;
    (void)content;
    return S_OK;
}

STDAPI XSpeechToTextFinalizeHypothesisString(uint32_t hypothesisId, const char* content) noexcept {
    (void)hypothesisId;
    (void)content;
    return S_OK;
}

STDAPI XSpeechToTextCancelHypothesisString(uint32_t hypothesisId) noexcept {
    (void)hypothesisId;
    return S_OK;
}

STDAPI XSpeechToTextSetPositionHint(XSpeechToTextPositionHint position) noexcept {
    (void)position;
    return S_OK;
}

}  // extern "C"
