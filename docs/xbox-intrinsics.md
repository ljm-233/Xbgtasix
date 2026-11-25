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
