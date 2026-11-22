// t21_user — XUser 本地语义 + 能力门
//
// 覆盖 XUser 里"不需要网络也能有真语义"的部分，以及 XGameRuntimeIsFeatureAvailable。
// 重点在**返回值类型**：bool / int32_t / HRESULT 混在一起，写错不报错但分支会走错。
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "xbgtasix/trace.h"

struct Token {
    unsigned long long token;
};

struct LocalId {
    unsigned long long value;
};

// 数据导出回归：.def 少了 DATA 修饰时，客户端读到的是函数跳转指令字节
extern "C" const GUID _GUID_014d1cc3_bcfe_41ff_b2f0_e1ef07155828;

extern "C" {
HRESULT WINAPI XGameRuntimeInitialize(void);
HRESULT WINAPI XUserAddAsync(int options, void* async_block);
HRESULT WINAPI XUserAddResult(void* async_block, void** user);
HRESULT WINAPI XUserGetId(void* user, uint64_t* id);
void WINAPI XUserCloseHandle(void* user);
HRESULT WINAPI XUserGetMaxUsers(uint32_t* max_users);
long WINAPI XUserCompare(void* a, void* b);
HRESULT WINAPI XUserDuplicateHandle(void* user, void** dup);
bool WINAPI XUserIsStoreUser(void* user);
bool WINAPI XUserIsSignOutPresent(void);
HRESULT WINAPI XUserGetAgeGroup(void* user, int* age_group);
HRESULT WINAPI XUserCheckPrivilege(void* user, int options, int privilege, bool* has, void* reason);
HRESULT WINAPI XUserFindUserByLocalId(LocalId local_id, void** user);
HRESULT WINAPI XUserGetGamertag(void* user, int component, size_t size, char* tag, size_t* used);
HRESULT WINAPI XUserGetLocalId(void* user, LocalId* local_id);
HRESULT WINAPI XUserGetIsGuest(void* user, bool* is_guest);
HRESULT WINAPI XUserGetState(void* user, int* state);
HRESULT WINAPI XUserSignOutAsync(void* user, void* async_block);
HRESULT WINAPI XUserSignOutResult(void* async_block);
bool WINAPI XGameRuntimeIsFeatureAvailable(int feature);

// UI：主机弹窗降级为"立即完成"
HRESULT WINAPI XGameUiShowMessageDialogAsync(void* async_block, const char* title, const char* content,
                                             const char* b1, const char* b2, const char* b3,
                                             unsigned default_button, unsigned cancel_button);
HRESULT WINAPI XGameUiShowMessageDialogResult(void* async_block, unsigned* result_button);
HRESULT WINAPI XGameUiShowTextEntryAsync(void* async_block, const char* title, const char* desc,
                                         const char* default_text, unsigned scope, unsigned max_len);
HRESULT WINAPI XGameUiShowTextEntryResultSize(void* async_block, unsigned* size);
HRESULT WINAPI XGameUiShowTextEntryResult(void* async_block, unsigned size, char* text, unsigned* used);
HRESULT WINAPI XGameUiShowWebAuthenticationAsync(void* async_block, void* user, const char* request_uri,
                                                 const char* completion_uri);
HRESULT WINAPI XGameUiShowWebAuthenticationResultSize(void* async_block, size_t* size);
HRESULT WINAPI XGameUiShowWebAuthenticationResult(void* async_block, size_t buffer_size, void* buffer,
                                                  void** ptr_to_buffer, size_t* buffer_used);
HRESULT WINAPI XAsyncGetStatus(void* async_block, bool wait);
HRESULT WINAPI XGameUiSetMessageDialogUiResponse(void* handle, unsigned response);
HRESULT WINAPI XGameUiSetTextEntryUiResponse(void* handle, const char* response);
HRESULT WINAPI XGameUiSetErrorDialogUiResponse(void* handle);
HRESULT WINAPI XGameUiSetUiCallbacks(const void* callbacks, bool use_system_ui);
}

namespace {

struct AsyncBlock {
    void* context;
    void* queue;
    void (*callback)(AsyncBlock*);
    unsigned internal[sizeof(void*) * 4 / sizeof(unsigned)];
};

int g_fail = 0;

void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t21]   PASS  %-38s %s", what, detail);
    } else {
        xbgtasix::logf("[t21]   FAIL  %-38s %s", what, detail);
        ++g_fail;
    }
}

}  // namespace

int main() {
    ok(SUCCEEDED(XGameRuntimeInitialize()), "XGameRuntimeInitialize");

    // ---- 能力门：不能全 true，也不能全 false ----
    // 2=XAsync 11=XPackage 12=XPersistentLocalStorage 16=XTaskQueue 18=XUser 18? 见枚举
    ok(XGameRuntimeIsFeatureAvailable(2), "IsFeatureAvailable(XAsync)=true");
    ok(XGameRuntimeIsFeatureAvailable(11), "IsFeatureAvailable(XPackage)=true");
    ok(XGameRuntimeIsFeatureAvailable(16), "IsFeatureAvailable(XTaskQueue)=true");
    ok(XGameRuntimeIsFeatureAvailable(18), "IsFeatureAvailable(XUser)=true");
    ok(!XGameRuntimeIsFeatureAvailable(1), "IsFeatureAvailable(XAppCapture)=false");
    ok(!XGameRuntimeIsFeatureAvailable(13), "IsFeatureAvailable(XSpeechSynthesizer)=false");

    // ---- 用户槽位与句柄 ----
    uint32_t maxUsers = 0;
    ok(SUCCEEDED(XUserGetMaxUsers(&maxUsers)) && maxUsers == 8, "GetMaxUsers=8");

    AsyncBlock block{};
    void* u1 = nullptr;
    ok(SUCCEEDED(XUserAddAsync(0, &block)) && SUCCEEDED(XUserAddResult(&block, &u1)) && u1,
       "AddAsync/AddResult");

    void* u2 = nullptr;
    AsyncBlock b2{};
    ok(SUCCEEDED(XUserAddAsync(0, &b2)) && SUCCEEDED(XUserAddResult(&b2, &u2)), "第二个用户");

    ok(XUserCompare(u1, u1) == 0, "Compare(自己,自己)=0");
    ok(XUserCompare(u1, u2) != 0, "Compare(不同用户)!=0");
    ok(XUserCompare(nullptr, u1) < 0, "Compare(nullptr,u1)<0");

    // ---- 查询 ----
    char tag[64] = {0};
    size_t used = 0;
    ok(SUCCEEDED(XUserGetGamertag(u1, 0, sizeof(tag), tag, &used)) && std::strlen(tag) > 0,
       "GetGamertag", tag);
    char small[3] = {0};
    ok(XUserGetGamertag(u1, 0, sizeof(small), small, &used) == E_NOT_SUFFICIENT_BUFFER,
       "小缓冲区报 E_NOT_SUFFICIENT_BUFFER");

    LocalId lid{0};
    ok(SUCCEEDED(XUserGetLocalId(u1, &lid)) && lid.value != 0, "GetLocalId");
    bool guest = true;
    ok(SUCCEEDED(XUserGetIsGuest(u1, &guest)) && !guest, "GetIsGuest=false");
    int state = -1;
    ok(SUCCEEDED(XUserGetState(u1, &state)) && state == 0, "GetState=SignedIn(0)");
    int age = -1;
    ok(SUCCEEDED(XUserGetAgeGroup(u1, &age)) && age == 3, "GetAgeGroup=Adult(3)");
    bool priv = false;
    ok(SUCCEEDED(XUserCheckPrivilege(u1, 0, 185, &priv, nullptr)) && priv, "CheckPrivilege=true");
    ok(XUserIsStoreUser(u1), "IsStoreUser=true");
    ok(!XUserIsSignOutPresent(), "IsSignOutPresent=false");

    // ---- 按 LocalId 找回用户 ----
    void* u3 = nullptr;
    ok(SUCCEEDED(XUserFindUserByLocalId(lid, &u3)) && u3 && XUserCompare(u3, u1) == 0,
       "FindUserByLocalId 找回同一个 id");
    XUserCloseHandle(u3);

    // ---- 引用计数：DuplicateHandle 后关一份，原句柄仍可用 ----
    void* dup = nullptr;
    ok(SUCCEEDED(XUserDuplicateHandle(u1, &dup)) && dup == u1, "DuplicateHandle");
    XUserCloseHandle(dup);
    uint64_t id1 = 0;
    ok(SUCCEEDED(XUserGetId(u1, &id1)) && id1 != 0, "关掉重复句柄后原句柄仍可用");

    // ---- 登出 ----
    AsyncBlock so{};
    ok(SUCCEEDED(XUserSignOutAsync(u1, &so)), "SignOutAsync");
    ok(XUserSignOutResult(&so) == S_OK, "SignOutResult=S_OK");

    // ---- 数据导出：GUID 必须是真值，不能是代码字节 ----
    {
        const GUID& g = _GUID_014d1cc3_bcfe_41ff_b2f0_e1ef07155828;
        char buf[64];
        _snprintf(buf, sizeof(buf), "%08lX-%04X-%04X", (unsigned long)g.Data1, g.Data2, g.Data3);
        xbgtasix::logf("[t21] GUID = %s", buf);
        ok(g.Data1 == 0x014d1cc3 && g.Data2 == 0xbcfe && g.Data3 == 0x41ff,
           "GUID 导出是真数据（不是函数跳转桩）", buf);
        ok(g.Data4[0] == 0xb2 && g.Data4[7] == 0x28, "GUID 的 Data4 正确");
    }

    // ---- UI：必须立即完成，且结果能取回（关键：不能挂在 E_PENDING）----
    {
        AsyncBlock dlgt{};
        ok(SUCCEEDED(XGameUiShowMessageDialogAsync(&dlgt, "标题", "内容", "确定", nullptr, nullptr,
                                                   1, 1)),
           "ShowMessageDialogAsync");
        ok(XAsyncGetStatus(&dlgt, false) == S_OK, "消息框立即完成（不是 E_PENDING）");
        unsigned btn = 9;
        ok(SUCCEEDED(XGameUiShowMessageDialogResult(&dlgt, &btn)) && btn == 1,
           "MessageDialogResult 返回默认按钮");

        AsyncBlock te{};
        ok(SUCCEEDED(XGameUiShowTextEntryAsync(&te, "名字", "请输入", "默认名", 0, 32)),
           "ShowTextEntryAsync");
        unsigned size = 0;
        ok(SUCCEEDED(XGameUiShowTextEntryResultSize(&te, &size)), "TextEntryResultSize");
        std::vector<char> buf(size > 0 ? size : 1, 0);
        unsigned used = 0;
        ok(SUCCEEDED(XGameUiShowTextEntryResult(&te, size, buf.data(), &used)) &&
               std::strcmp(buf.data(), "默认名") == 0,
           "TextEntryResult 回填默认文本", buf.data());

        AsyncBlock wa{};
        ok(SUCCEEDED(XGameUiShowWebAuthenticationAsync(&wa, u1, "https://example/x", "app://done")),
           "ShowWebAuthenticationAsync");
        size_t wsize = 0;
        const size_t expect = 24 + std::strlen("app://done") + 1;   // 结构体 + URI + NUL
        ok(SUCCEEDED(XGameUiShowWebAuthenticationResultSize(&wa, &wsize)) && wsize == expect,
           "WebAuth ResultSize 精确等于 24+URI+NUL");
        ok(XAsyncGetStatus(&wa, false) == S_OK, "WebAuth 立即完成");

        std::vector<unsigned char> wbuf(wsize > 0 ? wsize : 1, 0);
        void* pin = nullptr;
        size_t wused = 0;
        ok(SUCCEEDED(XGameUiShowWebAuthenticationResult(&wa, wsize, wbuf.data(), &pin, &wused)) &&
               pin == wbuf.data() && wused == expect,
           "WebAuth Result：ptrToBuffer 指进 caller buffer");
        if (pin) {
            // 结构体首 4 字节是 HRESULT，后面是 URI 指针
            const auto* uri = *reinterpret_cast<const char* const*>(
                reinterpret_cast<const unsigned char*>(pin) + 16);
            ok(uri && std::strcmp(uri, "app://done") == 0, "WebAuth 回填的 URI 正确");
        }

        // UI 回调注入与本实现兼容：不接管 UI，但调用链不能崩
        ok(SUCCEEDED(XGameUiSetUiCallbacks(nullptr, true)), "SetUiCallbacks(nullptr)");
        ok(SUCCEEDED(XGameUiSetMessageDialogUiResponse(nullptr, 0)), "SetMessageDialogUiResponse");
        ok(SUCCEEDED(XGameUiSetTextEntryUiResponse(nullptr, "x")), "SetTextEntryUiResponse");
        ok(SUCCEEDED(XGameUiSetErrorDialogUiResponse(nullptr)), "SetErrorDialogUiResponse");
    }

    XUserCloseHandle(u2);
    XUserCloseHandle(u1);
    xbgtasix::logf("[t21] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
