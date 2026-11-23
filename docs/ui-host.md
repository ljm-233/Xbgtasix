# UI 与"Xbox 风格窗口"：两种模式，可切换

## 主机上的分工

| 层 | 谁画 | 例子 |
|---|---|---|
| 系统 shell | 主机 OS | Guide 面板、成就弹窗、切换用户、错误提示 |
| 游戏 | 游戏自己 | 主菜单、HUD、暂停菜单 |

游戏通过 `XGameUi*` **请求** shell 弹面板，shell 显示后回填结果。
PC 上没有这个 shell，所以兼容层有两种做法，现在**两种都实现了**：

| 模式 | 开关 | 行为 |
|---|---|---|
| `immediate`（默认） | 不设置 / `XBGTASIX_UI=immediate` | 立刻完成，结果是默认值。保证没人接管时游戏不卡住 |
| `callback` | `XBGTASIX_UI=callback` | 面板交给注册过 `XGameUiSetUiCallbacks` 的宿主；异步**保持挂起**，等宿主回填 |

切换是运行时环境变量，同一份 DLL 两种都能跑——因为"有没有宿主"是部署时的属性，
不是编译时的。

## callback 模式的完整链路

```
游戏 XGameUiShowMessageDialogAsync(async, "退出?", "进度会丢", "继续","退出", …)
   ↓ shim 存下 async、登记句柄
   ↓ 调宿主回调 showMessageDialogCallback(handle, queue, title, content, b1,b2,b3, …)
宿主（我们的窗口）显示对话框，用户点"退出"
   ↓ XGameUiSetMessageDialogUiResponse(handle, Second)
shim 回填按钮 = Second → complete_async → 游戏 XAsyncGetStatus(wait) 立刻返回 S_OK
   ↓ XGameUiShowMessageDialogResult(async, &btn)  → btn == Second（用户选的，不是默认值）
```

已接管的三个面板：**消息框、错误框、文本输入**（有对应回调的那三个）。
成就/玩家选择/邀请/档案卡没有宿主实现，仍走 `E_NOTIMPL`（已完成的异步，不会挂）。

## 实测（`t36_ui_host`）

```
PASS  immediate: 立刻完成（不是 E_PENDING）
PASS  immediate: 结果是默认按钮 1
PASS  callback: 宿主回调被调用 1 次
PASS  callback: 宿主拿到了标题              退出游戏?
PASS  callback: 异步**挂起**（这就是接管成立）
PASS  callback: 宿主回填按钮 2
PASS  callback: 结果 = 宿主选的 2（不是默认值）
PASS  callback: 结果是宿主输入的文本        玩家自己输入的名字
PASS  注销后回到立即完成
```

**最关键的一条是"异步挂起"**：如果 Async 在调回调之前就完成了，
说明回调根本没被用上，"接管 UI"只是形式主义。

## 系统侧 UI（真的开窗）

`XBGTASIX_UI=system`（或 `auto`：有回调交给游戏，没有就我们画）时，
**游戏没注册回调也没关系——我们自己开窗画面板**，而不是把默认值当答案。

实现组合（都已在别处验证过）：

| 部分 | 用什么 | 依据 |
|---|---|---|
| 窗口 + 背景 | Win32 窗口 + D3D12 **flip 交换链**画一帧底色 | `t31`/`t32` 验过的路径 |
| 文字与按钮 | 原生 `STATIC` / `EDIT` / `BUTTON` 子控件 | 它们自己处理字形与输入，省掉字体渲染 |
| 同步 | 面板跑在后台线程，跑消息循环；点完回填并完成 async | 不阻塞调用方 |
| 等待 | 默认 60s 超时，超时按"取消"处理 | 避免无人值守时卡死 |

**实测**（`t37_system_ui`，14 条断言）：

```
PASS  提交后异步挂起（证明真的交给窗口了）     ← 关键：不是"假装画了"
PASS  结果是脚本化的'取消'(1)，不是默认值 0
PASS  输入框也挂起
PASS  结果 = 脚本化文本                       玩家输入的内容
PASS  immediate 模式仍然立刻完成
```

自动化用的是 `XBGTASIX_UI_AUTORESPONSE`（`first`/`second`/`third` 或任意文本），
这样没有人点鼠标也能验端到端。

### 踩到的两个坑

1. **参数只在回调分支里拷贝**：`system` 模式下标题/内容/按钮文字全是空串。
   改成"无条件先拷参数，再按模式分派"。
2. **`SetEnvironmentVariableA` 对 UTF-8 中文返回 0（失败）**：
   于是中文回答根本传不进来，读到的还是上次的 `second`。
   改为读 `GetEnvironmentVariableW` + `WideCharToMultiByte(CP_UTF8)`。
   结论：**这条链路上的字符串必须全程 UTF-8**，不能用 ANSI API 中转。

### 还没做的

| 项 | 说明 |
|---|---|
| 字体渲染 | 现在靠原生子控件显示文字；要换成自绘需要字体图集与排版 |
| 视觉风格 | 现在是"功能可见"，不是 Xbox 外观 |
| 覆盖层合成 | 还没用 `CreateSwapChainForComposition`；目前是独立窗口 |

## 关于"嵌入宿主窗口"（暂不做）

把游戏窗口 `SetParent` 成我们窗口的子窗口，观感最像 Xbox 应用，但代价高：

- 游戏用 flip-model 交换链（`t31` 验过），重设父窗口会让它失效、`Present` 报错
- DPI 要按新父窗口重算，输入焦点要重定向
- 一旦出问题，表现出来是"黑屏"，而根因在窗口层级——极难查

**当前建议**：宿主作为独立进程/独立窗口（Launcher + 覆盖层），不改游戏窗口的父子关系。

## 已知限制

| 限制 | 原因 |
|---|---|
| 只在 `XBGTASIX_UI=callback` 下接管 | 默认必须保证"无宿主也能跑" |
| 回调在发起线程同步调用 | 宿主若要做异步 UI，需自己切线程再回填 |
| 没有 WebAuth 回调 | `XGameUiUiCallbacks` 结构体里就没有这一项，`ShowWebAuthentication*` 仍走"视为已通过" |
| 成就/玩家选择等面板未接管 | 宿主没实现，保持 `E_NOTIMPL` |
| 没有 Xbox 外观 | 现在是"功能接管"，不是"视觉还原" |
