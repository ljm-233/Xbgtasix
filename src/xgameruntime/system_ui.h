// SPDX-License-Identifier: GPL-3.0-or-later
// system_ui.h —— 系统侧 UI（游戏没注册回调时，由我们画）
//
// 两个角色要分清（见 docs/ui-references.md）：
//   1) 标题自绘：游戏调 XGameUiSetUiCallbacks 注册处理器，自己画面板
//   2) 系统侧绘制：没人注册时由"系统"画 —— 就是这里
//
// 实现策略：Win32 窗口 + D3D12 flip 交换链画背景，
// 文字与按钮用原生子控件（STATIC/EDIT/BUTTON）——这样不需要字体渲染，
// 而窗口与交换链这条路径已经被 t31/t32 验证过。
#pragma once

#include <cstdint>
#include <string>

namespace xbgtasix {

enum class SysUiMessageButton : int { First = 0, Second = 1, Third = 2, None = -1 };

// 有没有可能真的显示 UI（有交互桌面 + 能建设备）。
bool system_ui_available();

// 显示消息框，阻塞直到用户点按钮或超时。返回被点的按钮（First/Second/Third），
// 失败或超时返回 None。
SysUiMessageButton system_ui_show_message(const char* title, const char* content,
                                          const char* b1, const char* b2, const char* b3,
                                          int default_button, int cancel_button,
                                          uint32_t timeout_ms);

// 显示输入框。成功时写 *out，返回 true。
bool system_ui_show_text_entry(const char* title, const char* description, const char* default_text,
                               uint32_t max_len, std::string* out, uint32_t timeout_ms);

}  // namespace xbgtasix
