# AnyPS5 架构笔记 → 对 Xbox 静态移植的可用结论

> 来源：公开报道与项目文档（korben 报道、anyps5.org 官方指南、README 摘要、PR/issue 讨论）。
> 本机无法直连 GitHub，以下为二手整理，版本以 v0.1.5（2026-11）为参照。

## 1. 它到底是什么

不是模拟器，是 **"可执行文件重链接 + 系统库重实现 + 图形翻译"** 三件事的组合：

| 层 | 组件 | 干什么 |
|---|---|---|
| 静态转换 | `core/relinker` | 读 PS5 ELF，重建为目标平台原生格式（Linux ELF / Windows PE），把导入绑定到宿主库，输出 `app.elf` / `app.exe` |
| 系统库 | `core/libs/prx` | 用 C++ 重写 libkernel、libc、libSce 家族，编译成 `.prx`（宿主上就是 DLL/SO），供动态链接 |
| 图形提交 | `libSceAgcDriver` | 接住 AGC 的 DCB/ACB 命令缓冲，解码 PM4 包（`Execution/src/Pm4.cpp`），转成 Vulkan 调用 |
| 着色器 | `Recompiler.cpp`（RdnaDecoder + 控制流重建）+ ShaderDiskCache | RDNA 指令流 → SPIR-V，运行时按需编译并落盘缓存 |

关键点：**CPU 代码不翻译**。PS5 是 x86-64，PC 也是 x86-64，所以原机器码原样跑，
只换掉"系统调用"和"显卡驱动"两圈边界。进程就是普通进程，没有独立 runtime。

## 2. 它的成绩单（重要，用来校准预期）

- PRX 格式系统库就绪度：89.65%
- GPU 着色器指令覆盖：宣称 100%
- 官方兼容列表：**1 个游戏**（Dreaming Sarah，2015 年的 2D 独立平台游戏），
  GTX 1050 Ti 跑 60fps，Intel HD 620 跑 36fps

社区自己给出的定性："这些百分比既不是'模拟了 PS5 的百分之多少'，也不是'能玩多少游戏'。"
—— 这是这个项目最值得学的一条：覆盖度指标会骗人，"能启动 / 能出第一帧 / 能进菜单"才是真指标。

## 3. 移植到 Xbox 路线上的四个直接结论

### 结论一：Xbox 比 PS5 容易，别照抄它的难点

| 环节 | PS5 | Xbox Series | 对你的影响 |
|---|---|---|---|
| CPU 架构 | x86-64 | x86-64 | 都不用动机器码 |
| 可执行格式 | ELF（需转 PE/ELF） | **本来就是 PE** | 省掉整个 relinker 的格式重建 |
| 导入方式 | **NID 哈希符号**，要建映射表 | **按名字导出**，普通 PE 导入表 | 省掉 NID 表，绑定直接可做 |
| 图形 | 裸 PM4 包，必须解码 | **D3D12 API 调用**（`d3d12_xs.dll`） | 在 API 层拦截，不用解包 |
| 现成实现 | 没有 | **vkd3d-proton**（D3D12→Vulkan 已成熟） | 最大的一笔白捡 |

也就是说 AnyPS5 里最贵的两块（NID 映射 + PM4 解码）在你的路线上**根本不存在**。
你的等价物是：PE 加载器 + 导入 thunk + 一个 D3D12 前端（直接 fork vkd3d-proton）+ DXBC→SPIR-V（dxil-spirv）。
工作量大概是它的 1/3 到 1/2，难点集中在 D3D12 的"主机变体"语义（residency、XMem 分配、bindless 上限），而不是指令翻译。

### 结论二：分层方式值得照搬

- **静态**：加载 / 重定位 / 导入绑定，一次性做完
- **动态**：系统库是独立模块（DLL），按需实现、按缺失报错
- **失败要响**：遇到没实现的调用就抛异常 + 打印调用名 + 停下。这会把"还缺什么"变成一张自动生成的待办清单，
  也避免"猜错语义继续跑"导致的诡异崩溃

### 结论三：从最小的目标开始，不要从 3A 开始

他们从 2015 年的 2D 独立游戏起步。你的等价物不是 GTA6，而是：
**用公开的 Microsoft GDK 写一个自己的 hello-triangle 程序**，先让它在你的加载器 + shim 下跑起来。
那是唯一能在没有游戏 exe 的情况下验证整条链路的办法。

### 结论四：内存布局和模块布局是隐形杀手

他们的两个真实 bug 都很朴素：
- 部分游戏在 2^40 以上地址建 guest libc 堆会失败 → 固定低基址 `0x400000`
- 插件模块的初始化时机（`DT_INIT` 即时 vs 显式 load 时）导致 Unity 游戏挂掉

Xbox 侧等价物：`XMemAlloc` 的 placement 语义、大页对齐、
以及游戏对"物理内存直通 / residency 上限"的假设。这些不会报编译错，只会让你在某个加载点黑屏。

## 4. 可直接落地的组件清单（你的项目）

| 能力 | 用什么 | 备注 |
|---|---|---|
| PE 加载 | 自写 loader（参考 Leonida 的 PE loader） | 手工映射 + 导入 thunk，别用系统 LoadLibrary |
| D3D12 → Vulkan | fork **vkd3d-proton** | 补 xs 变体入口：`D3D12CreateDevice`、residency、`D3D12SerializeVersionedRootSignature` |
| DXBC → SPIR-V | **dxil-spirv** / dxc 反汇编 | vkd3d-proton 内部已用，可直接复用 |
| 内存 | `xmem.dll` 六个导出：Alloc/Free/VirtualQuery/GetWorkingSetStatistics | 先 `VirtualAlloc` 顶，再逐项对齐语义 |
| 音频 | XAudio2_9 → FAudio，或 wine 的 xaudio2 | Bink 只能先静音（RAD 不开源） |
| 输入 | SDL3 | 手柄/键鼠映射 |
| 测试输入 | **Microsoft GDK 公开样例** | 合法、可编译、有头文件 |
| 渲染调试 | RenderDoc + Vulkan validation | 每加一个特性必抓帧 |

## 5. 里程碑（可验证，别再写"完成度%"）

| # | 里程碑 | 通过标准 |
|---|---|---|
| M0 | GDK hello-triangle 自建程序 | 在你的 loader 下画出一个三角形，RenderDoc 抓到 draw |
| M1 | D3D12 最小集 | pipeline / root signature / descriptor heap / draw / barrier 全过 |
| M2 | residency + XMem 语义 | 一个模拟"托管内存紧张"的测试程序不崩 |
| M3 | 真实 GDK 样例（Microsoft 公开示例） | 未经修改的样例二进制在你 shim 上运行 |
| M4 | 需要真实 exe | ⛔ 阻塞：泄露包内没有 |

## 6. 现状结论

AnyPS5 证明了"同指令集 + 重实现系统库 + 图形翻译"这条路**在技术上成立**，
但同时也证明了它的**成本上限**：一年量级的社区投入，换来 1 个可玩的小游戏。
放到你的目标（3A 开放世界 + 没有 exe）上，M0–M3 值得做、能做出东西；
M4 之前的所有工作才是你的真实产出。
