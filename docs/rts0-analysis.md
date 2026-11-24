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
>
> **2026-11-25 追加（见 §10）**：那 5 个的名单仍未到手 —— E1 的代码已改好（每 shader 一台新设备 +
> 显式 WARP），但本机没有 mingw / Windows，跑不了；顺带查出**上一版 t49 其实没启用 WARP**
> （`D3D12CreateDevice(nullptr)` = 默认硬件适配器：日志里 `d3d10warp.dll` 出现 0 次、
> `igd12um64xeh.dll` 75 次），所以 §29 的「WARP 48/53」应读作「Intel 驱动 48/53」。

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

（**2026-11-25 更新**：E1 要求的改法已落地在 `tests/t49_pso_warp/main.cpp`，待 Windows 侧执行即可产出名单；
本节结尾的 `cs_49..cs_53` 断言因此**标记为未证**，见 §10.4。）

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

**自洽检验（53 个 shader + 17 个 rs，70/70 通过）**：`24 + 24*n == total 字段`，
且每条记录的"尾表偏移"= `24 + 12n + 12i`，误差为零。
例：`rs_09`（n=9，240 字节）参数表 `0x18..0x84`，尾表 `0x84..0xF0`，正好收口。

**2026-11-25 修正（用 victory 的 184 个 rootsig 校准出来的）**：`total` 字段**不含静态采样器**，
正确的两条恒等式是

```
total 字段 == 24 + 24*numParams
段体长度   == total + 52*numStatic        ← D3D12_STATIC_SAMPLER_DESC 每项 52 字节
```

GTA6 这 70 个 blob 的 `numStatic` 恒为 0，所以旧写法（`24+24n == 段体长`）碰巧也对；
victory 的 rootsig 里有 59 个带 1~5 个静态采样器，旧写法会把它们全判成"不自洽"。
`tools/dxbc_rts0.py` 已改成两条恒等式 + 按 `24+24n` 之后解析 52 字节采样器记录。

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
| 参数类型分布（53 个 shader 自带的签名，共 250 个参数） | UAV 164、SRV 27、32bitConst 59、**DescriptorTable 0**、CBV 0、Sampler 0 |
| 参数类型分布（17 个 rs，共 87 个参数） | UAV 64、SRV 5、32bitConst 18、**table 0** |
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

---

## 10. 2026-11-25 追加：E1 已改好，但顺带查出一件更要紧的事 —— **上一版 t49 根本没用 WARP**

### 10.1 代码已按 E1 要求改完

`tests/t49_pso_warp/main.cpp` 现在：

1. **显式枚举 WARP 适配器**（`IDXGIFactory4::EnumWarpAdapter`，退化路径按
   `DXGI_ADAPTER_FLAG_SOFTWARE` 找），并把选中的适配器描述/LUID 打出来；
   拿不到就 SKIP，**不再**默默退回 `D3D12CreateDevice(nullptr)`。
2. **每个 shader 一台全新设备**（17 个根签名在每台设备上重建，blob 只读一次盘）。
3. 逐条输出，可 grep：
   ```
   [t49] RESULT cs_49_len38248.bin  len=38248  hr=0x887A0005 fail  removed=0x887A0005
   [t49] FAIL   cs_49_len38248.bin  len=38248  hr=0x887A0005 removed=0x887A0005
   [t49] 汇总：编译成功 N / 被拒 M / 共 53（DEVICE_REMOVED K 次）
   ```
   另外打 `GetDeviceRemovedReason()`，把"设备被移除"和"普通拒绝（如 E_INVALIDARG）"分开。

构建脚本不用改：`build.sh:562` 已经带 `-ld3d12 -ldxgi -ldxguid`。
语法已用 wine 的 `d3d12.h / dxgi1_6.h` + 一个 `ComPtr` 桩做过 `-fsyntax-only -Wall -Wextra` 检查（干净），
但**本机没有 mingw，生成不了 exe，也没跑过**。

### 10.2 为什么跑不了（不是偷懒）

- 本机是 Arch（`Linux Archlinux 7.3.5-zen1-1-zen`），**没有** `x86_64-w64-mingw32-g++`；
  `build.sh` 要求 `/d/mingw64`（git-bash on Windows）。
- 本任务明确禁用 wine 且 GPU 被另一个子代理占着，WARP 之外的路径也不该动。

⇒ 名单这一步必须到 Windows 侧执行（双系统 / 有 mingw 的那台）：

```bash
# Windows + git-bash，仓库根目录
./build.sh                       # 或只编 t49：见 build.sh:561
XBGTASIX_DUMP_SHADERS=build/dumps ./build/t49_pso_warp.exe | tee build/t49-list.log
grep -c "\[t49\] FAIL" build/t49-list.log
```

### 10.3 真正要紧的发现：`D3D12CreateDevice(nullptr, …)` 选的**不是** WARP

上一版 t49（以及 `XBGTASIX_FORCE_WARP=1` 的做法）都靠"传 nullptr 让系统选 WARP"。
`D3D12CreateDevice` 的 `pAdapter = nullptr` 意思是**默认（硬件）适配器**；要 WARP 必须
`EnumWarpAdapter`。日志侧的证据是硬的：

```
build/warp1.log（XBGTASIX_FORCE_WARP=1 那次）
  igd12um64xeh.dll   出现 75 次        ← Intel 用户态驱动
  d3d10warp.dll      出现  0 次        ← WARP 的用户态驱动，一次都没加载
  崩溃栈帧 #4..#14 全部落在 igd12um64xeh.dll 内
所有 161 个日志里 grep d3d10warp：0 个文件命中
```

由此三处叙述要改：

| 原文 | 实际 |
|---|---|
| §29 "**WARP** 编译成功 48/53" | 是 **Intel 驱动**在一个独立进程里编过 48/53（不是软件驱动） |
| §30 "连 **WARP**（软件驱动）也返回 DEVICE_REMOVED" | 那次设备仍然是 Intel 的（栈在 igd12um64xeh 里），WARP 从未被真正启用 |
| §30 标题"WARP 也丢设备" | 应改为"换成默认适配器重跑同样丢设备" |

**注意这不削弱 §30 的结论，反而把它钉得更死**：同一个 Intel 驱动在独立进程里能编过 48/53，
在游戏进程里 12 个 PSO 之后就把设备丢了 ⇒ 失败与"这批 shader 编不编得过"无关，
"我们（或经我们）执行的某个操作让设备失效"是唯一站得住的解释。
§30 的四条单变量对照（`*e` 写回 / 16 GiB 预留 / 描述符 arena / 槽 133）仍然是下一刀。

### 10.4 名单现状

`cs_49..cs_53` 这个后缀推断**未证**（不是已证也不是已否）——它依赖"失败是一段连续后缀"这个前提，
而前提本身现在也带着疑点：上一版用的是硬件驱动，失败形态可能比 §29 记的更复杂。
跑完 §10.2 就能一次定两件事：真名单，以及后缀前提成不成立。

---

## 11. 2026-11-25 追加：`XDAT` 是可解的带类型条目表 —— 版本必须当分组维度

E2 开工前先把结构钉住（`python3 tools/dxbc_rts0.py xdat <目录>`，已加进工具）。

### 11.1 布局（53/53 自洽，且 4 个 victory 样本也自洽）

```
+0  'XDAT'
+4  u32 版本              GTA6 这批 = 5；victory 的 console blob = 3
+8  u32 条目表绝对偏移     本语料恒为 36（= XDAT 起点 + 12）
+12 条目表 (type, offset, size) × N     offset 相对整个 blob
+  各条目 payload
```

**自洽式的三条**（对 53 个 GTA6 blob 全成立，无一例外）：
1. 首条 payload 偏移 == 表尾（`tab + 12N`）⇒ 条目数可由它反推 `N = (首条 offset - tab) / 12`
2. 条目不重叠（前一条 `offset+size ≤` 后一条 `offset`）
3. **末条 `offset+size` 恰好等于 blob 末尾**（例：`cs_01` 末条 0xA48+0x10 = 0xA58 = DXIL 段长 2648；`cs_34` 0x93A4+0x10 = 0x93B4 = 37812）

### 11.2 版本 5（GTA6 的 53 个）：条目类型序列固定 `0x07 / 0x0A / 0x12 / 0x0D / 0x10`

| 类型 | 尺寸 | 观察 |
|---|---|---|
| `0x07` | 1468 – 43476（随 shader 变） | **DXIL 程序块包装**：payload 开头是 `[0x00050065][一句可变 dword]`，紧跟 `'DXIL'` 子头 `{0x105, ?, size}`，再往后 24 字节处就是 bitcode（magic `BC C0 DE`），且 `bitcode 结束 == payload 结束`（53/53 成立） |
| `0x0A` | **恒 26 字节** | 未解读，53/53 唯一值 |
| `0x12` | **恒 16 字节** | 疑似摘要 |
| `0x0D` | 287 – 26764（随 shader 变） | **未解读，E2 的第一嫌疑**：与 `0x07` 的比例在 0.150 – 0.616 之间波动，不是固定比例，所以不像"同一份数据的另一种编码" |
| `0x10` | **恒 16 字节** | 疑似摘要 |

### 11.3 版本 3（victory 的 console blob）：只有 3 条，且**类型码的含义变了**

```
compute-052a2316a03b9ba1.console.dxil（7768 字节，ver=3）
  0x12 → 16 字节     0x0D → 7650 字节（**程序数据本体，但不是明文 bitcode，见 §15**）     0x07 → 28 字节
pixel-02c6cf86ca283ce1: 0x12:16  0x0D:529   0x07:28
vertex-0602c2384acbb3b2: 0x12:16  0x0D:615   0x07:28
mesh-00a5896a6ac7b7c7:  0x12:16  0x0D:1895  0x07:28
```

对照 §11.2：**v5 里 `0x0D` 是中等块、`0x07` 才是程序块；v3 里正好反过来（`0x0D` 装 bitcode、`0x07` 只有 28 字节）**。
所以：

- **跨版本按类型码比较是无效的**，必须先按 version 分组（这就是"版本当分组维度"的具体理由，不是防呆而已）。
- `0x12` 在 v3 和 v5 里都是 16 字节且位置都是第一条/中间，唯一跨版本稳定的类型。

### 11.4 一条顺手排掉的假线索

早前用 fourcc 扫标签时，victory 的 console blob 里报出过 `SAI@77`。那是**误报**：它是 16 字节摘要里的三个字节恰好是 `0x53 0x41 0x49`（`'S''A''I'`）。GTA6 这批没有这个误报，因为摘要取值不同。谁再跑标签扫描就会再撞上，记在这里省一次排查。

### 11.5 给 E2 的具体切法

1. 先把 53 个 `0x0D` 块单独落盘（`xdat` 子命令已经给出行程表：type/offset/size 都有）
2. 按"是否在 §1 的失败后缀里"分组比 `0x0D` 的内部结构（若 E1 的名单出来，这组对比就直接成立；名单没出来就先按 RTS0 组对比）
3. 版本次维度：v5 与 v3 的 `0x0D` 必须分开看，别混

---

## 12. 2026-11-25 追加：崩溃样本 `5d113aa6830760b6` 的 XDAT 对比 —— **没有异常**（支线结论：解释不了崩溃）

### 12.1 覆盖情况

崩溃样本在**两批里都在**：

```
/tmp/vm/shaders/compute-5d113aa6830760b6.{console.dxil, host.dxil, shader.bin}
/tmp/vm/shaders2/compute-5d113aa6830760b6.{console.dxil, host.dxil, shader.bin, rootsig.bin, rootsig.txt}
```

`shaders2` 一共 240 组有 console 的样本（184 组另带 rootsig），**全部 ver=3、全部 3 条目、
XDAT 三条自洽式 240/240 通过**。

### 12.2 崩溃样本 vs 其余 239 个

| 项 | 崩溃样本 | 其余分布 | 判读 |
|---|---|---|---|
| 条目数 / 类型序列 | 3，`0x12/0x0D/0x07` | 同 | 同构 |
| 自洽式 | ✅ 通过 | 239/239 通过 | — |
| `0x0D`（v3 的 bitcode） | **2406** | min 273 / p50 1559 / max 27173 | **中游，不是异常值** |
| `0x12` 尺寸 | 16 | 239 个全 16 | 同 |
| `0x07` 尺寸 | 28 | 239 个全 28 | 同 |
| `host/console` 体积比 | 3.06（7716/2524） | min 1.60 / p50 4.84 / max 23.63 | 中游 |
| 相对偏移 | 首条 payload == 表尾，末条收口 == blob 末尾 | 同 | 同 |

`0x07` 的 28 字节里有 4 字节随 shader 变（崩溃样本 `66000500 0f060080 'DXIL' 06010000 10000000 04000000 'NOBC'`，
典型样本 `66000500 bb050080 'DXIL' …`），语义未解；顺手纠正本文件 §11.2 那句"`0x07` 恒 28 字节"
应读作"**尺寸恒 28、内容不恒定**"（240 个里 223 个取值不同）。

**结论：XDAT 里没有任何可测量的异常**，崩溃样本在结构、尺寸、比值上都落在同批样本的中游。
这条支线到此为止 —— 它解释不了那次崩溃，与主线的独立复现（NVIDIA 第 0 次调用就崩、Intel 能建、
换 root signature 不影响）一致。

### 12.3 顺带：崩溃样本的 root signature 长什么样

用 `detail` 解 `/tmp/vm/shaders2/compute-5d113aa6830760b6.rootsig.bin`：

```
RTS0 v2  参数=1  静态采样器=2  total=48（段体长 152）  flags=0x00000C00
  参数[0] 32bitConst  vis=ALL  寄存器 r0 space=0  Num32BitValues=8
  静态采样器[0] reg=0 space=0 vis=ALL max_lod=12
  静态采样器[1] reg=1 space=0 vis=ALL max_lod=12
```

和它同批的 124 个 rootsig 一样是 `flags=0xc00`、单参数 + 静态采样器。也就是说
"这份 rootsig 本身也不特殊"，进一步支持主线那句"根签名不是变量"。

---

## 13. 2026-11-25 追加：**主机标志位不是固定掩码** —— 0x03000000 只在 GTA6 成立

这条是把 victory 的 184 个 rootsig 拉进来解之后才暴露的，对本仓代码有直接影响。

| 来源 | RTS0 `flags` 取值 | 非标准位 |
|---|---|---|
| GTA6（`build/dumps` 53 个 shader + 17 个 rs） | **`0x03000000`** | bit24 + bit25 |
| victory 的标题（`shaders2` 184 个 rootsig） | **`0xC00`（124 个）/ `0xC01`（60 个）** | bit10 + bit11（`0xC01` 还带标准位 `ALLOW_INPUT_ASSEMBLER`） |

`0x03000000` 拆开正是本仓 `root_sig.h:44-58` 记的那两位：
`0x01000000`（memory-based ABI）+ `0x02000000`（compute-2 spill 区）—— 与 §28 的说法一致
（**这两个位确实是主机专有的 ABI 位，也确实全部置上了**；§28 的问题只在于它不是"哪几个 shader 崩"的判别量）。

**但对另一个标题就完全失效**：本仓的剥离掩码是

```cpp
kXboxRootFlags = 0x01000000 | 0x02000000 | 0x80000000   // = 0x83000000
pc_flags(x)  = x & ~kXboxRootFlags
xbox_bits(x) = x &  kXboxRootFlags
```

拿 victory 的 `0xC00` 去套：`xbox_bits(0xC00) == 0` ⇒ `strip_blob_for_pc()` 直接 `return false`，
**一个字节都不改、也不落盘（`dump_root_sig` 只在剥离成功的分支里调用）**。
反过来说，如果哪天真去跑别的标题，写死 `0x03000000` 会让剥离静默失效。

建议改成"只保留 PC 认识的位"这种写法，两个标题都能覆盖：

```cpp
// 标准位只有 D3D12_ROOT_SIGNATURE_FLAGS 的 0x1..0x40；其余一律按主机位处理
inline constexpr std::uint32_t kPcKnownFlags = 0x7Fu;
inline constexpr std::uint32_t pc_flags (std::uint32_t f) { return f &  kPcKnownFlags; }
inline constexpr std::uint32_t xbox_bits(std::uint32_t f) { return f & ~kPcKnownFlags; }
```

（这条只是建议，`src/` 没动。改之前要先在 GTA6 上回归：`0x03000000` / `0x80000000` 都会被归入主机位，
行为等价；但要确认没有把某个桌面真用的位剥掉。）

---

## 14. 2026-11-25：掩码改规则式（已在 GTA6 上回归）+ `dump_root_sig` 移出剥离分支

### 14.1 回归结果：旧掩码与新规则在 GTA6 上 **70/70 等价**

把 70 个 blob（53 个 shader 内嵌 RTS0 + 17 个 `rs_*.bin`）的 flags 逐个跑两套算法：

| | 旧：`kXboxRootFlags = 0x03000000|0x80000000` | 新：`kPcKnownFlags = 0x7F` |
|---|---|---|
| `cs_*`（53 个，flags `0x03000000`） | pc=`0x00000000`，主机位=`0x03000000` | pc=`0x00000000`，主机位=`0x03000000` |
| `rs_*`（17 个，flags `0x00000000`） | pc=`0x00000000`，主机位=`0` | 同 |

- **等价 70 / 不同 0**
- 语料里出现过的 D3D12 标准位（`0x1..0x40`）：**一个都没有** ⇒ 新规则没有"该留的被剥掉"的风险
- `0x03000000` 与 `0x80000000` 都整体落进主机位（不在 `0x7F` 里），与预期一致

### 14.2 改了什么

| 文件 | 改动 |
|---|---|
| `src/d3d12_xs/root_sig.h` | `kXboxRootFlags`（写死 `0x83000000`）→ `kPcKnownFlags = 0x7F`；`pc_flags = f & kPcKnownFlags`、`xbox_bits = f & ~kPcKnownFlags`。旧常量留作 `kXboxRootFlagsLegacy` 对照。注释里写清代价：微软日后若加 >0x7F 的新标准位会被误剥，届时补上那一位并重跑本文件头注的回归 |
| `src/d3d12_xs/device_abi.h` | `dump_root_sig()` 从"剥离成功分支"里挪出来：**不管剥没剥成，只要 `XBGTASIX_DUMP_SHADERS` 开着就落盘**；新增第三个参数 `flags_before_strip`，原始主机位写进文件名 `rs_%02d_len%zu_flags%08X.bin`（落盘内容仍是喂给桌面的那份，已剥位，t49 直接可用）；未剥的情况补一条日志"消费端根签名未剥 …（要么本来就是 PC 值，要么掩码不认这个标题）" |

**为什么文件名带 flags**：剥离之后段体里就是 PC 值，原始主机位在产物上空了。上一轮那个静默失效
（另一标题 `0xC00` 掩码不认 ⇒ 不剥 + 不 dump + 无日志）就是因为没有任何地方留痕。

### 14.3 编译验证（本机没有 mingw，用 wine 头 + 桩做语法检查）

```
wineg++ -fsyntax-only -std=c++17 -Wall -I/tmp/wrlstub -Isrc -Iinclude \
        -D_snprintf=snprintf /tmp/tu.cpp     # tu.cpp = #include "d3d12_xs/device_abi.h"
```
- 与 `HEAD` 对比：**改动前后诊断条数一致（12 条 warning，均为此前既有）**，无新增
- 唯一 error 是 `_snprintf` —— wine 的 stdio.h 没有它（mingw 有），属检查环境差异，用 `-D` 绕过后零 error
- 仍未生成 exe、未运行（本机无 mingw / 无 Windows）

---

## 15. 2026-11-25：E2 第一遍 —— 53 份 DXIL 反汇编成功，ABI 分歧在**程序块里看得见**

新工具 `tools/dxil_scan.py`（纯本地，不需要 DXC/Windows）：

```
DXBC → DXIL 段 → XDAT[0x07] → DXIL 子头(24B) → 裸 bitcode → llvm-dis
```

本机 `llvm-dis`（LLVM 23）能读 DXC 出的旧位码，**53/53 反汇编成功**。

### 15.1 硬结论

| 项 | 结果 |
|---|---|
| `target triple` | **`dxil-ms-xb2`**（53/53）—— "xb2" 就是 Xbox Series X\|S，模块自己写着的 |
| `%dx.types.Handle` 的定义 | **`type { ptr }`**（53/53）—— 句柄是**裸 64 位指针**，这就是 memory-based ABI 的机械证据（桌面 DXIL 是 `{ i32, i32, i32, i32 }` 的四元组） |
| 对 float 做原子（`call float @dx.op.atomic*`） | **0 个** |
| 原子操作的返回类型 | 只有 `i32`，8 个 shader 用了 `atomicBinOp`（cs_02/03/34/35/38/39/40/43） |
| raw（untyped）缓冲访问 | **51/53**（只有 cs_06、cs_45 不用） |
| typed texture/buffer 访问 | **0/53** |

### 15.2 对我们那条"PSV0 声明 R32_FLOAT 却做整数原子"的判读

那条维度**在 GTA6 语料上不存在** —— 不是"没检出"，是**表达不出来**：这批 shader 全走
`rawBufferLoad/Store`（untyped，字节寻址），加一个 `{ ptr }` 句柄直指内存，**没有 typed UAV
声明，也就没有"声明的格式"可对不上**。所以：

- 你们抓到的 `OpAtomicOr %float` / `OpImageTexelPoint %_ptr_Image_float` 是**另一套机制**（typed UAV），
  和 GTA6 这批不是同一条路；拿 GTA6 语料验证不了它，也不该拿它当 GTA6 的解释
- 反过来说，GTA6 侧的分歧点现在很明确了：**`dxil-ms-xb2` + `{ ptr }` 句柄**。
  DXIL 语法合法（validator 过、WARP 能编 48/53），N 卡的崩溃更可能是它没走过
  "句柄即指针"这条编译路径

### 15.3 顺手的指纹（给 E1 名单出来后做对照用）

`cs_48`（后缀推断里的最后一个成功）vs `cs_49`（推断的第一个失败），程序块差别巨大，但都不是原子：

```
cs_48  lines= 97  createHandle=4  rawLoad=5   rawStore=3   cbuffer=2
       op 指纹: rawBufferLoad.i32 ×5, rawBufferStore.i32 ×3, threadId.i32 ×2, cbufferLoadLegacy.i32 ×2
cs_49  lines=3900  createHandle=5  rawLoad=3   rawStore=10  cbuffer=4
       op 指纹: tertiary.i32 ×129, tertiary.f32 ×49, rawBufferLoad.i64 ×48, binary.f32 ×41, legacyF16ToF32 ×22
```

`cs_49` 里 `rawBufferLoad.i64 ×48` + `tertiary.*` 密集，和其余样本的画风明显不同；
等 E1 的真名单出来，这份指纹可以直接按"失败组 / 成功组"做统计对比。

---

## 16. 2026-11-25：v3 的 `0x0D` 不是明文 bitcode —— 中间还有 `XBCH` + zlib + `FSSM`

（修正 §11.3 / §12 里"v3 的 `0x0D` 是 bitcode 本体"的措辞。）

```
0x0D payload（compute-5d113aa6830760b6，7650 字节）
  58 42 43 48 = 'XBCH'
  03000000           版本 3
  4e090000 00000000  压缩后长度 0x94E = 2382
  531a0000 00000000  解压后长度 0x1A53 = 6739
  78 9c ...          ← zlib 流
解压后：46 53 53 4d = 'FSSM'，后面是条目表 + 0xDEADBEEF 填充
  FSSM 内部：没有 BC C0 DE、没有 'DXIL'、没有 PSV0/STAT 任何标签
```

也就是说：**victory 落盘的 console blob 里没有明文 DXIL**（v3 路径是"压缩 + 主机子模块"），
只有 v5（GTA6 这条路）才是 `0x07` 里直接放裸 bitcode。两点影响：

1. 任何人想从 `/tmp/vm/shaders{,2}` 里捞 DXIL 去反汇编，**捞不到**（我试过，llvm-dis 直接
   "Invalid bitcode signature"）；要么解 `FSSM`（还没解），要么从他们自己的编译路径拿
2. §12 的 XDAT 对比结论不受影响（比的是条目表与尺寸，不是 bitcode 内容）
