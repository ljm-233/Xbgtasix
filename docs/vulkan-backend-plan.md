# Vulkan 后端可行性评估（评估稿，不含实现）

写于 2026-11-25 凌晨。任务来源：给 `d3d12_xs` 加 Vulkan 渲染能力，让项目在**有 Vulkan、没有 D3D12**
的环境（Linux / Wine）里直接出画面，而不是靠 `WINEDLLOVERRIDES="d3d12=n"` 转给 vkd3d-proton。

口径声明，先说清楚：

- **[实测]** = 本机跑出来的（出处写进表里）；**[源码]** = 从本仓库源码数出来的；
  **[文档]** = 引别人的文档（Leonida / palmetto / vkd3d）；**[推断]** = 由前两者推出、未直接验；
  **未统计** = 查不到，不填数字。
- 本轮**没有改任何实现代码**（`git status` 里唯一的改动是别人留下的 `src/xgameruntime/api_impl.cpp`，
  与本任务无关）。本轮**没有跑游戏**。
- 唯一的脚本产出是 `/tmp/dxil_scan2.py`（一次性核对 53 个 dump 的 DXIL 块），逻辑写在第 2.4 节。

---

## 一句话结论

**推荐 C（混合），但不是"先 A 再自写一整个 B"，而是"先 A、之后只按 A 暴露的具体缺口做点状替换"。**

理由只有三条，都能落到数字上：

1. 我们的 `d3d12_xs` 已经是"主机 ABI 隔离层"了 —— 它把 140 个主机设备槽、600 个命令列表槽、11 个
   描述符堆槽翻译/转发出去。**缝在正确的位置**：底下只需要一个"合格的桌面 D3D12 设备"，
   而 vkd3d-proton 就是这个东西（本机已构建好，见 2.1）。
2. 自写最小 Vulkan 后端的成本有 90% 压在 **DXIL→SPIR-V 编译器**上。这一块 vkd3d 已经做完
   （`libs/vkd3d-shader` 6,008 行 + `dxil-spirv` subproject），而我们的语料确定是 DXIL SM 6.0
   （`docs/transient-block-wall.md` §28，本轮另做了独立复核，见 2.4）。自己重写这一段是几个月的活，
   换来的只是"不依赖外部库"。
3. 现有 E2E 死在 **`DEVICE_REMOVED`**，且连 WARP（软件驱动）也丢设备
   （`docs/transient-block-wall.md` §30）。Vulkan 路线走的是**完全另一套驱动栈**
   （Linux 的 Mesa / NVIDIA-on-Linux），是眼下唯一能在"Windows 驱动这条死路"之外另开一路的选项。
   这不是"顺手加个功能"，这是**换一条能验证的水路**。

代价也说明白：A 路线**不解决**任何当前卡点，它只把"我们有没有能力在 Vulkan 上出一帧"这个问题
从"要写 6 千到 1 万 5 千行"降成"今天就能验"。

---

## 一、现有 `d3d12_xs` 的 API 覆盖面

### 1.1 源码规模 [源码]

```
src/d3d12_xs/                    3,714 行（含 .txt/.def）
  device_abi.h                   2,277   设备/队列/命令列表三套代理 + 主机扩展桩
  d3d12_xs.cpp                     430   5 个导出入口（CreateDevice 等）
  root_sig.h                       375   根签名 blob 主机位剥离（序列化端 + 消费端）
  descriptor_heap_abi.h            275   描述符堆代理 + 句柄翻译表
  present_x.h                      238   PresentX → 自建 HWND + DXGI flip 交换链
  frame_events.h                    95   78/79/80 帧事件的软件时钟
```

导出面：19 个源导出 → 13 个 PE 转发器（`d3d12.*`）+ 6 个手写（`exports.txt` 7 条含
`D3D12XboxCreateDevice`）。导出面**已全覆盖**，不会再因为缺符号加载失败（`docs/d3d12xs-evidence.md`）。

### 1.2 vtable 覆盖（这是工作量的分母）[源码]

| 对象 | 槽表大小 | 手写实现 | 同号转发到底层 | 明确失败桩（E_NOTIMPL / 记账） |
|---|---|---|---|---|
| 设备 `ID3D12DeviceX` | **140** | **27** | **34** | **79** |
| 命令列表 `ID3D12GraphicsCommandListX` | **600** | **8** | **72**（槽 < 80） | **520**（槽 ≥ 80，返回 S_OK） |
| 命令队列 `ID3D12CommandQueueX` | **64** | **5** | **59** | 0 |
| 描述符堆 | **11** | **11** | 0（全手写） | 0 |
| 根签名 | 3 个导出函数 + 1 条 blob 剥离规则 | — | — | — |

设备侧 27 个手写槽的确切名单（`device_slot<N>()` 枚举）[源码]：

```
0 1 2       IUnknown
8           CreateCommandQueueX（含 compute 队列 0x2 位的实测剥离）
12 16       CreateRootSignature / 转发（都挂剥离，主机槽号未定论）
14          CreateDescriptorHeap（含 1,245,184 项 → 1,000,000 的钳制）
19          CreateUnorderedAccessView
25          GetResourceAllocationInfo（清主机可见性位）
26          GetCustomHeapProperties（RCX/RDX 参数对调，实测）
48          OpenExistingHeapFromAddress / 队列第二入口（按 IID 分流）
55 60 61 62 66 67   主机 PSO 家族（62 已真实现；其余带 12 参 dump 的桩）
73          CreateCommandListX（实测 DIRECT {0,2,1,0} / COMPUTE {2,2,1,0}）
74 86 132   主机槽（74/132 返回 S_OK 试探；86 记 IID）
78 79 80 81 SetFrameIntervalX / ScheduleFrameEventX / WaitFrameEventX / GetFrameStatisticsX
127         CreateCommittedResource3（剥 0x300005 → 0x5）
133         CreateResourceFromAddress
```

三档分法的依据是源码注释里写明的规则：**槽 ≥ 44 一律当主机扩展**（`device_extension_stub_n`），
因为同号转发过一次实测直接崩在 `D3D12Core`（slot 70）。这条规则和
`docs/host-vtable-offset.md` 里"主机槽 = 桌面槽 + 4"的静态结论**互相矛盾且都未定论** ——
这是本项目最贵的一条开放问题，也是路线 A 唯一真正需要复核的地方（见 3.1 风险栏）。

### 1.3 游戏**实际调用**了哪些槽 [实测]

数据源：`build/dump3.log`（2026-11-24 14:17，Intel ARL 核显，已经走到 PSO 创建的那一跑，
275 KB / 3,966 条 d3d12_xs 日志）。计数用定点 grep，一行 = 一次调用（日志自身每次调用打一行）。

| 槽 | 语义（已确证 / 推断） | 调用次数 | 我们的状态 |
|---|---|---:|---|
| 8 | `CreateCommandQueueX` | 2 | 已实现 |
| 14 | `CreateDescriptorHeap` | 6 | 已实现（含钳制） |
| 16 | 根签名转发（落盘 17 个 `rs_*.bin`） | 18 | 已实现（剥 `0x03000000`） |
| 19 | `CreateUnorderedAccessView` | 8 | 已实现 |
| 25 | `GetResourceAllocationInfo` | 2 | 已实现 |
| 48 | `CreateCommandQueue`（第二入口） | 4 | 已实现 |
| 61 | 主机 PSO / samplerX（语义未定） | **1** | E_NOTIMPL 桩 |
| 62 | `CreateComputePipelineState` | **53** | 已实现（PSO 建成 53 个） |
| 70 | 主机描述符表（推断） | 3 | E_NOTIMPL 桩 |
| 73 | `CreateCommandListX` | **585** | 已实现 |
| 74 | 主机槽（返回值须非负） | 1 | S_OK 探测桩 |
| 78 | `SetFrameIntervalX` | 1 | 已实现 |
| 79 | `ScheduleFrameEventX` | 2 | 已实现 |
| 86 | 主机槽（带 IID 的创建型） | 6 | E_NOTIMPL 桩 |
| 127 | `CreateCommittedResource3` | 2 | 已实现 |
| 131 | 命令列表寄存器写（`0xc3c0`/`0xc380`/`0xc280`） | 5 | 只记日志（void） |
| 132 | 命令列表寄存器写 | 5 | 只记日志（void） |

**同一跑里 0 次的东西（重要）**：

```
PresentX（队列槽 30）     0
ExecuteCommandLists（队列槽 10）  0
Draw* / Dispatch*         0
CreateFence（槽 36）      0
RTV / DSV（槽 20/21）     0
描述符步进（槽 15）       0
```

探针跑（`XBGTASIX_SLOTPROBE=1`，`build/real-probe.log` / `build/sp2.log`）另外看到
**7 / 9 / 15 / 17 / 18 / 20 / 27 / 36 / 39**（各 1 次，构造期一次性路径）。

**合计**：140 个设备槽里，把所有跑法加在一起，**观测到 24 个不同槽位**
（18 个在正式跑里，6 个只在探针跑里）。也就是 **17%**。

### 1.4 必需 / 可 stub / 未统计 的三档

**必须（进第一帧绕不过）** —— 由"已实测调用"∩"功能不可省"取交：

```
设备  8 14 16 19 20 21 25 26 36 48 73 127 133
描述符堆 9 10（起始句柄，走 RAX）
命令列表 9 10 28（Close/Reset/SetDescriptorHeaps）+ 131 132（寄存器写，void 语义可吞）
队列  10 30（ExecuteCommandLists / PresentX）
```
外加**尚未被调用但第一帧一定需要**的：`Draw*`、`Clear*View`、`ResourceBarrier`、
`OMSetRenderTargets`、`SetGraphicsRootDescriptorTable`。后五项本机 **0 次观测**，
清单只能引 palmetto/leonida 的 M0–M3（`shader-xcheck/palmetto/docs/d3d12_vulkan.md` §6.1，
"Minimum API set for the first frame"）[文档]。

**可以 stub 着跑**（现在的状态就是证据）：`61`（19 行日志、1 次调用，返回 E_NOTIMPL 后游戏继续）、
`70`（返回 E_NOTIMPL 会把 BVH 初始化推进默认值 `{0, 64 MiB}`）、`74`、`86`、`132`。
其中 `70` 是**已知的有损 stub**：它的 E_NOTIMPL 正是 64 MiB 断言的直接成因
（`docs/d3d12xs-evidence.md` §补测三，A/B 两跑唯一差别就是它）。

**未统计**：

- 绘制循环真实用到哪些槽 —— **0 次观测**，游戏从没走到过任何 `Draw`/`Present`。
- 一次真实 `PresentX` 的参数形态 —— 0 次。
- 命令列表高位槽（80–599）里哪些是活代码 —— 只见到 131/132 两个，其余 520 个是桩，
  从没被调用过。

---

## 二、三份现成实现能复用多少

### 2.1 vkd3d-proton 3.2.0（`vkd3d-src/`，只读）

| 项 | 值 | 出处 |
|---|---|---|
| License | **LGPL-2.1**（or later） | `vkd3d-src/COPYING` |
| 版本 | 3.2.0 | `meson.build` `project(...)` |
| 规模 | `libs/vkd3d` **103,388** 行；`libs/vkd3d-shader` **6,008** 行；另有 `subprojects/dxil-spirv` | `wc -l` |
| **已构建产物** | `build/libs/d3d12/d3d12.dll`（624,441 B）+ `build/libs/d3d12core/d3d12core.dll`（20,882,864 B），PE32+ x86-64 | `file` |
| 构建工具链 | `x86_64-w64-mingw32-gcc 16.2.0` —— **和我们 build.sh 同一套** | `build/meson-logs/meson-log.txt` |
| 覆盖 | 完整桌面 D3D12：资源/堆/描述符/根签名/PSO/命令列表/队列/栅栏/查询/光追/mesh shader | 上游能力，未逐项复核 |
| 关键可复用件 | `vkd3d_shader_compile_dxbc()`（吃 DXBC 容器，内含 DXIL 块 → SPIR-V）、`vkd3d_shader_convert_root_signature()` | `include/vkd3d_shader.h:1050,1071` |

**能不能用**：能，而且**现在就能用**。LGPL-2.1 作为独立 DLL 被我们的 GPL-3.0-or-later 程序按名字
加载（PE 转发器 / `LoadLibrary`）是教科书用法，不传染；即便静态链，LGPL-2.1 §3 与 GPLv3 的组合
也是标准允许的。**两个 DLL 已经在盘上，同工具链，不需要重新构建。**

**形状差多远**：`d3d12.dll` 就是"桌面 D3D12 设备"本身。而我们的 `d3d12_xs.dll` 是主机 ABI 的
代理层，只把**桌面存在的那部分**转发下去。两者的接缝是 `ID3D12Device` 的虚表本身 ——
**完全对得上**，不需要适配层。（唯一的未知是 1.2 节最后那条"主机槽 = 桌面槽 + 4"的争议。）

### 2.2 Leonida（`_leonida/`，只读）

| 项 | 值 |
|---|---|
| License | **GPL-3.0-or-later**（`README.md` §License；与本仓库同证） |
| 规模 | `rt_vk_device.cpp` 9,518 / `runtime_shims_d3d12_com.cpp` 2,069 / `runtime_shims_d3d12_xs_device.cpp` 1,852 / `runtime_src_command_list.h` 953 / `runtime_src_root_signature.cpp` 518 = **14,910 行** |
| 配套文档 | `docs/d3d12_vulkan.md` 4,179 行（含 M0–M7 里程碑、逐节对象模型映射） |
| 覆盖（自述） | 队列/命令列表/栅栏/描述符堆/committed-placed-reserved 资源/RTV/DSV/3D 目标/纹理视图/采样器/根签名/管线/查询堆/**draw 已在 Vulkan 上跑**；barrier、clear、upload、copy（含 BC）、resolve、predication、CPU 侧读写、遮挡/时间戳/管线统计查询 —— **"在一张 GPU 上验过"** |
| 明确未做（自述） | 命令列表尾部、描述符表 record 模型、mesh shader、ExecuteIndirect 的全部形态、光追 |

**能不能用**：license 允许（同证）。**但形状差得远**：

| | 本项目 | Leonida |
|---|---|---|
| 谁加载 exe | Windows 加载器（我们只换 DLL） | **自写 PE loader 手工映射**（`gta6rt`） |
| 拦截方式 | COM 代理对象（我们的 vtable） | 同样代理，但整套跑在它自己的 loader 里 |
| 呈现 | 自建 HWND + DXGI flip 交换链 | 自建窗口 + **Vulkan swapchain**（`VK_KHR_swapchain`） |
| 构建 | MinGW + build.sh | CMake + MSVC（CI 产物 78 MB） |

所以 **Leonida 不能当库用，只能当设计与实测事实的来源**。它最值钱的东西是那 4,179 行文档里的
**ABI 事实**（槽号、返回约定、RAX 句柄、`0x03000000`、`+4` 偏移、list 槽 148 = `AllocateUploadMemory`
等），本仓库 `docs/leonida-crossref.md` 已经把这些摘过一遍了。

### 2.3 palmetto（`shader-xcheck/palmetto/`，只读）

| 项 | 值 |
|---|---|
| License | **GPL-3.0**（`LICENSE`） |
| 规模 | `runtime/shims/d3d12_vulkan_device.cpp` 10,363 / `d3d12_xs_device.cpp` 3,099 / `d3d12_com.cpp` 2,121 / `d3d12_xs.cpp` 234 = **15,817 行** |
| 文档 | `docs/d3d12_vulkan.md` 4,213 行（比 Leonida 版本多 ~30 行，是同一篇的续写）+ `d3d12_xs_surface.md` 786 行 |
| 定位 | Leonida 的续作；同样的设计，更完整的实现 + 一套 `runtime/test` 合成测试 |

`docs/design.md` 是**另一份**东西（652 行，讲 shader 转换器/runtime contract），
后端设计在 `docs/d3d12_vulkan.md`。里面有一样东西对我们价值最高：
**§6.1 第一帧最小 API 集** 和 **§6 的 M0–M7 里程碑 + 每条的"完成判据"**。

### 2.4 一条本轮新增的实测：我们的 DXIL 是真 bitcode

palmetto/Leonida 都把"主机着色器是 RDNA2 ISA（FSSM）、DXIL 块是 `NOBC` 桩"写成结论
（`docs/leonida-crossref.md` 第二节）。那是 `shaders.rpf` 的**归档**语料。
但游戏**运行时**送进 `CreateComputePipelineState` 的 blob 是另一回事 ——
`docs/transient-block-wall.md` §28 已经实测是 DXIL SM 6.0。本轮独立复核了一次
（`/tmp/dxil_scan2.py`，复用 `tools/dxbc_rts0.py` 的分块解析器）：

```
53 个 cs_*.bin：
  DXIL chunk 缺失           0
  DXIL chunk 内含真 bitcode  53      ← 判据：chunk 负载偏移 120 处命中 BC C0 DE
  解析失败                   0
样本 cs_01_len5136.bin 分块：
  SFI0 8 / ISG1 8 / OSG1 8 / PSV0 104 / RTS0 96 / STAT 1976 / ILDN 128 / HASH 20 / DXIL 2648
  DXIL 前 8 B: 65 00 05 00 96 02 00 00   （version=0x00050065 = DXIL 6.0，长度 0x296）
```

**这说明：一个 DXIL→SPIR-V 翻译器（vkd3d-shader / dxil-spirv）在这批语料上有真输入。**
`docs/rts0-analysis.md` §18 的红标（"所有 DXIL 合法的结论都依赖它真的被读进去了"）针对的是
**validator 读入**这一环，与本条不冲突：本条只说"字节是 LLVM bitcode 容器"。

---

## 三、三条路线

### 3.1 对比表

| | **A. 转发给 vkd3d-proton** | **B. 自写最小 Vulkan 后端** | **C. 混合** |
|---|---|---|---|
| 要改的文件 | `include/xbgtasix/platform.h`（后端枚举）、`src/d3d12_xs/d3d12_xs.cpp`（选后端 + 日志）、新增 `docs/` + 一个部署脚本 | `src/d3d12_xs/*` 全动：新增 `vk_backend.{h,cpp}`（约 5,400 行）+ 借 `vkd3d-shader`（6,008 行）；`build.sh` 加 Vulkan | A 的全部 + 一个显式的后端开关与"点状替换"记录表 |
| 新代码量 | **≈150 行**（选择/日志/回退），渲染代码 **0** | **≈5,400 行自写** + 6,008 行借用 ≈ palmetto 一半体量 | 第一阶段 = A；之后每替换一块 200–800 行 |
| 能多快看到第一帧 | **几小时**（两个 DLL 拷贝 + `t41` 跑通） | 纯色帧 **2–4 周**；游戏内容帧取决于 shader 编译器 | 几小时（=A） |
| 最大技术风险 | ① 1.2 节那条"槽 +4"争议在 vkd3d 上可能表现不同；② 出问题只能靠 vkd3d 的日志/版本，我们的代码不在那条路径里 | **DXIL→SPIR-V 编译器**：自写数月；D3D12 语义（placed 资源、heap 别名、layout backtracking、fence 语义）做错时极难定位 | 同 A；额外风险是"两块都归我们管"以后职责混淆 |
| 依赖 | 外部：vkd3d-proton（LGPL-2.1，已构建在盘上） | 自持（若能借 `vkd3d-shader` 则不算完全自持） | 依赖可切换，Windows 上仍走原生 D3D12 |
| Windows 上行为 | 不变（默认仍选原生 d3d12） | 需保证不劣化 | 不变 |

### 3.2 推荐：C

**为什么不是 A ——** A 单独看是对的，但它把"Linux 上有画面"变成一个完全不由我们控制的黑盒；
一旦 vkd3d 拒绝我们送进去的某个 desc，我们连"是我们错了还是它错了"都分不清（我们没有它的调用记录）。

**为什么不是纯 B ——** 数字说话：palmetto 用了 15,817 行 + 一个独立的 shader 翻译器项目才到 M4；
我们的语料是 DXIL（2.4 节），而 DXIL→SPIR-V 恰好是 `vkd3d-shader` 已经做完的 6,008 行。
重写它的收益只有"不依赖外部库"，成本是几个月 —— **这不是本项目的瓶颈**。

**C 的实际形态（要写死，别含糊）**：

```
platform.h 里加的是"渲染后端"，不是"平台"：
  HostPlatform::Windows / Wine            （已有，保持不变）
  RenderBackend::NativeD3D12 / Vkd3d      （新增）
  判定：显式开关 XBGTASIX_RENDER=vkd3d 优先；否则 Windows→Native、Wine→Vkd3d
d3d12_xs.cpp 的 D3D12CreateDevice：只在选 Vkd3d 时检查 d3d12core.dll 是否可达，并打一条开工日志；
  其余一行不加 —— 转发器 `d3d12.*` 本来就按名字解析，不区分谁在下面。
```

**"点状替换"是什么**：A 跑起来之后，凡是 vkd3d 不接受、或我们必须自己控制的**单个 D3D12 方法**，
就在 `device_abi.h` 里用我们自己的 Vulkan 实现替换掉那**一个**槽 —— 而不是替换整个设备。
按现有证据，最可能先撞上的三个点是：`D3D12ConfigureVirtualMemoryX` 的 16 GiB VA 预留、
槽 133 的 `CreateResourceFromAddress`、`PresentX` 的自建窗口路径（这三个本来就不经过桌面 D3D12）。

### 3.3 判断 A 死掉的判据（提前写好，免得事后找理由）

| 现象 | 读法 |
|---|---|
| vkd3d 在 instance/device 创建就失败 | **环境问题**，不是路线问题：查 `VK_LOADER_LAYERS_DISABLE`、ICD、`vkd3d` 日志 |
| 我们的 `Create*` 返回 `E_INVALIDARG`，而原生 D3D12 能过 | **我们送进去的 desc 形状不对**（vkd3d 更严格），这一条是可修的，且能反过来修正 1.2 节那条偏移争议 |
| 同一批 DXIL + 根签名，在 vkd3d 上也编不过 | **shader 本身的问题**，与后端选择无关 —— 这是有价值的信息，直接写进 `transient-block-wall.md` |

---

## 四、和当前"启动链"任务的关系

### 4.1 先更正一个前提

任务书里写的当前卡点是 `game+0x2237178`（按名字找 `appdata0.rpf`）。**那个已经修完了**：
`docs/wine-host.md` §9.5 —— Wine 的盘符必须靠 `$WINEPREFIX/dosdevices` 符号链接、
且必须在起进程**之前**建好；用 `DefineDosDeviceW` 三种写法全是假成功。
修完的效果（§9.6，[实测]）：

| | 修盘符前 | 修盘符后 |
|---|---|---|
| 耗时 | 318 ms | **3,186 ms** |
| 日志 | 141 行 | **2,699 行** |
| `vtable[45]` | `0xFFFFFFFF` | **`0x80`** |
| 停在哪 | `0x2237178`（空 vector 断言） | **`0x247AF2D`（64 MiB 断言）→ `DEVICE_REMOVED`** |

**所以现在的真实前沿是"图形初始化里的 DEVICE_REMOVED"，不是启动链。** 这一点很重要：
它意味着"渲染后端"和"启动链"在**当前这一刻**已经不是无关的两个任务了。
（同一跑里也确认了：**0 次 Draw、0 次 PresentX、0 次 ExecuteCommandLists**。）

### 4.2 并行性判断

| 维度 | 判断 |
|---|---|
| **代码** | 完全并行。`d3d12_xs/*` 与 `src/fstrace/`、`src/xgameruntime/` 无共享文件；唯一交点是 `platform.h`（加一个 enum 而已） |
| **验收面** | 并行，且**不需要游戏**：A 的验收是 `tests/t41_d3d12_device_abi` + 53 shader 离线重放；启动链的验收是"日志行数 / 停止地址"。两套判据互不依赖 |
| **GPU** | **冲突**。单卡单实例：跑游戏时不能同时跑 Vulkan 重放；`pgrep -x wineserver` 当前**有**一个（PID 2191530，属于无关的 Proton/winedevice 套件），开工前要确认它不是我们要用的那一个 |
| **依赖顺序** | 不需要等启动通。**反过来更成立**：A 能把"DEVICE_REMOVED 是不是 Windows 驱动的锅"这个问题在 Linux 上问一遍 |
| **真正必须等的** | 只有一件事：**"游戏自己出画面"这个验收**要等启动链接到 `Present`。在那之前，渲染侧只能靠合成测试 + 离线重放验收 |

**结论：可以并行，而且应该并行。**但两边的里程碑要分开定义，别互相借进度：

- 渲染侧里程碑：`t41` 通过 → 53 shader 重放统计 → 纯色帧窗口 → 一帧带 draw 的合成场景。
- 启动侧里程碑：日志行数 → 停止地址 → 是否能到 `PresentX`。

---

## 五、最小可验证目标

### MVG-1（半天 ~ 1 天）：`d3d12_xs` → vkd3d → Vulkan，出一帧纯色

**目标**：走完整链路 `D3D12CreateDevice`（我们的代理）→ `CreateCommandQueue` → `CreateCommittedResource`
→ `CreateRenderTargetView` → `ClearRenderTargetView` → `PresentX`，窗口里出现一帧可断言的颜色。

**为什么不写新代码**：`tests/t41_d3d12_device_abi` 已经覆盖 Direct 队列、描述符句柄 ABI、帧事件、
连续两次 `PresentX` 和窗口销毁，且在 WARP 与 RTX 5060 两条路径上验过（`docs/rendering.md` §4）。
本 MVG 只是把底下那个"D3D12 设备"换成 vkd3d。

**前置条件**（全部已在本机核过）：

| # | 条件 | 状态 |
|---|---|---|
| 1 | 构建链：`wine` + `mingw64` | ✅ 实测可用：`wine .../x86_64-w64-mingw32-g++.exe --version` → 16.1.0；编出 `PE32+ DLL` 验证过 |
| 2 | vkd3d 产物 | ✅ `build/libs/d3d12/d3d12.dll` 624 KB + `d3d12core.dll` 20.9 MB（3.2.0，mingw 16.2.0） |
| 3 | Vulkan 驱动 | ✅ 2 个 ICD：NVIDIA RTX 5060 Laptop（1.4.357）、Intel ARL Mesa（1.4.363） |
| 4 | **坏层必须关掉** | ⚠️ 实测：`VkLayer_LSFGVK_frame_generation` 指向的 `liblsfg-vk-layer.so` 是坏的，**任何 Vulkan 程序在 instance 创建时就 core dump**。必须 `VK_LOADER_LAYERS_DISABLE='~implicit~'`（实测这样 `vulkaninfo` 才出得来） |
| 5 | 无并发游戏实例 | ⚠️ `pgrep -x wineserver` 有 1 个（PID 2191530，非本任务），开工前确认 |
| 6 | 部署方式 | `d3d12.dll` + `d3d12core.dll` 放游戏目录 + `WINEDLLOVERRIDES="d3d12=n"`（这条现在已经在用） |

**验收**：窗口出现；`t41` 的像素断言通过；vkd3d 日志里能看到 `vkCreateSwapchainKHR`
和一次 `vkQueuePresentKHR`。

### MVG-2（1–2 天，**信息量最大**）：把 53 个真实 shader + 17 个根签名在 Linux 上重放

> **已完成（2026-11-25）→ [pso-matrix.md](pso-matrix.md)**
> 结果：**9/53 编过**，Intel 与 NVIDIA 逐条相同（含同一批错误码）→ 失败与驱动无关，卡在
> dxil-spirv。缺口是可枚举的：**13 个 `xbox.*` 主机外部符号**（`*_with_cache` 族 7 个、
> wave/通道族 3 个、整数辅助 3 个）+ **一类 LLVM bitcode 记录**（5 条）。
> 工具：`tools/pso_matrix.py`（跑表，可重入）、`tools/pso_matrix/pso_matrix.cpp`（PE 侧）、
> `tools/pso_matrix_classify.py`（三档分类）。表在 `build/pso_matrix/`。

**为什么这条比 MVG-1 更值钱**：它**不需要 exe**，而且直接回答"Vulkan 路线能不能吃下游戏着色器"。
现有 Windows 侧的证据是矛盾的：`transient-block-wall.md` §29 说"WARP 编过 48/53"，
`rts0-analysis.md` §10.3 又查出**那一版 t49 根本没用 WARP**（`igd12um64xeh.dll` 出现 75 次、
`d3d10warp.dll` 0 次），所以"48/53"实际是 **Intel 驱动**的成绩；而 §30 又记录游戏进程内 WARP 也丢设备。

**做法**：拿 `build/dumps/` 里现成的 53 个 `cs_*.bin` + 17 个 `rs_*.bin`（820 KB，已在盘上），
在 vkd3d 上逐个 `CreateComputePipelineState`，统计成功/失败，按 shader 序号落盘名单
（这正好补上 `rts0-analysis.md` §1 说"名单不存在"的那个洞）。

**额外已验证的前置**：53/53 的 DXIL 块里有真 LLVM bitcode（2.4 节）。

**验收**：一张 `cs_序号 → ok/fail` 的表 + 失败的第一个错误码。这张表直接决定：
(a) Vulkan 路线能不能继续；(b) 现有 `DEVICE_REMOVED` 是驱动问题还是我们的问题。

### MVG-3（可选，4–6 小时）：不依赖 vkd3d 的最小 Vulkan 程序

**目标**：一个独立的 Win32 程序（`vulkan-1.dll` + `VK_KHR_swapchain`），开窗、清屏、present 一帧。

价值最低（只证明"我们能调 Vulkan"），但它是 B 的种子，且能把一个隐藏的前置条件逼出来：
**MinGW 工具链里没有 Vulkan 头，也没有 `libvulkan` 导入库**（`/mnt/winbot-software/mingw64/x86_64-w64-mingw32/include`
下无 `vulkan/`，`lib/` 下无 `libvulkan*`）。两条解：

```
甲：拷 /usr/include/vulkan + 拷 Windows 侧 vulkan-1.dll 的导出表 gen_def → dlltool 出 libvulkan-1.a
乙（推荐，零新依赖）：只声明我们要用的 ~40 个函数类型，LoadLibraryA("vulkan-1.dll") + vkGetInstanceProcAddr
```

（`/winboot/Windows/System32/vulkan-1.dll` 与 `/usr/lib/wine/x86_64-windows/vulkan-1.dll` 都在。）

---

## 六、未统计 / 不能引用的

写在这里，免得后面有人把推断当实测：

1. **绘制循环实际调用面**：0 次观测。第一帧要哪些 D3D12 方法，目前只能引 palmetto §6.1 的清单。
2. **一次真实 `PresentX` 的参数形态**：0 次观测（只在合成测试里跑过单平面 SDR）。
3. **vkd3d 在我们这套代理下的行为**：0 次实测 —— MVG-1 就是它的第一次。
4. **"主机槽 = 桌面槽 + 4"到底成不成立**：9 个静态交叉验证点支持它，
   但同号转发能推进到 `0x247AF2D` 又和它矛盾（`docs/host-vtable-offset.md`）。**未定论，别当结论用。**
5. **自写后端（B）的真实码量**：只有参考实现的行数（14,910 / 15,817），没有我们自己的样本。
6. **`PS\x00\x09` 库 21,522 vs 归档 25,734 vs 对方 26,503** 三个口径的差（16.4%）：
   `leonida-crossref.md` 第三节列进问题清单，未解释。

---

## 七、复现命令

```bash
# 本轮的源码统计
wc -l src/d3d12_xs/*
sed -n '/^Slot device_slot/,/^}/p' src/d3d12_xs/device_abi.h | grep -oE 'N == [0-9]+'

# 实测调用面（dump3.log = 走到 PSO 的那一跑）
cd build
grep -ac 'Xbox slot 73 -> desktop CreateCommandList' dump3.log     # 585
grep -ac '槽62 写回 \*e' dump3.log                                  # 53
grep -ac 'CreateDescriptorHeap type' dump3.log                      # 6
grep -ac 'PresentX' dump3.log                                       # 0
grep -ac 'ExecuteCommandLists' dump3.log                            # 0

# 探针跑（构造期一次性路径）
grep -ao '槽位探针 slot=[0-9]*' real-probe.log | sort -u

# 参考实现的规模与 license
wc -l ../_leonida/rt_vk_device.cpp ../_leonida/runtime_shims_d3d12_xs_device.cpp
wc -l ../shader-xcheck/palmetto/runtime/shims/d3d12_vulkan_device.cpp
head -3 ../vkd3d-src/COPYING          # LGPL-2.1
ls -la ../vkd3d-src/build/libs/d3d12/d3d12.dll ../vkd3d-src/build/libs/d3d12core/d3d12core.dll

# 53 个 dump 的 DXIL 复核（脚本见 2.4 节描述）
python3 /tmp/dxil_scan2.py

# 构建链自检（Linux 侧）
wine /mnt/winbot-software/mingw64/bin/x86_64-w64-mingw32-g++.exe --version

# Vulkan 环境自检（必须关掉坏的 implicit layer，否则 instance 创建即崩）
VK_LOADER_LAYERS_DISABLE='~implicit~' vulkaninfo --summary
```
