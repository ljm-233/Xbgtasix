# Xbgtasix

[![ci](https://github.com/ljm-233/Xbgtasix/actions/workflows/ci.yml/badge.svg)](https://github.com/ljm-233/Xbgtasix/actions/workflows/ci.yml)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

面向 Xbox Series / GDK 的**兼容层**与**资产格式研究**工具集。纯自写代码：
27 个可断言测试、17 份带来源标注的结论文档。

> 不包含任何游戏文件、微软 GDK 头文件/库、解密密钥或绕过手段。

---

## 两条线

**1. 兼容层（可运行代码）**
让导入 `xgameruntime.dll` / `xmem.dll` / `d3d12_xs.dll` 的 x64 程序在 Windows 上启动。
不模拟主机、不写 PE loader —— 靠"同名 DLL 放在 exe 旁边"让 Windows 加载器完成绑定。

**2. 数据面研究（结论 + 工具）**
对 RPF9 资源包、RDNA2 着色器语料、TTPF 贴图库的实测结论，用来回答"移植到底缺什么"。

---

## 现状

| 级别 | 能力 | 证据 |
|---|---|---|
| L0 | 加载 / 导入绑定 | `t0_hello --smoke` |
| L1 | XMem（64 KB 页语义） | `t3_xmem`，93 条断言 |
| L2 | D3D12 设备 | `t0_hello` |
| L3 | root signature → PSO → draw | `t1_triangle`，像素回读 |
| L4 | 纹理 + CBV + SRV + 深度 | `t2_textured`，两次渲染比对 |
| L5 | bindless：98 槽动态索引 + 64K 堆 | `t4_bindless` |
| L6 | mesh shader | `t5_mesh`，`MeshShaderTier=10` |
| L7 | 光追 DXR 1.1 | `t6_rt`，命中绿 / 未命中蓝 |
| L8 | 真实资产渲染 | `t8_model`，占屏 25% |
| L9 | 真任务队列 + 模拟用户 | `t9_async`，64 回调 |
| L10 | 端到端（读真配置） | `t10_integration`，`TitleId=0x7952987A` |
| L11 | 持久化本地存储 | `t11_storage`，容量 2048 MB |
| L12 | 拼图工具 | `t12_sheet` |
| L13 | 显卡适配 / 能力断言 | `t13_caps`，双 GPU 交叉验证 |
| L14 | 显存预算实测 | `t14_vram`，7.0 GB 账面 / 6.0 GB 实测 |
| L15 | 预算变化通知 + 降级 | `t15_budget`，21 次通知 |
| L16 | **真实游戏侧 DLL 加载** | `t16`，包内 16 个 DLL 全部加载成功 |
| L17 | **通用异步机制** | `t17_async2`：`XAsyncRun`/`GetResultSize`/`Cancel` |
| L18 | **包挂载 + 包身份** | `t18_package`：真配置的包身份、挂载到真实目录、坏包返回 `E_NOTFOUND` |
| L19 | **存档目录 + 配额** | `t19_save`：1 MB 往返逐字节一致、配额随占用减少 |
| L20 | **队列 waiter / monitor** | `t20_taskqueue2`：signal 语义、注销后不再触发、引用计数、进程级队列 |
| L21 | **用户 + 能力门 + UI 降级** | `t21_user`：能力门真假混合、特权、登出、消息框/输入框/WebAuth 立即完成 |
| L22 | **UI 面板 + XPackage chunk** | `t22_extras`：chunk 枚举回调 2 次、安装监控填充、面板异步 16ms 完成 |
| L23 | **XPackage 剩余** | `t23_pkg2`：包类型、features.txt 解析、安装注册 token |
| L24 | **XGameSave 容器式** | `t24_save2`：4KB 落盘→重开→逐字节一致、删容器后读回 `ERROR_NOT_FOUND` |
| L25 | **系统 / 显示 / 线程** | `t25_sys`：device id 稳定、句柄校验双向、超时抑制嵌套 |
| L26 | **串流 / 捕获 / 网络** | `t26_stream`：77 导出，"无串流"语义、token 非零、结构体清零 |
| L27 | **复合任务队列** | `t27_composite`：Work 路由到 A、Completion 路由到 B、自身不排队 —— **XTaskQueue 家族桩清零** |

**唯一缺口：游戏可执行文件**（见 [docs/package-completeness.md](docs/package-completeness.md)：
包清单 104 条，只缺 `game_scarlett_master.exe` 一个）。

---

## 快速开始

```bash
./build.sh              # MinGW64 + g++ + dxc
./tools/run_suite.sh    # 21 项测试一条命令
```

返回码语义：`0 = PASS`、`2 = SKIP`（本机不支持该能力，如无光追）、其它 `FAIL`。
同时写 `build/suite/report.txt`（TSV）供脚本消费。

实测：**27 PASS / 0 SKIP / 0 FAIL，约 12 秒（21 项）**（`t6_rt` 首次约 30 秒，之后走驱动缓存降到 0.4 秒）。

只跑子集：`./tools/run_suite.sh --only t5,t6`；强制核显：`XBGTASIX_ADAPTER=0 ./tools/run_suite.sh`。

---

## 三个 shim 怎么工作

Windows 加载器本身就是 PE loader。把同名 DLL 放在目标 exe 旁边，导入解析、重定位、
TLS、SEH 全由系统完成 —— 要写的只是"这些 DLL 里该有什么"。

| DLL | 导出 | 内容 |
|---|---|---|
| `xgameruntime.dll` | **461**（86 手写 + 325 生成桩 + 50 个 GUID 数据导出） | 生命周期（真读 `MicrosoftGame.Config`）、任务队列、用户、持久化存储、XAsync |
| `xmem.dll` | 4 | `XMemAlloc/Free/VirtualQuery/GetWorkingSetStatistics`，64 KB 页语义 |
| `d3d12_xs.dll` | 18 | 4 个手写（带日志 + 选卡）+ 14 个原生转发到系统 `d3d12.dll` |

生成桩来自 GDK 官方导入库（[gen_stubs.py](tools/gen_stubs.py) 自解 COFF archive）。
未实现的调用会打印名字并退出 —— **`xbgtasix.log` 就是待办清单**。

```bash
XBGTASIX_UNIMPL=log     # 只记录不退出，用于铺开调用面
XBGTASIX_ADAPTER=0      # 强制指定 GPU；默认自动挑显存最大的独显
XBGTASIX_CONFIG=<path>  # 指定 MicrosoftGame.Config
```

---

## 研究发现（每条都有来源标注）

| 文档 | 结论 |
|---|---|
| [package-completeness](docs/package-completeness.md) | 用包自带 layout 核对：**104 条只缺 1 个 = exe** |
| [encryption-map](docs/encryption-map.md) | `levels.rpf`（76 GB）**99.1% 是明文**；稳定加密的只有目录表 |
| [rpf9-findings](docs/rpf9-findings.md) | 目录表加密、载荷可雕刻；贴图名字来自载荷内部而非目录 |
| [shader-corpus](docs/shader-corpus.md) | 21,522 条着色器：**mesh 34.8%、光追 64 个、绑定槽 p99=98** |
| [assets-inventory](docs/assets-inventory.md) | 333 载具 / 160 服装 / 57 GB TTPF 贴图 / 21,522 着色器 |
| [gpu-adaptation](docs/gpu-adaptation.md) | 默认会选中核显的坑；两块 GPU 交叉验证矩阵 |
| [gpu-memory](docs/gpu-memory.md) | 账面 7.0 GB / 实测 6.0 GB；224 GB 资产 → 必须流式加载 |
| [graphics-pitfalls](docs/graphics-pitfalls.md) | 7 个"不报错只黑屏"的坑（stream padding / IA flag / row_major / RH 视图…） |
| [xmem-evidence](docs/xmem-evidence.md) | XMem 语义 13 条断言，每条标 `[事实]/[推断]/[自定]` |
| [d3d12xs-evidence](docs/d3d12xs-evidence.md) | D3D12.x 与桌面 D3D12 的差异面，哪些能转发解决 |
| [report](docs/report.md) | 叙事版技术报告（给不读代码的人） |
| [where-is-the-exe](docs/where-is-the-exe.md) | 全包 PE 扫描：没有 exe |
| [exe-day-checklist](docs/exe-day-checklist.md) | exe 到手当天的 10 分钟操作顺序 |
| [HANDOFF](docs/HANDOFF.md) | 交接摘要（状态、复跑、下一步） |
| [env-gotchas](docs/env-gotchas.md) | 环境与工具链的 7 个坑（沙箱按进程放行、路径分隔符、git TLS、mingw 头文件不一致…） |
| [publish](docs/publish.md) | 发布到 GitHub 的步骤与认证方式 |
| [audit-2026-11](docs/audit-2026-11.md) | 五个子代理的全项目审查记录（含未修项与优先级） |
| [CONTRIBUTING](CONTRIBUTING.md) | 怎么加测试 / 补 shim 导出 / 标证据等级 / 提交规范 |
| [anyps5-notes](docs/anyps5-notes.md) | 同类项目 AnyPS5 的分层方式与教训 |
| [gdk_api_surface](docs/gdk_api_surface.md) | 包内 16 个 DLL 的导入/导出面 |

三个最重要的数字：

- 包内 16 个 DLL 的 66 个导入模块里，**只有 3 个是主机专有**（`d3d12_xs`、`xmem`、`XAudio2_9`）
  → "做 Xbox 系统"实际是补这几个模块的语义，不是重写 OS
- **34.8% 是 mesh shader**、**64 个 BVH64 光追** → PC 侧必须有对应能力，不能降级
- 绑定槽 **p99 = 98** → bindless 设计，`t4_bindless` 已验证

---

## 仓库结构

```
Xbgtasix/
  build.sh                     MinGW 构建（合并手写导出 + 生成导出 + dxc 编 shader）
  include/xbgtasix/trace.h     日志 / 未实现策略
  src/xgameruntime/            242 个手写实现（7 个 .cpp，按模块拆分）
  src/xmem/                    XMem* → VirtualAlloc（64 KB 页语义）
  src/d3d12_xs/                手写 + forward.def（14 个原生转发）+ 选卡
  src/generated/               325 个桩 + 50 个 GUID 数据导出（生成物，勿手改）
  tests/t0…t27/                26 个目录（t16 是 python 步），全部离屏可跑
  tools/gen_stubs.py           .lib → 全量导出桩
  tools/forward_shim.py        DLL → 全量导出转发 .def
  tools/find_gdk.py            扫盘定位 GDK
  tools/run_suite.sh           一条命令跑全套 + TSV 报告
  tools/selftest_tools.py      Python 工具自检（CI 门槛）
  tools/check_docs.py          文档相对链接检查
  tools/check_real_dlls.py     用包内真实 DLL 验证 shim 导出面（静态核对 + 动态加载）
  tools/push_via_api.py        git TLS 不可用时用 API 重放提交历史
  tools/match_texture.py       模型名 ↔ 贴图名 匹配
  tools/render_gallery.sh      批量渲染 OBJ
  tools/packaging/             rpf9_probe / rpf_carve / encryption_survey / shader_stats / find_pe / pe_surface / verify_package
  docs/                        18 份结论文档（+ 审查记录）
  .github/workflows/ci.yml     3 个 job（见下）
```

规模：**7,829 行**代码（C++/HLSL），49 个提交，`.git` 2.7 MB。

---

## 依赖

| 依赖 | 说明 |
|---|---|
| MinGW-w64 | 默认取 `D:/mingw64`，可用 `CXX=` 覆盖 |
| Microsoft GDK 头 + 导入库 | 放仓库旁 `../gdk/include_clean/` 与 `../gdk/lib/xgameruntime.lib`；`GDK_INC=` 可覆盖 |
| Vulkan SDK 的 `dxc.exe` | 默认 `C:/VulkanSDK/1.4.363.1/Bin/dxc.exe`，`DXC=` 可覆盖 |

微软的头/库**不入库**，需各自本地解出（`tools/find_gdk.py` 可定位）。

---

## CI

| job | 内容 | 条件 |
|---|---|---|
| `python-tools` | 合成样本自检 5 个 Python 工具 | 无依赖 |
| `docs-links` | 检查 md 相对链接 | 无依赖 |
| `windows-build` | 构建 + 全套 21 项 | 需要 GDK 头；缺失时打印说明后跳过（`continue-on-error`） |

---

## 限制与非目标

| 做不到 | 原因 |
|---|---|
| 启动 GTA6 | 包内唯一缺失项就是 exe（已用 layout 清单证明） |
| 按路径解 RPF 目录 | 目录表加密；密钥在缺失的 exe 里 |
| 主机专有 D3D12 接口（`ID3D12DeviceX`） | IID 在 NDA 内 |
| Reserved 资源 + residency | 同上 |

**不做**：获取/破解 exe 或密钥、分发游戏资产、绕过授权。

---

## 许可证

[MIT](LICENSE) —— 代码与文档可自由使用、修改、再分发（保留版权声明即可）。

**许可范围仅限本仓库的内容**：

| 对象 | 是否被本许可证覆盖 |
|---|---|
| `src/`、`tests/`、`tools/`、`docs/` 里的自写代码与文档 | ✅ MIT |
| 游戏资产（模型/贴图/音频/脚本） | ❌ 不在仓库内，版权归 Rockstar/Take-Two |
| 微软 GDK 头文件与导入库 | ❌ 不在仓库内，微软自有许可 |
| 第三方工具（PercyStar / SixNG / rpf9-explorer） | ❌ 未随仓库分发，各有自己的许可 |

## 法律

本仓库只含自写代码、自写测试与观察结论。不含游戏文件、exe、密钥、微软头文件。
数据面工具只读取使用者本地已有的文件，用于格式研究与互操作。
复现任何数据面结论都需要使用者自己的游戏副本。
