// SPDX-License-Identifier: GPL-3.0-or-later
// input_extra.cpp —— 手柄与用户的关联（GDK 里唯一需要兼容层参与的一步）
//
// 手柄读取本身在 PC 上是原生能力，不需要我们转译：
//   - XInput1_4.dll / XInput9_1_0.dll  Windows 自带
//   - GameInput.dll                    Windows 自带，GDK 的 GameInput.lib 直接链它
//     （t35_gamepad 实测：GameInputCreate 成功，枚举到 vid=0x045E pid=0x028E）
//
// 但 GDK 还有"哪个手柄属于哪个用户"这一层：
//   XUserFindForDevice / XUserRegisterForDeviceAssociationChanged
// 主机上这由系统维护；PC 上我们只有一个本地用户，所以语义是：
//   - 任何非空设备 ID 都映射到该本地用户
//   - 关联变化事件永不触发（用户不会切），但注册必须给出 token、注销必须返回 true
#include <windows.h>

#include "XUser.h"

#include "xbgtasix/trace.h"
#include "trace_entry.h"   // 统一入口追踪（XB_TRACE_ENTRY）

namespace {

// 单本地用户：所有设备都指向它
uint64_t g_local_user_id = 1;

bool device_id_is_null(const APP_LOCAL_DEVICE_ID* id) {
    if (!id) return true;
    for (size_t i = 0; i < APP_LOCAL_DEVICE_ID_SIZE; ++i) {
        if (id->value[i] != 0) return false;
    }
    return true;
}

}  // namespace

extern "C" {

// 由设备 ID 找用户。PC 上只有本地用户：设备 ID 有效就返回它。
STDAPI XUserFindForDevice(const APP_LOCAL_DEVICE_ID* deviceId, XUserHandle* handle) noexcept {
    XB_TRACE_ENTRY("XUserFindForDevice", deviceId, handle, 0ULL, 0ULL);
    if (!handle) return E_INVALIDARG;
    *handle = nullptr;
    // 全零 ID 是 XUserNullDeviceId，按"没有这台设备"处理
    if (device_id_is_null(deviceId)) {
        xbgtasix::logf("[xinput] XUserFindForDevice(空设备 ID) -> E_INVALIDARG");
        return E_INVALIDARG;
    }
    *handle = reinterpret_cast<XUserHandle>(g_local_user_id);
    xbgtasix::logf("[xinput] XUserFindForDevice(设备) -> 本地用户 %llu",
                   static_cast<unsigned long long>(g_local_user_id));
    return S_OK;
}

// 关联变化：注册表 + token；PC 上事件永不触发（用户不会切换）
static uint64_t g_assoc_token_next = 1;

STDAPI XUserRegisterForDeviceAssociationChanged(XTaskQueueHandle queue, void* context,
                                                XUserDeviceAssociationChangedCallback* callback,
                                                XTaskQueueRegistrationToken* token) noexcept {
    XB_TRACE_ENTRY("XUserRegisterForDeviceAssociationChanged", queue, context, callback, token);
    (void)queue;
    (void)context;
    (void)callback;
    if (!token) return E_INVALIDARG;
    // 出参必须先写：调用方失败路径上若拿到栈垃圾会去 Unregister 一个假 token
    token->token = g_assoc_token_next++;
    xbgtasix::logf("[xinput] RegisterForDeviceAssociationChanged -> token=%llu（PC 上不触发事件）",
                   static_cast<unsigned long long>(token->token));
    return S_OK;
}

STDAPI_(bool) XUserUnregisterForDeviceAssociationChanged(XTaskQueueRegistrationToken token,
                                                         bool wait) noexcept {
    XB_TRACE_ENTRY("XUserUnregisterForDeviceAssociationChanged", token, wait, 0ULL, 0ULL);
    (void)token;
    (void)wait;
    return true;   // 与仓内其他 Unregister 一致：注销未知 token 也返回 true
}

}  // extern "C"
