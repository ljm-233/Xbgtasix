# Xbgtasix

[![ci](https://github.com/ljm-233/Xbgtasix/actions/workflows/ci.yml/badge.svg)](https://github.com/ljm-233/Xbgtasix/actions/workflows/ci.yml)
[![license](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)

面向 Xbox GDK 程序的 **Windows 兼容层**，加上配套的**资产格式研究**工具。
自写代码、可断言的测试、带证据标注的结论文档。

> 仓库内不含游戏文件、exe、解密密钥、微软 GDK 头文件，也不含破解或 DRM 绕过。

---

## 当前进展（2026-11-24）

**真实 Xbox Series 游戏 EXE 已在 Windows 上执行到资源归档读取阶段，但还没有游戏出画面或可游玩的记录。**

- **真实启动**：本地开发构建已通过早期的内存分配和 DirectStorage 工厂初始化。修正资源路径后，真实 EXE 已实际读取 `appdata0.rpf`、`common.rpf`、`levels.rpf` 等归档的开头；这不代表归档已完整加载，也不代表相关修复已合入 `main` 或通过 CI。
- **图形验证**：独立测试已覆盖 D3D12 绘制、交换链 `Present` 和部分游戏资产渲染。这些不是由真实游戏 EXE 驱动的画面。
- **下一个验收点**：从资源读取继续推进，确认真实 EXE 能用游戏自身的着色器创建图形管线并呈现第一帧。

---

## 这个项目在解决什么

目标是让 Xbox Series 的 x64 游戏 EXE 在 Windows 上运行。CPU 指令无需翻译，
但它依赖桌面 Windows 不提供的 Xbox GDK 接口，包括 `xgameruntime.dll`、`xmem.dll` 和 `d3d12_xs.dll`。

项目不另写 PE 加载器：兼容 DLL 放在目标程序旁边，导入解析、重定位和 TLS 交给 Windows，
由兼容层实现所需的主机接口，并把可映射的图形调用接到桌面 D3D12。
机制细节与失败点见 [docs/how-it-runs.md](docs/how-it-runs.md)。

```
目标游戏 exe
   ├── xgameruntime.dll   GDK 运行时（任务、用户、存储等）
   ├── xmem.dll           Xbox 内存分配与页语义
   ├── d3d12_xs.dll       Xbox D3D12 入口与桌面 D3D12 对接
   └── 其他主机兼容模块    包括 DirectStorage 接口
```

---

## 兼容层与独立测试

| 级别 | 能力 | 测试 |
|---|---|---|
| L0 | 加载 / 导入绑定 | `t0_hello` |
| L1 | XMem（64 KB 页语义，93 条断言） | `t3_xmem` |
| L2–L3 | D3D12 设备 → root signature → PSO → draw | `t1_triangle` |
| L4 | 纹理 + CBV + SRV + 深度 | `t2_textured` |
| L5 | bindless：98 槽动态索引（六点采样） | `t4_bindless` |
| L6 | mesh shader | `t5_mesh` |
| L7 | 光追 DXR 1.1 | `t6_rt` |
| L8 | 真实资产渲染（GTA6 载具/衣物） | `t8_model` |
| L9 | 任务队列 + 模拟用户 | `t9_async` |
| L10 | 端到端：真配置读 `TitleId=0x7952987A` | `t10_integration` |
| L11 | 持久化本地存储（容量 2048 MB 来自配置） | `t11_storage` |
| L12 | 拼图工具 | `t12_sheet` |
| L13 | 显卡适配 / 能力断言（部分图形测试在核显下复测过：t1/t5/t6，其余未交叉验证） | `t13_caps` |
| L14 | 显存预算（7.0 GB 账面 / 6.0 GB 实测） | `t14_vram` |
| L15 | 预算变化通知 + 降级（实测每次 8~40 次通知，测试只断言 >0） | `t15_budget` |
| L16 | **16/16 真实游戏侧 DLL 加载成功** | `t16` |
| L17 | 通用异步（`XAsyncRun` / `GetResultSize` / `Cancel`） | `t17_async2` |
| L18 | 包身份 + 挂载（坏包返回 `ERROR_NOT_FOUND`） | `t18_package` |
| L19 | 存档目录 + 配额（1 MB 往返逐字节一致） | `t19_save` |
| L20 | 队列 waiter / monitor / 引用计数 | `t20_taskqueue2` |
| L21 | 用户本地语义 + 能力门 | `t21_user` |
| L22 | UI 面板 + XPackage chunk | `t22_extras` |
| L23 | XPackage 剩余（包类型 / features / 安装注册） | `t23_pkg2` |
| L24 | XGameSave 容器式（27 个导出全实现） | `t24_save2` |
| L25 | 系统 / 显示 / 线程（device id 稳定、超时抑制） | `t25_sys` |
| L26 | 串流 / 捕获 / 网络（77 个导出） | `t26_stream` |
| L27 | 复合任务队列 —— **XTaskQueue 家族桩清零** | `t27_composite` |
| L28 | 语音合成（SAPI；本机无音频输出 → SKIP） | `t28_speech` |
| L29 | 性能基准 | `t29_perf` |
| L30 | **启动链路演练**（假游戏 + 启动器） | `t30_fakegame` |
| L31 | 窗口交换链（flip-model 两帧 Present） | `t31_swapchain` |
| L32 | 窗口生命周期 + CPU 并行 | `t32_window` |

---

## 快速开始

```bash
./build.sh                 # MinGW-w64 + g++ + dxc：构建兼容 DLL、启动器和测试
./tools/run_suite.sh       # 40 步测试一条命令，约 20 秒
./tools/ci_build.sh        # CI 同款：无 GDK 也能验 xmem 子集
```

返回码约定：`0 = PASS`、`2 = SKIP`（本机不支持该能力）、其它 `FAIL`。
同时写 `build/suite/report.txt`（TSV）。

只跑子集：`./tools/run_suite.sh --only t27,t31`；强制核显：`XBGTASIX_ADAPTER=0 ./tools/run_suite.sh`。

### 回归断言：DXIL 句柄的 kind 与组件类型

victory 侧生成桌面对应 DXIL 时，`dx.op.annotateHandle` 的 kind 由访问指令决定，
`props.1`（typed/texture 上是 `(组件数 << 8) | ComponentType`）由组件类型决定，
**两者都必须进入句柄身份**（`victory-1k/crates/victory-shader/src/emit/lower.rs` 的 `handle()`）。

```bash
python3 tools/handle_kind_census.py comp <装着 .ll 的目录>   # 期望 0 命中
```

`comp` 归零是本项目的回归断言：**任何人动 `handle()` 或其调用点的 props，都要跑一次**。
非零 = 同一个描述符被不同组件类型共用了一条句柄（声明 `4×F32` 却跑 `atomicOr` 就是这一条），
vkd3d 会按 f32 建 view，SPIR-V 里就是 `OpAtomicOr %float`，NVIDIA 驱动直接崩。
背景、真值对照与另外两类精度问题见 `docs/handle-kind-analysis.md`。

对自己有权使用的本地游戏副本，先用启动器静态检查。`verify` 和 `check` 不启动游戏。
`run` 和 `firstrun` 会运行目标程序，仅适合在隔离的测试环境中诊断，不是游玩入口。

```bash
build/xbgtasix.exe check    <exe>      # 静态核对导入面，不运行目标 EXE
build/xbgtasix.exe run      <exe>      # 跑起来并按退出码分类（覆盖同名 DLL 前备份为 *.xbgtasix-backup）
build/xbgtasix.exe verify   "<包目录>"  # 按 layout 清单核对完整性
build/xbgtasix.exe doctor              # 环境自检
build/xbgtasix.exe restore  "<包目录>"  # 撤销 run 的写入：有备份→还原，没备份→删掉我们放的
build/xbgtasix.exe firstrun "<包目录>"  # verify → doctor → check → run 四步一次走完
```

流程见 [exe-day-checklist](docs/exe-day-checklist.md)。

---

## 在 Linux / Wine 下运行

项目原来只跑在 Windows 上；现在有了一条经 Wine 的 Linux 路径。**目前支持到“跑起来、越过引导、停在下一堵墙”，
不是可玩状态**。细节、反汇编与原始日志见 [docs/wine-host.md](docs/wine-host.md)。

```bash
python3 tools/host_probe.py --doctor     # 宿主是不是 Wine + 真实 GPU（读 /sys/class/drm）
./tools/run_wine.sh                      # 跑包：补 dxvk/vkd3d-proton 路径，带上异常钩子
./tools/run_wine.sh --dll-dir build/wine-bridge --timeout 180
```

已知差异与当前状态：

| 项 | 真 Windows | Linux + Wine（本机实测） |
|---|---|---|
| 图形栈 | 桌面 D3D12 | **必须**走 dxvk + vkd3d-proton（`run_wine.sh` 已配）；用 wine builtin 的 `dxgi`/`wined3d` 会在导入阶段就 `0xC0000135` |
| 适配器信息 | 真卡 | 详情随环境变：给对栈时报真卡但不保证厂商号（实测把 NVIDIA 报成 `1002`），走 builtin 路径时会报占位卡 —— **别只信 DXGI**，用 `host_probe.py` / `doctor` 的 `真实 GPU` 行 |
| 进度 | 到资源归档读取 / `game+0x247AF2D`（64 MiB 断言） | 到 `game+0x2237178`（空的诊断 vector 断言）；路上依次越过 `game+0x5A91C21`（缺四个内部导出）、`game+0x55DE020`（`QueryApiImpl` 空对象）、`game+0x5925915`（Wine 的 `GameInputCreate` 返回 `E_NOTIMPL`，靠 `SteamGameId` 越过）。第 4 堵墙已用硬件写断点定位到"按 `appdata0.rpf` 查东西的 gate 返回 false"，见 `docs/wine-host.md` 第 8 节 |
| 主机检测 | — | `XBGTASIX_PLATFORM=windows\|wine` 可手动钉死；不设则探测 `ntdll!wine_get_version` |

怎么跑起来的关键一步（本次实测的根因）：标题的 GDK 引导要按名字从 `xgameruntime.dll` 解析
`InitializeApiImpl` / `InitializeApiImplEx` / `InitializeApiImplEx2` / `UninitializeApiImpl` 四个内部入口点。
缺它们 → 引导返回 `0x89240101` → 标题在 `game+0x5A91C21` `int3`（就是 Wine 上“482 ms 就死”那一枪）。
手工实现见 [src/xgameruntime/api_impl.cpp](src/xgameruntime/api_impl.cpp)；
本机没有 mingw-w64，用 [tools/add_exports.py](tools/add_exports.py) 给现成 DLL 追加导出完成了验证（461 → 465）。

> 交叉编译（在有 mingw-w64 的机器上）用 `./tools/build_linux_cross.sh`；产物仍是 Windows PE，由 Wine 加载。

---

## 性能（本机实测）

| 项目 | 实测 |
|---|---|
| 启动（含读配置） | 0.3 ms |
| 解析全部 461 导出 | 0.06 ms |
| 转发一次调用（额外开销） | ≈ 0（相对系统直调） |
| `XMemAlloc`+`Free` 往返 | 1.62–1.72 µs（`VirtualAlloc` 的 1.54–1.76 倍） |
| 队列提交 / 派发 | 0.028 µs / 0.019 µs |
| 异步往返 | 51–83 µs |
| ThreadPool | 2000 个回调全部执行（t32 断言），耗时见 t32 日志 |

以上是 6 次运行的区间（隔离区引入后重测，见 t29）。

修掉过一个真问题：异步往返原本 **15.6 ms**，因为 `XAsyncGetStatus(wait)` 用
`Sleep(1)` 轮询，而 Windows 默认计时器精度就是 15.6 ms。改成条件变量后约 53 µs，
**快约 290 倍**。

---

## 研究发现

| 文档 | 一句话结论 |
|---|---|
| [package-completeness](docs/package-completeness.md) | 早期样本的 104 项清单核对记录（该样本缺 EXE，不是当前启动状态） |
| [encryption-map](docs/encryption-map.md) | `levels.rpf`（76 GB）**99% 明文**；稳定加密的只有目录表 |
| [rpf9-findings](docs/rpf9-findings.md) | 目录表加密；载荷可雕刻，路径来自载荷内部 |
| [shader-corpus](docs/shader-corpus.md) | 21,522 条 RDNA2 着色器：mesh 34.8%、光追 64 个、绑定槽 p99=98 |
| [assets-inventory](docs/assets-inventory.md) | 333 载具 / 160 服装 / 57 GB TTPF 贴图 |
| [gpu-adaptation](docs/gpu-adaptation.md) | 默认会选中核显的坑；t1/t5/t6 在核显下复测过 |
| [gpu-memory](docs/gpu-memory.md) | 账面 7.0 GB / 实测 6.0 GB；224 GB 资产 → 必须流式加载 |
| [smartscreen](docs/smartscreen.md) | 被 Defender 拦下时怎么办（解除锁定 / 本机自签名 / 从源码构建） |
| [launcher](docs/launcher.md) | **xbgtasix.exe**：run / check / verify / doctor / restore / firstrun 六条命令与失败分类 |
| [license-and-store](docs/license-and-store.md) | **XStore 为什么是桩**：78 个导出（77 桩 + 1 手写）返回什么、卡授权时看到什么、以及"不要伪造授权"的边界 |
| [how-it-runs](docs/how-it-runs.md) | **机制**：为什么不需要 PE loader、依赖怎么被满足、五个检查点 |
| [feasibility](docs/feasibility.md) | **可行性判定 + 性能实测**：已证 / 无法判定 / 不可行 三层 |
| [graphics-pitfalls](docs/graphics-pitfalls.md) | **9 个"不报错只黑屏"的坑** |
| [env-gotchas](docs/env-gotchas.md) | 7 个环境/工具链的坑（沙箱按进程放行、git TLS、mingw 头文件不一致…） |
| [arch-linux](docs/arch-linux.md) | **Arch Linux**：交叉编译已通，运行时靠 Wine/Proton；mesh 与光追是未知数 |
| [ui-references](docs/ui-references.md) | **现成方案调研**：Game Bar SDK / Theseus / recomp-ui / DirectComposition 覆盖窗，含许可对比 |
| [ui-host](docs/ui-host.md) | **UI 两种模式**：立即完成 vs 交给宿主（挂起/回填链路） |
| [rendering](docs/rendering.md) | **渲染**：主机没有交换链（Present 是队列方法 `PresentX`）；设备/队列 vtable 代理、root signature 主机标志、着色器与纹理平铺 |
| [audio-video](docs/audio-video.md) | **音视频**：Bink 只依赖 xmem+XAudio2_9（无主机解码器）；缺 XAudio2_9 会在加载 Bink 时 0xC0000135 |
| [input](docs/input.md) | **手柄**：XInput / GameInput 都是原生，只有"设备↔用户关联"需要兼容层 |
| [audit-2026-11](docs/audit-2026-11.md) | 五路子代理的全项目审查记录（含未修项与优先级） |
| [xmem-evidence](docs/xmem-evidence.md) | XMem 语义，每条标 `[事实]/[推断]/[自定]` |
| [d3d12xs-evidence](docs/d3d12xs-evidence.md) | D3D12.x 与桌面 D3D12 的差异面 |
| [report](docs/report.md) | 叙事版技术报告 |
| [where-is-the-exe](docs/where-is-the-exe.md) | 早期样本的 PE 扫描记录 |
| [exe-day-checklist](docs/exe-day-checklist.md) | EXE 的静态检查与启动诊断流程 |
| [publish](docs/publish.md) | 发布与认证方式 |
| [anyps5-notes](docs/anyps5-notes.md) | 同类项目 AnyPS5 的分层与教训 |
| [leonida-crossref](docs/leonida-crossref.md) | **与 Leonida 的互补映射**：容器里只有 FSSM（无 DXIL）、可借的 ABI 事实、唯一的对接面 |
| [host-stall](docs/host-stall.md) | **标题 30 秒自杀**：它的 hang check 把我们的驱动编译当成主线程卡死；记账 + Sleep thunk 让它的"> 2 s 就重数"规则生效 |
| [gdk_api_surface](docs/gdk_api_surface.md) | 包内 16 个 DLL 的导入/导出面 |
| [HANDOFF](docs/HANDOFF.md) | 交接摘要 |

三个最有用的数字：

- 包内 16 个 DLL 的 66 个导入模块里，**只有 3 个是主机专有**（`d3d12_xs` / `xmem` / `XAudio2_9`）
- **34.8% 是 mesh shader**、**64 个 BVH64** —— PC 侧必须真有这些能力，不能降级
- 绑定槽 **p99 = 98** —— bindless 设计

---

## 仓库结构

```
Xbgtasix/
  build.sh                     MinGW 构建（手写 + 生成桩 + GUID 数据 + dxc 编 shader）
  include/xbgtasix/trace.h     日志 / 未实现策略（stop | log）
  src/xgameruntime/            268 个手写实现（10 个 .cpp，按模块拆分）
  src/xmem/                    XMem 内存分配、映射及页语义
  src/d3d12_xs/                Xbox D3D12 入口、桌面转发与适配器选择
  src/console_hw/              主机音频、存储、GPU 纹理等兼容模块
  src/generated/               143 个桩（按返回类型给默认值）+ 50 个 GUID 数据
  tests/t0…t42/                41 个测试目录（无 t7、t16），全部离屏可跑
  tools/fetch_gdk.py           从 nuget.org 拉官方 GDK 包并解出头文件/导入库
  tools/gen_stubs.py           .lib → 全量导出桩（返回类型从 GDK 头推断）
  tools/gen_guids.py           _GUID_xxx → 真 GUID 数据符号
  tools/forward_shim.py        DLL → 全量导出转发 .def
  tools/analyze_exe.py         目标 exe 导入面 vs shim 导出面（纯 Python，补充；以 check 为准）
  tools/launch.sh              放进 shim 环境跑起来 + 失败分类
  tools/ci_build.sh            CI 同款构建+测试（分"需 GDK / 不需 GDK"两层）
  tools/ci_watch.sh            短轮询 CI 状态（默认最多 240s，超时先干别的）
  src/host/                    **xbgtasix.exe**：启动器（run/check/verify/doctor/restore/firstrun）+ PE 解析
  tools/run_suite.sh           一条命令跑全套 + TSV 报告
  tools/check_real_dlls.py     用真实游戏侧 DLL 验证导出面（静态 + 动态）
  tools/selftest_tools.py      Python 工具自检（CI 门槛）
  tools/check_docs.py          文档相对链接检查
  tools/push_via_api.py        git TLS 不可用时用 API 重放提交历史
  tools/match_texture.py       模型名 ↔ 贴图名 匹配
  tools/handle_kind_census.py  DXIL 句柄 kind / 组件类型普查（gta6 | victory | slots | comp）
  tools/packaging/             rpf9_probe / rpf_carve / encryption_survey / shader_stats
                               / find_pe / pe_surface / verify_package
  docs/                        28 份结论文档
  .github/workflows/ci.yml     4 个 job
```

规模：**20,145 行**（C++ / HLSL），103 个提交，`.git` 7.2 MB。统计口径写清楚：

```bash
git ls-files '*.cpp' '*.h' '*.hlsl' | wc -l     # 67 个受版本控制的文件
for f in $(git ls-files '*.cpp' '*.h' '*.hlsl'); do git show HEAD:"$f"; done | wc -l   # 20,145
#  ↑ 按 HEAD 已提交内容逐文件统计（含 src/generated/ 里 205 行生成代码）；
#    工作区若正在改代码，直接 xargs wc -l 会更大，那不是已提交规模。
du -sh .git                                     # 7.2 MB（块占用；du -sb = 3.6 MB 实际字节）
```

---

## 依赖

| 依赖 | 说明 |
|---|---|
| MinGW-w64 | 默认 `D:/mingw64`，`CXX=` 可覆盖 |
| Microsoft GDK 头 + 导入库 | **`python tools/fetch_gdk.py --out ../gdk`** 从 nuget.org 的官方 `Microsoft.GDK.Windows` 包自动取（版本 2610.0.9661）；或手工解到 `../gdk/include_clean/`；`GDK_INC=` 可覆盖 |
| Vulkan SDK 的 `dxc.exe` | 默认 `C:/VulkanSDK/1.4.363.1/Bin/dxc.exe`，`DXC=` 可覆盖 |

微软头/库**不入库**：`fetch_gdk.py` 从 nuget.org 取官方包并解出，仓库不转发微软二进制。
（`tools/find_gdk.py` 可扫描本机已有的 GDK 安装。）

---

## CI

| job | 内容 | 条件 |
|---|---|---|
| `python-tools` | 合成样本自检 5 个 Python 工具 | 无依赖 |
| `docs-links` | 检查 md 相对链接 | 无依赖 |
| `linux-cross` | `ubuntu-latest` 上交叉编译三个 shim，校验导出数 461/4/18 |
| `windows-build` | **从 NuGet 拉官方 GDK**（`tools/fetch_gdk.py`）→ `tools/ci_build.sh` 跑**完整构建 + 全套 40 步**；拉不到时 **job 直接失败**（`STRICT=1`），不再悄悄退化成"无 GDK 子集" | GDK 走微软自己的 nuget.org 源，仓库不转发微软二进制 |

---

## 限制与非目标

| 做不到 | 原因 |
|---|---|
| 完整启动、出画面与游玩 | 真实 EXE 已能读取部分资源归档的开头，但尚未验证完整启动或由游戏驱动的首帧 |
| RPF 目录与资产研究 | 工具只读取已有数据，不提供目录表解密或密钥 |
| Xbox 图形兼容性 | 桌面 D3D12 测试通过不代表主机专有接口或游戏预编译着色器已能在真实启动链中正常工作 |
| `XStore` 购买/授权 | 不伪造授权（设计决定，见 [license-and-store](docs/license-and-store.md)） |
| 需要 XBL token 的功能 | token 由微软服务按用户签发，离线不可能 |

**不做**：获取/破解 exe 或密钥、分发游戏资产、绕过授权。

---

## 许可证

[GPL-3.0](LICENSE)（`GPL-3.0-or-later`）。

选用它的两个实际后果，先说清楚：

- **对使用者**：可以自由使用、修改、再分发，但**分发时必须一并提供源码**并保留同样的许可。
  个人本地使用、自己编译自己跑，不受限制。
- **对本项目**：因为同是 GPL-3.0，**现在可以合法借鉴/移植其他 GPL-3.0 项目的代码**
  （例如 Theseus/UIX、uLaunch、SwitchU 这些此前只能"看思路"的仓库）。
  但**无许可**（默认保留一切权利）的项目仍然不能抄。

许可范围仅限本仓库内容：游戏资产、微软 GDK 头/库、第三方工具都不在其中。

---

## 法律

本仓库只含自写代码、自写测试与观察结论。不含游戏文件、exe、密钥、微软头文件，
也不含破解或 DRM 绕过。数据面工具只读取使用者本地已有的文件，用于格式研究与互操作。
复现任何数据面结论都需要使用者自己的游戏副本。
