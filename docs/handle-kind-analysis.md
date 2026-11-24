# 资源句柄 kind 的交叉对照：GTA6 主机产物 vs victory 生成物

纯静态分析。没跑游戏、没跑 wine、没碰 GPU；`/tmp/atomscan/`、`/tmp/vm/shaders3/`、`victory-1k/`
一个字节没改（只在 `victory-1k` 里**只读**看了 `emit/lower.rs` 当证据）。
分析脚本：`tools/handle_kind_census.py`（复用了 `tools/dxil_scan.py` 的拆包/反汇编函数，
`dxil_scan.py` 本身没有改动）。

---

## 0. 摘要

| 问题 | 答案 |
|---|---|
| GTA6 侧 `dx.op.annotateHandle` 次数 | **0 次**（53/53 模块，一个都没有）。真值侧是 DXIL 1.0 风格，kind 由 `dx.op.createHandle` 的 class 字节 + 后面的访问指令共同决定，不走 annotateHandle |
| GTA6 侧 kind 取值分布 | 只有三种 class：SRV（`i8 0`）15 次、UAV（`i8 1`）117 次、CBV（`i8 2`）54 次；`0x1002/0x100B/0x100C` 这些常量在这批产物里**不存在** |
| 同一 class 对应什么用法 | SRV → 只 `rawBufferLoad`；UAV → `rawBufferLoad/Store` + 全部原子；CBV → 只 `cbufferLoadLegacy`。**53 个模块里没有一次 texture 访问，没有一个 texture 句柄** |
| victory 侧 annotateHandle | **6341 次 / 237 个模块**（254 个里有 17 个没用句柄），17 种 kind 取值 |
| 句柄级矛盾（kind 说 texture、紧邻用法是 rawBuffer*/buffer*） | **0 条**。原因是 victory 的 kind 本来就是"看用法猜"出来的，句柄级必然自洽——查错必须换到**槽口径** |
| 槽口径矛盾 | **20 个 descriptor 槽 / 17 个模块**（共 1991 个 heap 槽、254 个模块） |
| 其中最接近本 bug 的一类 | kind 标成 `Texture2D+UAV (0x1002)`、后面跟 `atomicBinOp.i32`：**2 个槽 / 2 个模块** |
| 另外两类错标 | 同一槽被标成两种 buffer kind（RawBuffer↔StructuredBuffer）13 槽；同一槽被标成 `CBuffer(13)` 又被当 `RawBuffer(11)` 用 5 槽 |

三条前提修正（与分析结论直接相关）：

1. **GTA6 这批 dump 里没有 annotateHandle**。任务书假设的"53 个模块的 annotateHandle 第三个参数分布"
   在这批素材上无法统计——不是我没找到，是数据里真没有（`tools/handle_kind_census.py gta6` 输出
   `annotateHandle 调用总数: 0`）。
2. **GTA6 侧没有 texture 用法**。53 个 compute 模块只碰 raw buffer（SRV/UAV）和 cbuffer，
   `textureLoad/textureStore/textureGather` 一次都没出现。所以"texture kind 怎么标才对"的
   真值来源只能是 DXIL 规范 + victory 自身用法，不能靠主机产物。
3. **victory 侧不存在"句柄标 Texture2D 却紧跟着 rawBufferStore"这种形态**（0 条）。
   错标出现在**同一个 descriptor 槽在不同位置被标成不同 kind**，以及**原子操作落在 texture 头上**。
   所以"字符串匹配 `store` 猜 uav"不是唯一病灶，`descriptor_data` 里"拿第一个使用点决定 kind"
   同样在出错（见 §4）。

---

## 1. 素材与方法

- GTA6 真值：`Xbgtasix/build/dumps/` 70 个文件，其中 `cs_*.bin` 53 个（另 17 个 `rs_*.bin` 是根签名，不是 shader）。
  路径照 `dxil_scan.py`：DXBC → `DXIL` 段 → XDAT `0x07` 条目 → 24 字节子头 → 裸 bitcode → `llvm-dis`。
  **53/53 反汇编成功**（`llvm-dis` 23.1.5）。
- victory 生成物：`/tmp/atomscan/ll/` 254 个 `.ll`（只读），`llvm-dis` 已成功产出，直接文本解析。
- kind 解码：`annotateHandle` 第三个参数是 `%dx.types.ResourceProperties { i32 A, i32 B }`。
  `A` 的低 12 bit 是 DXIL `ResourceKind`（1 Texture1D / 2 Texture2D / 4 Texture3D / 7 Texture2DMS /
  10 TypedBuffer / 11 RawBuffer / 12 StructuredBuffer / 13 CBuffer / 14 Sampler），bit 12（`0x1000`）是 UAV 标志。
  这个解码被数据验证过：**所有裸 kind（无 `0x1000`）的句柄只被 load 类指令使用，
  所有带 `0x1000` 的句柄才有 store/atomic**（见 §3 表）。
- 槽口径（判断"同一资源"）：把 `createHandleFromHeap` 的索引表达式折成
  `(语义变量, 常量)`——变量落到 `cbuf(<handle>,<offset>).<field>`，常量是 `lshr (add %base, C), 5` 里的
  `C>>5`。同一 CBV 字段 + 同一常量 = 同一个 descriptor 槽。C 不是 32 的倍数时记为不可精确解析，
  这类槽不参与判罪（本次 20 个异常槽全部可精确解析）。

---

## 2. GTA6 真值侧

`python3 tools/handle_kind_census.py gta6 build/dumps`

```
53 个 cs_*.bin，反汇编成功 53 个
annotateHandle 调用总数: 0
createHandle 的 class 字节分布（186 次）
   class=0 SRV          15
   class=1 UAV         117
   class=2 CBV          54
每个 class 后面出现的访问指令（全量）
   class=0 SRV: rawBufferLoad:15
   class=1 UAV: rawBufferStore:85, rawBufferLoad:65, atomicBinOp:10
   class=2 CBV: cbufferLoadLegacy:54
```

### 2.1 "kind" 在主机产物里是什么

老式 `dx.op.createHandle` 的第 2 个参数就是资源类别字节（SRV/UAV/CBV/Sampler），
**它只区分"读 / 写 / 常量 / 采样器"，不区分 raw buffer 还是 structured/texture**。
具体是什么资源，只能由后面那条访问指令推出来：

| class | 语义 | 后面跟的指令 | 次数 |
|---|---|---|---|
| 0 | SRV | `rawBufferLoad` | 15 |
| 1 | UAV | `rawBufferStore` 85 / `rawBufferLoad` 65 / `atomicBinOp` 10 | 117 |
| 2 | CBV | `cbufferLoadLegacy` | 54 |

也就是说：主机产物里 **UAV 一律是 raw buffer 通道**，没有 typed/structured/texture 的区分——
因为字节地址就在指令里。这也解释了为什么 victory 必须在 DXIL 侧把 kind 补出来，
也解释了为什么它一补错就会误导下游。

### 2.2 原子操作（最接近本 bug 的参照）

15 次 `dx.op.atomicBinOp.i32`，全部落在 class=1（UAV）句柄上，**没有一次落在 image/texture 上**。
其中 10 个句柄带原子，5 个句柄同时还有 `rawBufferLoad/Store`（同一资源既被原子又被 raw 访问）：

```
cs_02_len7876.bin   L12  createHandle(class=1)  → L210 atomicBinOp (rawBufferLoad×5, rawBufferStore×7 同句柄)
cs_03_len5504.bin   L12  createHandle(class=1)  → L51/L56 atomicBinOp (rawBufferLoad×2, rawBufferStore×2)
cs_43_len19928.bin  L27  createHandle(class=1)  → L125/L1346 atomicBinOp (rawBufferLoad×1)
（其余 10 次分散在 cs_34/35/38/39/40，句柄上只有原子，仍是 class=1）
```

坐标操作数的形状是另一条硬证据——原子只有**一维**索引：

```
cs_02  L210: atomicBinOp.i32(i32 78, %dx.types.Handle %1, i32 2, i32 76, i32 undef, i32 undef, i32 %156)
cs_03  L51 : atomicBinOp.i32(i32 78, %dx.types.Handle %1, i32 2, i32 76, i32 undef, i32 undef, i32 %31)
cs_43  L125: atomicBinOp.i32(i32 78, %dx.types.Handle %4, i32 0, i32 %71, i32 undef, i32 undef, i32 %61)
```

`(coord0, undef, undef)` = 线性字节/元素索引 = buffer 语义。texture 的原子会有 2~3 个坐标。

**结论**：在主机产物里，"被原子访问的资源" 100% 是 raw buffer。任何把原子资源标成
`Texture2D(0x1002)` 的做法，与真值相反。

---

## 3. victory 侧

`python3 tools/handle_kind_census.py victory /tmp/atomscan/ll`

```
254 个 .ll，其中 237 个有 annotateHandle，共 6341 次调用
```

### 3.1 kind 取值分布

| kind | 名字 | 次数 | 占比 |
|---|---|---|---|
| 11 | RawBuffer | 1752 | 27.6% |
| 13 | CBuffer | 1072 | 16.9% |
| 12 | StructuredBuffer | 910 | 14.4% |
| 14 | Sampler | 757 | 11.9% |
| 2 | Texture2D | 681 | 10.7% |
| 4107 | RawBuffer **+UAV** (0x100B) | 340 | 5.4% |
| 4106 | TypedBuffer +UAV (0x100A) | 195 | 3.1% |
| 4098 | Texture2D **+UAV** (0x1002) | 179 | 2.8% |
| 7 | Texture2DMS | 141 | 2.2% |
| 10 | TypedBuffer | 115 | 1.8% |
| 4108 | StructuredBuffer +UAV (0x100C) | 67 | 1.1% |
| 4 | Texture3D | 57 | 0.9% |
| 4100 | Texture3D +UAV (0x1004) | 29 | 0.5% |
| 1 | Texture1D | 26 | 0.4% |
| 32782 | Sampler (0x800E) | 11 | 0.2% |
| 4097 | Texture1D +UAV (0x1001) | 6 | 0.1% |
| 4103 | Texture2DMS +UAV (0x1007) | 3 | 0.0% |

两种编码同时存在（`11` 与 `4107`、`2` 与 `4098`…），差别只在 `0x1000` 位。
实测该位与用法完全吻合，可以确认它就是 UAV 标志：

| kind | 紧跟的访问指令（前几名） |
|---|---|
| 11 RawBuffer | `rawBufferLoad` 1743（没有一条 store/atomic） |
| 12 StructuredBuffer | `rawBufferLoad` 874 |
| 13 CBuffer | `cbufferLoadLegacy` 1072 |
| 14 Sampler | `textureGather` 59、`calculateLOD` 2 |
| 2 Texture2D | `textureLoad` 88、`textureGather` 28、`calculateLOD` 2 |
| 4107 RawBuffer+UAV | `rawBufferStore` 279、`rawBufferLoad` 15、`atomicBinOp` 7 |
| 4106 TypedBuffer+UAV | `bufferStore` 100、`bufferLoad` 68 |
| 4098 Texture2D+UAV | `textureStore` 93、`textureLoad` 64、`atomicBinOp` **2** |
| 4108 StructuredBuffer+UAV | `rawBufferStore` 43、`rawBufferLoad` 7、`atomicBinOp` 7 |

### 3.2 句柄级：0 条矛盾（以及为什么这个 0 不能当成没问题）

按"这一条 annotate 自己的句柄后面跟了什么指令"统计，跨资源族的矛盾是 **0 条**：
没有一个 `Texture2D` 句柄被 `rawBuffer*`/`buffer*` 用过。
`12 StructuredBuffer + rawBufferLoad` 出现 924 次 / 68 个模块，那是 buffer 族内部的精度差别
（raw 访问 structured 时按字节寻址），不构成"把 buffer 当 texture"，没有算进矛盾。

0 条不代表健康，因为 victory 的 kind **本来就是从用法反推的**（`emit/lower.rs:2796-2801`
拿 descriptor 的"第一个使用点"决定 kind），句柄级永远自洽。要抓错必须看**同一个 descriptor
槽在不同位置被标成了什么**。

### 3.3 槽口径：20 个槽 / 17 个模块

`python3 tools/handle_kind_census.py slots /tmp/atomscan/ll`

```
模块 254，heap 槽 1991，异常槽 20（全部索引可精确解析），异常模块 17
```

| 模块 | 槽 | 被标成 | 该槽上的访问指令 | 证据行 |
|---|---|---|---|---|
| `compute-2f03a2df10503cef` | `%19\|6` | **Texture2D+UAV (4098)** | `atomicBinOp`×1 | 4098@L3494，原子@L3522 |
| `compute-5d113aa6830760b6` | `%8\|3` | **Texture2D+UAV (4098)** | `atomicBinOp`×1 | 4098@L785，getDimensions@L786，原子@L808 |
| `compute-a1f6cdf8874960a9` | `%14\|4` | RawBuffer+UAV(4107) / StructuredBuffer+UAV(4108) | `atomicBinOp`×2 | 4107@L2649,2699；4108@L2778 |
| `compute-1374616d04451041` | `%9\|0` | RawBuffer+UAV / StructuredBuffer+UAV | `rawBufferStore`×2 | 4107@L25,51；4108@L67 |
| `compute-3cf39f4495b908d0` | `%16\|14` | RawBuffer(11) / StructuredBuffer(12, stride 176) | `rawBufferLoad`×17 | 11@L49；12@L97,151,1387 |
| `compute-4518151afe8c6c09` | `%16\|14` | RawBuffer(11) / StructuredBuffer(12) | `rawBufferLoad`×19 | 11@L49；12@L97,167,1403 |
| `compute-8f9eb68d0c6dd5cf` | `%16\|14` | RawBuffer(11) / StructuredBuffer(12) | `rawBufferLoad`×18 | 11@L49；12@L97,151,972 |
| `compute-ced45910ad577dcb` | `%19\|14` | RawBuffer(11) / StructuredBuffer(12) | `rawBufferLoad`×8 | 11@L48；12@L96,221 |
| `compute-e06a21f3cea46973` | `%16\|14` | RawBuffer(11) / StructuredBuffer(12) | `rawBufferLoad`×15 | 11@L49；12@L97,146,729 |
| `mesh-29f7805ffefc2e63` | `%9\|8` | StructuredBuffer(12) / RawBuffer(11) | `rawBufferLoad`×2 | 12@L2496；11@L3371 |
| `mesh-29f7805ffefc2e63` | `%67\|80` | StructuredBuffer(12) / RawBuffer(11) | `rawBufferLoad`×2 | 12@L2484；11@L3654 |
| `mesh-867e520305f703b2` | `%9\|11` | StructuredBuffer(12) / RawBuffer(11) | `rawBufferLoad`×2 | 12@L2884；11@L3062 |
| `mesh-dbbc6ccc56963935` | `%50\|80` | RawBuffer(11) / StructuredBuffer(12) | `rawBufferLoad`×4 | 11@L2553,2586,5800；12@L4227 |
| `mesh-dbbc6ccc56963935` | `%9\|8` | StructuredBuffer(12) / RawBuffer(11) | `rawBufferLoad`×2 | 12@L4196；11@L5438 |
| `mesh-dbbc6ccc56963935` | `%50\|89` | StructuredBuffer(12) / RawBuffer(11) | `rawBufferLoad`×2 | 12@L69；11@L92 |
| `compute-24f989b98dcb03f0` | `%28\|0` | **CBuffer(13, elem 65536) / RawBuffer(11)** | `cbufferLoadLegacy`×2, `rawBufferLoad`×49 | 13@L71；11@L260,567,639 |
| `compute-3a79195f06d3ed85` | `%28\|0` | CBuffer(13) / RawBuffer(11) | `cbufferLoadLegacy`×2, `rawBufferLoad`×21 | 13@L71；11@L260,567,639 |
| `compute-a1d865bdfb3559eb` | `%28\|0` | CBuffer(13) / RawBuffer(11) | `cbufferLoadLegacy`×2, `rawBufferLoad`×33 | 13@L71；11@L260,567,639 |
| `compute-fb332f777bae57fb` | `%20\|0` | CBuffer(13) / RawBuffer(11) | `cbufferLoadLegacy`×2, `rawBufferLoad`×37 | 13@L55；11@L269,588,660 |
| `compute-53af89c0e1dccd2a` | `%20\|0` | CBuffer(13) / RawBuffer(11) | `cbufferLoadLegacy`×2, `rawBufferLoad`×4 | 13@L87；11@L183,258,302 |

三类分开读：

**(A) 原子落在 Texture2D 头上：2 槽 / 2 模块。**
`compute-5d113aa6830760b6.ll`

```
L785  %729 = annotateHandle(i32 216, %728, ResourceProperties { i32 4098, i32 1033 })
L786  %730 = getDimensions(i32 72, %729, i32 undef)
L808  %752 = atomicBinOp.i32(i32 78, %729, i32 2 /*Or*/, %745, %747, i32 undef, i32 %751)
```

`0x1033`(=1033=0x409) 是 victory 自己给 image 描述符的哨兵值
（`emit/lower.rs:2812` → `((kind | u, 0x409), Some(kind))`）。也就是说 vkd3d 看到的是一条
"类型未知的 typed texture UAV"，没有 format，按你说的默认落到 **R32f**，而这条原子是
`atomicOr`——就是在 float 资源上做整数原子。
`compute-2f03a2df10503cef.ll` 同形：`4098@L3494` + `atomicBinOp@L3522`。

与 GTA6 真值对照：主机产物 15 次原子全部一维坐标 + raw buffer，**没有一次落在 image 上**。
所以这两个标签至少和真值不一致。

要说清的一点：这两个槽上**没有** `rawBuffer*` 访问（只有 atomic + getDimensions），
所以单凭静态数据只能说"标签可疑、与真值方向相反"，不能说"它一定是 buffer 被错标成 texture"。
另一种解释同样成立：它确实是 image 原子，缺的是 UAV format（`R32_UINT`），而不是 kind。
能区分这两者的判据在 §4 第 3 条。

**(B) 同一槽被标成两种 buffer kind（RawBuffer ↔ StructuredBuffer）：13 槽。**
最干净的一例 `compute-3cf39f4495b908d0.ll`：

```
L46  %29 = add i32 %16, 448          L94  %61 = add i32 %16, 448
L47  %30 = lshr i32 %29, 5           L95  %62 = lshr i32 %61, 5      ← 索引表达式完全相同
L49  annotateHandle(%31, { i32 11, i32 0   })      ← RawBuffer
L97  annotateHandle(%63, { i32 12, i32 176 })      ← StructuredBuffer, stride 176
```

同一个 descriptor 索引上，一处按 RawBuffer 用（`rawBufferLoad(byteOffset)`），
一处按 StructuredBuffer（stride 176）用。vkd3d 会拿到两个不同 kind 的 view。
`compute-a1f6cdf8874960a9` 更难看：槽 `%14|4` 上 `4107`(Raw, elem 0) 与 `4108`(Structured, stride 20)
两种标注**都在做原子**（L2700 / L2779），原子的坐标形状也跟着变（`(4,undef,undef)` vs `(1,4,undef)`）。

**(C) 同一槽既被当 CBuffer 又被当 RawBuffer：5 槽 / 5 模块。**
`compute-53af89c0e1dccd2a.ll`：

```
L84  %72  = lshr i32 %71, 5      （%71 = add 0, %40）
L87  %74  = annotateHandle(%73, { i32 13, i32 65536 })   ← CBuffer
L88  %75  = cbufferLoadLegacy(%74, 7)
L183 %144 = lshr i32 %143, 5     （%143 = add 0, %40）    ← 同一个索引值
L186 %146 = annotateHandle(%145, { i32 11, i32 0 })      ← RawBuffer
```

同一个 descriptor 索引，一处 `cbufferLoadLegacy`、一处 `rawBufferLoad`。
这两个 kind 互斥，必有一个错。也有另一种可能：**索引计算丢了常量**（两个本该不同的
descriptor 被算成了同一个索引），那问题在索引侧而不在 kind 侧——静态数据分不开这两者，
但两者都在 victory 的同一段代码里（`descriptor_data`）。

---

## 4. 判据：kind 只能由"后面跟着的那条访问指令"决定

从 GTA6 的 53 个真值模块 + victory 自身 6341 条标注归纳出来的规则：

### 4.1 映射表（硬规则）

| 句柄后面出现的访问指令 | 正确的 kind | 说明 |
|---|---|---|
| `rawBufferLoad` / `rawBufferStore` | `0x100B` RawBuffer（带 stride 的按结构化用则为 `0x100C`） | 字节寻址 |
| `bufferLoad` / `bufferStore` | `0x100A` TypedBuffer；`bufferLoad` 带结构步长时 `0x100C` | 元素寻址 |
| `textureLoad` / `textureStore` / `textureGather*` / `textureSample*` / `calculateLOD` | `0x1001` / `0x1002` / `0x1004` / `0x1007` … 按维度，不是 2D 就写 2D | 维度要与 `dim` 一致 |
| `cbufferLoad` / `cbufferLoadLegacy` | `0x000D` CBuffer | |
| `sampler` 类指令（作为第 2 个操作数） | `0x000E` Sampler | sampler 是独立句柄，别和资源混 |
| `atomicBinOp` / `atomicCompareExchange` | **不能单独决定**：看同槽其它访问。有 `rawBuffer*` → `0x100B`；有 `buffer*` → `0x100A/0x100C`；只有原子 + 一维坐标 → 仍按 raw/structured；只有原子 + 二维坐标且该描述符被读成 image 尺寸数据 → 才允许 texture，且必须补 UAV format（如 `R32_UINT`） | 见 4.3 |
| `getDimensions` | **不能单独决定** | 任何资源都能问尺寸；victory 现在靠它把描述符标成 Texture2D，这是错标的主要入口之一 |

UAV 位（`0x1000`）也必须由用法定：句柄被 `*Store` / `atomic*` / `bufferUpdateCounter` 用过，
或该资源的声明是 UAV。

### 4.2 现在这段代码的三个具体问题

`victory-1k/crates/victory-shader/src/emit/lower.rs`（同一段逻辑在 `:5815` 还有一份副本）：

```
2796  let ui: &Inst = &self.x.p.blocks[at.0].insts[at.1];   ← 用"第一个使用点"的指令决定整个资源的类型
2797  let un = ui.name();
2798  let uav = un.contains("store")
2799      || un.contains("atomic")
2800      || used.is_some_and(|u| u.kind == ResourceType::Uav);
2801  let u = if uav { 0x1000 } else { 0 };
2803  Fields::Image { dim, .. } => {
2804      let kind = match dim { 0 => 1, 1 => 2, 2 => 4, 4 => 6, 5 => 7, ... };
2812      ((kind | u, 0x409), Some(kind))
2814  Fields::Buffer { idxen, format, .. } => { ... 10 / 12 / 11 ... }
```

1. `un.contains("store")` / `contains("atomic")` 是字符串匹配指令名，不是读字段。
   指令名里带 `store` 但实际是读、或反之（`buffer_load_format_x`、`image_store` 的变体）都会走错。
2. **`at`/`first` 只取"第一个使用点"**。同一个 descriptor 在 shader 里被两处不同的指令用过时，
   类型由先出现的那一处决定，与后一处冲突——§3.3 的 (B)(C) 两类共 18 个槽就是这么来的。
   正确做法是：把该描述符的**全部**使用点收集起来，取交集；交集为空就报错，不要挑第一个。
3. `0x409` 是个哨兵（不是 DXIL 里任何有意义的字段）。它让 vkd3d 认为"这是 typed 资源、格式未知"，
   于是默认 `R32f`。整数原子一旦落在这种句柄上就是非法 SPIR-V。
   要么给出真实 kind + 明确格式，要么按 raw buffer 标（`rawBuffer*` 路径不需要格式）。

### 4.3 最接近本 bug 的参照

GTA6 里"同一资源既被原子又被 raw 访问"的例子（§2.2，cs_02 L12/L210、cs_03 L12/L51、
cs_43 L27/L125）：kind 在产物里根本没写，但**类别字节是 UAV、坐标是一维、同句柄上有
`rawBufferLoad/Store`**——三条一起指向"raw buffer"。把这套搬到 DXIL 侧就是：

> 先看同槽有没有 `rawBuffer*`/`buffer*`；有，就按 buffer 标（`0x100B`/`0x100C`/`0x100A`），
> 原子的存在只影响 `0x1000` 位，不影响 kind。
> 只有"同槽完全没有 buffer 访问、且描述符被读成 image 尺寸数据、原子坐标是二维"三条同时成立，
> 才允许标 texture —— 而那时必须给出格式，否则整数原子还是会绑到 `R32f` 上。

---

## 5. 影响面

口径：254 个 host 模块（124 compute / 60 pixel / 48 mesh / 22 vertex），1991 个 heap descriptor 槽，
6341 次 `annotateHandle`。

| 类别 | 槽 | 模块 | 占槽 | 占模块 |
|---|---|---|---|---|
| 原子落在 `Texture2D+UAV` 上（最接近崩溃形态） | 2 | 2 | 0.10% | 0.79% |
| 同一槽两种 buffer kind（Raw↔Structured） | 13 | 10 | 0.65% | 3.9% |
| 同一槽 CBuffer ↔ RawBuffer | 5 | 5 | 0.25% | 2.0% |
| **合计异常槽（去重）** | **20** | **17** | **1.0%** | **6.7%** |
| 参考：kind 12 + `rawBuffer*`（精度问题，不判罪） | — | 68 | — | 26.8% |

含原子操作的模块只有 10 个（16 次 `atomicBinOp.i32`，`i64` 与 compareExchange 均 0 次），
其中 2 个的原子落在 texture 标注上。这就是"崩溃面"的全部静态候选。

如果 vkd3d 的类型推断粒度不是 per-descriptor-slot 而是整张 heap，风险面会从 2 个槽扩到
"10 个含原子的模块"，但那一步静态定不了，得在 vkd3d 侧做最小复现。

---

## 6. 复现

```bash
cd /mnt/winbot-software/GTA6/Xbgtasix
python3 tools/handle_kind_census.py gta6    build/dumps        --json /tmp/handle-kind/gta6.json
python3 tools/handle_kind_census.py victory /tmp/atomscan/ll  --json /tmp/handle-kind/victory.json
python3 tools/handle_kind_census.py slots   /tmp/atomscan/ll  --json /tmp/handle-kind/slots.json
python3 tools/handle_kind_census.py victory /tmp/atomscan/ll --conflicts   # 句柄级矛盾（本批 0 条）
```

抽取路径复用 `tools/dxil_scan.py` 的 `extract_bitcode()` / `disassemble()`（没改那个文件）。
脚本只读素材；`llvm-dis` 用系统 23.1.5。

---

## 7. 没找到 / 定不下来的

1. **GTA6 侧的 annotateHandle 真值不存在**（0 次）。要拿"主机侧怎么标 kind"的直接真值，
   需要 DXIL 1.6+ 的主机模块（会走 `createHandleFromBinding` + `annotateHandle`），
   这批 dump 是 DXIL 1.0 风格（`dxil-ms-xb2` + `dx.op.createHandle`），给不了。
   能给的替代真值是"class 字节 + 访问指令"的对应关系（§2.1）。
2. **没有找到一个"kind=Texture2D 且紧邻 rawBufferStore"的句柄**。这类形态在本批 0 条；
   错标表现为槽级（§3.3）。
3. **两个原子槽（`compute-2f03a2df10503cef`、`compute-5d113aa6830760b6`）到底是
   "buffer 被标成 texture" 还是 "texture 缺 format"**，静态分不开：这两个槽上没有 buffer 访问，
   原子坐标是二维（`%95=%1<<5+%89`、`%98=%2<<5+%91`），像像素坐标；但 elem 哨兵 `0x409` +
   整数 `atomicOr` 又正好落进"默认 R32f"的坑。两条路的修法不同（改 kind vs 补格式），
   得靠一次真实编译 + vkd3d 侧 dump 才能定。
4. 5 个 "CBuffer ↔ RawBuffer" 槽里，有 4 个模块（`compute-24f989…`/`3a7919…`/`a1d865…`/`fb332f…`）
   行号完全一致，是同一 shader 的变体；**到底是 kind 标错还是索引丢了常量**，静态同样分不开。
