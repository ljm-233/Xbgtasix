# 与 Leonida 的互补映射：谁在哪一半，接口在哪

对照对象：[ProjectLeonida/Leonida](https://github.com/ProjectLeonida/Leonida)（GPL-3.0-or-later，
2026-11 取到的 `main`）。本项目同为 GPL-3.0-or-later。

等级标注：**[本机实测]** 在本机包上跑出来的数字 · **[Leonida 文档]** 对方文档/代码里的记载 ·
**[推断]** 由前两者推出、尚未直接验证。

---

## 一、一句话结论

两个项目是**同一件事的两半，而且卡在同一堵墙上**：

| | Xbgtasix | Leonida |
|---|---|---|
| 做的是哪一半 | **API 语义侧**：主机 GDK/D3D12 槽位、描述符句柄、xmem、盘符 | **字节码执行侧**：RDNA2 → SPIR-V、Vulkan 后端、BVH4 |
| 真实 EXE 状态 | **已跑到图形初始化**（12 个 RPF、设备建好、帧节奏）[事实] | 自己的 PE loader 跑到第 13 个 stop，D3D12 调用 1185 条 [Leonida 文档] |
| 着色器 | **一步没走**，靠"转发给桌面驱动" | 26,503 条全量翻译 + `spirv-val` 全过 [Leonida 文档] |
| 卡点 | 描述符堆句柄 / 未建模的扩展槽 | 描述符堆起始句柄断言 |
| 加载方式 | 靠 Windows 自己加载，兼容 DLL 放在 exe 旁边 | 自写 PE loader 手工映射 |

结论：**不是二选一，也不是合并代码**。Xbgtasix 提供"让真 exe 活着走到图形初始化"，
Leonida 提供"到了以后怎么把主机着色器喂给 GPU"。中间唯一的对接面是
**主机 PSO 创建入口 + 主机着色器字节流**（见第七节）。

---

## 二、第一手验证：主机着色器里到底有没有 DXIL

[d3d12xs-evidence.md](d3d12xs-evidence.md#L138-L146) 早先的字节扫描结论是
`DXBC=0 / DXIL=0 / SHDR=0 / RDEF=0`，并推断"数据可能加密/压缩"。**这个扫描必然为 0，
因为每个着色器都是 raw deflate 单独存储的**——明文魔数不经过解压根本看不到。

用 Leonida 的 `tools/shader_carve.py`（它不读 RPF9 加密 TOC，按存储框架雕刻）在本机
`shaders.rpf`（411,695,600 B）上重跑 **[本机实测]**：

| 步骤 | 命令 | 结果 |
|---|---|---|
| 雕刻 | `shader_carve.py shaders.rpf carve/` | **25,734** 个 DXBC 容器，284 MB |
| 解包 | `fssm_unpack.py carve/` | **25,734** 个 FSSM，378 MB |

任取两条 PS 容器拆 part 表 **[本机实测]**：

| 项 | 样本 A | 样本 B |
|---|---|---|
| DXBC 总大小 | 1,748 B | 1,376 B |
| `DXIL` part | 1,028 B，**含 `NOBC`** | 732 B，**含 `NOBC`** |
| └ XDAT tag `0x07`（`DxilProgramHeader`） | **28 B** | **28 B** |
| └ XDAT tag `0x0D`（`XBCH`） | 887 B 存储 | 590 B 存储 |
| 解压后 | **`FSSM` 5,028 B** | **`FSSM` 4,524 B** |
| `PRIV` part | 24 B | 20 B |

**所以：容器里没有任何 DXIL 字节码。** `DXIL` part 只是 `DxilProgramHeader` 桩 +
XDAT 目录，tag `0x07` 明确写 `NOBC`（不发 LLVM bitcode），真代码在 `XBCH → zlib → FSSM`
（原生 RDNA2）。

本节之前，[rendering.md](rendering.md) 对这一点的记载是"PC 的 D3D12 是否照收需要实测"，
[d3d12xs-evidence.md](d3d12xs-evidence.md#L26) 写的是"主机可能发行非标准 DXIL"。现在两条都结论化了
（rendering.md 已按本节更新）：

> **桌面 D3D12 永远不会接受这份 blob。不是驱动挑剔，是里面没有它能吃的东西。**
> 转发路线在 `CreateGraphicsPipelineState` 这一步在事实上已死，与槽位表填得多全无关。

顺带一条更正 **[本机实测]**：容器的 `DXIL` part 程序头里**是有阶段码的**（样本 A/B 都是
`ver=6.6 stage=0` → `ps`），阶段也可以从存储框架的 kind 字节取。
（本文档初稿曾误读 part 表的偏移，把 `DXIL` 这个 fourcc 的字符当成阶段码，
得到"恒为 19,529"的错误结论，已改正。）

### 2.1 更关键：这条链**不需要 LLVM，也不需要 PercyStar**

上面那一步只是"拆开看一眼"。真正要紧的是：把 `FSSM → SPIR-V` 这一段在本机跑通，
而且**全程开源、无外部工具** **[本机实测]**：

```
6 个容器 → shader_disasm.py --backend python   → 6/6 clean
         → rdna2spirv.py --assembler python     → 6 translated / 6 pass spirv-val
         → spirv-dis                            → OpEntryPoint Fragment "main"
```

产物大小 21,044–45,564 B/条，`spirv-val --target-env vulkan1.2` **独立复验 6/6 通过**。
工具自己报出一条诚实的限制：6 条里有 1 条属于 wave64 在 32 宽 subgroup 下的
**tier C**（VS/PS/GS 跨半边 wave 操作不精确），这正是对方 README 里写明的近似边界。

这条的意义：本项目现在依赖的 `PercyStar.exe`（闭源、外部）在"反汇编 + 翻译"这件事上
**有全开源的替代品**，而且不挑机器（纯 Python 解码器 + 进程内 SPIR-V 汇编器，
只要有 `spirv-val` 就能自检；本机 `spirv-val` 来自 Vulkan SDK 1.4.363.1）。

### 2.1.1 不止 PS：三个主要阶段都验了

用同一条链（carve → FSSM → 纯 Python 反汇编 → SPIR-V → **驱动真建管线**），
对占语料 91% 的三个阶段各抽 8 条（PS 6 条）**[本机实测]**：

| 阶段 | 语料条数 | 样本 | 翻译 | spirv-val | **驱动建管线** | 编译均值 / 最慢 |
|---|---|---|---|---|---|---|
| PS | 11,983 | 6 | 6 | 6 | **6/6** | 0.20 s / 0.20 s |
| mesh | 8,666 | 8 | 8 | 8 | **8/8** | 2.16 s / 3.87 s |
| mesh（`--wave64-emulate`） | — | 8 | 8 | 8 | **8/8**，全部归为 wave32 | 1.14 s / 1.65 s |
| CS | 2,676 | 8 | 8 | 8 | **8/8** | 1.08 s / 2.59 s |

两条要点：

1. **mesh 在本机走"精确"路径**：加 `--wave64-emulate` 之后，这 8 条从"无 required
   subgroup size"变成 wave32 分类——即 32 宽 subgroup 上语义精确。对方 README 的说法是
   compute/mesh 在模拟下精确、VS/PS/GS 的跨半边 wave 仍近似；我们没抽 VS，这一条未验证。
2. **单条管线编译 1–4 秒**，而标题的 hang check 只给 30 轮 × 1 s →
   **连续编译十几到几十条管线就足以让它自杀**。本机 8 条 mesh 就花掉 17 s（未模拟）/
   9 s（模拟）。这把 [host-stall](host-stall.md) 从"预防性设计"变成"必需品"，
   也解释了为什么参考实现要专门为 `vkCreateComputePipelines` 记账。

边界：`vk_pipeline_check` 只证明**驱动接受**并建出了管线对象，**不验证输出正确**；
像素级正确性要靠之后的真机首帧。

---

## 二之二、实测：直接跑它的 runtime（2026-11-24）

不需要 MSVC —— 用它 CI 的 `runtime-windows` 产物（78,166,622 B，分支
`runtime/fix-c4456-warnings`，GitHub Actions artifact `14867675762`）就能验。

| 环节 | 命令/工具 | 结果 |
|---|---|---|
| 加载本包的真实 EXE | `gta6rt --dry-run` / `--init` | **12 个模块全部映射，0 个未实现导入**，exit 0 |
| 它的 Vulkan 设备 | `vk_device_pick_test` | 全部用例通过 |
| 它的渲染回放 | `clear_color_test` | **462** 项 integer/float replay 检查通过 |
| **驱动能不能吃我们的 SPIR-V** | `vk_pipeline_check <2.1 节的 spirv 目录>` | **6 ok / 0 failed**，RTX 5060 Laptop，mean 0.202 s；工具自报 "64-wide subgroups not supported for ps"，6 条按无 required subgroup size 编译 |

最后一行是这次最值钱的结论：**它自己的工具证明驱动接受了我们翻译出来的 SPIR-V**，
不是只过 `spirv-val`。

### 跑真标题时撞到的三件事（都不是它的层失败）

1. **`%TEMP%` 不可写 → 它的静态 TLS 克隆失败**，报错却是
   `gta6rt_tls_host.dll is missing`。实测那个 DLL 是好的（TLS index=2），
   失败的是 `CopyFileW` 到 `%TEMP%`（err=5）。把 `TMP`/`TEMP` 指到工作区后克隆成功（index=6）。
   已记进 [env-gotchas](env-gotchas.md) 第 8 条。
2. **本包的原版 `d3d12_xs.dll` / `xmem.dll` 已经不在了**：两个
   `*.xbgtasix-backup` 里都含我们的构建标记（"xbgtasix"），说明当年备份时目标已经是我们的产物。
   换回去等于换回旧 shim，于是它的层根本没被调用——这次运行的 trace 里 **D3D12 事件 0 条**。
3. 标题在 0.96 s 于 `game+0x1E1D01B` 触发 `0x80000003`(int3)。用 `tools/xref.py` 定位：
   所属函数 `0x1E1CF60..0x1E1D5ED`，**只引用两个导入**
   `d3d12_xs.dll!D3D12ConfigureVirtualMemoryX` 与 `xmem.dll!XMemVirtualAlloc`（后者被调 6 次）。
   即显卡虚拟内存配置那段。因为第 2 条，这次跑到的是"我们的旧 shim + 它的 loader"，
   **不能算它的层失败**；但它给出了一个有用的事实：`D3D12ConfigureVirtualMemoryX`
   返回之后紧跟的断言对返回值和分配结果有硬要求，我们现在"记日志 + 返回 S_OK"是恰好没炸，不是对了。

要把它验完整，只差一步：**从原始包把这两个原版 DLL 取回来**。

### 顺带修掉的一个我们自己的 bug

`xbgtasix run` 的备份机制不幂等：只要背包里已经出现过"目标是我们自己的 DLL"的情况
（例如按 [HANDOFF](HANDOFF-GPT.md) 用 `bash cp` 手工部署），它会把我们的产物存成
`*.xbgtasix-backup`，**从此原版就没了，而 `restore` 还会报告"已还原"**。

修法（已落地于 `src/host/main.cpp`）：

- 备份前判断目标是否含我们的构建标记；含则**不产生备份**，并提示"原版不在这里，需从原始包取回"；
- `restore` 遇到带标记的备份时**明说"原件已丢失"**，汇总行改成 `PASS（不完整）`；
- `--dry-run` 同步显示这条判断。

隔离目录实测三种情形（假 exe + 三个假 DLL）：

```
已备份 xgameruntime.dll -> ....xbgtasix-backup          ← 原版，正常备份
已备份 xmem.dll -> ....xbgtasix-backup                  ← 原版，正常备份
d3d12_xs.dll 已经是我们自己的产物（原版不在这里）——不写入 .xbgtasix-backup
...
? xmem.dll 的备份其实是我们自己的构建 —— 原件已丢失，需要从原始包把同名 DLL 取回
PASS（不完整）：2 个位置已复位，其中 1 个不是原件
```

回归：`t30_fakegame` 通过，全套 **39 PASS / 2 SKIP / 0 FAIL**。

---

## 三、三个数字：21,522 / 25,734 / 26,503

[shader-corpus.md](shader-corpus.md) 的 21,522 条、34.8% NEG、p99=98 这些统计，
口径是 `PS\x00\x09` 这个**管线库**（PercyStar 用 `--disasm-shaders` 导出的索引），
不是"归档里的全部着色器"。

把该库原样重组出来读头部 **[本机实测]**（`shader_library.py --save library.bin`，
275,540,780 B）：

```
magic 'PS\0\9'   name '0x65F41EDD'
graphics_pipelines = 25,471      compute_pipelines = 2,693
state_blobs        = 54,576      programs          = 21,522   ← 与 PercyStar 打印一致
blob_section = 31,120,020 B      code_section = 240,894,464 B
```

| 数字 | 是什么 | 来源 |
|---|---|---|
| **21,522** | 管线库 program 表里的程序数（25,471 图形 + 2,693 计算 PSO 引用） | [本机实测] 库头 + PercyStar 输出 |
| **25,734** | 归档里独立的着色器容器数（按存储框架雕刻） | [本机实测] |
| **26,503** | 对方在同一版本包上的全量容器数 | [Leonida 文档] |

差 **4,212 个（16.4%）** 容器不在库的 program 表里 [推断：库按 hash 去重，或这些容器
只被别处引用]。

两个口径的阶段分布（本机全量按 DXIL 程序头分类）**[本机实测]**：

| 阶段 | 全量容器 25,734 | `PS\x00\x09` 库 21,522 | 差 |
|---|---|---|---|
| ps | 11,983（46.6%） | 9,535（44.3%） | +2,448 |
| **mesh** | **8,666（33.7%）** | 7,498（34.8%） | +1,168 |
| cs | 2,676（10.4%） | 3,082（14.3%） | **−406** |
| vs | 2,214（8.6%） | 1,343（6.2%） | +871 |
| gs | 195（0.8%） | —（对方记作 "geometry 195"） | — |

三点结论：

1. **占比结论基本站得住**：mesh 33.7% vs 34.8%，"三分之一是 mesh"仍然成立，
   描述符容量与 mesh 支持面的判断不需要推翻。
2. **数量差 16%**，规划总量时要按 25,734 算。
3. **CS 反而少了 406 条**，说明两个口径不是严格包含关系（对方记 2,689 CS，
   与库内 2,693 条 compute PSO 接近）。这条差异没有解释，列进第九节的问题清单。

`shader-corpus.md` 的局限一节已按此加了口径提醒。

---

## 四、Leonida 侧可借的实测事实

这些是"事实"级别的可移植知识（可独立重实现），不是可抄的代码。每条都写明出处。

### 4.1 槽位与 ABI 怪癖

| 事实 | 对本项目的用处 | 出处 |
|---|---|---|
| `PresentX` 在队列槽 **30**（旧布局）/ **32**（新布局），且对方**没有接线** | 我们的 30 与之吻合；32 是必须支持的第二种布局 | [Leonida `docs/d3d12_vulkan.md:2896-2905`] |
| 命令列表相对公开顺序的偏移：`SetDescriptorHeaps` **+4（=32）**、`ExecuteIndirect` **+2**、console-only 在 **85 / 141** | 直接解释我们 131/132 为什么语义定不下来 | 同上 `:3871` |
| BVH builder 补的根组 **+4 → 37/39/43/45**；console 槽 **71/73/80/146(Barrier)/148** | 我们的槽 73 已被实测为命令列表创建，与之相容 | 同上 |
| 命令列表槽 **148 = `AllocateUploadMemory`**，形状 `(list, count*40, 16) -> ptr`，**返回的 CPU 指针原样当 GPU 地址**用 | 任何一个 XDXR/上传路径都要它 | [Leonida `docs/d3d12_command_objects.md:366-367`] |
| 设备槽 **62 被当 `CreateComputePipelineState` 调**（PC 同号是 `CreateStateObject`，arity 不同）；**67** 是 console-only pipeline；**76** 是 Xbox 纹理创建（描述符 56 B + 扩展字） | PSO 入口就在 60–67 这一段，不是我们原先假设的 60/61/62 直接对位 | [Leonida `docs/d3d12_vulkan.md:3764-3767`, `:3798-3804`] |
| 堆起始句柄在 Xbox 上走 **RAX** 返回 8 字节；PC 是 RDX 隐藏指针 | 我们 `get_cpu_start` 已经在做"只吃堆指针、从 RAX 返回"，方向对 | [Leonida `docs/d3d12_vulkan.md:3765`] |
| **结构体返回方法的首参是返回缓冲区**：`(return buffer, this, node_mask, heap_type)` vs PC 的 `(this, return buffer, …)`；对方只在槽 26 上实测到，其余"大概率同规则" | 与我们 2026-11-24 对槽 26 的实测**完全一致**，且给出了"这是个族"的假设 | [Leonida `docs/retail_boot_chain.md:61-68`] |

### 4.2 Root signature 的主机标志

| 事实 | 出处 |
|---|---|
| Xbox 根签名描述符带 `0x03000000`，PC 序列化器直接 `E_INVALIDARG` | [Leonida `docs/d3d12_vulkan.md:3766`] |
| 做法：**在私有副本上清掉高字节 → 转发序列化 → 把原始位写回 blob** | 同上 `:3805` |
| 包内**只有 `XboxBvhBuilder.dll` 设这两位**（52 个 `RTS0` 片段，`0xc30e` 处无条件设置）；FSR DLL 的 29 个 `RTS0` 全 0 | 同上 `:3888` |
| v1.0 的 `D3D12SerializeRootSignature` 标志在 **+32**，`0x3000001` 同样失败 | 同上 |

这条对我们是"下一堵墙"的提前预警：`src/d3d12_xs/d3d12_xs.cpp` 现在是**纯转发**，
没有任何标志处理 [观察]。而 [rendering.md](rendering.md) 里列的 `0x01000000` /
`0x02000000` / `0x80000000` 与对方实测的 `0x03000000` 掩码**不是同一组位**，需要并集处理。

### 4.3 描述符记录模型（可直接照抄的设计，不是代码）

| 事实 | 出处 |
|---|---|
| 堆有 **host shadow + arena 范围**；CPU 句柄指 shadow，GPU 句柄是 arena VA | [Leonida `docs/d3d12_vulkan.md:1589-1651`] |
| `Create*View` / `CopyDescriptors` **只写 shadow 并置脏**，正确性走**执行期扫描**（所以游戏自己写裸字节也行） | 同上 |
| 描述符字节就是 **GFX10 T# / S#**；执行时遍历声明的 range 逐条解析 → bindless 索引 | 同上 |
| 无界 range（`NumDescriptors=UINT_MAX`）从表句柄扫到绑定堆尾 | 同上 |
| 堆增量暂定 32 / 32 字节，**console 实际增量仍 Open** | 同上 |
| 堆基址是**输入 SGPR 里的 32 位指针**，stride 编进代码（T# 步长 32、S# 16） | 同上 `:1562-1583` |

这一条把[本机实测]的两件互相印证了：RTX 5060 步长 32 → 1,245,184 项 `E_INVALIDARG`；
Intel 核显步长 64 → 802,816 项通过。**步长不是常量，是按适配器变的**，对方也把它列为 Open。

### 4.4 着色器运行期约定（要接 SPIR-V 就必须知道）

| 约定 | 内容 | 出处 |
|---|---|---|
| push constant **144 B** | dword 0–31 = 初始 `s0..s31`（root signature 灌进来的 user SGPR）；**32–33 = 64 位 heap base**；**34–35 = code base**（整个 FSSM blob 的 Xbox VA）；`--mesh-compute` 追加 36–37 | [Leonida `docs/design.md:270`] |
| 访存模型 | 所有访存 = **heap base + Xbox VA**；运行期把 console GPU 地址窗映射进**单个** buffer-device-address 分配 | 同上 `:271` |
| bindless set 0 | 0 = 2D sampled、1 = samplers、2 = 描述符哈希表、3 = 2D-array（**cube = 6 层 array**）、4 = r32ui storage、5 = 1D、6 = 3D | 同上 `:284` |
| 哈希表格式 | `uint mask; entries[mask+1] of 10 uints {key[0..7], index, pad}`，**FNV-1a** 选首槽、探测至多 8 槽、miss 返回 0 | 同上 `:286` |
| vertex fetch | VS 开头的驱动 fetch 程序不在 blob 内 → 换成 Vulkan vertex input；每个 `ISG1` 元素 **location = 它的 D3D input register**，写进 `v(4 + 4*reg + comp)`；788 个调用者中 783 个符合 | 同上 `:294` |
| wave64 | 26,503 条里 **23,841 是 wave64**；64 宽（AMD）精确；32 宽（NV/Intel）下 compute/mesh 靠 `--wave64-emulate` 精确，**VS/PS/GS 的跨半边 wave 操作仍是近似** | [Leonida `README.md:52`] |
| 代码区间 | FSSM 偏移 `0xC0` 处 u32 是代码段偏移，**代码从该段 +0x28 起**到最后一条 `s_endpgm`；`--mcpu gfx1013` 在 300 个随机样本上 284 干净（gfx1030 只有 201） | [Leonida `docs/design.md:569-577`] |

### 4.5 BVH4 / XDXR

| 事实 | 出处 |
|---|---|
| `BATCHED_DESC2` = **0x50 B**：`0x00` Dest、`0x08` 24 B 公开 INPUTS、`0x20` Source、`0x28` Scratch、`0x30` NumPostbuildInfoDescs、`0x38` pPostbuildInfoDescs（16 B 步长，`InfoType` 在 `+8`）、`0x40` 16 B STREAM_DESC | [Leonida `docs/xdxr.md:30-50`] |
| `XDXR_STREAM_DESC` = **0x10 B**；读取规则：从流首走，取"所需类型的第一个"，**遇到未知类型就停**（一条未知记录会遮住后面全部） | 同上 `:52-67` |
| BVH4 头 **64 B**，位于 payload `+0x100`；**偏移相对 0x100 处的头**而不是 payload 起点；结构大小 = `64*(totalSizeIn64BNodes+1)` | [Leonida `docs/bvh.md:66-137`] |
| 节点指针编码 `type = ptr & 7`、`node_address = header + (ptr>>3)*64`；`0..3`=TRI0..3、`4`=BOX16、`5`=BOX32、`6`=INSTANCE、`7`=AABB | 同上 `:139-166` |
| 包里的 BVH 是**脱水**的（`flags` 置 `0x80000000`/`0x00020000`/`0x01000000`，不置 `0x00010000`），脱水根节点**不能**直接喂遍历路径 | 同上 `:129-131`, `:183-187` |
| `XboxBvhBuilder.dll` **能在 PC 上直接跑**：把各 buffer 按 GPU VA `VirtualAlloc` 映射拷贝即返回 `S_OK`，**不发设备调用**（默认 WARP，硬件驱动会崩） | 同上 `:472-476` |

这对 `t6_rt` 是明确的升级方向：现在的 t6 验的是"桌面 DXR 1.1 能用"，
跟"能吃主机的 BVH64"是两件事。

### 4.6 版本清单范式

`game/xbox-1.0.2.1.json`：`{title, version, files: {name: {size, sha256}}, expected_exe}`，
108 条 / 223,880,932,870 B，**只有大小和哈希，不含任何数据**，天然过 no-game-data 检查。

我们的 `verify` 读的是 makepkg 的 `layout_*.xml`，**只有路径，没有大小也没有哈希**，
所以它现在做的是"对账 + 0 字节检测"，不是完整性校验 [观察]。同一个游戏版本（1.0.2.1），
对方那份清单可以直接拿来当"哪些文件该存在、该多大"的对照源。

---

## 五、Xbgtasix 能回给 Leonida 的

对方**没有**、而我们有实测的：

| 我们的东西 | 为什么对方需要 | 我们的出处 |
|---|---|---|
| **真 exe 跑过图形初始化的路径**（12 个 RPF → 设备 → 帧节奏） | 对方的 loader 停在描述符堆；它没有"一个真能跑起来的环境"的对照 | [HANDOFF-GPT.md](HANDOFF-GPT.md) |
| **盘符映射开关**：`DefineDosDeviceW` 映射 `G:`（内容分区）/ `T:`（临时盘）→ 12 个 RPF 才加载成功 | 对方是"原地映射包目录"，若它的 file shim 撞到 `G:\` 路径，这是实测可用的解法 | 同上 |
| **xmem 5 参数 ABI** + 物理页真语义（一次 11,360 页 = 710 MB）+ `VirtualAlloc2` 占位替换 | 对方 xmem 面比我们薄 | 同上 |
| **描述符步长按适配器变的实测**：RTX 5060 = 32 B → 1,245,184 项失败；Intel = 64 B → 802,816 项通过 | 对方把"console 堆增量"列为 Open，这两个数是直接对照 | [rendering.md](rendering.md#L29) |
| **槽 73 的真实调用形状**：描述符 4 DWORD `{0,2,1,0}`(DIRECT) / `{2,2,1,0}`(COMPUTE)，第 2 个 DWORD 实测取 2 和 6 | 对方把 console list 尾部列为 Open | [d3d12xs-evidence.md](d3d12xs-evidence.md#L98-L113) |
| 槽 131 传入寄存器号 `0xc3c0` / `0xc380` / `0xc280`，与 AMD PAL `gfx9_plus_merged_offset.h` 的编号一致 | 一条寄存器级线索，双方都还没解释 | 同上 `:108-L113` |
| **41 步测试套件 + "有断言证据才算 PASS"** 的判定纪律 | 防假绿，对方的测试纪律更重但没这套 | [how-it-runs.md](how-it-runs.md) |

---

## 六、两个项目撞在同一堵墙上

这是这次对照里最值得记的一条：**两个项目独立走到了同一个断言。**

| | 现象 | 出处 |
|---|---|---|
| Leonida | 首次 `CreateDescriptorHeap` 之后，标题调堆槽 9（PC 上是 `GetCPUDescriptorHandleForHeapStart`），从 **RAX** 取句柄，然后**断言它等于一个它自己已持有的值** | [Leonida `docs/retail_boot_chain.md:29`] |
| Xbgtasix | 游戏申请 1,245,184 项堆失败后换核显跑通，随后在 `game+0x1CF336C` **断言 CPU 句柄必须等于预留地址** | [HANDOFF-GPT.md](HANDOFF-GPT.md#L7) |

两边的措辞不同，但约束是同一个：**标题期望堆起始 VA 是它自己选定/可预测的，
而不是驱动返回的任意地址。**

这条的工程含义：修一行 thunk 不够，需要"**堆起始 VA 可指定**"的模型——CPU 句柄返回
标题的 arena 地址，真实堆地址由我们自己做偏移翻译（[descriptor_heap_abi.h](../src/d3d12_xs/descriptor_heap_abi.h)
现在已经有 `virtual_cpu_base` + 翻译表的雏形，但视图创建还没接上槽位 [观察]）。

**这是两个项目最有价值的共同攻坚点，也是唯一"两边都还没解决、但都已经有半成品"的地方。**

---

## 七、唯一能对接的接口：PSO 入口 + FSSM 哈希

对方的 `shader_meta.py` 写明了它的运行期契约 **[Leonida 文档]**：

> 为 `spirv/index.json` 提取每个 shader 的静态元数据，使 D3D12→Vulkan 运行期可以
> 把 root signature 映射到 user SGPR、**按 FSSM 内容哈希匹配 PSO 的着色器**、
> 按 ISG1 语义绑定顶点输入、预注册字面量采样器、检查描述符索引模式。

这对我们意味着一条**不需要名字映射**的对接方式：

```
游戏调 PSO 创建（设备槽 60–67 某处），传入 DXBC 容器
   ↓ 我们拦截
从容器里取 XDAT tag 0x0D → 解 zlib → FSSM 字节
   ↓
hash(FSSM) → 查 spirv/index.json → 拿到该 shader 的 SPIR-V + 元数据
   ↓
建 Vulkan pipeline（root signature 用 index.json 里的 user-SGPR 映射）
```

为什么这条路稳：

1. **键是内容哈希，不是名字**——容器自带，游戏在运行期传的就是它，不存在"名字对不上"。
2. **元数据是预先算好的**——148 B 的 push constant 怎么填、顶点输入怎么绑、采样器怎么预注册，
   都不需要在运行期反解 SPIR-V。
3. **两边各出一半**：我们出"拦得到 + 槽位对 + 句柄对 + 真 exe 在跑"，对方出"FSSM → 能用的 GPU 程序"。

代价也必须说清：**这条路要求 `d3d12_xs` 有 Vulkan 后端**。桌面 D3D12 吃不了 SPIR-V，
这是硬约束（第二节）。对方的 Vulkan 设备实现有 587 KB（`runtime/shims/d3d12_vulkan_device.cpp`），
这是本项目目前完全没有的东西 [观察]。

---

## 八、建议路线

### 立刻（零风险，只是读和记）

1. 把第 4.1 / 4.2 节那几条 ABI 怪癖当** checklist** 过一遍代码，凡是"现在纯转发"的位置
   标记出来。特别是一条：**≥44 的槽返回 `S_OK` 却不写输出对象**
   （[device_abi.h:244-252](../src/d3d12_xs/device_abi.h)）——这是"假成功"，
   现场无法归因，且这次崩溃就落在它后面。
   （本节写作期间的工作区已经在改这一处，见下面第 4 条。）
2. 把对方 `docs/retail_boot_chain.md` 的 **13 个 stop 顺序**当回归清单
   （他们是同一个游戏版本，stop 顺序对我们同样是预测）。
3. 用对方 `game/xbox-1.0.2.1.json` 给 `verify` 补上 size + sha256。

### 短期（修已有的结构性问题）

4. **槽 ≥44 的 stub 改成记录槽号 + 返回 `E_NOTIMPL`**，别再假成功。改完再跑一次真机，
   崩溃点能不能归因到具体槽位，是这一轮成败的唯一判据。
   *状态（2026-11-24 06:2x）：工作区未提交的改动已经在做这一条（`device_extension_stub`
   改成 `E_NOTIMPL`），并新增了槽 19 = `CreateUnorderedAccessView` 的绑定。
   注意 `E_NOTIMPL` 会让游戏在检查 HRESULT 处 `int3`（此前实测过），所以这一步换来的
   是"能归因"，不是"能前进"——下一格必须在归因之后再填实。*
5. **命令列表的 598 个 `naked` 汇编尾跳换掉**（[device_abi.h:284-289](../src/d3d12_xs/device_abi.h)）。
   仓库自己的铁律写着"不要用 naked asm 做 COM 转发"（[HANDOFF-GPT.md](HANDOFF-GPT.md#L105-L111)），
   设备侧改了、列表侧没改。任何一个未识别的主机列表槽都会以同号打到桌面列表的**任意其他方法**上。
6. **t41 与生产代码是两份实现**（`present_test` vs `xbgtasix::d3d12::detail`），
   `t41` 绿不等于生产绿。合并成一份。
7. 语料统计按 25,734 重跑一遍（第 3 节），再定描述符堆容量与 mesh 支持面。

### 战略（要选边，必须明确取舍）

8. **后端二选一**（这一步的选择在 2.1 节跑通之后变得更清楚了）：
   - **A. 走 Vulkan 后端**（吃对方的 SPIR-V + 元数据）：唯一全开源路径。翻译侧已经被
     2.1 节证明在本机可复现（无需 LLVM、无需 PercyStar），缺的只是**一个 Vulkan 设备层**。
     对方写了 587 KB，但**没有可移植的 D3D12→Vulkan 桥梁**（它是"Xbox→Vulkan"，
     不转发到 host D3D12）。
   - **B. 走 HLSL/DXIL 再生**（保住桌面 D3D12）：翻译器输出的是 SPIR-V，不是 DXIL，
     所以这条路**用不上 2.1 节的成果**，仍然要靠 PercyStar 那类能出寄存器级 HLSL 的
     闭源工具，再 dxc 成 DXIL。而 PercyStar 从来没有验证过 DXIL 编译。
   → **2.1 节的结论实际上把天平压向了 A**：开源可复现的那一端只通向 SPIR-V。
   仍建议先用 10 条（含 mesh 与 wave64 VS/PS/GS）做端到端冒烟再定，但
   **不要两条路同时开工**。

### 不要做

- 不要照抄对方的 `README.md:60` 当现状——它声称 `PresentX` 30/32 已接线、
  `AllocateUploadMemory` 已实现，但对方自己的 `docs/d3d12_vulkan.md:3153` 写
  "nothing here is wired to those slots yet"，代码里也搜不到 148。
- 不要把对方的代码直接搬进来。虽然**双方都是 GPL-3.0-or-later，法律上可以**
  （这与它明确拒绝的 xcl / americas-recomp 的 GPL-2.0-only 不同），但对方的规则是
  "facts are fine; code is not"，而且对方有"先在协调 issue 认领再提 PR"的流程。
  要复用代码就走它的流程，要借事实就在本文档里记出处。

---

## 九、待互相确认的六个问题

写下来是为了以后真对接时不用重新讨论：

1. **堆起始 VA**：你们那个"断言句柄等于自己持有的值"的槽 9 模型，最后打算让
   `GetCPUDescriptorHandleForHeapStart` 返回什么？我们这边有 `virtual_cpu_base` 半成品。
2. **FSSM 哈希**：你们 `index.json` 里的 key 是 FSSM 整个 blob 的哈希，还是 code 段？
   （我们拦截到的容器能算出前者，后者需要先按 `0xC0 + 0x28` 切。）
3. **26,503 vs 25,734**：差 769 是版本/重打包差异，还是你们把双 payload 容器算了两次
   （你们文档说 195/26,503 有双 payload）？
4. **CS 口径**：我们按 DXIL 程序头在全量 25,734 上数出 **2,676** 条 CS，
   比 `PS\x00\x09` 库里的 3,082 还少。你们记 2,689。这 400 条的量级差从哪来的？
5. **PresentX 布局检测**：30 / 32 的"布局检测"具体检测什么？我们需要它来定队列 vtable。
6. **管线验证**：`vk_pipeline_check` 能不能对**别人的** SPIR-V 目录跑一遍？
   我们目前只有 `spirv-val`（静态校验），没有"驱动真的能建出 PSO"这一层。

---

## 十、复跑命令

以下命令在本机跑过，产物都在 `D:/GTA6/leonida-ref`（**不在任何仓库内**，
属游戏数据衍生，不要提交）。

```bash
# 0. 工具（git 的 schannel TLS 在本机不可用：SEC_E_NO_CREDENTIALS，改走 gh）
cd D:/GTA6/leonida-ref/tools
for f in shader_carve.py fssm_header.py fssm_unpack.py shader_disasm.py \
         rdna2_decode.py rdna2_isa_tables.py rdna2spirv.py shader_ir.py \
         shader_meta.py shader_stage_manifest.py replace_retry.py mc_command.py \
         report_paths.py shader_library.py fspx_unpack.py gfx10_formats.json \
         cfg_structure.py lane_masks.py wave64_emulate_opt.py wave64_halves.py \
         spirv_stubs.py spirv_asm.py spirv_asm_tables.py; do
  gh api -H "Accept: application/vnd.github.raw" \
     "repos/ProjectLeonida/Leonida/contents/tools/$f" > "$f"
done

PKG="D:/GTA6/GTA6.Xbox.v1.0.2.1-CYBERLEEK"
export PATH="/c/VulkanSDK/1.4.363.1/Bin:$PATH"     # spirv-val / spirv-dis

# 1. 雕刻容器：25,734 个，284 MB
python -I tools/shader_carve.py "$PKG/shaders.rpf" ../carve

# 2. 解包 FSSM：25,734 个，378 MB
python -I tools/fssm_unpack.py ../carve

# 3. 重组管线库并读头：programs = 21,522
python -I tools/shader_library.py "$PKG/shaders.rpf" --save ../library.bin

# 4. 单条：反汇编（纯 Python）→ SPIR-V → 校验
#    目录布局要求：<work>/index.json + <work>/<name>.dxbc + <work>/disasm/<name>.s
python -I tools/shader_disasm.py ../carve6/fssm --backend python --out ../w/disasm
python -I tools/rdna2spirv.py ../w/disasm --out ../w/spirv --assembler python
spirv-val --target-env vulkan1.2 ../w/spirv/*.spv
```

两个工具在结尾会在输出目录里 `rmtree` 一个临时目录，本机沙箱会拒一次 `chmod`
（`PermissionError: [WinError 5]`），**文件已经写完，只有 `index.json` /
`fssm_index.json` 没落盘**。`fssm_unpack` 不依赖那个索引，可以直接对目录跑；
但 `rdna2spirv.py` 需要 `<work>/index.json`（`file` + `stage` 两个字段）来定阶段，
手搓一份即可（阶段 = 容器 `DXIL` part 程序头的 kind，`0=ps 1=vs 2=gs 3=hs 4=ds 5=cs 6=lib 13=ms 14=as`）。

**LLVM 不是必需的了**：`--backend python` + `--assembler python` 全程不需要 `llvm-mc`。
本机那两条 LLVM 路都不通——`D:/GTA6/llvm-mc.exe` 只编了 x86 target（Registered Targets
里没有 `amdgcn`），`C:\Program Files\LLVM\bin` 里没有 `llvm-mc.exe`，
而用 `msiexec /a` 从本地 `LLVM-23.1.6-win64.msi` 解包会被环境挡住
（exit 1601，Windows Installer 服务不可访问）。
