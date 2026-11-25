# `xbox.*` 主机着色器内建：真清单与补齐进度

> 2026-11-25。对 `docs/pso-matrix.md` 的续篇：那份只看到"每个 shader 的**第一个**未定义符号"，
> 本篇是把整条链挖完之后的清单。改动在 `vkd3d-patch/`（`vkd3d-src/` 保持只读），
> 补丁与新增文件见 [tools/vkd3d-xbox/](../tools/vkd3d-xbox/)。

## 0. 一句话

**"13 个符号"是下界，真清单是 24 个。** 而且这 24 个不是一个难度：

| 族 | 符号数 | 调用点 | 能不能一对一映射 |
|---|---:|---:|---|
| A 内存访问（`*_with_cache` / `global_atomic_*`） | 10 | 72 | **不能**：全部带 `i64 裸地址` |
| B 三数统计（`Min3/Max3/Med3`，f32 与 u32） | 6 | 21 | 能（`Med3_F32` 已实现） |
| C wave / 通道（`LaneSwizzle` / `ReadLane` / `GetLaneID` / `Ballot64` / `MakeUniform`） | 6 | 20 | 能，但要证明等价 |
| D 杂项（`BFI` / `V_CNDMASK_B32` / `V_ALIGNBYTE_B32` / `TypedLoad.i32`） | 4 | 22 | 前三个能（`BFI` 已实现），`TypedLoad` 不能 |

关键结构差异：**A 族占了调用点的一半以上，而它恰好是最难的那一族**（`i64` 裸 GPU 地址，
Vulkan 侧没有对应指令，要走缓冲区设备地址/描述符翻译）。原计划里"7 个 `*_with_cache` 就是普通
`OpLoad`"这一句不成立——它们不是"读一个缓冲区"，是"读一个 64 位地址"。

## 1. 真清单（dummy 跑法挖出来的全量）

把未实现的符号临时替换成"该类型的零常量"（诊断开关 `XBGTASIX_XBOX_DUMMY=1`，
**只用于枚举，不能用于验证**），让翻译继续走下去，一个 shader 一个进程跑完 53 条，
再从 stderr 收集所有 `未实现：` 行。得到的 24 个符号（括号内是引用它的 shader 数）：

```
A 内存访问
  xbox.s_load_dword_with_cache      (i32 sel, i64 addr, i32, i32) -> i32            16
  xbox.global_load_dword_with_cache (i32 sel, i64 addr, i32, i32) -> i32            15
  xbox.global_load_dwordx3_with_cache(...) -> {i32,i32,i32}                         11
  xbox.global_load_dwordx4_with_cache(...) -> {i32,i32,i32,i32}                      9
  xbox.global_load_dwordx2_with_cache(...) -> {i32,i32}                              2
  xbox.global_store_dwordx3_with_cache(i32, i64, {i32,i32,i32}, i32, i32) -> void    1
  xbox.global_atomic_fmin  (i32 sel, i64 addr, f32) -> f32                           7
  xbox.global_atomic_fmax  (i32 sel, i64 addr, f32) -> f32                           7
  xbox.global_atomic_add   (i32 sel, i64 addr, i32) -> i32                           4
  xbox.global_atomic_sub   (i32 sel, i64 addr, i32) -> i32                           4
B 三数统计
  xbox.Min3_F32 (i32 sel, f32, f32, f32) -> f32                                      5
  xbox.Max3_F32 (i32 sel, f32, f32, f32) -> f32                                      5
  xbox.Min3_U32 / Max3_U32 / Med3_U32 (i32 sel, i32, i32, i32) -> i32            2/2/2
  xbox.Med3_F32 (i32 sel, f32, f32, f32) -> f32                          ✅ 已实现
C wave / 通道
  xbox.LaneSwizzle.i32 (i32 sel, i32, i32) -> i32                                    7
  xbox.ReadLane.i32    (i32 sel, i32, i32) -> i32                                    5
  xbox.GetLaneID       (i32 sel) -> i32                                              3
  xbox.MakeUniform.i32 (i32 sel, i32) -> i32                                         2
  xbox.MakeUniform.i64 (i32 sel, i64) -> i64                                         1
  xbox.Ballot64        (i32 sel, i32) -> {i32,i32}                                   1
D 杂项
  xbox.BFI              (i32 sel=0x17, i32 m, i32 a, i32 b) -> i32       ✅ 已实现
  xbox.V_CNDMASK_B32    (i32 sel, {i32,i32}, {i32,i32}) -> i32                       7
  xbox.V_ALIGNBYTE_B32  (i32 sel, i32, i32, i32) -> i32                              2
  xbox.TypedLoad.i32    (i32 sel, {ptr[as0]{i8}} desc, i32, i32) -> {i32 x5}         1
```

每个符号的第一个实参都是**常量选择子**（BFI=0x17、Med3_F32=0x0a、s_load=0x16c、
atomic_add=0x163…），不是函数参数；同一符号上恒定。所以实现里对选择子做硬校验，
值不对就大声失败——静默编错的像素比编不过更难查。

## 2. 已实现的两个（`vkd3d-patch/`）

### `xbox.BFI(sel=0x17, m, a, b) -> i32`
**PC 上等价于按位选择**：`dst = (m & a) | (~m & b)`。
名字极具误导性——它是 RDNA2 的 `V_BFI_B32`（mask 在第一个源操作数），
**不是**"把 a 的若干位插进 b、用 (width, offset) 指定"的那个 bfi。
依据：victory 的 RDNA2 降级实现 `"v_bfi_b32" => (m & a) | (~m & b)` 同构，
以及 `crates/victory-shader/src/gfx10/opcodes.rs` 里 `v_bfi_b32 = 0x14a` 的指令身份。
SPIR-V：`OpNot` + `OpBitwiseAnd×2` + `OpBitwiseOr`，逐位等价，**无近似**。

### `xbox.Med3_F32(sel=0x0a, x, y, z) -> f32`
**PC 上等价于三数中值**：`max(min(x,y), min(max(x,y), z))`。
依据：RDNA2 `V_MED3_F32`（同族 `MAX3/MIN3` 在同一行上）。
边界：NaN 传播按 SPIR-V `GLSLstd450FMin/FMax` 走（返回非 NaN 那个）；AMD 的具体规则未实测，
本批 shader 里也没有 NaN 路径的证据——**当作近似记录**，别当精确等价。
顺带一条实现坑：这份 SPIR-V 头里**没有** core 的 `OpFMin/OpFMax`，
项目统一走 `GLSL.std.450` 扩展指令（`OpExtInst + GLSLstd450FMin/FMax`）。

### 怎么验证的（以及验证到了哪一步）
不是"这 5 条变 ok"——**它证明不了**。原因写在下一节。

可用的判据是**日志位移**：cs_15 在补之前停在
`xbox.BFI ... declared but has no definition`，补之后停在**下一个**未实现符号
`xbox.s_load_dword_with_cache`。也就是"翻译器吃下了 BFI 并继续往下走"。
这是真实进度，但**不是语义正确性**——语义正确要等那 5 条 shader 整条编过、
位图级比对（那要等 A 族补完）。

## 3. 为什么"4 个符号 → 6 条 shader ok"不成立

dxil-spirv 只报**遇到的第一处**失败，所以 `docs/pso-matrix.md` 那份表是"首报视图"。
实际是**链式**的：cs_15 的链是 `BFI → s_load_dword_with_cache → …`；
cs_49 的链是 `TypedLoad.i32 → global_load_dwordx3_with_cache → global_load_dword_with_cache
→ global_load_dwordx2_with_cache → V_ALIGNBYTE_B32 → global_load_dwordx4_with_cache`。

诊断开关 `XBGTASIX_XBOX_DUMMY=1`（未实现的符号返回该类型的零常量）测出的**上界**：

| 跑法 | ok 条数 |
|---|---:|
| 参考（未打补丁） | 9/53 |
| 打了 BFI + Med3_F32 | 9/53（还没到验收点） |
| BFI + Med3 + **所有符号零常量占位** | **34/53** |

34 是"符号层补完之后的上界"，不是成果；而且剩下 19 条到了驱动被拒
（`vkd3d_create_compute_pipeline: Failed to create Vulkan compute pipeline`），
在符号语义是假的之前**没法判读**那 19 条属于哪一类问题。

## 4. 那 4 个"一对一映射"里，2 个其实不是

| 符号 | 实测形状 | 判断 |
|---|---|---|
| `global_atomic_add/sub/fmin/fmax` | `(i32 sel, **i64 addr**, T value) -> T` | **不是一对一**：Vulkan 没有"对裸 64 位地址做原子"这个指令，要走缓冲区设备地址 + `OpAtomicIAdd/OpAtomicFMinEXT` 的指针翻译。 |
| `TypedLoad.i32` | `(i32 sel, {ptr[as0]{i8}} desc, i32, i32) -> {i32×5}` | **不是一对一**：要主机**类型化资源描述符**的字段布局（我们手上没有），返回还是 5 个 i32 的结构。 |
| `BFI` | `(i32 sel, i32, i32, i32) -> i32` | ✅ 一对一，已实现 |
| `Med3_F32` | `(i32 sel, f32, f32, f32) -> f32` | ✅ 一对一，已实现 |

## 5. 建议的批次（按"每批都能独立验收"排）

1. **B 族剩下的 5 个**（`Min3/Max3/Med3_U32`）：与 `Med3_F32` 同构，机械活。
   验收判据仍是"日志位移"（它们所在的 shader 还卡在 A 族）。
2. **D 族 2 个**（`V_CNDMASK_B32` = 按位选择、`V_ALIGNBYTE_B32` = 向上对齐到字节）：
   一对一可映射，7+2 个引用。
3. **C 族 6 个**：`subgroupShuffle` / `subgroupBroadcast` / `subgroupBallot` 可表达，
   但要证明 wave 宽度语义（和 palmetto 的 wave64 近似同一类风险）。
4. **A 族 10 个**：这一族要先做一个**设计**再写代码——`i64 裸地址`怎么落到 Vulkan
   （缓冲区设备地址 + 一个地址区间描述符，还是复用 vkd3d 已有的 raw-buffer 路径）。
   它决定 72 个调用点、以及剩下 19 条 shader 能不能过驱动。

## 6. 怎么重建（Linux，无 MSVC）

```bash
# 一次性：wine 包装 mingw（这台机器上 mingw 只有 .exe，没有本地编译器）
mkdir -p /tmp/mingw-wrap
for t in gcc g++ ar strip windres widl; do
  printf '#!/bin/sh\nexec wine /mnt/winbot-software/mingw64/bin/%s.exe "$@"\n' "$t" \
    > /tmp/mingw-wrap/x86_64-w64-mingw32-$t; chmod +x /tmp/mingw-wrap/x86_64-w64-mingw32-$t
done

# 树：vkd3d-src 只读 → 拷一份改
rsync -a --exclude 'build/' vkd3d-src/ vkd3d-patch/
cd vkd3d-patch
sed 's/^needs_exe_wrapper = true/needs_exe_wrapper = true\nexe_wrapper = '"'"'wine'"'"'/' \
    build-win64.txt > build-win64-wine.txt
PATH=/tmp/mingw-wrap:$PATH meson setup build --cross-file build-win64-wine.txt -Dbuildtype=release
PATH=/tmp/mingw-wrap:$PATH ninja -C build libs/d3d12core/d3d12core.dll

# 打补丁（新增两个文件 + 三处接入 + meson 一行）
git apply ../Xbgtasix/tools/vkd3d-xbox/dxil-spirv-xbox-intrinsics.txt
cp ../Xbgtasix/tools/vkd3d-xbox/opcodes_xbox_intrinsics.{cpp,hpp} subprojects/dxil-spirv/opcodes/
PATH=/tmp/mingw-wrap:$PATH ninja -C build libs/d3d12core/d3d12core.dll
```

基线对账（重要）：用 patch 树**未打补丁**的产物跑前 4 条，HRESULT 与参考构建**逐条相同**
（cs_01 `0x80004005`、cs_02/03/04 `0x80070057`）——所以后面的差异都归因于补丁本身，
不是构建配置差异。产物大小不同（10 MB vs 20.9 MB）是 mingw 16.1 vs 16.2 与符号量差异，
行为一致。

## 7. 未统计

- 24 个符号的**选择子**在全量语料上是否都恒定：只在小样本上核对过（BFI 0x17、Med3 0x0a 在
  cs_15/18/39/40/46 上一致）。其余符号只见到一次或几次，未逐个核。
- 34/53 里那 19 条驱动拒绝的**真实原因**：现在符号语义是零占位，判读没有意义。
- `TypedLoad.i32` 返回的 `{i32×5}` 每个分量是什么：未查。

---

## 8. 2026-11-25 下午：A 族打通，22/53（原 9/53）

### 8.1 关键结论：`OpConvertUToPtr(PhysicalStorageBuffer)` 这条路成立

`xbox.global_load_dword*_with_cache` 翻成 `OpConvertUToPtr` + `OpLoad`，**Mesa 接受**：

```
cs_03  0x80070057 -> 0x00000000  ok
cs_04  0x80070057 -> 0x00000000  ok
cs_06  （store_dwordx3）ok      cs_35（atomic add/sub/fmin/fmax） ok
```

为什么能成立（源码核过，不是猜）：

| 事实 | 出处 |
|---|---|
| vkd3d 给每个 buffer 都加 `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` | `libs/vkd3d/resource.c:252` |
| `ID3D12Resource::GetGPUVirtualAddress()` 返回的就是 `vkGetBufferDeviceAddress` 的值 | `resource.c:4938`、`:2677` |
| dxil-spirv 支持 PhysicalStorageBuffer 寻址模型（含 capability/extension 声明） | `spirv_module.cpp:410-418` |

也就是说：**游戏从 vkd3d 拿到的 GPU VA 本来就是 Vulkan 设备地址**，所以裸地址解引用是可行的 ——
不需要在 shim 里另建一套 VA 表。（这一条和我们最初对 victory 的判断相反：victory 那边
`gpu_address` 返回的是 CPU 指针，那是它自己的实现选择；我们这条链路不存在那个问题。）

### 8.2 但 `XBGTASIX_FORCE_BDA` 不是免费的

vkd3d 原本只在"有 RAW_VA 绑定或根描述符"时才开 physical storage（`dxil.c:1630`）；
我们的根签名两者都没有，所以要靠环境变量无条件打开。代价实测：

```
23 条 shader 从「未实现符号」变成「dxil-spirv: Failed to emit instruction」
（cs 12-19, 21-26, 30, 31, 34, 41-43, 45, 47, 53）
```

因为寻址模型是**模块级**的，一开就影响所有描述符访问的 lowering。
**正式做法必须是"按 shader 决定"**：预扫该模块声明的函数里有没有 `xbox.global_*`，
有才开。落点在 `dxil_spv_c.cpp` 解析完成之后、`begin_converter_pass` 之前。

### 8.3 选择子不是常量（推翻了我自己前面的话）

同一个符号会给不同选择子：`global_load_dword_with_cache` 见过 `0x164`（cs_03）、`0x166`（cs_04）、
`0x167`（cs_05/33）、`0x16c`（cs_s_load）。语义由**符号名**（读几个 dword）决定，
选择子是"缓存策略/变体"，Vulkan 侧由内存模型承担。所以按 `0x160..0x16f` 这一档接受，
每个新值首次出现打一条日志留痕；超出这一档明确失败。

### 8.4 当前分布（Cold cache，Intel Mesa，FORCE_BDA=1）

| 结果 | 条数 | 说明 |
|---|---:|---|
| ok | **22** | cs 02 03 04 06 07 09 10 11 20 27 28 33 35 36 37 38 39 44 46 48 49 52 |
| `Unhandled instruction!` | 5 | bitcode 记录缺口（cs 01 08 29 50 51），与符号无关 |
| `Failed to convert function` | 23 | **FORCE_BDA 的副作用**，按 shader 开关后应能回收大部分 |
| 驱动拒绝（hr 0） | 3 | cs 05（用了浮点原子）、cs 32、cs 40 |

### 8.5 下一步（两条都不大）

1. **按 shader 开 BDA**（预扫声明函数名）→ 回收那 23 条。
2. **浮点原子**：`OpAtomicFMinEXT/FMaxEXT` 要靠 `VK_EXT_shader_atomic_float`
   （capability `AtomicFloat32MinMaxEXT` = 5612）。vkd3d 侧要确认该 feature 已启用；
   cs_05 现在是"翻译通过、驱动拒收"，就是这一步。
3. `V_ALIGNBYTE_B32` 语义未定（AMD 的字节对齐指令），要单独查，不猜。

### 8.6 按 shader 开 BDA（做完）：31/53

`XBGTASIX_FORCE_BDA` 那种全局打开换成**按模块自己的声明**判断：`need_bda` 多一条判据 ——
本模块是否声明了 `xbox.global_load_*` / `xbox.global_store_*` / `xbox.global_atomic_*` /
`xbox.s_load_*` 中的任何一个。粒度是每个 shader 一份。

| 跑法 | ok |
|---|---:|
| 参考（未打补丁） | 9 |
| 全局 FORCE_BDA + 已实现符号 | 22 |
| **按 shader 开 BDA + 已实现符号** | **31** |

新增回收 9 条（cs 05 21 22 23 24 25 26 32 40），**无回退**。
cs_05 是浮点原子那条：`OpAtomicFMinEXT/FMaxEXT` 在 Intel Mesa 上**不额外要求** feature
就编过了（capability 5612 已声明）；NVIDIA 侧未验。

**剩下 22 条**：17 条 `Failed to convert function`（用到还没实现的 `s_load_dword_with_cache` /
`LaneSwizzle` / `ReadLane` / `GetLaneID` / `Ballot64` / `MakeUniform` / `V_ALIGNBYTE_B32` /
`TypedLoad.i32`），5 条 bitcode 记录缺口。
