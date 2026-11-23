# Xbgtasix

[![ci](https://github.com/ljm-233/Xbgtasix/actions/workflows/ci.yml/badge.svg)](https://github.com/ljm-233/Xbgtasix/actions/workflows/ci.yml)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

面向 Xbox GDK 程序的 **Windows 兼容层**，加上配套的**资产格式研究**工具。
自写代码、可断言的测试、带证据标注的结论文档。

> 仓库内不含游戏文件、exe、解密密钥、微软 GDK 头文件或任何绕过手段。

---

## 这个项目在解决什么

Xbox Series 上的游戏是普通 x64 PE，但它导入的是主机版 DLL：`xgameruntime.dll`、
`xmem.dll`、`d3d12_xs.dll`。Windows 加载器找不到这些模块，程序根本起不来。

项目的做法不是写 PE loader，而是**把同名 DLL 放在目标程序旁边**：
导入解析、重定位、TLS、SEH 全部交给系统加载器，我们只负责"这些 DLL 里该有什么"。
机制细节与失败点见 [docs/how-it-runs.md](docs/how-it-runs.md)。

```
目标游戏 exe
   ├── xgameruntime.dll   461 个导出（265 手写 + 146 生成桩 + 50 个 GUID 数据）
   ├── xmem.dll             4 个导出（64 KB 页语义的内存分配）
   └── d3d12_xs.dll        18 个导出（4 个手写 + 14 个转发到系统 d3d12.dll）
```

---

## 现状（全部有测试）

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
| L13 | 显卡适配 / 能力断言（双 GPU 交叉验证） | `t13_caps` |
| L14 | 显存预算（7.0 GB 账面 / 6.0 GB 实测） | `t14_vram` |
| L15 | 预算变化通知 + 降级（21 次通知） | `t15_budget` |
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

**唯一缺口：游戏可执行文件。**
[package-completeness](docs/package-completeness.md) 用包自带 layout 清单核对：
104 个条目里只缺 `game_scarlett_master.exe` 一个。

---

## 快速开始

```bash
./build.sh                 # MinGW64 + g++ + dxc，产出 3 个 shim（约 90 秒）
./tools/run_suite.sh       # 38 项测试一条命令，约 12 秒
./tools/ci_build.sh        # CI 同款：无 GDK 也能验 xmem 子集
```

返回码约定：`0 = PASS`、`2 = SKIP`（本机不支持该能力）、其它 `FAIL`。
同时写 `build/suite/report.txt`（TSV）。

只跑子集：`./tools/run_suite.sh --only t27,t31`；强制核显：`XBGTASIX_ADAPTER=0 ./tools/run_suite.sh`。

拿到 exe 之后（用启动器，纯 C++ 无依赖）：

```bash
# 首次运行若被 Defender SmartScreen 拦下：见 docs/smartscreen.md
build/xbgtasix.exe check  <exe>     # 静态核对导入面（10 秒，不用跑）
build/xbgtasix.exe run    <exe>     # 跑起来并按退出码分类
build/xbgtasix.exe verify "<包目录>" # 按 layout 清单核对完整性
build/xbgtasix.exe doctor           # 环境自检
```

流程见 [exe-day-checklist](docs/exe-day-checklist.md)。

---

## 性能（本机实测）

| 项目 | 实测 |
|---|---|
| 启动（含读配置） | 0.52 ms |
| 解析全部 461 导出 | 0.058 ms |
| 转发一次调用（额外开销） | ≈ 0（相对系统直调） |
| `XMemAlloc`+`Free` 往返 | 1.15 µs（`VirtualAlloc` 的 1.13 倍） |
| 队列提交 / 派发 | 0.036 µs / 0.024 µs |
| 异步往返 | 54 µs |
| ThreadPool 吞吐 | 2000 回调 / 16 ms |

修掉过一个真问题：异步往返原本 **15.6 ms**，因为 `XAsyncGetStatus(wait)` 用
`Sleep(1)` 轮询，而 Windows 默认计时器精度就是 15.6 ms。改成条件变量后 54 µs，
**快 287 倍**。

---

## 研究发现

| 文档 | 一句话结论 |
|---|---|
| [package-completeness](docs/package-completeness.md) | 104 条清单只缺 1 个：exe |
| [encryption-map](docs/encryption-map.md) | `levels.rpf`（76 GB）**99% 明文**；稳定加密的只有目录表 |
| [rpf9-findings](docs/rpf9-findings.md) | 目录表加密；载荷可雕刻，路径来自载荷内部 |
| [shader-corpus](docs/shader-corpus.md) | 21,522 条 RDNA2 着色器：mesh 34.8%、光追 64 个、绑定槽 p99=98 |
| [assets-inventory](docs/assets-inventory.md) | 333 载具 / 160 服装 / 57 GB TTPF 贴图 |
| [gpu-adaptation](docs/gpu-adaptation.md) | 默认会选中核显的坑；两块 GPU 交叉验证 |
| [gpu-memory](docs/gpu-memory.md) | 账面 7.0 GB / 实测 6.0 GB；224 GB 资产 → 必须流式加载 |
| [smartscreen](docs/smartscreen.md) | 被 Defender 拦下时怎么办（解除锁定 / 本机自签名 / 从源码构建） |
| [launcher](docs/launcher.md) | **xbgtasix.exe**：run / check / verify / doctor 四条命令与失败分类 |
| [how-it-runs](docs/how-it-runs.md) | **机制**：为什么不需要 PE loader、依赖怎么被满足、五个检查点 |
| [feasibility](docs/feasibility.md) | **可行性判定 + 性能实测**：已证 / 无法判定 / 不可行 三层 |
| [graphics-pitfalls](docs/graphics-pitfalls.md) | **9 个"不报错只黑屏"的坑** |
| [env-gotchas](docs/env-gotchas.md) | 7 个环境/工具链的坑（沙箱按进程放行、git TLS、mingw 头文件不一致…） |
| [arch-linux](docs/arch-linux.md) | **Arch Linux**：交叉编译已通，运行时靠 Wine/Proton；mesh 与光追是未知数 |
| [ui-references](docs/ui-references.md) | **现成方案调研**：Game Bar SDK / Theseus / recomp-ui / DirectComposition 覆盖窗，含许可对比 |
| [ui-host](docs/ui-host.md) | **UI 两种模式**：立即完成 vs 交给宿主（挂起/回填链路） |
| [input](docs/input.md) | **手柄**：XInput / GameInput 都是原生，只有"设备↔用户关联"需要兼容层 |
| [audit-2026-11](docs/audit-2026-11.md) | 五路子代理的全项目审查记录（含未修项与优先级） |
| [xmem-evidence](docs/xmem-evidence.md) | XMem 语义，每条标 `[事实]/[推断]/[自定]` |
| [d3d12xs-evidence](docs/d3d12xs-evidence.md) | D3D12.x 与桌面 D3D12 的差异面 |
| [report](docs/report.md) | 叙事版技术报告 |
| [where-is-the-exe](docs/where-is-the-exe.md) | 全包 PE 扫描结果 |
| [exe-day-checklist](docs/exe-day-checklist.md) | exe 到手后的 6 步流程 |
| [publish](docs/publish.md) | 发布与认证方式 |
| [anyps5-notes](docs/anyps5-notes.md) | 同类项目 AnyPS5 的分层与教训 |
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
  src/xgameruntime/            265 个手写实现（8 个 .cpp，按模块拆分）
  src/xmem/                    XMem* → VirtualAlloc（64 KB 页语义）
  src/d3d12_xs/                手写 4 个 + forward.def 14 个转发 + 选卡缓存
  src/generated/               146 个桩（按返回类型给默认值）+ 50 个 GUID 数据
  tests/t0…t32/                31 个测试目录，全部离屏可跑
  tools/fetch_gdk.py           从 nuget.org 拉官方 GDK 包并解出头文件/导入库
  tools/gen_stubs.py           .lib → 全量导出桩（返回类型从 GDK 头推断）
  tools/gen_guids.py           _GUID_xxx → 真 GUID 数据符号
  tools/forward_shim.py        DLL → 全量导出转发 .def
  tools/analyze_exe.py         目标 exe 导入面 vs shim 导出面（跑之前就能对表）
  tools/launch.sh              放进 shim 环境跑起来 + 失败分类
  tools/ci_build.sh            CI 同款构建+测试（分"需 GDK / 不需 GDK"两层）
  tools/ci_watch.sh            短轮询 CI 状态（默认最多 240s，超时先干别的）
  src/host/                    **xbgtasix.exe**：启动器（run/check/verify/doctor）+ PE 解析
  tools/run_suite.sh           一条命令跑全套 + TSV 报告
  tools/check_real_dlls.py     用真实游戏侧 DLL 验证导出面（静态 + 动态）
  tools/selftest_tools.py      Python 工具自检（CI 门槛）
  tools/check_docs.py          文档相对链接检查
  tools/push_via_api.py        git TLS 不可用时用 API 重放提交历史
  tools/match_texture.py       模型名 ↔ 贴图名 匹配
  tools/packaging/             rpf9_probe / rpf_carve / encryption_survey / shader_stats
                               / find_pe / pe_surface / verify_package
  docs/                        20 份结论文档
  .github/workflows/ci.yml     3 个 job
```

规模：**14,520 行**（C++ / HLSL），63 个提交，`.git` 3.9 MB。

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
| `windows-build` | **从 NuGet 拉官方 GDK**（`tools/fetch_gdk.py`）→ `tools/ci_build.sh` 跑**完整构建 + 全套 38 项**；拉不到时退化成"无 GDK 子集"，并在 summary 写明未覆盖部分 | GDK 走微软自己的 nuget.org 源，仓库不转发微软二进制 |

---

## 限制与非目标

| 做不到 | 原因 |
|---|---|
| 启动游戏 | 包内唯一缺失就是 exe |
| 按路径解 RPF 目录 | 目录表加密，密钥在缺失的 exe 里 |
| 主机专有 D3D12 接口（`ID3D12DeviceX`） | IID 在 NDA 内 |
| `XStore` 购买/授权 | 不伪造授权（设计决定） |
| 需要 XBL token 的功能 | token 由微软服务按用户签发，离线不可能 |

**不做**：获取/破解 exe 或密钥、分发游戏资产、绕过授权。

---

## 许可证

[MIT](LICENSE) —— 代码与文档可自由使用、修改、再分发（保留版权声明）。

许可范围仅限本仓库内容：游戏资产、微软 GDK 头/库、第三方工具都不在其中。

---

## 法律

本仓库只含自写代码、自写测试与观察结论。不含游戏文件、exe、密钥、微软头文件。
数据面工具只读取使用者本地已有的文件，用于格式研究与互操作。
复现任何数据面结论都需要使用者自己的游戏副本。
