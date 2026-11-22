# Xbgtasix — Xbox GDK 兼容层骨架（Windows 原生，L0–L4 已跑通）

在普通 Windows 上，让导入 `xgameruntime.dll` / `xmem.dll` / `d3d12_xs.dll` 的程序启动，
并建出真实 D3D12 设备。

## 为什么 Windows 上不需要自己写 PE loader

Windows 加载器本身就是 PE loader。**把同名 shim DLL 放在 exe 旁边**，导入解析、重定位、
TLS、SEH 全部由系统完成。要写的只是"这些 DLL 里该有什么"。

## GDK 是怎么装进去的（没跑安装器）

本机 MSI 服务不可用（`msiexec` 返回 1601），所以直接拆包：

1. `GDK_2610.0.9661.zip` → `gdk_setup/`（内含 `PGDK.exe` + 56 个 `Installers/*`）
2. `gdk_setup/dump_msi.ps1`：用 `WindowsInstaller.Installer` COM 读五个 MSI 的
   `File / Media / Component / Directory` 表 → `msi_json/`（COM 读库不需要 MSI 服务）
3. `gdk_setup/extract_gdk.py`：按 `Media.lastSequence` 把 Sequence 映射到 cabinet，
   再用系统自带 `expand.exe -F:<成员名>` 精确抽取，重命名为真实文件名 → `gdk/include_clean/`、`gdk/lib/`

结果：**255 个头文件 + xgameruntime.lib / xgameruntime.thunks.lib**。

- `XGameRuntime.h`、`XTaskQueue.h`、`XUser.h`、`XStore.h`、`XSystem.h`、`XGame.h` 等全部拿到
- **`xmem.h` 不存在**：XMem 属于 NDA 范围（[官方文档页](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xmem/functions/xmemalloc?view=gdk-2610)要求 Xbox 开发者协议），公开 GDK 不含它
  → `src/xmem` 的签名仍是 PROVISIONAL

## 构建

```bash
./build.sh        # MinGW64 + g++；CMake 在本机会卡在编译器探测，故不用
```

产物：`build/{xgameruntime,xmem,d3d12_xs}.dll` + `build/t0_hello.exe`

注意：MinGW 的 g++ 不认 MSYS 风格 `/d/...` 路径，`build.sh` 用 `cygpath -m` 转成 `D:/...`。

## 三个 DLL 的现状

| DLL | 内容 | 状态 |
|---|---|---|
| `xgameruntime.dll` | **10 个手写实现**（签名由 GDK 头文件强制）+ **482 个生成桩**，共 492 个导出 | 手写的走真逻辑/明确失败；其余一被调用就打印名字并退出 |
| `xmem.dll` | `XMemAlloc/Free/VirtualQuery/GetWorkingSetStatistics` | VirtualAlloc + 对齐 + 统计；签名 PROVISIONAL（无头文件） |
| `d3d12_xs.dll` | **18 个导出**：4 个手写（带日志）+ 14 个原生转发到 `System32\d3d12.dll` | 已验证能建出真设备 |

生成了一个桩就少一个"未知失败"：现在任何 GDK 程序导入的名字都能解析到，
第一个缺的东西会以 `[xbgtasix] UNIMPLEMENTED: <名字>` 出现在 `xbgtasix.log`。

## 验证记录（本机）

```
[t0] t0_hello start (smoke=1) pid=34244
[xgamert] XGameRuntimeInitialize()
[t0] xmem roundtrip ok
[d3d12_xs] real d3d12 -> C:\Windows\system32\d3d12.dll
[d3d12_xs] D3D12CreateDevice(feature_level=0xb000)
[t0] device ok, adapter luid=00000000:00014a65, nodes=1
[t0] smoke done with 0 failure(s)
```

L3（t1_triangle，离屏渲染 + 像素回读，无桌面也能验证）：

```
[t1] start, offscreen 256x256
[xgamert] XGameRuntimeInitialize()
[d3d12_xs] real d3d12 -> C:\Windows\system32\d3d12.dll
[t1] center pixel RGBA = (106,131,143,255), clear R = 26
[t1] PASS: 三角形画上去了
```

链路：root signature → PSO（dxc 编译的 DXIL）→ RTV → DrawInstanced(3) → barrier →
CopyTextureRegion → Map 读中心像素。**像素对得上，说明整条管线真的通了**，不是"没报错"。

L4（t2_textured，纹理 + CBV + 深度，同一场景渲两次比对）：

```
[t2] tint=1.0 -> (240,60,40,255)
[t2] tint=0.5 -> (120,30,20,255)
[t2] PASS: 纹理采样 + CBV 调制 + 深度都生效
```

`(240,60,40)` 与棋盘纹理源数据**逐位相同**，说明采样器、SRV 格式、描述符表绑定全对；
0.5 的 tint 精确得到一半，说明 CBV 数据通路正确；深度没把几何裁掉，说明 DSV/PSO 深度状态正确。

三个测试一起跑：

| 测试 | 覆盖 | 结果 |
|---|---|---|
| `t0_hello --smoke` | 加载 + XMem + 设备 | rc=0 |
| `t1_triangle` | root sig + PSO + draw + 像素回读 | PASS |
| `t2_textured` | 描述符表 + CBV + SRV + 采样器 + 深度 | PASS |
| `t3_xmem` | 对齐矩阵 / canary / 二次释放 / 10 万次 churn / 64MB / 8 线程 | 88 checks, 0 failures, 345 ms |

## XMem 行为契约（由 `t3_xmem` 锁住）

`xmem.h` 拿不到，所以把"我们选定的行为"写成断言，任何改动破坏它就红：

| 契约 | 值 |
|---|---|
| 对齐规则 | attributes 低 32 位若是 ≥64KB 的 2 的幂 → 用该值；否则 **64KB**（主机页粒度，也正好是 Windows 分配粒度） |
| 每块占用 | 向上取整到 **整数个 64KB** 并全部提交（1 字节分配也占 64KB，与主机一致） |
| `size == 0` | 返回 `nullptr` |
| 二次释放 / 野指针 | 忽略 + 记日志，**不改统计、不崩** |
| `XMemFree(nullptr)` | no-op |
| `TotalBytes` | 当前存活字节 |
| `PeakBytes` | 历史峰值 |
| `AllocationCount` | 存活块数 |
| `Reserved[0]` | 累计分配次数（我们自己的扩展，便于测泄漏） |
| `XMemVirtualQueryInfo.RegionSize` | ≥64KB 且是 64KB 的整数倍 |
| 释放后 `XMemVirtualQuery` | 返回 `MEM_FREE`（`state=0x10000`） |
| 线程安全 | mutex + 哈希表；8 线程并发已验证 |
| `XBGTASIX_XMEM_POISON=1` | free 时 `memset(0xDD)`，用于抓 use-after-free |

**性能注记**：每次 `XMemAlloc` 一次 `VirtualAlloc`，约 5 µs/次（6.6 万次 350 ms）。
真游戏有百万级小分配时这会偏慢，也更容易碎片化；等真程序压上来再考虑页池。

**证据表**：每条断言的来源与可信度在 [docs/xmem-evidence.md](docs/xmem-evidence.md)，
代码里对应位置也标了 `[事实]/[推断]/[自定]`。

窗口模式在无桌面会话里 `Present` 返回 `DXGI_ERROR_DEVICE_REMOVED (0x887A0005)`，属预期。

## 未实现 API 策略

`XBGTASIX_UNIMPL=stop`（默认）→ 打印调用名后 `ExitProcess(70)`；
`XBGTASIX_UNIMPL=log` → 只记录，用于铺开调用面。日志同时写 stderr 与 exe 同目录 `xbgtasix.log`。

## 生成桩

```bash
python tools/gen_stubs.py ../gdk/lib/xgameruntime.lib --dll xgameruntime \
    --out src/generated --exclude "$(paste -sd, <(grep -v '^;' src/xgameruntime/exports.txt))"
```

`gen_stubs.py` 自己解 COFF archive（MS 的链接器成员是大端，GNU 是小端，两种都试），
剥 `__imp_`、丢弃 C++ 装饰名与 `_guard_*` 伪符号，输出 `.def` + 桩源。

## 目录

```
xbgtasix/
  build.sh                     MinGW 构建（合并手写导出 + 生成导出 + dxc 编 shader）
  include/xbgtasix/trace.h       日志 / 未实现策略
  src/trace.cpp
  src/xmem/                    XMem* → VirtualAlloc（64KB 页语义）
  src/xgameruntime/            手写实现 + exports.txt
  src/d3d12_xs/                4 个手写 + forward.def（14 个原生转发）
  src/generated/               482 个桩（由 gen_stubs.py 生成，勿手改）
  tests/t0_hello/              初始化 + XMem + 设备（--smoke 无需桌面）
  tests/t1_triangle/           L3：root sig + PSO + draw + 回读像素校验
  tests/t2_textured/           L4：描述符表 + CBV + SRV + 采样器 + 深度
  tests/t3_xmem/               XMem 行为回归 + 压力（10 万次 churn / 8 线程）
  tools/gen_stubs.py           .lib → 全量导出桩
  tools/forward_shim.py        DLL → 全量导出转发 .def
  tools/find_gdk.py            扫盘定位 GDK
  docs/                        证据表与设计笔记（每条断言标了来源与可信度）
```

## 依赖

- MinGW-w64（`D:/mingw64`，构建脚本默认从该路径取 g++，可用 `CXX=` 覆盖）
- Microsoft GDK 解出来的头文件与 `xgameruntime.lib`，放在仓库旁的 `../gdk/`：
  - `../gdk/include_clean/`（255 个头文件）
  - `../gdk/lib/xgameruntime.lib`
  - 可用 `tools/find_gdk.py` 定位，或用 `gdk_setup/extract_gdk.py` 从官方安装包直接抽
  - 环境变量 `GDK_INC` 可覆盖头文件目录
- Vulkan SDK 的 `dxc.exe`（默认 `C:/VulkanSDK/1.4.363.1/Bin/dxc.exe`，可用 `DXC=` 覆盖）

> 这些微软的头/库**不入库**：各自本地解出来即可。
