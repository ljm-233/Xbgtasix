# D3D12.x（Xbox）与 D3D12（桌面）差异表

目标：搞清楚 `d3d12_xs.dll` 到底要提供什么，以及哪些差异靠"转发"就能解决、哪些不能。
等级：**[事实]** 公开原文 · **[观察]** 从包内文件实测 · **[推断]** 合理推导 · **[未知]** 查不到

## 已确认的事实

| # | 事实 | 依据 | 等级 |
|---|---|---|---|
| 1 | Xbox 上 GDK **只支持 D3D12.x**，不支持 D3D11.x；PC 上两者都支持 | [GDK 介绍页](https://learn.microsoft.com/en-us/gaming/gdk/docs/gdk-dev/intro/introduction?view=gdk-2610) | 事实 |
| 2 | D3D12.x 是"D3D12 + 主机扩展"，且 GDK 的 D3D12.x 与 Xbox One ERA 的 D3D12.x **只有很小差异** | 同上 | 事实 |
| 3 | 面向 Xbox 编译时链接的是 `d3d12_x.lib` + `xgameplatform.lib` + `xgameruntime.lib`，并加 `/NODEFAULTLIB` | 同上 | 事实 |
| 4 | D3D12.x 的参考文档在 `xbox-nda-docs` 下 | 介绍页链接指向 `/gaming/xbox-nda-docs/.../d3d12_x/` | 事实 |
| 5 | 图形内存按 **64 KB / 2 MB 页**管理，`XMemVirtualAlloc` 是新的显存分配 API | devdocs《XBOX Series X\|S 图形内存概述》 | 事实 |
| 6 | 包内 16 个 DLL 只从 `d3d12_x.dll` 导入 **2 个**函数：`D3D12SerializeVersionedRootSignature`、`D3D12CreateRootSignatureDeserializer` | [gdk_api_surface.md](gdk_api_surface.md) | 观察 |
| 7 | 系统 `d3d12.dll` 只有 **19 个**导出（真正实现在 `d3d12core.dll`） | 实测 dump | 观察 |

## 差异面清单

| 面 | 差异 | 我们的处理 | 风险 |
|---|---|---|---|
| **导出表** | `d3d12_x.dll` 应是 `d3d12.dll` 的超集（同名 + `X` 变体） | **全量转发**（见下） | 若真有 `X` 独有导出，名字对不上就解析失败 |
| **设备创建** | 主机上 `D3D12CreateDevice` 的 adapter 参数/返回接口可能不同 | 手写转发 + 日志，参数原样透传 | 若 xs 版签名不同，参数会错位 → 静默错乱 |
| **Residency** | 主机内存是固定物理分区，`MakeResident`/`Evict` 大概是记账而非真实换页 | 未处理（转发到桌面实现） | 桌面驱动会按自己的预算驱逐，可能导致纹理丢失/花屏 |
| **Bindless / 描述符上限** | 主机的描述符堆上限未公开 | 未处理 | 超过桌面上限时 `CreateDescriptorHeap` 返回失败 |
| **着色器 blob** | 主机可能发行**非标准 DXIL** 的预编译着色器 | 未处理 | 转发给桌面驱动会直接拒绝 → 这一步是硬墙 |
| **特性查询** | `CheckFeatureSupport` 的返回值在主机上不同 | 未处理 | 游戏可能走进它以为存在、实际没有的路径 |
| **内存分配** | 显存通过 `XMemAlloc` / `XMemVirtualAlloc` 走 64KB/2MB 页 | `xmem.dll` 已实现（见 [xmem-evidence.md](xmem-evidence.md)） | 对齐/页大小口径 |

## 已完成：全量转发

`d3d12_xs.dll` 现在有 **18 个导出**：

- **4 个手写**（带日志）：`D3D12CreateDevice`、`D3D12SerializeVersionedRootSignature`、
  `D3D12CreateRootSignatureDeserializer`、`D3D12GetDebugInterface`
- **14 个转发**：其余全部原样转发到 `d3d12.dll`，例如
  `D3D12CoreCreateLayeredDevice`、`D3D12CreateVersionedRootSignatureDeserializer`、
  `D3D12EnableExperimentalFeatures`、`D3D12GetInterface`

转发由 PE 加载器原生解析（`Forwarder RVA`），零运行时开销、零手写代码。
生成方式：`python tools/forward_shim.py /c/Windows/System32/d3d12.dll --dll-name d3d12_xs --target d3d12 --out src/d3d12_xs/forward.def --exclude <手写的4个>`。

这样任何 GDK 程序对 `d3d12_xs.dll` 的导入都能解析到，不会因为"缺导出"而加载失败 ——
这是"先让它能启动"的必要条件。

## 转发解决不了的：需要拦截方法调用

`D3D12CreateDevice` 返回的是 **COM 接口指针**，之后所有调用都走虚表，转发层看不见。
所以下面这些只能靠**包装设备对象**（自己实现 `ID3D12Device`/`ID3D12DeviceX` 的代理，
把方法转发给真对象并顺手做记账）：

- Residency 记账（`MakeResident` / `Evict` / `SetResidencyPriority`）
- 描述符堆创建时的上限检查（超过桌面限制就提前报错，而不是等崩溃）
- `CheckFeatureSupport` 的取值改写（把主机特性表映射到桌面）

这条路的前提是知道 `ID3D12DeviceX` 的 **IID 和虚表布局** —— 那是 NDA 内容。
没有它，代理只能实现到 `ID3D12Device` 为止，游戏 `QueryInterface` 一个 X 接口就会拿到
`E_NOINTERFACE`。**这就是下一堵墙。**

## 2026-11-24：启动实测更新

- **[观察] 槽 26：** `GetCustomHeapProperties` 的主机调用将返回缓冲区放在
  `RCX`，设备指针放在 `RDX`，与桌面虚方法相反。设备代理交换这两个参数后，
  诊断运行越过了原来的 `D3D12Core.dll+0x2A0EE` 访问冲突。
- **[观察] 槽 48：** 游戏以 `IID_ID3D12CommandQueue` 调用此槽；桌面 vtable
  的同一槽实际是 `ID3D12Device3::OpenExistingHeapFromAddress`，不是
  `CreateQueryHeap`。设备代理仅对队列 IID 转到桌面 `CreateCommandQueue`。
  对实测计算队列的 `0x2`、`0x3` 标志清除桌面不接受的 `0x2` 位；
  该主机位的确切语义仍未知。这不是通用的主机标志转换。
- **[观察] 验证：** WARP 和 RTX 5060 Laptop 上，`t41` 均成功创建四条真实队列、
  提交栅栏信号，并验证了桌面堆路径。诊断启动也确认四次队列创建均返回 `S_OK`，
  越过 `game+0x2478826`；独立桌面交换链测试 `t31` 通过，但游戏仍未确认首帧。
- **[观察/推断] 当前边界：** `game+0x2478B9E` 访问冲突前，游戏通过设备槽 73
  传入描述符、命令分配器与扩展 IID，随后在输出对象上调用槽 132、131。桌面设备槽 73
  是 `CreateShaderCacheSession`，签名和输出位置不匹配；后续调用形态表明该对象很可能
  是主机扩展命令列表，但两个扩展方法的功能尚未验证。不能用空实现或伪造成功替代。

### 2026-11-24：槽 73 隔离复测

- **旧 DLL（03:03:48）**：最新旧版转储的 `game+0x2478B9E` 是 `call [rax+0x420]`。
  `[rsp+0x60]` 应由槽 73 写入命令列表，实际仍指向栈 `0x3a452ff290`；
  该地址内的第一个值是 `6`，于是槽 132 的间接调用读了 `0x426`。这是输出参数未按 Xbox
  ABI 写入，不是已经获得的有效对象指针被截断。
- **诊断 DLL（03:12:51）**：Xbox 设备代理不再把槽 73 转发到桌面的着色器缓存接口。
  它记录描述符 `{0,2,1,0}`，清空第六个参数指向的输出位置并返回 `E_NOTIMPL`
  (`0x80004001`)。四次队列创建仍成功；游戏在检查此 HRESULT 时触发 `int3`
  (`game+0x2478B7B`, `0x80000003`)，没有再走到旧的非法指针读取，也没有确认首帧。
  `t41` 在 WARP 和 RTX 5060 Laptop 上验证了图形/计算两种输入、失败码及输出边界；
  桌面路径的 `t13`、`t31`、`t32` 通过。诊断 DLL 未部署到正式运行目录。
  日志：`build/slot73-abi-guard-20261124-031159/game.log`。
- **实际适配仍待完成**：`ivall/xcl` 的 `HANDOFF.md`（`956a476`）印证了扩展列表 IID
  `c3827890-4024-474c-98cf-645b51dc4874`，但同样把列表槽 131/132 标为桩。
  独立桌面探针只证实可以创建、关闭和重置真实图形/计算命令列表，不代表这些扩展槽兼容。
  下一步是校验 Xbox 描述符的字段含义和这两个方法的语义，再提供正确虚表和引用计数的代理，
  用真实队列提交加栅栏验证前进，并在游戏中以成功的首帧 `Present` 作为验收。


### 2026-11-24：槽 73 桌面后端

- **已实现**：槽 73 只在明确请求桌面 `ID3D12GraphicsCommandList` 时返回真实 PC 命令列表。
  接受实测的 `{0,2,1,0}`（DIRECT）和 `{2,2,1,0}`（COMPUTE）。第 1、3 号 DWORD（从 0
  开始）的语义未确定，其他值不接受；单节点 PC 使用 node mask `0`。原生设备入口不改。
  请求 Xbox 扩展 IID 时仍返回 `E_NOTIMPL`；不会把桌面对象交给游戏。
- **实测**：WARP 与 RTX 5060 Laptop 两种适配器上，DIRECT 和 COMPUTE 列表分别完成两次
  `CopyBufferRegion`、提交、栅栏等待及 CPU 回读比对，第二次先重置分配器和列表。
  `t41` 也检验无效描述符和 Xbox IID 拦截；`t13`、`t31`、`t32` 通过。
  日志在 `build/slot73-desktop-backend-20261124-031954/`。本轮未再次启动游戏，未部署该 DLL。
- **扩展槽线索**：反汇编确认槽 131 的启动调用传入 `0xc3c0`（DIRECT）/ `0xc380`（COMPUTE），
  另一个调用点 `game+0x1560994` 传入 `0xc280`，同样以 `0xffffffffff00` 为第五个参数。
  三个编号与 AMD GPUOpen PAL 的 `gfx9_plus_merged_offset.h`（`c5e8000`）中
  `mmDB_OCCLUSION_COUNT0_LOW`、`mmTA_CS_BC_BASE_ADDR`、`mmPA_SU_LINE_STIPPLE_VALUE` 的编号一致，
  这只是一条寄存器操作线索，尚不能确定 Xbox 上的真实映射、掩码含义或槽 132 的行为。
  不应按这些编号向 PC 显卡直接写寄存器；应先确定可观测效果，再用桌面 D3D12 语义实现。

## 早期判断（以下为静态阶段记录）

- **能做的已做完**：导出面全覆盖，加载不会因缺符号失败
- **需要观察的**：真程序启动后会依次撞上 residency → 描述符上限 → 着色器格式 → `ID3D12DeviceX`
- **最先要预判的**：如果泄露包里的着色器是主机专有格式而非标准 DXIL，那么即使前面全过，
  `CreateGraphicsPipelineState` 也会失败。

### 已做的一次探测（2026-11-22）

对 `shaders.rpf`（411 MB）做原始字节扫描，找 DXBC/DXIL 容器魔数：

```
DXBC  count=0    DXIL  count=0    SHDR  count=0    RDEF  count=0
```

同时测了四个 RPF9 包的统计：

| 文件 | 熵（bits/byte） | 可打印字符占比 |
|---|---|---|
| common.rpf | 6.42 | 0.255 |
| script.rpf | 7.83 | 0.334 |
| shaders.rpf | 7.59 | 0.327 |
| appdata0.rpf | 6.36 | 0.258 |

参考值：随机数据熵 ≈8.00、可打印 ≈0.37；纯文本熵 ≈4.5、可打印 ≈0.99。

**结论**：找不到任何明文的着色器魔数，也找不到明文的文件名 TOC。这与社区说法
（"缺失的 exe 里带着解密密钥"）一致 —— 数据很可能是加密/压缩存储的。
但这只是**字节级旁证**，不是定论：RPF9 的 TOC 可能在压缩后的块里。

下一步验证方式：用 `rpf9-explorer`（第三方工具，不在本仓库内） 打开 `common.rpf` 试列目录。
列得出来 → 只是压缩；列不出来 → 加密，资产路线也得等密钥。

### 2026-11-24：走到 PresentX 之后的断言（`game+0x247AF2D`）

真实 EXE 在槽 127/25/19 都返回成功之后，停在 `0x80000003`（int3）。两次运行同一地址，
不是竞态。

**现场**

```
XBGTASIX_VEH=1 XBGTASIX_LOG=build/real-veh.log ./game_scarlett_master.exe
[xbgtasix] FATAL 异常 0x80000003 at game_scarlett_master.exe+0x247AF2D
```

复现前后最后几条 D3D12 调用是：槽 25 `GetResourceAllocationInfo`（`SizeInBytes=33423360`
= 3840x2160x4，`Alignment=0x10000`）-> 槽 127 `CreateCommittedResource3`
（`flags 0x300005 -> 0x5` 剥离主机位）-> 槽 19 `CreateUnorderedAccessView`。
也就是说**两块 4K 资源已经建出来了**，断言发生在它们之后。

**xref 结果**（`python tools/xref.py <exe> --addr 0x247AF2D --depth 1`）

| 项 | 值 |
|---|---|
| 断言所在函数 | `0x247AD10..0x247AF58`（长度 `0x248`），自身**不引用任何导入** |
| 调用者 | `0x2478440..0x2479682`（长度 `0x1242`），单调用点 `0x247942B(call)` |
| 调用者引用的导入 | `XboxBvhBuilder.dll!??0XboxBvhBuilder@@QEAA@PEAUID3D12Device11@@_N@Z`（**构造函数，参数是 ID3D12Device11**）、`d3d12_xs.dll!D3D12XboxCreateDevice`、`CreateWindowExA`、`RegisterClassExA`、`ShowWindow`、`xmem.dll!XMemVirtualAlloc`、`VCRUNTIME140.dll!memset` |
| 调用者里的字符串 | `sgaWindowClass` |

**读法** [推断]：这一段是**"建窗口 + 构造 XboxBvhBuilder"的初始化**，断言紧跟在
`XboxBvhBuilder` 构造之后。所以下一堵墙不在渲染循环，而在**光追加速结构的构建器**：
它要一个 `ID3D12Device11`，用我们的设备代理构造，然后某个返回值得不到它期望的东西。

**下一步的三条具体检查**

1. `XboxBvhBuilder` 的构造函数是否真的成功？我们的设备代理已经能被 `QueryInterface`
   成 `ID3D12Device11`（`t41` 断言过），但主机那份 DLL 可能还会查它自己的扩展 IID。
   `XBGTASIX_SLOTPROBE=1` 跑一次，看构造期间进了哪些槽。
2. 窗口类 `sgaWindowClass` 注册/创建是否成功——这一段同时在建窗口，`CreateWindowExA`
   拿到 nullptr 也会让后面的断言炸，看上去像图形问题。
3. 值得直接借的实测（[leonida-crossref](leonida-crossref.md) 4.5 节）：
   `XboxBvhBuilder.dll` 的 rehydrate 路径把各 buffer 当 **CPU 指针**用、**不发设备调用**，
   所以它本身能在 PC 上跑；参考实现用"层的 XDXR builder 顶替这个 DLL"（Leonida PR #7516），
   并记下了 `BATCHED_DESC2`（0x50 B）与 BVH4 头的完整布局。

**复跑**

```bash
cd "D:/GTA6/GTA6.Xbox.v1.0.2.1-CYBERLEEK"
XBGTASIX_VEH=1 XBGTASIX_LOG="D:/GTA6/Xbgtasix/build/real-veh.log" ./game_scarlett_master.exe
grep -a "FATAL" "D:/GTA6/Xbgtasix/build/real-veh.log"
python ../Xbgtasix/tools/xref.py ./game_scarlett_master.exe --addr 0x247AF2D --depth 1
```

（`XBGTASIX_VEH=1` 是必须的：默认不开异常钩子时，int3 只表现为进程以 `0x80000003` 退出，
日志里什么都没有。这次就是靠它才拿到地址。）

#### 补测：槽位探针 + 断言条件的反汇编

**槽位探针**（`XBGTASIX_SLOTPROBE=1`，构造期间进过的槽，各记一次）：
`36`（建栅栏）、`9`（堆起始句柄）、`15`（建根签名）、`18`/`17`/`20`（SRV/CBV/RTV 视图）。
**没有**出现"假成功的扩展槽"——说明构造 `XboxBvhBuilder` 的那段在设备侧是走通的，
断言不是被假 S_OK 骗出来的。

**栈**（VEH 扫描）：

```
#4 game+0x247AF2D   ← 断言点（int3）
#5 game+0x2479430   ← 与 xref 的调用点 0x247942B(call) 吻合
#6 game+0x19F4F54   ← 旧文档里的 0x19F4F58 断言就在这几条指令内
```

`#6` 这次只是**返回地址**，不是断言点——那条老墙已经翻过去了。

**断言条件**（`tools/rva_bytes.py <exe> 0x247AEE0 0x78 --disasm`）：

```asm
movq  0x78(%rsp), %rax        ; 48 8b 44 24 78
subq  0x70(%rsp), %rax        ; 48 2b 44 24 70
addq  0x516c0cb(%rip), %rax   ; 48 03 05 cb c0 16 05   <- 加一个全局
cmpq  $0x1FFFFF, %rax         ; 48 3d ff ff 1f 00
jbe   1                       ; 76 01  <- 跳过下面那一个字节
int3                          ; cc
```

即断言要求 **`([rsp+0x78] - [rsp+0x70] + G) <= 0x1FFFFF`（2 MB - 1）**，
而崩溃瞬间 `rax = 0x4000000`（**64 MiB**）。

两种读法 [推断]，都指向同一个方向：

1. `[rsp+0x78] - [rsp+0x70]` 是两个地址之差（一段跨度/大小），加上全局基址 G 之后
   仍要求落在 2 MB 以内 —— 也就是**这段必须在 2 MB 大页之内**；
2. 64 MiB 恰好是 **32 × 2 MB**，而 `xmem` 的大页路径（`XMEM_LARGE_PAGES`，
   要求 2 MB 对齐、物理页数按 32 的倍数）正好是这个量级 —— 我们返回的区域跨度
   可能比标题期望的大了一个数量级。

**下一步最省事的两个动作**

1. 在 VEH 的现场 dump 里补上 `[rsp+0x60..0x80]` 这几个 qword —— 一眼就能看到
   `[rsp+0x70]`/`[rsp+0x78]` 到底是什么（现在只有寄存器 + rdi 区域，问不出这个）；
2. 对 `XMemVirtualAlloc` 走大页分支时，记录**请求 size / 返回地址 / 跨度**，
   看标题拿到的是不是 64 MiB 跨度、以及它期望的 2 MB 窗口是哪一段。

#### 补测二：断言函数自己的栈帧（`[rsp+0x70]` / `[rsp+0x78]`）

`src/trace.cpp` 的现场 dump 现在多一行**故障指令自己的栈窗口**（用
`ContextRecord->Rsp`，不是 VEH 当时的 rsp；逐 qword 做 `VirtualQuery`，读到未提交页就停）。
跑一次就拿到了断言真正在比的两个值：

```
[xbgtasix] 故障栈帧 +0x00=... +0x30=1bc00000080 +0x38=7ff700000000 +0x40=0 +0x48=8
                     +0x50=108700f000020 +0x58=1008000100ff1c01
                     +0x60=0 +0x68=0 +0x70=0 +0x78=4000000
                     +0x80=2 +0x88=1 +0x90=1 +0x98=7ff7001c0400
[xbgtasix] 现场寄存器 rax=0000000004000000 ... rsi=0000000000000f84
```

代入前面那六条指令：

```
rax = [rsp+0x78] - [rsp+0x70] + G = 0x4000000 - 0 + G = 0x4000000   （即 G=0）
cmp 0x1FFFFF / jbe -> 失败
```

**结论 [事实]：标题在检查"某个跨度/大小 ≤ 2 MB - 1"，而我们这条路径上它是 64 MiB。**

**64 MiB 是哪来的？两个算术都精确命中，目前无法区分：**

| 候选 | 算式 | 依据 |
|---|---|---|
| A. 4096 项 DSV 堆 × 16 KB/项 | `4096 × 16384 = 0x4000000` | 崩溃前刚建过 `type=2 descriptors=4096` 的堆（日志第 2421 行） |
| B. 4096×4096×4 缓冲 | `4096² × 4 = 0x4000000` | 4K 渲染阶段很自然的一块 RGBA/深度靶 |

**已经排除**：不是 `XMemVirtualAlloc` 出来的。日志里每次分配都打了 size，
没有 64 MiB 那一条（最大的是 2.15 GB / 0.58 GB / 0.55 GB，以及物理页 11360 页）。

**下一步的两个判据**

1. 在 `CreateDescriptorHeap` 的日志里带上 `GetDescriptorHandleIncrementSize(type)` ——
   如果这个驱动给 DSV 报 16 KB，候选 A 立刻成立；若是 32 B，A 出局。
2. 在槽 25 `GetResourceAllocationInfo` 的日志里保留完整 desc —— 如果后面出现
   `4096×4096` 的请求，候选 B 成立。当前日志里只有 `3840×2160`（`SizeInBytes=33423360`）。

顺带一条：`rsi = 0xF84`，VEH 把它打印成"对象偏移"。如果这是断言辅助函数的
行号/偏移参数，它说明这条检查在标题源码里的位置，值得和候选 A/B 一起对照。
