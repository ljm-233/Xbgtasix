# Xbgtasix

两条并行的线：

1. **兼容层（可运行代码）** —— 在 Windows 上让 Xbox GDK 程序启动的 API 层。不模拟主机、
   不写 PE loader，靠"同名 DLL 放在 exe 旁边"让 Windows 加载器完成绑定。
   已有三个 shim DLL、五个可断言的测试。
2. **数据面研究（结论与工具）** —— 对 RPF9 资源包与 RDNA2 着色器语料的实测结论，
   用来回答"移植到 PC 到底缺什么"。所有断言都标了来源与可信度。

> 这个仓库不含任何游戏文件、不含微软的头文件或库、不含密钥。所有工具只读取使用者本地的数据。

---

## 当前状态

| 环节 | 状态 | 证据 |
|---|---|---|
| 加载 / 导入绑定（L0） | ✅ | `t0_hello --smoke` |
| XMem（64KB 页语义） | ✅ | `t3_xmem`，93 条断言 |
| 真实 D3D12 设备（L2） | ✅ | `t0_hello` |
| 图形管线：root sig → PSO → draw（L3） | ✅ | `t1_triangle`，像素回读 |
| 描述符表 + CBV + SRV + 采样器 + 深度（L4） | ✅ | `t2_textured`，两次渲染比对 |
| **bindless：98 槽动态索引 + 64K 大堆（L5）** | ✅ | `t4_bindless`，索引 97 命中绿色 |
| `d3d12_xs` 导出面 | ✅ | 4 手写 + 14 原生转发 = 18 |
| `xgameruntime` 导出面 | ✅ | 10 手写 + 482 生成桩 = 492 |
| 游戏 exe | ❌ 缺失 | 泄露包内没有 `game_scarlett_master.exe` |

---

## 快速开始

```bash
./build.sh                 # MinGW64 + g++ + dxc
cd build
./t0_hello.exe --smoke     # 加载 + XMem + 设备
./t1_triangle.exe          # 三角形 + 像素校验
./t2_textured.exe          # 纹理 + CBV + 深度
./t3_xmem.exe              # 内存压力（10 万次 churn + 8 线程）
./t4_bindless.exe          # 98 槽描述符表 + 动态索引
```

五个测试全部**离屏渲染 + 像素/字节级断言**，没有显示器也能跑，`rc=0` 才有意义。

依赖见文末。

---

## 三个 shim 怎么工作

Windows 加载器本身就是 PE loader。把同名 DLL 放在目标 exe 旁边，导入解析、重定位、
TLS、SEH 全由系统完成 —— 我们要写的只是"这些 DLL 里该有什么"。

| DLL | 内容 | 关键点 |
|---|---|---|
| `xgameruntime.dll` | 生命周期 + 任务队列 + 用户 + 商店 | 10 个手写（签名由 GDK 真头文件强制）+ 482 个生成桩，未实现即打印名字并退出 |
| `xmem.dll` | `XMemAlloc/Free/VirtualQuery/GetWorkingSetStatistics` | 64KB 页语义；行为由 `t3_xmem` 锁住 |
| `d3d12_xs.dll` | 主机 D3D12 变体入口 | 4 个手写（带日志）+ 14 个原生转发到系统 `d3d12.dll` |

未实现调用的策略：`XBGTASIX_UNIMPL=stop`（默认，打印调用名后退出）或 `log`（只记录）。
日志同时写 stderr 与 exe 同目录的 `xbgtasix.log` ——**它就是待办清单**。

---

## 研究发现

按重要性排序，每份文档都标注了 `[事实]/[推断]/[自定]` 或 `[观察]`：

| 文档 | 结论 |
|---|---|
| [docs/rpf9-findings.md](docs/rpf9-findings.md) | RPF9 目录表加密且公开工具不解；载荷可扫描；贴图名字来自载荷内部 |
| [docs/shader-corpus.md](docs/shader-corpus.md) | 21,522 条着色器：**mesh shader 34.8%、光追 64 个、绑定槽 p99=98** |
| [docs/d3d12xs-evidence.md](docs/d3d12xs-evidence.md) | D3D12.x 与桌面 D3D12 的差异面与哪些能用转发解决 |
| [docs/xmem-evidence.md](docs/xmem-evidence.md) | XMem 语义的 13 条断言、来源与可信度 |
| [docs/anyps5-notes.md](docs/anyps5-notes.md) | 同类项目（AnyPS5）的分层方式与里程碑教训 |
| [docs/gdk_api_surface.md](docs/gdk_api_surface.md) | 包内 16 个 DLL 的导入/导出面（66 个模块，只有 3 个主机专有） |

三个最有用的数字：

- 包内 DLL 的 66 个导入模块里，**只有 3 个是主机专有**（`d3d12_xs`、`xmem`、`XAudio2_9`）——
  所以"做 Xbox 系统"实际是把少数几个模块的语义补上，不是重写 OS
- 着色器 **34.8% 是 mesh shader**、**64 个 BVH64 光追** —— PC 侧必须有相应能力，不能降级
- 绑定槽 **p99 = 98** —— bindless 式设计，`t4_bindless` 已验证 98 槽动态索引可用

---

## 已知的墙

| 墙 | 性质 |
|---|---|
| 缺 `game_scarlett_master.exe` | 决定性。没有它，shim 再完美也没有可运行的输入 |
| RPF9 目录表加密 | 阻塞"我们"按路径组织资产；不阻塞游戏（游戏自带解码器） |
| `xmem.h` 在 NDA 内 | 签名靠推定，是 ABI 静默错误的最大来源 |
| `ID3D12DeviceX` 等主机接口 | IID 在 NDA 内，包装层最多做到 `ID3D12Device` |

---

## 仓库结构

```
Xbgtasix/
  build.sh                     MinGW 构建（合并手写导出 + 生成导出 + dxc 编 shader）
  include/xbgtasix/trace.h     日志 / 未实现策略
  src/xmem/                    XMem* → VirtualAlloc（64KB 页语义）
  src/xgameruntime/            手写实现 + exports.txt
  src/d3d12_xs/                手写 + forward.def（14 个原生转发）
  src/generated/               482 个桩（gen_stubs.py 生成，勿手改）
  tests/t0_hello/              初始化 + XMem + 设备
  tests/t1_triangle/           L3：root sig + PSO + draw + 像素校验
  tests/t2_textured/           L4：描述符表 + CBV + SRV + 采样器 + 深度
  tests/t3_xmem/               内存行为回归 + 压力
  tests/t4_bindless/           L5：98 槽描述符表 + 动态索引 + 64K 大堆
  tools/gen_stubs.py           .lib → 全量导出桩
  tools/forward_shim.py        DLL → 全量导出转发 .def
  tools/find_gdk.py            扫盘定位 GDK
  tools/packaging/             针对游戏数据包的探针与统计（rpf9_probe / shader_stats / pe_surface）
  docs/                        证据表与结论
```

---

## 依赖

- **MinGW-w64**（默认取 `D:/mingw64`，可用 `CXX=` 覆盖）
- **Microsoft GDK** 解出的头文件与导入库，放在仓库旁的 `../gdk/`：
  - `../gdk/include_clean/`（255 个头文件）
  - `../gdk/lib/xgameruntime.lib`
  - 可用 `tools/find_gdk.py` 定位；或从官方安装包直接抽（见 README 历史提交里的 `extract_gdk.py` 方案）
  - 环境变量 `GDK_INC` 可覆盖
- **Vulkan SDK** 的 `dxc.exe`（默认 `C:/VulkanSDK/1.4.363.1/Bin/dxc.exe`，可用 `DXC=` 覆盖）

> 微软的头/库不入库，各自本地解出。

---

## 法律与范围

- 本仓库只包含自写代码、自写测试与观察结论
- 不包含：游戏资产、游戏 exe、解密密钥、微软的头文件/库
- 数据面工具（`tools/packaging/`）只读取使用者本地已有的文件，用于格式研究与互操作
- 目标仍受制于缺失的 exe；本项目不提供获取它的方法
