# Xbox UI：现成方案调研与我们的选择

调研时间 2026-11-23。所有链接为外部项目，**许可各不相同，抄代码前先看许可**。

## 先修正一个概念（很重要）

`XGameUiSetUiCallbacks` 的官方说明是：

> 「指向包含**开发人员想要处理的**每个 UI 的处理程序和上下文的结构的指针」
> `useSystemUiIfAvailable`：「是否应使用**系统 UI** 而不是开发人员回调（如果可用）」

也就是说存在**两个角色**，方向相反：

| 角色 | 谁 | 什么时候 | 我们的现状 |
|---|---|---|---|
| 标题自绘 | 游戏注册回调，自己画面板 | 游戏想控制外观（多数大作用这个） | ✅ `XBGTASIX_UI=callback` 模式下已支持（`t36` 验证） |
| 系统侧绘制 | 没注册回调时，由"系统"画 | 系统提供兜底面板 | ✅ 已实现（`t37_system_ui`） |

所以"做一个 Xbox UI 窗口"这件事，正确的定位是**补上第二个角色**：
当游戏没注册回调时，我们作为"系统"真的画一个面板，而不是把默认值当答案 ——
这一角色已由 `t37_system_ui` 落地。

## 四条现成路线

### 1. Xbox Game Bar 小组件（官方，但约束大）

微软官方的 PC 端 Xbox 覆盖层扩展机制，2020 年起对外开放 SDK 与小组件商店。

- 链接：[Game Bar SDK 入门](https://learn.microsoft.com/en-us/gaming/game-bar/quickstart/introduction)
- 关键限制（官方原文）：*"If it's not a UWP XAML app and you want to take advantage of the SDK you will need to create a new app for your overlay"*
- 代价：**必须是 UWP XAML 应用**，要打包身份；我们的东西是 C++/D3D12 的 DLL，跨进程通信才能接上
- 适合：将来真的想进"官方覆盖层生态"；**不适合**当前阶段

### 2. Theseus / UIX Desktop（反向工程 Xbox 仪表盘）

把 Xbox 5960 dashboard 的引擎反向工程出来，能编到 PC（还有 macOS/Linux/Steam Deck）。

- 链接：[MrMilenko/Theseus](https://github.com/MrMilenko/Theseus)、[UIX-History 的逆向说明](https://github.com/MrMilenko/UIX-History/blob/main/docs/reverse-engineering.md)
- 许可：**GPL-3.0-or-later**（`theseus/toolbox/` 部分还继承 GPL-3.0）
- 对我们的价值：**视觉与交互的参考标本**——面板层级、动画、Guide 键行为
- 此前：不能抄进我们（当时是 MIT）的仓库。**现在本项目也是 GPL-3.0，许可上已兼容**，但仍建议只借鉴结构、独立实现，避免把 GPL 代码与本项目自有实现搅在一起难以追溯

### 3. recomp-ui（静态重编译项目共用的启动器 UI）

为静态重编译项目提供的通用启动器 + 游戏内设置界面，Dear ImGui 核心，**schema 驱动**
（feature 列表由宿主提供，UI 自己长出来）。

- 链接：[TekRantGaming/recomp-ui](https://github.com/TekRantGaming/recomp-ui)、[RetroPortingToolKit/recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui)
- 许可：MIT（Copyright 2026 Matthew Stanley）—— 与 GPL-3.0 单向兼容（可引入，需保留其署名）
- 对我们的价值：**结构范本**。它的分层正是我们要的：
  启动器（选游戏/配置/存档/手柄映射）+ 游戏内覆盖层（设置面板）
- 注意：它不含 Xbox 外观，视觉要自己做

### 4. DirectComposition 独立覆盖窗（技术方案，最贴合我们）

有项目把"游戏内覆盖层"做成**完全独立、不碰游戏交换链**的窗口：
`CreateSwapChainForComposition`（`FLIP_SEQUENTIAL` + 预乘 alpha）+ 私有 D3D 设备 + ImGui，
覆盖在游戏窗口上方。

- 参考：[pbz-framework-menu 的架构文档](https://github.com/lin-414/pbz-framework-menu/blob/8740c954c5457166feb06c5e72d282edf4a65430/docs/ARCHITECTURE.md)（DirectComposition 独立覆盖窗 + 自测方式）
- 为什么最贴合我们：**这正是我已经验证过的能力**——
  `t31_swapchain` 证明了我们能在自己的窗口里建 flip 交换链并 Present，
  `t32_window` 证明了消息循环与 `ResizeBuffers` 都正常
- 另一种做法是 hook 游戏的渲染管线把 UI 混进画面（[示例文章](https://gitcode.csdn.net/69f9d65e54b52172bc71e609.html)），
  但那要改别人的渲染流程，风险高；我们是"系统侧"，没有理由去改游戏的帧

### 5. 官方 GDK 示例

- [microsoft/Xbox-GDK-Samples](https://github.com/microsoft/Xbox-GDK-Samples)（当前 GDK 的官方示例集合）
- 老的 [Xbox-ATG-Samples](https://github.com/microsoft/Xbox-ATG-Samples) 已标注"给旧 XDK 用"
- 价值：`XGameUi*` 的正确调用序列、`useSystemUiIfAvailable` 的实际用法

## 对比表

| 方案 | 许可 | 能直接给我们的 | 代价 | 现在做? |
|---|---|---|---|---|
| Game Bar 小组件 | 官方 SDK | 进官方覆盖层 | UWP/XAML + 跨进程 | ❌ |
| Theseus/UIX | GPL-3.0 | **视觉参考** | 不能抄代码 | 参考 |
| recomp-ui | **MIT** | **结构范本**（启动器+覆盖层） | 无 Xbox 外观 | ✅ 借鉴结构 |
| DirectComposition 覆盖窗 | 技术方案 | 实现路径 | 需要自己写 | ✅ 选它 |
| GDK 示例 | 微软示例许可 | 调用序列 | — | ✅ 参考 |

## 我们的选择

**在进程内画**（而不是 IPC 到外部进程），理由是：`XGameUiSetUiCallbacks` 和 `Show*Async`
都在游戏进程里，shim 就在那里——跨进程要把请求序列化出去再等回复，多一层故障点。

技术组合：

| 层 | 用什么 | 依据 |
|---|---|---|
| 窗口 + 交换链 | 已有能力：`t31`/`t32` 验证过 flip 交换链、`ResizeBuffers`、Present | 现成 |
| 覆盖层合成 | `CreateSwapChainForComposition` + 预乘 alpha（独立覆盖窗，不碰游戏交换链） | 参考 pbz-framework-menu |
| UI 控件 | Dear ImGui（MIT）| 生态成熟、D3D12 后端现成 |
| 面板结构 | schema 驱动（借鉴 recomp-ui 的思路，不复制代码） | MIT，仅参考结构 |
| 视觉 | 参考 Xbox dashboard 的层级与交互，**自己画** | Theseus 只作标本（GPL） |

## 分步落地（每步都能单独验证）

| 步 | 内容 | 验证方式 |
|---|---|---|
| 1 | shim 提供一个"系统侧 UI 请求"回调点（游戏没注册回调时走我们） | 单测：不注册回调 + 面板请求 → 我们的实现被调用 |
| 2 | 进程内开一个独立窗口 + flip 交换链，画一个纯色面板 | 复用 `t31` 的代码路径 |
| 3 | 接 ImGui，画三个面板（消息/错误/输入），点按回填 `Set*UiResponse` | 端到端：Async 挂起 → 点击 → 完成 → 结果=点击值 |
| 4 | 视觉打磨（Xbox 风格） | 截图对比 |

第 1、2 步都不需要 exe 就能做完。

## 已经做出来的样子

`t38_xboxui` 渲染出的第一版界面（自绘，无第三方 UI 库）：

> **效果图不入库**：`t38_xboxui` 会把自绘的 shell 窗口渲染成一帧并落盘到
> `build/suite/xboxui.bmp`（`tools/run_suite.sh` 里已经这么调了）。
> 需要看图时本地跑一次套件即可，仓库里不保留二进制图片 —— 这是合规护栏
> （[asset-policy.md](asset-policy.md)）的要求，例外表因此保持为空。

包含：顶部品牌条 + 绿色分隔线、左侧四项导航（选中态高亮）、右侧三张卡片、
底部操作提示。全部由「色块 + GDI 文字图集」两种图元组成。

技术选择（都是为了避开已知坑）：

| 决定 | 原因 |
|---|---|
| 顶点走 `StructuredBuffer` + `SV_VertexID` | 项目里所有验证过的着色器都没用过 D3D12 **输入布局**；缺它 PSO 会 E_INVALIDARG |
| 坐标在 CPU 算成 NDC | 不需要 cbuffer，少一个绑定点 |
| 文字用 GDI 渲染进图集 | 不用做字形与排版；原生 `DrawText` 支持中文 |
| 图集底色为黑 | 之后按亮度算 alpha；底色是白会导致整片文字变成实心块 |
| 纯色块采样"不透明白像素" | 图集 alpha 全 0 时什么都画不出来（第一版截图全黑） |

### 渲染链路

```
GDI 画文字 -> 1024x1024 位图（按亮度转 alpha）-> 上传成纹理图集
D3D12：SV_VertexID 取 StructuredBuffer 顶点 -> 采样图集 * 顶点色
先渲到离屏 RT -> CopyTextureRegion 读回 -> BMP 截图
```

`--shot <file.bmp>` 不需要人操作就能出图，所以它能进自动化测试（`run_suite` 里会生成
`build/suite/xboxui.bmp`）。

### 这一轮踩的四个坑（都记进了图形坑清单）

1. **PSO E_INVALIDARG**：没给 `InputLayout`（VS 吃 POSITION/TEXCOORD/COLOR）
2. **静态采样器 `ComparisonFunc = 0`** 不是合法枚举，同样让 PSO 创建失败
3. **图集 alpha 全 0** → 截图全黑（纯色块也是采样图集）
4. **纹理→缓冲用 `CopyResource`** 是非法操作，D3D12 静默忽略 → 读回永远黑

## 一句话

**没有现成的"Xbox UI 中间件"可以直接拿**：官方的绑 UWP，最像的（Theseus）是 GPL，
最实用的（recomp-ui）是启动器不是面板。所以合理做法是
**用 MIT 的 ImGui 自己画 + 借鉴 MIT 项目的结构 + 参考 GPL 项目的视觉**（本项目 2026-11 起改用 GPL-3.0，许可上与 GPL 参考项目已兼容）。
