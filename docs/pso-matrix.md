# PSO 矩阵：53 个游戏 compute shader 在 Vulkan 路线上到底能不能编

> 2026-11-25。MVG-2 的产物。工具：`tools/pso_matrix.py`（驱动 + 映射）、
> `tools/pso_matrix/pso_matrix.cpp`（建 PSO 的 PE 侧）、`tools/pso_matrix_classify.py`（按原因分档）。
> 数据：`build/dumps/` 里 53 个 `cs_*.bin` + 17 个 `rs_*.bin`（游戏真实送进
> `CreateComputePipelineState` 的字节码，`XBGTASIX_DUMP_SHADERS` 落盘）。
> 全程没跑游戏。

## 一句话结论

**53 个里能编 9 个（17%），而且 Intel（Mesa ANV）与 NVIDIA 的结果逐条完全相同。**

两件事同时被这条数字定死了：

1. **不是驱动的问题。** 两台完全不同的 Vulkan 实现给出同一份 ok 名单、同一批错误码
   → 失败发生在驱动之前，在 **dxil-spirv（DXIL→SPIR-V 翻译器）** 里。
   Windows 侧的 `DEVICE_REMOVED`（`transient-block-wall.md` §30）因此有了一个更可能的解释：
   驱动撞上的是同一批它解不开的符号。
2. **Vulkan 路线现在吃不下这批 shader，但缺口是可枚举的**：**13 个主机外部符号**
   + **一类 bitcode 记录**。不是"要重写渲染器"，也不是"驱动不兼容"。

## 结果表（两台 GPU）

| 分档 | 条数 | HRESULT | 含义 |
|---|---:|---|---|
| `ok` | **9** | `0x00000000` | 编过了（cs 07, 09, 10, 11, 20, 38, 44, 48, 52） |
| `undef-symbol` | **38 / 39** | `0x80070057`（E_INVALIDARG） | DXIL 里引用了**只有声明、没有定义**的主机外部函数 |
| `unhandled-instr` | **5** | `0x80004005`（E_FAIL） | dxil-spirv 的 LLVM bitcode 读取器遇到未实现的记录类型 |
| `timeout` | 1（仅 Intel） | — | cs 31 在 Mesa 上 60 s 没返回；同一份在 NVIDIA 上正常返回 undef-symbol |
| `crash` | **0** | — | 两台都没有驱动 trap（`vkguard` 想兜的那种事故这次没出现） |

厂商差异只有一条，就是 cs 31 在 Mesa 上挂起。**其余 52 条的判定完全一致。**

原始表：`build/pso_matrix/result_intel.tsv`、`result_nvidia.tsv`（列：cs / rs / shader_len /
rs_len / rs_hr / pso_hr / device / retried / verdict / reason / secs），
分档表：`build/pso_matrix/classify_{intel,nvidia}.tsv`。

## 13 个主机外部符号（这就是要补的清单）

`undef-symbol` 那 38 条全部落在 13 个符号上，按波及面排序：

| 符号 | 需要它的 shader 数 | cs 序号 |
|---|---:|---|
| `xbox.s_load_dword_with_cache` | 9 | 12, 13, 14, 16, 17, 19, 30, 31, 43, 45（NVIDIA 上多 cs 31） |
| `xbox.LaneSwizzle.i32` | 7 | 22–28 |
| `xbox.global_load_dword_with_cache` | 6 | 03, 04, 41, 42, 47, 53 |
| `xbox.global_load_dwordx3_with_cache` | 4 | 05, 33, 36, 37 |
| `xbox.Med3_F32` | 3 | 39, 40, 46 |
| `xbox.BFI` | 2 | 15, 18 |
| `xbox.global_load_dwordx4_with_cache` | 1 | 02 |
| `xbox.global_store_dwordx3_with_cache` | 1 | 06 |
| `xbox.MakeUniform.i32` | 1 | 21 |
| `xbox.global_load_dwordx2_with_cache` | 1 | 32 |
| `xbox.V_ALIGNBYTE_B32` | 1 | 34 |
| `xbox.global_atomic_add` | 1 | 35 |
| `xbox.TypedLoad.i32` | 1 | 49 |

读法：这 13 个不是 13 个独立难题，它们分三族，**一族一个实现**：

- **带 cache 的 load/store 族 7 个**（`*_with_cache`，dword / x2 / x3 / x4 / store / `s_load`）
  —— 主机上"走 L1 缓存的标量/向量读写"。在桌面 Vulkan 上都是普通 `OpLoad`/`OpStore`
  （cache 语义由内存模型和 barrier 承担），差别只在**地址空间**和**是否假定一致内存**。
- **wave/通道族 3 个**（`LaneSwizzle.i32`、`MakeUniform.i32`、`V_ALIGNBYTE_B32`）
  —— 主机 VGPR/SGPR 语义：通道交换、提到标量寄存器、对齐到 byte。
  `subgroupShuffle` / `SubgroupBroadcast` / 位运算可以表达，但要**证明等价**（有语义风险，见下）。
- **整数/浮点辅助 3 个**（`BFI` 位域插入、`Med3_F32` 三数中值、`global_atomic_add`）
  —— 这些是一对一可映射的：`BFI`→`bitfieldInsert`，`Med3`→min/max 组合，
  `atomic_add`→`OpAtomicIAdd`。**没有语义风险。**

## `unhandled-instr` 那 5 条（cs 01, 08, 29, 50, 51）

定位到源码：`dxil-spirv` 的 `bc/module.cpp:1870`，那是 LLVM **bitcode 记录**解析的
`default:` 分支 —— 不是 DXIL 指令，是**容器里出现了它没实现的记录类型**。
已实现的记录有 `INST_*`（CALL/RET/BR/PHI/GEP/LOAD/STORE/CAST/SELECT/…）+ `DEBUG_LOC`，
它缺的是剩下的那几种（`FENCE`/`INDIRECTBR`/`LANDINGPAD`/`VAARG` 一类）。

所以这一族**与 shader 语义无关**，修法是"换一个更新的 dxil-spirv 或补解析"，
而不是"补符号实现"。vkd3d 3.2.0 里这份 dxil-spirv 是 vendored 拷贝、没有版本号
（`subprojects/dxil-spirv/meson.build` 只写 `version: '0.0'`），**未统计**它落后上游多少。

## 与 Windows 侧"48/53"的对账（重要，别混用）

`transient-block-wall.md` §29 记过"WARP 编译成功 48/53"，`rts0-analysis.md` §10.3 后来又查出
那次实际用的是 Intel 硬件驱动（`igd12um64xeh.dll` 出现 75 次、`d3d10warp.dll` 0 次）。
拿它和本表对比要注意三件事：

| | Windows / Intel igc（§29，存疑） | Linux / vkd3d + Mesa（本表） |
|---|---|---|
| 成功数 | 48/53 | **9/53** |
| 谁在编译 | D3D12 runtime + 厂商编译器 | dxil-spirv 先翻成 SPIR-V，再交给 Mesa |
| 对未定义符号的态度 | **可能是宽松的**（生成调用/当外部符号处理），也可能那次 48 条本来就不同 | **硬失败**：`Failed to convert function` |

也就是说：**Vulkan 路线目前比 Windows 更严格，少过 39 条。** 这不代表 Vulkan 不可行，
代表"要不要为这 13 个符号补一层"。反过来说，如果 Windows 侧确实宽松到能出图，
那 `DEVICE_REMOVED` 的原因就不是这些符号 —— 这件事**现在还没结论**，
`rts0-analysis.md` §18 的红标（"合法的是字节流，不合法的是它引用的东西"）正好可以用这张表去验。

顺带一条对照：`victory-fork` 能出世界画面，是因为它**自己把主机 RDNA2 重编译成 DXIL**
（README：mesh 8,191/8,506、VS/PS 10,617/12,346、CS 1,643/2,539），从来没让任何编译器去读
游戏原始的 DXBC。它和本表不是同一条路，也不是同一个缺口。

## 对路线选择的影响

| 观察 | 结论 |
|---|---|
| 失败与 GPU 厂商无关 | 换卡、换驱动、换 WARP 都不会改善；**别再往"驱动 bug"这个方向投时间** |
| 13 个符号里有 3 个是一对一可映射（`BFI`/`Med3_F32`/`global_atomic_add`） | 补完这 3 个，`undef-symbol` 从 38 条降到约 33 条（cs 15,18,39,40,46 解封） |
| 7 个 `*_with_cache` 是同一族 | 一次实现覆盖 22 条 shader（约 40%） |
| 3 个 wave/通道族有语义风险 | 要按"近似"记录在案（和 palmetto 的 wave64 近似同一类问题），别当精确等价 |
| 5 条卡在 bitcode 解析 | **不在我们的代码里**，只能换 dxil-spirv 版本；优先级最低（占 9%） |

所以"Vulkan 路线要吃下这批 shader"的真实工作量是：

```
补 13 个外部符号的实现（其中 10 个低风险、3 个需证明）
+ 换/补 dxil-spirv 的 bitcode 记录解析
= 覆盖全部 53 条（理论上），而不是"重写渲染器"
```

## 方法（含三个把人坑住的环境问题）

一个 shader 一个进程：驱动在 `vkCreateComputePipelinePipelines` 里崩会带走整个进程，
一个进程跑 53 条的话第一条崩掉之后全是连带失败、且拿不到名单（`rts0-analysis.md` §1 就是这么丢的）。
`tools/pso_matrix.py` 每条 shader 起一次 wine，`--resume` 可反复重入，结果**写穿**（每条落盘）。

三个坑，症状都很有迷惑性：

| 症状 | 真因 | 判据 / 修法 |
|---|---|---|
| `unimplemented function d3d12core.dll.D3D12GetInterface`，进程立刻 abort | 加载到 wine builtin 那个 49 KB 空壳，**不是** vkd3d 的 20.9 MB 实现 | vkd3d 的 `d3d12.dll` 用 `LoadLibraryA("d3d12core.dll")` 找实现，按 **exe 所在目录** → 系统目录的顺序。**exe 必须和两个 dll 同目录**；保险再加 `WINEDLLOVERRIDES="d3d12core,d3d12=n,b"` |
| 任何 Vulkan 程序 `core dumped`，stderr 只有 `vkguard: loaded into pid N` | `~/.local/lib/vkguard` 那个层的 `guard_GetInstanceProcAddr` 在 `vkCreateInstance` 之前一律返回 NULL（`next_gipa` 还没赋值），`vkEnumerateInstanceVersion` 等查询拿到 NULL 后调用方解引用 | `VK_LOADER_LAYERS_DISABLE='VK_LAYER_VKGUARD_compute'`（该层现已停用） |
| 日志里 `Failed to find 'vkGetInstanceProcAddr' in ... liblsfg-vk-layer.so` | LSFG 帧生成层的 .so 与本机 loader 不兼容；**不致命**，后面跟着 `Skipping layer.` | `DISABLE_LSFGVK=1`（只是想清日志） |

## 复现

```bash
cd Xbgtasix
python3 tools/pso_matrix.py --icd intel                 # 53 条，约 4–6 分钟（单核显）
python3 tools/pso_matrix.py --icd nvidia
python3 tools/pso_matrix.py --icd intel --start 0 --limit 12 --resume   # 分块跑，可重入
python3 tools/pso_matrix_classify.py --icd all          # 三档分类 + 符号清单
```

`tools/pso_matrix.py` 自动把这些钉住：`WINEDLLOVERRIDES=d3d12core,d3d12=n,b`、`WINEDEBUG=-all`、
`VKD3D_DEBUG=err`、`VK_LOADER_LAYERS_DISABLE=VK_LAYER_VKGUARD_compute`、`DISABLE_LSFGVK=1`，
并把 `d3d12.dll`/`d3d12core.dll`/`pso_matrix.exe` 一起摆到 `build/pso_matrix/bin/`。
单条 stderr 存在 `build/pso_matrix/tmp/<cs>.err` —— **改分类规则不用重跑矩阵**。

## 未统计

- 那 9 条"ok"编出来的 SPIR-V **是否语义正确**：只证明驱动接受并建出了管线对象，
  像素/数值级正确性要等真机首帧（和 `leonida-crossref.md` §2.1.1 的 `vk_pipeline_check` 同一边界）。
- 5 条 `unhandled-instr` 具体缺哪个记录：dxil-spirv 只打一句 `Unhandled instruction!`，
  不带记录号；要精确定位得改它的源码（只读参考，没动）。
- cs 31 在 Mesa 上为什么挂起：未查。NVIDIA 上它正常返回 undef-symbol。
- `rts0-analysis.md` §1 的"5 个失败名单"：本表用**另一套判据**（Vulkan 侧）给出了 9/38/5，
  和 Windows 侧的 48/5 不是同一把尺子，不能相互替换。
