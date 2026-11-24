# RTS0 定点诊断：53 个 compute shader 的根签名到底有没有"系统性差异"

> 2026-11-25。对象：`build/dumps/` 里 53 个 `cs_*.bin` + 17 个 `rs_*.bin`。
> 工具：`tools/dxbc_rts0.py`（纯 Python 标准库，不依赖 Windows）。
> 交叉验证来源：`/tmp/vm/shaders/`（victory 落盘，只读，未改动一个字节）。
>
> **一句话结论**：RTS0 **没有**可用来区分"编得过 / 编不过"的系统性差异。53 个 shader 的
> RTS0 全部是 `flags=0x03000000` 的 RTS1.1 根签名，去掉 flags 后**逐字节命中** 17 个游戏根签名里的某一个；
> 其中最硬的一条对照是 cs_48 与 cs_49：两者的 RTS0 字节完全相同，一个编过、一个编不过。
> 所以第 28 节"问题在 RTS0 的主机 ABI 位"这个假设，作为**逐个 shader 的判别量**不成立。
> 真正还没被看的是每个 shader 都带的 `XDAT` 块（`XDAT@24 ver=5`）与 DXIL 程序块本身。

---

## 1. 任务①：WARP 那 5 个失败的是谁 —— **原始记录里没有名单**

### 查过哪些地方（都没有）

| 位置 | 结果 |
|---|---|
| `docs/transient-block-wall.md` §29 | 只有汇总数字 `编译成功 48 / 被拒 5 / 共 53`，**没有逐条名单** |
| `git show 642856d`（提交信息） | 同上一句，外加一句解释"被拒的 5 个是 WARP 设备在试错中被移除后的连带失败" |
| `build/*.log`（161 个） | 没有一个含 `编译成功 48` / `WARP 编译成功` / `被拒 5` |
| `build/suite/report.txt:46` | `t49	t49_pso_warp.exe	SKIP	2	118ms	SKIP` —— 最后一次全套跑是 **SKIP**（当时没有 dump 目录），根本没产生名单 |
| `git log -S "被拒" --all` / `git stash list` / `git branch -a` | 只有 642856d 命中；没有其它分支、没有 stash、没有未跟踪的输出文件 |

结论：**t49 的逐条输出只在当时的终端里，没有落盘，仓库里也没有。名单不存在。**

### 但可以给一个强推断：**cs_49 / cs_50 / cs_51 / cs_52 / cs_53**（推断，不是记录）

推理链（每一环都能被证伪，所以写清楚）：

1. t49 的遍历顺序是 `FindFirstFileA("<dir>\\cs_*.bin")`（`tests/t49_pso_warp/main.cpp:81`）。
   文件名是两位数补零的 `cs_01…cs_53`，NTFS 目录索引按名排序（大小写不敏感）⇒ 枚举顺序等于序号顺序。
   `cs_01..cs_12` 的长度与现有 12-PSO 基线日志里的 PSO#1..#12 **逐条一致**
   （5136 / 7876 / 5504 / 18748 / 29996 / 4376 / 4072 / 5256 / 4716 / 4100 / 4328 / 13216），
   说明 dump 序号与游戏侧 PSO 序号同序，序号可信。
2. §29 自己写的失败原因是 `0x887A0005 = DEVICE_REMOVED`，且"**后续组合全部连带失败**"。
   "后续…全部"⇒ 失败的是一段**连续后缀**。
3. 后缀长度 = 5 ⇒ 第一个失败的是第 `53-5+1 = 49` 个 ⇒ `cs_49`，其后 `cs_50..cs_53` 陪葬。

**这条推断的可证伪点**：如果 WARP 设备是在某个文件的"某一组根签名组合"中途死的，
那该文件自己也计入失败，失败集合仍是后缀，结论不变；只有当 Enumeration 顺序不等于序号顺序时才会错。
要一锤定音，见 §8 的 E1（每个 shader 单独开一个 device 重跑 t49），十分钟就能出真名单。

顺带一句：`cs_13..cs_53` 在现有日志里找不到对应的 `PSO #13..#53`（所有日志最大只到 `#12`），
说明它们来自更早那次"53 个 PSO 全部建成"的运行（提交 3ba1380），序号只能靠长度推，不能逐条对账。

---

## 2. 解析器：`tools/dxbc_rts0.py`

```
python3 tools/dxbc_rts0.py list   build/dumps            # 53 个 shader 的 RTS0 摘要
python3 tools/dxbc_rts0.py detail build/dumps/cs_01_len5136.bin
python3 tools/dxbc_rts0.py cmp    build/dumps            # shader RTS0 ↔ rs_*.bin 配对
python3 tools/dxbc_rts0.py stat   build/dumps            # 按 RTS0 指纹分组
python3 tools/dxbc_rts0.py pairs  /tmp/vm/shaders        # console.dxil vs host.dxil（只读）
```

DXBC 容器解析（magic / 16 字节摘要 / 版本 / 总长 / 段数 / 段偏移表 / 各段 `fourcc`+长度）已验证：
53 个 shader 全是 **9 段**，`SFI0 / ISG1 / OSG1 / PSV0 / RTS0 / STAT / ILDN / HASH / DXIL`，
`total` 字段与文件长度逐个一致（`detail` 会打印"声明一致"）。

### 顺手挖出来的两件事

1. **`STAT` 段第一个双字是 `0x00050065`**，53/53 一致 —— 这是 shader model 的另一种独立佐证
   （第 28 节当时是从 `DXIL` 段读的版本号）。
2. **每个 shader 的 `DXIL` 段内部还套着 `XDAT` 子块**（`XDAT@24 ver=5`，53/53）。
   `DXIL` 段体布局：`+0` 程序版本 `0x00050065`、`+8` `'DXIL'` 子块、`+24` `'XDAT'` 子块、
   `+120` 才是 LLVM bitcode（magic `BC C0 DE`）。第 28 节列的"九个 chunk"里没提这个，
   而它正是主机专有数据最可能待的地方（见 §7）。

---

## 3. RTS0 的真实布局（把 `root_sig.h` 的"每项 24 字节"掰开）

`src/d3d12_xs/root_sig.h:132-141` 已经钉死了 24 字节头：

```
+0  u32 版本（2 = RTS1.1）
+4  u32 根参数个数 n
+8  u32 参数表偏移（恒 0x18）
+12 u32 静态采样器个数
+16 u32 段体总长
+20 u32 Flags            ← 主机位 0x03000000 就在这里
```

注释说"1.1 每项 24 字节"，实测 24 是 **12 + 12**：

```
+24            参数记录 n 项 × 12 字节：[Type, aux, 尾表偏移]
+24+12n        尾表   n 项 × 12 字节：[ShaderRegister, RegisterSpace, Extra]
```

**自洽检验（53 个 shader + 17 个 rs，70/70 通过）**：`24 + 24*n == 段体总长`，
且每条记录的"尾表偏移"= `24 + 12n + 12i`，误差为零。
例：`rs_09`（n=9，240 字节）参数表 `0x18..0x84`，尾表 `0x84..0xF0`，正好收口。

> 踩过的坑：按 24 字节直接切片，会把"下一条记录的前半"当成上一条的后半，
> 于是读出 `type=5`、`type=6` 这种**非法值**，从而误判"存在 DescriptorTable"。
> 本脚本第一版就是这么错的，改过来之后类型全部落在 `0/1/2/3/4` 里。

字段语义（`Type` 用 D3D12 官方枚举：0=DescriptorTable, 1=32bitConst, 2=CBV, 3=SRV, 4=UAV；
`aux` 推断为 `ShaderVisibility`，全部为 0=ALL；`Extra` 对 32bitConst 是 `Num32BitValues`）：

```
cs_01 的 RTS0 段体（96 字节，十六进制）：
0000  02 00 00 00 03 00 00 00 18 00 00 00 00 00 00 00   ← v=2, n=3, table=0x18, static=0
0010  60 00 00 00 00 00 00 03 03 00 00 00 00 00 00 00   ← total=0x60, flags=0x03000000
0020  3c 00 00 00 04 00 00 00 00 00 00 00 48 00 00 00   ← rec0=SRV, aux=0, 尾@0x3C
0030  01 00 00 00 00 00 00 00 54 00 00 00 00 00 00 00   ← rec1=UAV, rec2=32bitConst
0040  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   ← 尾表：r0/sp0 ×2
0050  00 00 00 00 00 00 00 00 00 00 00 00 10 00 00 00   ← 尾表末项 Extra=0x10（16 个常量）
```

解读：`SRV(r0,space0) / UAV(r0,space0) / 32bitConst(r0,space0,16)`，`visibility=ALL`，`static sampler=0`。

---

## 4. 语料统计：53 个 shader → 16 个不同根签名

| 项 | 值 |
|---|---|
| shader 数 | 53（全部 9 段、全部 `STAT[0]=0x00050065`、全部 `XDAT@24 ver=5`） |
| rs 数 | 17，**去掉 flags 后只有 16 个不同体**（`rs_14` 与 `rs_17` 完全相同，同一根签名建了两次） |
| shader 的 RTS0 flags | **53/53 = `0x03000000`** |
| rs 的 RTS0 flags | **17/17 = `0x00000000`** |
| 参数类型分布（全语料 281 个参数） | UAV 164、SRV 27、32bitConst 24、**DescriptorTable 0**、CBV 0 |
| 静态采样器 | **0**（所有 17 个根签名） |
| `ShaderVisibility` | 全部 `ALL`（0） |
| `RegisterSpace` | 全部 0 |
| 32bitConst 的常量个数 | 1/2/3/4/5/6/8/10/14/16/19 都有 |

分组（shader 数 → 命中的 rs）：

```
11 → rs_01  (SRV/UAV/c32,   3 参数)     7 → rs_05  (UAV/UAV/c32,  3)
 6 → rs_03  (c32/c32/UAV/UAV, 4)        4 → rs_09  (UAV×5/SRV×3/c32, 9)
 4 → rs_15  (c32/UAV/UAV/SRV, 4)        3 → rs_04  (UAV×6/c32, 7)
 3 → rs_10  (UAV×8/c32, 9)              3 → rs_13  (UAV/UAV/c32, 3)
 2 → rs_02  (c32/UAV×5, 6)              2 → rs_06  (UAV×7/c32, 8)
 2 → rs_11  (UAV×3/c32, 4)              2 → rs_14 = rs_17 (UAV×3/c32, 4)
 1 → rs_07 / rs_08 / rs_12 / rs_16
```

### 这一节的附带结论：**这批根签名里一个描述符表都没有**

17 个根签名 100% 由"根描述符（根级 SRV/UAV 的 64 位 GPU 地址）+ 一块 32 位常量"组成，
没有 `DescriptorTable`、没有静态采样器、register space 全是 0、寄存器号就是 0..N 顺序。
这与"memory-based descriptor ABI"吻合：游戏把资源全走根描述符直连内存，不经描述符堆/表。
**这一点比 flags 重要得多**（见 §7）。

---

## 5. 系统性差异：**没有** —— 而且有一组决定性对照

### 5.1 全部 53 个 shader 的 RTS0 都能在 17 个 rs 里逐字节找到对应

`cmp` 的输出断言：

```
未匹配：0 / 53
53 个 shader 全部落在 "与 rs_XX 只差 flags 等头部字节（差字节 +0x17）"
```

只差 **一个字节**（`+0x17`，就是 flags 双字的最高字节：`0x03000000` → `0x00000000`）。

这一条同时证明两件事：

- **消费端剥离是生效的**：`rs_*.bin` 是在 `strip_blob_for_pc()` 成功**之后**才落盘的
  （`src/d3d12_xs/device_abi.h:1899-1900`：`if (strip_blob_for_pc(...)) { dump_root_sig(...) }`），
  所以 17 个 rs 的 flags 全 0，而 shader 自带的副本保留 `0x03000000` —— 两侧的关系被钉死了。
  附带一个 caveat：`dump_root_sig` 只在"剥离成功"的分支里调用，**没有主机位的根签名不会被落盘**，
  所以"17 个"不等于"游戏总共只建了 17 个"。
- **flags 位不是崩溃判别量**：t49 是把 `cs_*.bin` **原始字节**（带 `0x03000000`）直接喂给
  `CreateComputePipelineState` 的（`tests/t49_pso_warp/main.cpp:151-164`，没有任何改写），
  而 WARP 编过了 48 个 ⇒ `0x03000000` 至少对 WARP 的编译器**无害**。

### 5.2 决定性对照：cs_48 与 cs_49

```
cs_48_len4500.bin   RTS0 len=120  md5(内容)=916778a0  flags=0x03000000  [c32/UAV/UAV/SRV]
cs_49_len38248.bin  RTS0 len=120  md5(内容)=916778a0  flags=0x03000000  [c32/UAV/UAV/SRV]
cs_48 与 cs_49 的 RTS0 逐字节相同? True
```

按 §1 的后缀推断，cs_48 是**第 48 个成功**、cs_49 是**第一个失败**，而两者 RTS0 一模一样
（同一组 `rs_15`，cs_50/cs_51 也是同一组）。RTS0 相同而结果不同 ⇒ **RTS0 解释不了这次失败**。

同理，同一根签名组内部本来就是"一个编过、几个编不过"，任何"按 RTS0 分组"的判别都会在这里断掉。

### 5.3 那么还有什么差别？只剩程序块

同组内唯一变化的是 DXIL：cs_48 是 4500 字节容器（DXIL 段约 1.9 KB），cs_49 是 38248 字节
（DXIL 段约 35 KB）。也就是说，差异只能落在**bitcode 主体**或 `XDAT` 里，
两者都属于"程序"而不是"根签名"。

---

## 6. 交叉验证：victory 那边**根本没有 RTS0 可以转发**

`pairs /tmp/vm/shaders`（228 组成对文件，只读）：

```
成对文件 228 组；含 RTS0 的：console 0 / host 0
```

三类文件的实际形状（以 `compute-052a2316a03b9ba1` 为例）：

| 文件 | 长度 | 形状 |
|---|---|---|
| `.console.dxil` | 7768 | **裸 Xbox blob**，不是 DXBC 容器：`+0` 程序版本 `0x00050066`、`+8 'DXIL'`、`+24 'XDAT'`、`+88 'XBCH'`、末尾再出现 `'DXIL'`；**没有 RTS0 段** |
| `.shader.bin` | 8332 | DXBC 容器 7 段：`SFI0, ISG1, OSG1, PSV0, HASH, DXIL, PRIV`；`DXIL` 段就是 console 那份**原样 7768 字节**；**没有 RTS0** |
| `.host.dxil` | 49724 | DXBC 容器 6 段：`SFI0, ISG1, OSG1, PSV0, HASH, DXIL`；**没有 RTS0** |

所以：

- **"victory 是不是原样转发了主机的 root signature"这个问题在 shader 层不成立**：
  主机的 blob 里压根没有根签名段，victory 也无从转发。他们的 host 版容器补的是 `PRIV`
  （私有数据），不是 `RTS0`。
- 反过来说，**GTA6 的 GDK 容器把根签名塞进 shader 的 `RTS0` 段是个带有游戏自身特征的形态**，
  不能当成"GDK 通例"。第 28 节把"RTS0 主机 ABI 位"当成通用病灶，其实是把
  "这家游戏把根签名随 shader 一起带"当成了普遍情况。
- 另外注意版本号差异：Xbgtasix 这批是 `0x00050065`（53/53），victory 的 console 是 `0x00050066`
  （低字节差 1）。按第 28 节的解读方式，两者是**不同的 shader model 档位**；
  这条差异只记下来，不下结论（victory 是另一个项目、另一批 shader，不可直接类比）。

---

## 7. 对第 28 / 29 / 30 节结论的修正建议

| 原结论 | 现在的证据 | 建议表述 |
|---|---|---|
| §28"问题不在格式转换，在主机 ABI 对齐（RTS0 含 memory-based ABI 等位）" | RTS0 里唯一的非标准内容就是 `flags=0x03000000` 一个双字；53/53 一致；WARP 用它编过 48 个；同组对照里 RTS0 完全相同结果不同 | 降级为**未证实**：`0x03000000` 是目前唯一可见的主机位，但它解释不了哪几个 shader 崩 |
| §28"把 RTS0 也剥掉主机位 → 3376→2860 退步" | 剥的是 shader 侧的 flags，等于让 shader 声明的 ABI 与程序块不一致；退步符合"一致性被破坏" | 保留结论，但归因改为"程序块与 RTS0 的不一致"，不是"ABI 位本身有毒" |
| §29"48/53 成功 ⇒ DXIL 合法、问题在硬件驱动" | 5 个失败是**同一台 WARP 设备被移除后的连续后缀**（§1），不是 5 个独立失败 | 改为"至少 48 个 shader 能被 WARP 编过；剩余 5 个是设备已死的连带结果，不能算独立失败" |
| §30"WARP 也丢设备 ⇒ 我们某个操作让设备失效" | 与本文不冲突；RTS0 已排除 | **保留**，且优先级高于继续折腾 RTS0 |

**真正还没被看的地方（按可疑度排序）**：

1. 每个 shader 的 `XDAT` 子块（`XDAT@24 ver=5`）。它是主机专有数据块，最可能装
   memory-based descriptor 的布局/句柄翻译元数据。第 28 节的问题清单里提过 XDAT，但没解析。
2. DXIL bitcode 主体（`DXIL` 段 `+120`，magic `BC C0 DE`）。若用了"内存式描述符"，
   bitcode 里会出现 `dx.op.rawBufferLoad/Store`、`dx.op.annotateHandle` 一类别的东西
   （本机没有 dxc/llvm，无法直接反汇编；字符串表里也没留下 `dx.op.` 名字，
   说明名字被裁了，得靠 `libLLVM` 或 `dxc -dumpbin` 才能读）。
3. 遗留的 `0x03000000`：它到底是不是"Xbox 内部 XDXR 位"，看一眼 `XDAT` 就能定。

---

## 8. 下一步建议（可直接照做）

**E1（优先，10 分钟，能补上本文§1缺的名单）**
改 `tests/t49_pso_warp/main.cpp`：把 `D3D12CreateDevice` 挪进 shader 循环，**每个 shader 一个全新 WARP 设备**，
失败就记录 `文件名 + hr` 再接着下一个。跑出来的 5 个才是真名单，而不是后缀推断。
（现在的写法一台设备编 53 个，一次 `0x887A0005` 就把后面全带走了。）

**E2（验证 §7 的 XDAT 猜想）**
写个小工具把 `cs_*.bin` 的 `DXIL` 段拆成 `DXIL` / `XDAT` 两个子块落盘，
比 53 个 `XDAT` 的字节分布 —— 若失败的那几个（按 E1 定的）在 `XDAT` 上分成两类，就抓到真病灶了。
（`tools/dxbc_rts0.py detail` 已经能定位 `XDAT@24`，再往下就是按 `+28` 的版本和 `+32` 的段长拆表。）

**E3（RTS0 分支如果还要跑一次，只跑这一个对照）**
只把 shader 的 `RTS0` `+20` 的 flags 清零、**其它字节一个不动、也不重算 DXBC 摘要**，
与 `XBGTASIX_STRIP_SHADER_RTS0=1`（走 `strip_blob_for_pc` 的完整路径）分开对比。
已知结果：后者退步（3376→2860）。前者如果 hr 仍是 `0x887A0005`，RTS0 分支就可以整体结案。

**E4（按 §30 的顺序做单变量对照）**
`XBGTASIX_PSO62_OUT=0` → 关 16 GiB 预留 → 关描述符 arena → 槽 133 只做 placed。
RTS0 已被排除，这四条现在是**唯一**还没被排除的嫌疑。

**不建议再做**：继续在 RTS0 的 flags / 参数类型 / 描述符 range 里找"哪一位不对"。
数据已经说明那里没有可区分的位。

---

## 9. 复现命令与证据清单

```bash
# 环境：任意 Linux + python3，不需要 Windows
python3 tools/dxbc_rts0.py list   build/dumps | tee /tmp/rts0-list.txt
python3 tools/dxbc_rts0.py cmp    build/dumps | tee /tmp/rts0-cmp.txt   # 未匹配 0/53
python3 tools/dxbc_rts0.py detail build/dumps/cs_01_len5136.bin
python3 tools/dxbc_rts0.py detail build/dumps/rs_01_len140.bin         # 与上面只差 +0x17 一个字节
python3 tools/dxbc_rts0.py pairs  /tmp/vm/shaders                      # 228 组，RTS0 全无
```

证据一览：

| 结论 | 证据 |
|---|---|
| 53 个 shader 全是 9 段 DXBC + DXIL | `list`；`detail` 的 `chunks=9` 与 `total` 一致 |
| 53/53 `flags=0x03000000`，17/17 rs `flags=0` | `list`、`cmp` 两节输出 |
| 去掉 flags 后 53/53 逐字节命中 rs | `cmp`：`未匹配：0 / 53`，且只差 `+0x17` |
| RTS0 不能区分成功/失败 | cs_48 与 cs_49 的 RTS0 逐字节相同（120 字节，md5 同为 `916778a0`） |
| 无描述符表、无静态采样器 | 参数类型统计：UAV 164 / SRV 27 / c32 24 / table 0 |
| victory 两侧都无 RTS0 | `pairs`：`含 RTS0 的：console 0 / host 0` |
| 5 个失败没有名单 | §1 的六处检索全部落空 |

未改动：`/tmp/vm/shaders/`（只读扫描）、victory 的任何目录、任何游戏进程或 wine。
