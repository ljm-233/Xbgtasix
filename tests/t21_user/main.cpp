// t21_user — XUser 本地语义 + 能力门
//
// 覆盖 XUser 里"不需要网络也能有真语义"的部分，以及 XGameRuntimeIsFeatureAvailable。
// 重点在**返回值类型**：bool / int32_t / HRESULT 混在一起，写错不报错但分支会走错。
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "xbgtasix/trace.h"

struct Token {
    unsigned long long token;
};

struct LocalId {
    unsigned long long value;
};

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

    XUserCloseHandle(u2);
    XUserCloseHandle(u1);
    xbgtasix::logf("[t21] done failures=%d -> %s", g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
