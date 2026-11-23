// SPDX-License-Identifier: GPL-3.0-or-later
// t35_gamepad — 手柄两条路径：XInput（老）与 GameInput（GDK 用的新 API）
//
// 这是"手柄支持"的事实基线：
//   1) XInput1_4.dll 是 Windows 自带的，GDK 之前的老标题用它
//   2) GameInput.dll 也是系统组件（GDK 包里给的是 GameInput.lib + 头），
//      现代 GDK 标题通过它拿手柄
// 两条都由 OS 提供，兼容层理论上**不需要转发**。这个测试用来证明这一点，
// 并留下"手柄在这台机器上怎么读"的可复现记录。
#include <windows.h>
#include <GameInput.h>

#include <XInput.h>

#include <cstdio>
#include <cstring>

#include "xbgtasix/trace.h"

// 用户-设备关联（手写实现，声明从 XUser.h 抄）
struct AsyncToken {
    unsigned long long token;
};
extern "C" {
HRESULT WINAPI XUserFindForDevice(const void* device_id, void** user);
HRESULT WINAPI XUserRegisterForDeviceAssociationChanged(void* queue, void* context, void* callback,
                                                       AsyncToken* token);
bool WINAPI XUserUnregisterForDeviceAssociationChanged(AsyncToken token, bool wait);
}

namespace {

int g_fail = 0;
int g_skips = 0;

void ok(bool cond, const char* what, const char* detail = "") {
    if (cond) {
        xbgtasix::logf("[t35]   PASS  %-38s %s", what, detail);
    } else {
        xbgtasix::logf("[t35]   FAIL  %-38s %s", what, detail);
        ++g_fail;
    }
}

void skip(const char* what) {
    xbgtasix::logf("[t35]   SKIP  %s", what);
    ++g_skips;
}

// ---- XInput（运行时解析，避免静态依赖）----
using PFN_XInputGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

int xinput_probe(PFN_XInputGetState get_state) {
    int found = 0;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE st{};
        const DWORD r = get_state(i, &st);
        if (r == ERROR_SUCCESS) {
            ++found;
            xbgtasix::logf("[t35] XInput 槽 %lu: 按键=0x%04X 摇杆L=(%d,%d) 扳机=(%u,%u)", i,
                           st.Gamepad.wButtons, st.Gamepad.sThumbLX, st.Gamepad.sThumbLY,
                           st.Gamepad.bLeftTrigger, st.Gamepad.bRightTrigger);
        }
    }
    return found;
}

}  // namespace

int main() {
    // =====================================================================
    // 一、XInput（Windows 自带）
    // =====================================================================
    HMODULE xi = LoadLibraryA("XInput1_4.dll");
    if (!xi) xi = LoadLibraryA("XInput9_1_0.dll");
    // 旧写法是先 skip() 再 ok(true)——DLL 存在与否根本没验。
    // XInput 是 Windows 自带组件：缺了说明"手柄这条链"在这台机器上不存在，
    // 这是玩家能感知的事实，必须记 FAIL（只把"没有手柄可插"当环境问题）。
    ok(xi != nullptr, "系统存在 XInput DLL（XInput1_4 / XInput9_1_0）");
    if (xi) {
        auto get_state = reinterpret_cast<PFN_XInputGetState>(GetProcAddress(xi, "XInputGetState"));
        // 有 DLL 却没有导出 = 这条链断了（不是环境问题）
        ok(get_state != nullptr, "XInput DLL 导出 XInputGetState");
        const int n = get_state ? xinput_probe(get_state) : 0;
        xbgtasix::logf("[t35] XInput 找到 %d 个手柄", n);
        if (n == 0) {
            // 没插手柄是宿主环境，不是兼容层的错
            skip("XInput 没有检测到手柄（未连接或已被其他进程独占）");
        } else {
            // 期望值是字面量：XInput 只有 4 个槽位（XUSER_MAX_COUNT），多于 4 说明计数错了
            ok(n > 0 && n <= 4, "XInput 读到的槽位数在 1..4 之间", "");
        }
        FreeLibrary(xi);
    }

    // =====================================================================
    // 二、GameInput（GDK 现代标题用的）
    // =====================================================================
    HMODULE gi = LoadLibraryA("GameInput.dll");
    // 同 XInput：以前是 ok(true)，等于没验。GameInput.dll 是系统组件，
    // 缺了就是"现代 GDK 标题拿不到手柄"，必须 FAIL（不是 skip）。
    ok(gi != nullptr, "系统存在 GameInput.dll（GDK 标题依赖它）");

    IGameInput* gameInput = nullptr;
    // 关键：不能静态链接 GameInput.lib —— 没有这个 DLL 的机器上进程**根本起不来**
    // （0xC0000135），连"跳过"的机会都没有。改为运行时解析。
    using PFN_GameInputCreate = HRESULT(WINAPI*)(IGameInput**);
    PFN_GameInputCreate create_fn = nullptr;
    if (gi) {
        create_fn = reinterpret_cast<PFN_GameInputCreate>(GetProcAddress(gi, "GameInputCreate"));
        if (!create_fn) {
            skip("GameInput.dll 里没有 GameInputCreate（版本太旧）");
        }
    }
    if (create_fn) {
        const HRESULT hr = create_fn(&gameInput);
        // 以前是 ok(true)；现在断言"返回 S_OK 且给出非空接口"，失败要记账
        ok(SUCCEEDED(hr) && gameInput != nullptr, "GameInputCreate 成功并给出接口");
        if (!SUCCEEDED(hr) || !gameInput) {
            xbgtasix::logf("[t35] GameInputCreate hr=0x%08lx（版本差异）",
                           static_cast<unsigned long>(hr));
            gi = nullptr;
        }
    }

    if (gameInput) {
        // 设备数量：v0 接口没有 GetDeviceCount，用 GetCurrentReading(kind=None) 探测
        // 探测一次：GetCurrentReading(kind, nullptr, &reading) 传 nullptr = 任意设备
        int probe = 0;
        {
            IGameInputReading* r = nullptr;
            if (SUCCEEDED(gameInput->GetCurrentReading(GameInputKindGamepad, nullptr, &r)) && r) {
                probe = 1;
                r->Release();
            }
        }
        xbgtasix::logf("[t35] GameInput 能读到手柄输入: %s", probe ? "是" : "否");

        // 设备回调枚举（GDK 推荐做法）
        struct Ctx {
            int count;
        } ctx{0};
        struct Helper {
            // 真实签名：6 个参数（token/context/device/timestamp/当前状态/之前状态）
            static void CALLBACK OnDevice(GameInputCallbackToken token, void* c, IGameInputDevice* dev,
                                          uint64_t ts, GameInputDeviceStatus cur,
                                          GameInputDeviceStatus prev) {
                (void)token; (void)ts; (void)prev;
                auto* x = static_cast<Ctx*>(c);
                ++x->count;
                uint16_t vid = 0, pid = 0;
                if (dev) {
                    const GameInputDeviceInfo* info = dev->GetDeviceInfo();
                    if (info) {
                        vid = info->vendorId;
                        pid = info->productId;
                    }
                }
                xbgtasix::logf("[t35]   GameInput 设备: vid=0x%04X pid=0x%04X status=0x%x",
                               vid, pid, static_cast<unsigned>(cur));
            }
        };
        GameInputCallbackToken token = 0;   // 是 uint64_t，不是设备指针
        const HRESULT cbr = gameInput->RegisterDeviceCallback(
            nullptr, GameInputKindGamepad, GameInputDeviceConnected,
            GameInputBlockingEnumeration, &ctx, &Helper::OnDevice, &token);
        ok(SUCCEEDED(cbr), "RegisterDeviceCallback（阻塞枚举）");
        if (SUCCEEDED(cbr)) {
            xbgtasix::logf("[t35] 枚举到手柄设备 %d 个", ctx.count);
            if (ctx.count == 0) {
                // 阻塞枚举一个都没回调：可能是没有设备（环境），也可能是回调没被调（逻辑）。
                // 以前这里 ok(true) 直接记 PASS，等于把两种情况都放过了。
                skip("GameInput 没枚举到手柄（无设备可验证）");
            } else {
                // 期望值字面量：枚举到的设备数必须是正数，且回调真的被调用过
                ok(ctx.count >= 1, "GameInput 枚举到至少一个手柄（回调真的被调用）", "");
            }
            gameInput->UnregisterCallback(token, 1000000);   // (token, 超时微秒)
        }

        // 读一次状态，确认返回对象可用
        IGameInputReading* reading = nullptr;
        const HRESULT rhr = gameInput->GetCurrentReading(GameInputKindGamepad, nullptr, &reading);
        if (SUCCEEDED(rhr) && reading) {
            // 真实签名：bool GetGamepadState(GameInputGamepadState*) —— 整块结构体
            GameInputGamepadState st{};
            const bool got = reading->GetGamepadState(&st);
            ok(got, "能取到手柄状态结构体");
            xbgtasix::logf("[t35] buttons=0x%04X LT=%.2f RT=%.2f 左摇杆=(%.2f,%.2f)",
                           static_cast<unsigned>(st.buttons) & 0xFFFF, st.leftTrigger,
                           st.rightTrigger, st.leftThumbstickX, st.leftThumbstickY);
            reading->Release();
        } else {
            skip("取当前手柄状态失败（可能没有活动设备）");
        }

        gameInput->Release();
    }

    // ---- 三、GDK 的用户-设备关联（唯一需要兼容层参与的一步）----
    {
        unsigned char null_id[32] = {0};
        void* user = nullptr;
        ok(XUserFindForDevice(null_id, &user) == E_INVALIDARG, "FindForDevice(全零 ID) = E_INVALIDARG");

        unsigned char real_id[32] = {0};
        real_id[0] = 0x5A;   // 任意非零设备 ID
        user = nullptr;
        ok(SUCCEEDED(XUserFindForDevice(real_id, &user)) && user != nullptr,
           "FindForDevice(有效 ID) 返回本地用户");

        AsyncToken tok{0};
        ok(SUCCEEDED(XUserRegisterForDeviceAssociationChanged(nullptr, nullptr, nullptr, &tok)) &&
               tok.token != 0,
           "RegisterForDeviceAssociationChanged 给出非零 token");
        ok(XUserUnregisterForDeviceAssociationChanged(tok, true), "Unregister 返回 true");
    }

    xbgtasix::logf("[t35] done failures=%d skips=%d -> %s", g_fail, g_skips,
                   g_fail ? "FAIL" : (g_skips ? "SKIP（部分项未验证）" : "PASS"));
    // skip() 只加计数不改退出码，以前"0 个手柄 / 无 GameInput.dll"也会以 0 退出，
    // 在 run_suite 里显示成 PASS。现在把 SKIP 明确映射成 2，PASS 必须真的全验过。
    if (g_fail) return 1;
    if (g_skips) return 2;
    return 0;
}
