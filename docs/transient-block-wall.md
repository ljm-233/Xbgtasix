# 0x247AF2D 那堵墙：跳过它在物理上不成立

写于 2026-11-24，证据来自真机（Intel 核显，`XBGTASIX_SKIP_GAME_ASSERT=247AF2D`）。

## 1. 采集到的三个操作数（真机实测，与静态分析完全一致）

```
begin = [rsp+0x70]        = 0x0
end   = [rsp+0x78]        = 0x4000000      (64 MiB)
g     = [game+0x75E6FF0]  = 0x0
end - begin = 0x4000000（无无符号回绕）
+ g         = 0x4000000 > 0x1FFFFF  -> 触发 int3
```

`0x4000000` 是游戏里的立即数（另一分支是 `0x5200000`），由 `game+0x140075140`
那个平台门闩二选一；两条分支都 ≫ 2 MiB。`begin` 在本函数里被清零后再无写入者。
**没有任何 shim 返回值参与这条比较。**

## 2. 跳过它之后立刻撞到同一子系统的第二堵墙

用 `XBGTASIX_SKIP_GAME_ASSERT=247AF2D` 显式跳过（VEH 里 `Rip += 1` 并从 `rcx` 恢复 `rsi`），
游戏越过断言、继续执行约 100 行日志，然后崩在：

```
FATAL 异常 0xC0000005 at game_scarlett_master.exe+0x5C8FA96
   rax = 0x0
   rsi = 0x7ff6dcd32a60   <- 指向 game+0x56A2A60 那个 thunk
```

把 `rsi` 处的机器码 dump 出来：

```
56 57 48 83 EC 28        push rsi / push rdi / sub rsp,0x28
89 D0                    mov eax,edx
25 00 C0 00 00           and eax,0xC000
3D 00 80 00 00           cmp eax,0x8000
74 17                    je  ...
... 48 8B 05 ...         mov rax,[game+0x75E6FF0]
```

**这就是 `g_1475E6FF0`（瞬时块基址）那个拦截器的兄弟函数**（`0x5C8F990 / 0x5C8FA00 /
0x5C8FA60` 三个登记/注销/转发 thunk 共用同一套记账）。它读 `g`，`g == 0`，
于是 `rax = 0`，随后解引用即崩。

## 3. 结论：跳过断言只能把崩溃推迟约 100 行

那条约 2 MiB 的上界是在**守一个内存记账机制**：

- `g_1475E6FF0` 是"瞬时块基址"，唯一写点在 `0x1456A2ACA`（上面那个 thunk 的首次分配分支）
- 拦截条件是 `flagged & 0xC000 == 0x8000`
- `g` 恒为 0 ⇒ **这条被拦截的分配在 PC 环境下从未发生**

所以真正要修的不是断言，而是**让这个记账机制按游戏预期启动**：要么游戏的那类分配
走了我们的 `xmem` 而没有经过它自己的 thunk，要么它依赖的主机侧前提（页属性/VA 区间）
在我们这里不成立。

## 4. 下一步（按成本排序）

1. **查清 `g_1475E6FF0` 为什么不被写**：在 `0x1456A2ACA` 下写断点（或用 VEH 的单步），
   看游戏是否调用过 `0x1456A2A60`；若从未调用，追它的 6 个静态调用点
   （`0x15E6D6D / 0x4EC5B87 / 0x4EC5D06 / 0x4ECC658 / 0x501F6FE / 0x5BDBA67`）
   看哪一个在 PC 环境下被跳过，以及跳过的原因（通常是一个 feature/属性查询返回值）。
2. **对照 `xmem` 的可见性语义**：主机那个机制很可能依赖"这块内存的 CPU/GPU 可见性
   与缓存属性"，而我们用 Win32 `MEM_COMMIT` 满足它，属性上对不上；
   `XMemVirtualQuery` 返回的 `XMemBasicInformation` 需要更贴近主机语义。
3. 若 1/2 都指不出方向，再考虑"替游戏补上那个瞬时块"的 hack（把 `g` 指向一块我们
   自己分配的可写内存），但这属于掩盖不变量，必须门控 + 大声记录，且优先度最低。

## 5. 相关

- 主机 vtable 偏移的开放问题：`docs/host-vtable-offset.md`
- PSO 槽取证（游戏目前走不到）：`tests/t47_pso/README.md`
- 根签名剥离与"消费端需再剥一次"：`tests/t46_rootsig`（见 `src/d3d12_xs/root_sig.h`）

## 6. 2026-11-24 追加：探针证明该机制**不经过我们的 shim**，且 0x8000 是"释放"

在 `xmem` 的每个入口加了探针：只要某个参数满足 `(v & 0xC000) == 0x8000` 就记一条。
真机复跑（带陷阱跨过 0x247AF2D）结果：

- **探针 0 命中**
- 实际用到的分配类型只有：`0x20406000`(0x6000) / `0x20003000`(0x3000) / `0x20402000`(0x2000)
  / `0x42000` / `0x20403000`(0x3000) / `0x20401000`(0x1000)
- `XMemAlloc(...)` 调用次数：0

### 语义修正：0x8000 在 Win32 里是 MEM_RELEASE

| 值 | 含义 |
|---|---|
| 0x1000 | MEM_COMMIT |
| 0x2000 | MEM_RESERVE |
| 0x4000 | MEM_DECOMMIT |
| **0x8000** | **MEM_RELEASE** |
| 0x10000 | MEM_FREE |

所以 `game+0x56A2A60` 那个 thunk 拦的是**释放**（`flags & 0xC000 == 0x8000`），
不是分配。它的兄弟 `0x5C8FA60/0x5C8FA96` 在释放路径上读 `g_1475E6FF0` 当块基址。

**结论**：游戏在**释放一块从未分配过的瞬时块**（`g == 0`），
而 `game+0x247AF2D` 的守卫把这个 64 MiB 的区间当成"块 + 剩余量"去比 2 MiB 上限，
必然失败。

### 下一轮该查什么

1. 那块瞬时块的**分配路径**是谁（不是 `0x8000` 类，所以要找 `g_1475E6FF0` 的写入点
   `0x1456A2ACA` 被谁触发——在 VEH 里对 `EXCEPTION_SINGLE_STEP` 或写断点采样）
2. 该分配是否依赖某个查询的返回值（`XMemIsToolsMemAvailable` / 工作集统计 /
   某个 feature 枚举），我们的实现是否给了"让游戏关掉该机制"的答案
3. 若确认是游戏侧的 feature 判断，考虑让那个查询返回主机口径的值（而不是改守卫）

## 7. 2026-11-24 追加：这条守卫在两条分支下都不可满足 ⇒ 我们在错误的代码路径上

守护式是：

```
(end - begin) + g <= 0x1FFFFF        # 2 MiB - 1
```

`begin` 在本函数被清零后无写入者；`end` 由 `game+0x140075140` 的平台门闩二选一：
`0x4000000`(64 MiB) 或 `0x5200000`(82 MiB)。

**两条分支都 ≫ 2 MiB，而 `g` 在式子里是"加"上去的**（加得越多越超），
所以：

- 把 `g` 补上一块合法内存**不能**让它通过（反而更糟）
- 改平台门闩的返回值也**不能**让它通过

结论：**这堵守卫在当前到达方式下不可满足**，说明我们走在了错误的代码路径上——
正确的环境下，执行流根本不该走到这里。要找的不是"怎么满足它"，而是
"**哪个更早的判断让我们走上了这条路**"。

### 优先怀疑对象

1. **`D3D12ConfigureVirtualMemoryX` 目前是空实现（直接 `return S_OK`）**
   —— 游戏用它注册显存/虚拟内存区间；参考实现会校验 flag 与页表并登记配置。
   我们回了成功但什么都没登记，游戏可能因此选了一条"大块瞬时内存"的路径。
2. **设备能力查询**（`CheckFeatureSupport` 一族）：如果某个能力在我们这里返回了
   "支持"，游戏会启用对应的快路径。
3. **`XMemGetWorkingSetStatistics` / `XMemIsToolsMemAvailable`**：内存模式判断。
4. 主机专用的 **feature/config 枚举**（xgameruntime 里目前是桩的那批）。

### 下一轮的具体实验

在 `d3d12_xs` / `xgameruntime` 的每个"查询类"导出上加一行日志（返回值 + 参数），
跑一次真机，把**崩溃前所有查询的返回值**按时间序列比对：
找出"如果换个值就不会走大块路径"的那一个，再按主机语义修正它。
这比继续在守卫附近打转有效得多。

## 8. 2026-11-24 追加：`D3D12ConfigureVirtualMemoryX` 收的是**主机 GPU VA 区间**，而我们是空实现

给这个函数加了参数取证后，真机输出：

```
D3D12ConfigureVirtualMemoryX config=0x...  前 64 字节:
    1FBADD1A180 1FBADD1C0D0 1FBADD25CA0 1FBADD27000
    1FBADD2C3E0 1FBADD2D540 1FBADD2C500 1FBADD2F2D0

  config[0]=0x...A180 -> 0x8000000100000000 0x1 <ptr> 0x0
  config[1]=0x...C0D0 -> 0x8000000000000000 0xE00270ACB5001B2 ...
  config[2]=0x...E800 -> 0x8000000000000000 <ptr> 0x0 ...
```

特征非常明确：**首字段是 `0x8000000000000000` 基址的主机 GPU 虚拟地址**
（`0x8000_0001_0000_0000`、`0x8000_0000_0000_0000`），后面跟着类别/长度/下一级指针。

也就是说：游戏通过这个调用把它的**GPU 虚拟地址空间布局**交给设备层登记。
我们现在的实现是：

```cpp
__declspec(dllexport) HRESULT WINAPI D3D12ConfigureVirtualMemoryX(void* device, void* config) {
    xbgtasix::logf("... PC 由驱动管理显存，返回 S_OK");
    return S_OK;      // 什么都没登记
}
```

于是游戏侧那张"GPU VA → 分配记录"的表**从未被播种**。这与第 6 节观察到的
`g_1475E6FF0`（瞬时块基址）恒为 0 高度吻合：那个机制按地址查记录，
而记录表是空的。

### 下一轮的具体动作

1. 把这 8 个指针指向的结构**完整解析**（现在只 dump 了 4 个 qword）：
   判断每一项是 `{va, count, next_ptr, ...}` 还是别的形状（用长度 + 指针链验证）
2. 在 `xmem` 里建一张 **GPU VA → 区间** 登记表，`D3D12ConfigureVirtualMemoryX`
   把配置逐项登记进去
3. 让 `XMemVirtualQuery` / 地址换算路径能查到这些区间（主机语义：地址属于已登记
   的图形区间时，报回主机口径的属性）
4. 复跑，观察 `g_1475E6FF0` 是否开始被写入

这条线索比"补瞬时块基址"健康得多：它在**修真实的语义**，不是掩盖不变量。

## 9. 2026-11-24 追加：config 不是统一的区间数组

按 32 字节步长逐项 dump 后，形状被证伪：

```
config[0][0]: va=0x8000000100000000 f=0x1 next=0x...  pad=0x0     <- 像 {va, flag, ptr, 0}
config[0][1]: va=0x11 ...
config[1][0]: va=0x8000000000000000 f=0xE00E1EE0CF0E1A1 ...
config[1][3]: va=0x735F656D61672F2E ...                            <- 这是 UTF-16 文本！
```

把 `config[1]` 那组 qword 按 UTF-16LE 解出来，是可辨认的**模块路径片段**：

```
"Windows\ApiPortectio..."        (疑似 Windows\ApiProtection\...)
"./game_scarlett_master.exe" 的片段
```

结论：

1. `config` 里**混着两类内容**：`0x8000000000000000` 基址的主机 GPU VA，
   以及模块路径字符串。所以它不是"统一长度的区间数组"。
2. 我按 32 字节步长遍历的假设**错误**，读出来的"记录"其实是相邻的不同数据。
3. 要正确解析，需要 `D3D12ConfigureVirtualMemoryX` 的**结构定义**：
   优先查 GDK 头文件（`D:/GTA6/gdk/include_clean`）里是否已有该类型；
   其次用"长度字段 + 指针链"试探（每次只信任能在两个指针处自洽的假设）。

### 修正后的下一步

1. **先在 GDK 头里找结构定义**（`grep -rn ConfigureVirtualMemory`）—— 有定义就不用猜
2. 若头里没有，改成"只读第一层 64 字节 + 校验"的保守进程：
   不假设步长，只记录 `(va, 0x8000... 前缀)` 的项，作为后续登记表的种子
3. 登记表本身的实现（GPU VA → 区间）可以**独立于结构解析**先做：
   `D3D12ConfigureVirtualMemoryX` 把能识别的 VA 端点记进表，
   `XMemVirtualQuery` 能查到；结构解析清楚后再补全字段语义

### 已有文档里的旁证（重要）

`docs/leonida-crossref.md:138-146` 早就记过一条相关事实：

> 标题于 `game+0x1E1D01B` 触发 int3，所属函数只引用两个导入
> `d3d12_xs.dll!D3D12ConfigureVirtualMemoryX` 与 `xmem.dll!XMemVirtualAlloc`（后者 6 次）。
> **`D3D12ConfigureVirtualMemoryX` 返回之后紧跟的断言对返回值和分配结果有硬要求，
> 我们现在"记日志 + 返回 S_OK"是恰好没炸，不是对了。**

这与本轮的观察互相印证：该函数的**返回值**和它之后那次分配的**结果**都被游戏严格检查，
所以"空实现 + S_OK"是当前一切诡异路径的可能源头之一。

补充事实：GDK 头文件（`D:/GTA6/gdk/include_clean`）里**没有** `ConfigureVirtualMemory`
的结构定义，参考实现也没有实现它 —— 结构与语义只能靠真机取证。

## 10. 2026-11-24 追加：播种瞬时块基址**无效**，排除"只缺非空指针"的假设

实验（`XBGTASIX_TRANSIENT_BLOCK=1` + 跳过断言）：在陷阱里为
`g = game+0x75E6FF0` 播种一块我们自己分配的 2 MiB 可写内存，再越过守卫。

真机结果：

```
⚠ 跳过游戏断言 game+0x247AF2D
实验：已为瞬时块播种 g=0x1ce04e10000（2 MiB，仅测试释放路径）
FATAL 异常 0xC0000005 at game+0x5C8FA96
    rax=0  rsi=0x...56A2A60  rdi=0x...ed80  rdx=0xFFFFFFFF
```

**崩溃点、寄存器形态与播种前完全一致**（同为 `rax=0`）。所以：

- 排除了"释放路径只缺一个非空块基址"这个假设
- 那个 `rax=0` 来自别的东西（`rdx=0xFFFFFFFF`、
  `rsi` 恰好是 `0x56A2A60` 那个 thunk 的地址，说明它是在拿函数指针做某种分派）

### 目前排除掉的假设（都带证据）

| 假设 | 结论 |
|---|---|
| 守卫可由 `g` 满足 | ✗ 算术上不可能（g 是加项，两条分支都 ≫ 2 MiB） |
| 守卫可由平台门闩切换分支满足 | ✗ 64 MiB / 82 MiB 都超 2 MiB |
| 瞬时块机制经过我们的 shim | ✗ `xmem` 探针 0 命中 |
| 崩溃只因 `g == 0` | ✗ 播种后崩溃点与寄存器形态不变 |
| `D3D12ConfigureVirtualMemoryX` 空实现是上游原因 | **未验证**（结构定义缺失，仅真机取证可定） |

### 下一轮建议

1. 对 `game+0x5C8FA96` 所在的 `0x5C8FA60` 函数做**完整反汇编 + 调用点还原**，
   找出 `rax=0` 的来源（这是目前唯一还没被证伪的最近原因）
2. 同时验证 `D3D12ConfigureVirtualMemoryX` 的上游假设：把它的返回值改成失败
   （`E_NOTIMPL`）跑一次，看崩溃点是否前移/改变 —— 这是最便宜的因果检验

## 11. 2026-11-24 追加：因果检验 —— 该函数**必须成功**，问题在"内容"不在"返回值"

用 `XBGTASIX_VMCFG_FAIL=1` 让 `D3D12ConfigureVirtualMemoryX` 返回 `E_NOTIMPL`，真机结果：

```
[d3d12_xs] D3D12ConfigureVirtualMemoryX -> E_NOTIMPL（因果检验）
[xbgtasix] FATAL 异常 0x80000003 at game_scarlett_master.exe+0x1E1CFE9
```

反汇编 `0x1E1CFE9` 前后：

```asm
141e1cfe0  call 0x145f3fd60      ; 内部函数，实际上会走到我们的导出
141e1cfe5  test %eax,%eax
141e1cfe7  jns  0x141e1cfea      ; 非负（S_OK/S_FALSE）就继续
141e1cfe9  int3                  ; 负值 -> 立刻断言
```

结论：

1. **游戏硬依赖这个调用成功** —— 返回失败会立刻在下一行断言
2. 所以"空实现 + S_OK"是**必须保留**的表象，但它掩盖了真正要做的事：
   **把 config 里的 GPU VA 区间真正登记下来**
3. 这也排除了"改成失败能让游戏走别的路径"的可能

### 至此的完整因果链（已证）

```
D3D12ConfigureVirtualMemoryX 必须返回成功  (本轮实证：失败 -> 0x1E1CFE9 断言)
        ↓ 但我们没登记 config 里的 GPU VA 区间
游戏侧"GPU VA -> 记录"的表是空的
        ↓
瞬时块机制查不到记录，g_1475E6FF0 恒为 0
        ↓
game+0x247AF2D 的 "跨度 <= 2 MiB-1" 守卫恒假（begin=0, end=64MiB, g=0，算术上不可满足）
        ↓
即使跳过守卫，后续释放路径仍因缺少记录而在 game+0x5C8FA96 处 rax=0 崩溃
```

下一步只有一条路：**真正实现 config 的登记语义**（结构靠真机取证），
而不是继续在守卫/瞬时块附近做规避。

## 12. 2026-11-24 追加：跳过守卫后的崩溃是**链表遍历**，且两种 rsi 恢复策略都过不去

反汇编 `0x5C8FA70`（跳出守卫后第一个被调用的函数）：

```asm
145c8fa88  lea  rax,[0x146843758]   ; 哨兵/链表头
145c8fa8f  mov  rcx,rax
145c8fa92  mov  rax,[rax+0x18]      ; next = node->next
145c8fa96  cmp  [rax],rsi           ; ★ 崩在这里：rax 已被 next 变成 0
145c8fa99  jne  0x145c8fa8f
145c8fa9b  cmp  [rax+0x10],rdi      ; 第二个键
```

即：这是一个"按 (rsi, rdi) 两个键在链表里找节点"的注销函数（对应 `0x247AD64` 那次
登记）。找不到匹配就会一路 next 到 null，然后解引用崩溃。

### 两种恢复策略的实测

| 策略（`XBGTASIX_TRAP_RSI`） | 结果 |
|---|---|
| `i`：`rsi = game+0x56A2A60`（scope id）、`rdx = rcx` | 仍崩在 `0x5C8FA96` |
| `n`：完全不改 `rsi/rdx` | 也过不去（回到断言处） |
| 默认：`rsi = rcx`（上一轮） | 仍崩在 `0x5C8FA96` |

### 结论：跳过守卫这条路**走不通**

守卫被跳过意味着对应的**登记/注销配对被打断**，链表不变量随之破坏；
换哪个键都找不回一致状态，因为那条登记在"正确环境"下本该由瞬时块机制完成，
而机制从未启动。

所以：

- 「可恢复地越过 `0x247AF2D`」这个目标**在本构建里做不到**（守卫不可满足 +
  跳过会破坏后续链表不变量，两者都有实测）
- 真正的入口只有一条：**让瞬时块机制按规定启动**，即补上
  `D3D12ConfigureVirtualMemoryX` 的真实登记语义（`config` 结构仍缺定义）

## 13. 2026-11-24 追加：调用点被 IAT 解析钉死（并纠正一处我自己的算错）

### 工具修正

之前对 RVA `0x5F3D60` 反汇编得到空输出，我一度判断"objdump 对高 RVA 静默失败"——
**这个判断是错的**，实际是我把 RVA 当 VA 算错了一位：
`base(0x140000000) + RVA(0x5F3D60) = 0x1405F3D60`，我却传了 `0x145F3D60`（落在镜像外）。

为此新增 `tools/disasm_rva.py`：**只收 RVA**，自己换算 VA 并按节切字节再反汇编，
从接口上杜绝"RVA/VA 混用"。用法：

```
python tools/disasm_rva.py <exe> 0x247AF2D 0x80
```

### 调用点归属（决定性证据）

`0x1E1CFE0` 处 `call 0x145F3FD60`，而 `0x145F3FD60` 是一条**导入 thunk**：

```asm
145f3fd60:  ff 25 a2 3e 85 00   jmp *0x853ea2(%rip)   # IAT 0x146793C08
```

解析 PE 导入表后，IAT `0x6793C08`（RVA）对应：

```
d3d12_xs.dll!D3D12ConfigureVirtualMemoryX
```

于是第 11 节的因果检验链条完全闭合：

```
0x1E1CFE0  call [IAT -> D3D12ConfigureVirtualMemoryX]   <- 我们的导出
0x1E1CFE5  test eax,eax
0x1E1CFE7  jns  ok
0x1E1CFE9  int3                                           <- 返回负数即断言
```

**结论**：游戏对这个导出的返回值有硬要求（必须非负），且随后要用它配置出来的
GPU 地址空间。我们要做的是"真的把 config 登记进去"，而不是返回失败的实验。

### 下一轮

用 `tools/disasm_rva.py` 反汇编 `0x1E1CF60` 所在函数（已确认只引用两个导入），
从它构造 config 的指令里读出**真实结构布局**（哪些偏移是 VA、哪些是计数/指针），
再据此实现登记。

## 14. 2026-11-24 追加：config 是 **32 字节 / 4 指针**的栈结构，且被平台门闩门控

用 `tools/disasm_rva.py` 反汇编 `0x1E1CF60` 的序章：

```asm
141e1cf70  cmpb $0x0,[0x1471bf9f8]        ; 静态初始化闸（只跑一次）
141e1cf77  jne  <跳过>
141e1cf7d  movb $0x1,[0x1471bf9f8]
141e1cf84  call 0x1455deb88
141e1cf89  cmp  $0x8,%eax
141e1cf8c  ja   <另一分支>
141e1cf92  mov  $0x1c2,%ecx
141e1cf97  bt   %eax,%ecx                 ; 位集 0x1C2 = bit1|bit6|bit7|bit8
141e1cf9a  jae  <另一分支>
141e1cfa0  vxorps %xmm0,%xmm0,%xmm0
141e1cfa4  lea  0x30(%rsp),%rdi            ; ★ config 在这里
141e1cfa9  vmovups %ymm0,(%rdi)            ; ★ 清零 32 字节 = 4 个 qword
141e1cfad  vzeroupper
141e1cfb0  call 0x140075140                ; 平台门闩（与 0x247AF 那处同一个）
141e1cfb5  xor  $0x1,%al
141e1cfb7  movzbl %al,%esi
```

### 两个结论

1. **config 只有 32 字节（4 个指针）**，不是我们之前按 8 个 qword 扫的那样。
   之前 dump 到的第 5–8 个 qword 是相邻栈内容（邻近局部变量），被我们误当成 config 字段。
   所以 GPU VA 登记表里那两项要重新归因：它们来自前 4 个指针指向的结构，仍然有效，
   但"config 有 8 个字段"这个描述是错的。
2. **这段代码被门闩门控**：`0x1455deb88` 返回的 `eax` 必须 ∈ {1,6,7,8}
   （位集 `0x1C2`），并且 `0x140075140` 的布尔值决定后续取值。
   `eax` 是**机型/平台族**的判断结果 —— 也就是说"瞬时块大小 64/82 MiB"是**按机型**选的，
   而那条 2 MiB 的守卫在两种机型下都不成立，进一步佐证"我们不该走到这条路径"。

### 下一轮（路线 A 的收口动作）

1. 反汇编 `0x1E1CF60` 的**主体**（`0x1E1CFB8` 到 `0x1E1D120`），看那 4 个指针各自
   被写入什么结构（栈上还是堆上）、以及调用我们导出时传的是 `rdi`（=rsp+0x30）
2. 根据 4 个指针的真实类型实现登记 —— 这比之前"扫 8 个 qword 猜"要确定得多

## 15. 2026-11-24 追加：**ABI 错了** —— config 在 rcx，我们一直读 rdx（垃圾）

反汇编调用点（`0x1E1CFDA..0x1E1CFE0`）：

```asm
141e1cfda  mov  %rdi,%rcx            ; ★ rcx = config（= rsp+0x30）
141e1cfdd  mov  %eax,0x4(%rdi)       ; config+4 = u32（高 32 位）
141e1cfe0  call 0x145f3fd60          ; [IAT] d3d12_xs!D3D12ConfigureVirtualMemoryX
                                     ; rdx 在这段里没有被设置 -> 残留值
```

而我们的签名是 `D3D12ConfigureVirtualMemoryX(void* device, void* config)`，
即把 config 读在**第二个**参数（rdx）。**读的是垃圾。**

修正后真机日志：

```
ABI：rcx=0xdd153bfce0（真 config）  rdx=0x236423aec90（残留，忽略）
config 前 64 字节: 400000000 0 0 0 0 <栈指针> 0 <栈指针>
```

结合第 14 节的"序章只清零 32 字节"，可以确定：

- **config = 32 字节结构**：`{ u64 total = 0x400000000 (= 16 GiB), u64, u64, u64 }`
- 第 5 个 qword 起是相邻栈内容（不是字段）
- 之前那份"8 个指针 + GPU VA 表"的解读**全部作废**（那是 rdx 指向的垃圾）

### 结论

这个调用传的是"**要多少虚拟内存（16 GiB 量级）**"，不是地址区间表。
所以：

1. 我们的 GPU VA 登记表（上一轮实现）**方向错了**，应当按"总量/页数"来理解
2. 真正要做的多半是：按这个总量在 xmem 侧预留对应规模的虚拟地址空间，
   并让后续 `XMemVirtualAlloc` 落在这段空间里（主机语义），而不是登记外部 VA
3. 待确认：`config+4` 那个被 `mov %eax,0x4(%rdi)` 写入的 u32 语义
   （`eax` 来自 `0x144da2350`，输入是按 2 MiB 向上取整的页数）

### 下一轮

1. 从 `0x1E1CFC2..0x1E1CFCF` 的反汇编还原 `eax` 的算法（页数 -> 某个计数），
   确定 `config+4` 的含义
2. 按"总量"语义重写 GPU VA 登记表 -> 改成"虚拟内存配额"
3. 复跑，观察 `g_1475E6FF0` 是否开始被写入（判据不变）

## 16. 2026-11-24 追加：config 已正确解析（16 GiB），按主机语义预留后**崩溃点不变**

ABI 修正后的真机日志：

```
主机虚拟内存配置: total=0x400000000 (16.00 GiB) [lo32=0x0 hi32=0x4]
                  f08=0x0 f10=0x0 f18=0x0
```

即 `D3D12ConfigureVirtualMemoryX` 传的就是"**给我 16 GiB 的虚拟内存**"，没有别的字段。

按主机语义实现（只 `MEM_RESERVE`，不占用提交额度）：

```
按主机语义预留 16.00 GiB 虚拟地址 -> 0x28f22010000（成功）
FATAL 异常 0x80000003 at game+0x247AF2D        <- 仍然同一处
（跳过守卫后）FATAL 0xC0000005 at game+0x5C8FA96 <- 也是同一处
```

### 结论

- **虚拟内存配置与瞬时块机制无关**（至少不是同一因果链）
- 这条假设也排除了：把 16 GiB 预留出来并不改变游戏走向
- 已排除假设累计 10 条（见第 10/15 节表格与本轮追加）

### 现在的判断

瞬时块机制（`g_1475E6FF0`）的播种条件，很可能在**我们完全没接触过的子系统**里
（例如某个 feature 枚举、或游戏自己在 GPU 侧的 tiling/streaming 决策），
继续在 `d3d12_xs`/`xmem` 这两个模块里加探针的边际收益已经很低。

建议后续把这条线的排查成本重新评估，或者转为"绕开该子系统"的路线
（例如让游戏走另一条渲染初始化分支），而不是继续穷举假设。

## 17. 2026-11-24 追加（收口）：`0x1E1CF60` 读完了 —— 它是"16 GiB VA 双预留"，已成功

反汇编主体（`0x1E1CFF0..0x1E1D04B`）：

```asm
141e1cff0  xor  %ecx,%ecx
141e1cff2  mov  %rsi,%rdx            ; rdx = 0x400000000 (16 GiB，由机型门闩算出)
141e1cff5  mov  $0x20402000,%r8d     ; type = MEM_RESERVE | 64K 页
141e1cffb  mov  %rsi,[0x1471bf980]   ; 全局：记下这个尺寸
141e1d002  movl $0x4,0x20(%rsp)      ; 第 5 参 = 4 (PAGE_READWRITE)
141e1d00a  call 0x145f3fcc0          ; ★ XMemVirtualAlloc -> [0x1471bf970]
141e1d016  test %rax,%rax / jne / 141e1d01b: int3     ; 失败即断言
141e1d023  mov  $0x3,%r9d            ; 第二次：属性 3
141e1d031  movl $0x801,0x20(%rsp)    ; protect = 0x801
141e1d039  call 0x145f3fcc0          ; ★ XMemVirtualAlloc -> [0x1471bf978]
141e1d045  test %rax,%rax / jne / 141e1d04a: int3     ; 失败即断言
```

结论：

1. 这个函数做的是 **16 GiB 的两块 VA 预留**（CPU 视图 + 另一视图），
   分别存进 `game+0x71BF970` / `game+0x71BF978`，尺寸存 `game+0x71BF980`。
2. 两次都**必须成功**（失败立刻 int3）。我们的 `xmem` 两次都返回了非空
   （日志里看到 `XMemVirtualAlloc ... 24.00 GB ... -> 0x...` 成功），所以这两个
   断言在我们环境里是**通过**的。
3. 因此这条函数不是崩溃来源；**`0x247AF2D` 那堵守卫与它无直接关系**（假设 9 也由此再次印证）。

### 收口判断

- 这条线（"通过补 xmem/d3d12_xs 语义让瞬时块机制启动"）已穷举到边际：
  11 条假设排除 + 关键函数读完，仍没有让游戏越过 `0x247AF2D`。
- 继续投入的性价比已很低。若要再推，应换角度：
  **a)** 找 `g_1475E6FF0` 的写入者 `0x1456A2ACA` 由哪个上游调用触发（需要写断点/单步级工具）；
  **b)** 绕开该子系统（让游戏走另一条渲染初始化分支）；
  **c)** 接受"到不了第一帧"，把已完成的 ③ + 窗口路径 + 三套工程模式作为交付。
- 这三条都需要先做**成本/收益判断**，不是继续加探针。

## 18. ★ 2026-11-24 突破：正确的跳过方式被找到了，游戏一路走到 PSO 创建

### 关键纠正：**不要修改 rsi**

epilogue 的真实约定（反汇编 `0x247AF2E` 起）：

```asm
14247af2e  lea  rdx,[game+0x56A2A60]   ; pop 的键1 = scope id
14247af35  mov  rcx,rsi                ; pop 的键2 = rsi
14247af38  call 0x145c8fa70            ; 注销
```

而 `rsi` 在现场是 `0x1190` —— 用游戏自己的 TLS 路径
（`gs:0x58` → 索引自 `game+0x6E08B2C` → `blk+0x410`）取回来的**也是 `0x1190`**，
说明 `rsi` 本来就是对的，是**我们以为它被冲掉、擅自改成 `rcx`，把链表查找键改坏了**。

改成"不改 rsi"后：

| 指标 | 修之前 | 修之后 |
|---|---|---|
| 崩溃点 | `0x5C8FA96`（链表遍历 rax=0） | 不再出现 |
| 日志行数 | 2492 | **2517** |
| 退出码 | `0xC0000005` | `0xE06D7363`（C++ 异常） |
| PSO 槽调用 | 0 次 | **31 次** |

### 游戏已经走到 PSO 创建（slot 60/62）

真机日志（首次调用）：

```
主机 PSO 槽 slot=62（computePSO）第 1 次调用 调用方=+0xC4E4
    a=0x96b5bff148 b=0 c=0 d=0x7ff90568aad8 e=0x2707499d2e0 f=0x2 g=0x27046c18600 h=0x7ff9056896b0
```

参数含义已解出（用桩自己的 dump 交叉验证）：

| 观察 | 结论 |
|---|---|
| `a+0x00 = 0x27046c18600` 且 `g = 0x27046c18600` | `a` 是 **D3D12_COMPUTE_PIPELINE_STATE_DESC\***，`+0x00` 是 `pRootSignature` |
| `a+0x08 = 0x7ff9056896b0` 且 `h = 0x7ff9056896b0` | `+0x08` 是 `CS.pShaderBytecode` |
| `h+0x00 = 44 58 42 43 ...` | 着色器字节码以 **`DXBC`** 魔数开头（DXBC 容器 + 校验和） |
| `d` 前 16 字节是合法 GUID 形态 | `d` 很可能是某个 IID |

### 顺带推翻了一个此前的结论

`docs/d3d12xs-evidence.md` / 早前的分析认为"主机发的是 RDNA2 原生 ISA，必须反编译"。
**本次实测到的 PSO 输入是 `DXBC` 容器**（至少 compute PSO 这条路径是）。
这意味着着色器转换层的必要性需要**重新评估**——DXBC 是桌面 D3D12 能直接接受的格式。

### 下一步（明确）

1. 把 PSO 桩的参数从 8 个扩到 12 个，dump 完整签名（找出 out 指针在哪个位置）
2. 实现 slot 62：拿 `a` 当 desc、按桌面 `CreateComputePipelineState` 创建，
   把结果写回 out；slot 60 同理用 `CreateGraphicsPipelineState`
3. 跑真机，看是否推进到命令列表录制 / PresentX

## 19. ★★ 2026-11-24 进展：槽 62 真实实现，我们的三层能力在同一跑里被真实调用

### 槽 62 已从桩升级为真实现

用 12 参 dump 拿到完整签名后实现（`create_compute_pso_x`）：

```
槽62 compute PSO #1：desc=0x... shader=0x... len=5136 -> hr=0x00000000 pso=0x2637c6724a0
```

**5136 字节的 DXBC compute shader 被桌面驱动直接接受，PSO 创建成功。**

### 关键发现：返回约定影响推进距离

| `XBGTASIX_PSO62_RET` | 结果 |
|---|---|
| `ptr`（RAX 返回对象，默认） | 日志 2486 行，随后抛 C++ 异常 `0xE06D7363` |
| `hr`（返回 S_OK） | **日志 2509 行**，崩溃后移到 `XboxBvhBuilder.dll+0xC4F3` |

即：这条 ABI **不是**"RAX 返回对象"，而是标准 HRESULT + 其它方式取对象。
（与槽 25 的 sret 反向 ABI 不同，不能一概而论。）

### 同一跑里，我们此前实现的三层能力全部真机生效

```
D3D12SerializeVersionedRootSignature v=1.1: 原始 flags=0x03000000
    剥主机位=0x03000000 -> 剥后=0x00000000 写回=成功(+64) hr=0x00000000
消费端根签名剥离：blob=0x... size=140 主机位=0x3000000 位置=+64
槽62 compute PSO #1: ... hr=0x00000000
```

即：**主机位剥离 -> 桌面序列化 -> 写回 -> 消费端再剥 -> PSO 创建成功**，
这条链第一次在真机上完整跑通（此前只有单元测试覆盖）。

### 当前新阻塞：`XboxBvhBuilder.dll+0xC4F3`（访问冲突）

这是**标题自带**的 DLL（census 判定为 `Title`，7 个符号）。它是主机原生二进制，
在我们环境里执行时崩在内部。下一步要查：
1. 它调用了哪些导入（可能是我们只做了桩的 xgameruntime 导出）
2. 崩点的寄存器/内存形态（`XBGTASIX_VEH=1` 已有现场）

### 状态

全套：52 PASS / 1 SKIP / 0 FAIL / 1 已知失败（t47 的桩契约断言待随槽 62 升级而更新）。

## 20. ★★★ 2026-11-24 重大进展：PSO 输出槽找到了，12 个 PSO 建成，崩溃离开我们的层

### 输出槽 = 第 5 个参数 `e`（经验证）

16 参 dump 里没有任何写栈指针，但 `e == p`、`o == e+0x18`，形态像"上下文/输出槽"。
用 `XBGTASIX_PSO62_OUT='*e'` 把 PSO 写进 `*e`：

```
槽62 写回 *e：0x0000000000000000 -> 0x2140a684030     <- 原值恒为 0，确是输出槽
```

效果：

| 配置 | 日志行数 | 结局 |
|---|---|---|
| 不写 `*e` | 2509 | `C0000005 at XboxBvhBuilder.dll+0xC4F3`（标题 DLL 空指针） |
| 写 `*e`（Intel） | 2627 | `C0000005 at igc-default64.dll+0x190B106`（Intel 着色器编译器） |
| 写 `*e`（RTX） | 2598 | C++ 异常 `0xE06D7363` |

即：**我们层里的空指针崩溃被彻底解决**，游戏开始让**驱动的着色器编译器**干活。

> 注意：默认开启写回时出现过一次 `0xC0000409`（fail-fast），怀疑 `e` 并非每次都是
> 输出槽、无条件写会破坏调用方内存。因此该行为**保持 opt-in**，不拿默认安全冒险。

### 12 个 compute PSO 全部创建成功

```
#1  shader len=5136   hr=0    #7  len=4072   hr=0
#2          len=7876   hr=0    #8  len=5256   hr=0
#3          len=5504   hr=0    #9  len=4716   hr=0
#4          len=18748  hr=0    #10 len=4100   hr=0
#5          len=29996  hr=0    #11 len=4328   hr=0
#6          len=4376   hr=0    #12 len=13216  hr=0
```

**桌面驱动接受了全部 12 个 DXBC compute shader**（含 29,996 字节的大 shader）。

### 再次确认：着色器是 DXBC，不是 RDNA2 ISA

`h+0x00 = 44 58 42 43`（`DXBC` + 校验和），长度 5136/29996 等都是正常 DXBC 尺寸。
此前"必须反编译 RDNA2 ISA"的结论**至少对这条路径不成立**，着色器转换层的必要性
需要按实际遇到的 shader 类型重新评估（mesh/NGG 是下一个要验的）。

### 当前两条不同的阻塞

- **Intel**：`igc-default64.dll` 崩（Intel 图形编译器）。可能是该驱动对某些 DXBC 的 bug。
- **NVIDIA**：12 个 PSO 之后抛 C++ 异常（游戏自己的错误处理），需要定位是哪一步失败。

### 下一步

1. 用 NVIDIA 跑，抓 C++ 异常的抛出点（需要装 C++ 异常钩子或看最后一次成功调用之后的行为）
2. 对 Intel 的 IGC 崩溃，记录是哪个 shader（长度/校验和）触发，必要时按长度跳过以绕开驱动 bug
3. 检查接下来的槽：slot 60（graphics PSO）至今 0 次调用 —— 要等 compute 这一批走完

## 21. 2026-11-24 追加：C++ 异常被纳入观测，并做了"驱动异常收敛"

### 观测到抛出点

把 `0xE06D7363`（MSVC C++ 异常）纳入 VEH 过滤后，NVIDIA 上的抛出栈：

```
#3  KERNELBASE+0xC56CA      <- RaiseException
#4  ucrtbase+0xAE836
#5  D3D12Core+0x11612       <- 桌面 D3D12 运行时
#6  D3D12Core+0x81FAC
#7  D3D12Core+0x5EFA7
#8  d3d12_xs.dll+0x5D020    <- 我们的层
#9  game+0x1E86853          <- 标题调用点（槽 133 那一族）
```

即：**桌面 D3D12 在少数路径上抛 C++ 异常而不是返回 HRESULT**，而异常穿透到游戏
会让它直接走错误路径。

### 已做：在调用驱动的边界上收敛异常

新增 `catch_deep()`，把 `std::bad_alloc` -> `E_OUTOFMEMORY`、其它 -> `E_FAIL`，
并接到槽 133（CreatePlacedResource / CreateCommittedResource）、
槽 127（CreateCommittedResource2）、槽 62（CreateComputePipelineState）。

### 实测（NVIDIA）

| 配置 | 行数 | PSO 数 | 退出码 |
|---|---|---|---|
| 写 `*e` | 2622 | 12 | `0xC0000409`（fail-fast） |
| 不写 `*e` | 2535 | 1 | `0xE06D7363`（异常） |

- 写 `*e` 能稳定推进到 12 个 PSO（复现），但最终 `0xC0000409`（fail-fast）
  **进一步坐实了"`e` 并非每次都是输出槽"的怀疑** —— 无条件写会破坏内存。
- 不写 `*e` 时第 1 个 PSO 之后游戏就走了错误路径。

### 结论与下一步

`*e` 只是**经验上有效**，还不是确定了签名的实现。要真正确认，需要：
1. 反汇编标题侧调用点 `game+0x1E8684C` 一族，看它把哪个寄存器/栈槽当作 `ppPSO`
2. 或用一次性写断点：只在"该指针当前为空且随后被游戏读取"时才写

在拿到确定签名前，这一行为保持 opt-in（`XBGTASIX_PSO62_OUT='*e'`），不默认开启。

全套：51 PASS / 2 SKIP / 0 FAIL / 1 已知失败。

## 22. ★ 2026-11-24：槽 62 签名**已确证**（不是经验猜测了）

反汇编 `XboxBvhBuilder.dll+0xC480..+0xC4E6`（就是调用我们槽 62 的那段）：

```asm
mov  (%rcx),%rax          ; rax = 设备 vtable
mov  %r9,%rdi             ; ★ rdi 保存输出指针（后面 *(rdi) 就是 PSO）
mov  0xc0(%rsp),%rbx
...
mov  %rdx,0x38(%rsp)      ; 局部：desc
xor  %r8d,%r8d            ; arg3 = 0
mov  0x1f0(%rax),%r10     ; r10 = vtable[0x1F0/8 = 62]
lea  0x38(%rsp),%rdx      ; arg2 = &desc
mov  %r9,0x28(%rsp)       ; ★ arg6 = 输出指针
lea  <IID>,%rax
mov  %rax,0x20(%rsp)      ; arg5 = IID
xor  %r9d,%r9d            ; arg4 = 0
call *%r10
test %eax,%eax ; js <失败分支>
mov  (%rdi),%rcx          ; ★ 读回 PSO：若为 0 就空指针崩
mov  (%rcx),%rax
call *0x30(%rax)          ; vtable 槽 6
```

对照我们的 16 参 dump：`a=desc b=0 c=0 d=IID e=输出指针`。

**结论：`e` 就是 `ppPSO`，上一轮的"经验发现"被静态确证了。**

### 仍存在的现象

- 写回后：12（实际 48）次 PSO 创建全部成功，日志 2626 行
- 但最终退出码仍是 `0xC0000409`（fail-fast / 栈检查失败），且**该码没有被 VEH 作为异常捕获**
  （已把 0xC0000409 / 0xC0000374 加进过滤，仍无记录 → 说明它来自别的路径或第 9 个之后的异常）
- 槽 62 的 IID 实测为 `{765A30F3-F624-4C6F-A828-ACE948622445}`，加了 IID 守卫后仍写回，
  说明不是"IID 不匹配却写了"

### 下一步（不必再猜的地方）

1. 把 VEH 的 `count >= 8` 上限提高（或只对 fail-fast 不限次），确认 0xC0000409 是否被看到
2. 若是我们的跳过断言（`int3` resync）破坏了调用方的栈/不变量，应改走"patch 该断言"路线：
   把 `int3` 换成 `jmp +1`（`EB 01`），让检查本身不触发，而不是在异常里改 Rip
3. 若 fail-fast 来自游戏自身的安全检查，需要记录它的触发地址再决定

## 23. ★★★ 2026-11-24：找到 PSO 之后的真实缺口 —— 返回的对象必须是**代理**，不能是裸桌面对象

### 证据

槽 62 只有**一个调用点**（`XboxBvhBuilder.dll+0xC4E4`，模块内相对地址），
调用后它立刻：

```asm
call *%r10            ; 我们的槽 62
test %eax,%eax ; js <失败分支>
mov  (%rdi),%rcx      ; rcx = 我们写进 *e 的对象
mov  %rbx,%rdx
mov  (%rcx),%rax      ; rax = 该对象的 vtable
call *0x30(%rax)      ; ★ 调 vtable 槽 6
```

随后在 `XboxBvhBuilder+0xC525` 抛 C++ 异常（实测：第 1 个 PSO 之后立刻发生）。

**关键**：`*e` 里我们放的是**桌面 `ID3D12PipelineState`**。它的 vtable 槽 6 是桌面接口的
方法（`GetPrivateData` 之类），而调用方按**主机接口**理解槽 6 —— 于是行为完全错位，
随后抛异常。

### 结论

和 device / queue / command list 一样，**PSO 也需要一层代理**：
- 对外：主机 vtable（槽号与主机语义一致）
- 对内：持有桌面 `ID3D12PipelineState`
- `QueryInterface/AddRef/Release` 要维持正确的引用计数

我们目前直接返回裸桌面对象，这是设计缺口，不是参数错误。

### 已验证/已否定的东西（本轮到目前）

| 项 | 结论 |
|---|---|
| 槽 62 调用点数量 | 1 个（`+0xC4E4`，XboxBvhBuilder） |
| IID | `{765A30F3-F624-4C6F-A828-ACE948622445}`（与桌面 ID3D12PipelineState 同名 IID 一致） |
| 只允许"当前为空"才写 | ✗ 会退步（下次复用输出槽拿到旧对象）→ 已改为允许覆盖 |
| 返回裸桌面对象 | ✗ 调用方按主机 vtable 用 → 抛异常（本轮定位） |

### 下一步

实现 `PsoProxy`：桌面 PSO 包一层主机 vtable 代理（参照现有 `CommandListProxy` 的形式），
槽 62 返回代理而不是裸对象；必要时给槽 6 等热点方法做显式映射。

## 24. ★★★ 2026-11-24：连破三层，进度 2622 -> 2844 行

### 第一层：槽 62 必须返回 **HRESULT**，不是指针（我上一轮的"代理"结论是错的）

反汇编 XboxBvhBuilder 调用点之后：

```asm
14000c4e6  js 0x14000c510        ; ★ 返回负 -> 失败分支
14000c4ed  mov (%rdi),%rcx       ; 否则读 *e 里的对象
14000c4f6  call *0x30(%rax)      ; 槽 6 = SetName(obj, rbx) —— 语义正确
...
14000c510  lea rdx,<err> ; call <throw helper> ; int3   ; 失败分支直接 throw
```

我们默认返回的是 **PSO 指针**，其低 32 位常 ≥ 0x80000000，被当成负数 ->
必走 throw 分支。**对象应该通过 `*e` 交付，返回值必须是 HRESULT。**

改完后：**53 次 PSO 创建成功**，日志到 2792 行。

> 同时纠正上一节：之前判断"需要 PSO 代理层"是**错的** —— 槽 6 就是 SetName，
> 语义本来就对，问题纯粹在返回值约定。

### 第二层：断言是成对的，patch 要一起打

内层 `0x247AF2D`（2 MiB 跨度）patch 成 nop 后，外层还有个：

```asm
1419f4f4f  call 0x142478440      ; 内层函数，返回 bool
1419f4f54  test %al,%al ; jne ok
1419f4f58  int3                  ; 内层返回 false 就断言
```

于是 `patch_game_assert()` 改成支持**逗号分隔的多个地址**：
`XBGTASIX_PATCH_ASSERT=247AF2D,19F4F58`。两层都 patch 后：2822 行。

### 第三层：设备槽 74 必须返回非负

```asm
141e6c2bb  call *0x250(%rax)     ; 0x250/8 = 槽 74
141e6c2c1  test %eax,%eax
141e6c2c3  jns ok
141e6c2c5  int3                  ; 负值即断言
```

槽 74 原来落在通用桩返回 `E_NOTIMPL`（负数）-> 断言。实现为"记录参数 + 返回 S_OK"后：**2844 行**。

### 当前前沿

最新的未建模槽是 **86**（参数 dump 已能看到），之后仍以 C++ 异常收场。
下一步按同样方法：看调用点是否也是"失败即断言"，再决定是实现还是返回成功。

### 累计进度（同一台 Intel 机器、同一套 patch）

| 里程碑 | 行数 |
|---|---|
| 最初卡在 0x247AF2D | 2492 |
| 找到正确的跳过方式（不动 rsi） | 2517 |
| 槽 62 真实创建 PSO | 2627 |
| + 默认返回 HRESULT（53 个 PSO） | 2792 |
| + 外层断言 patch | 2822 |
| + 槽 74 返回 S_OK | **2844** |

## 25. 2026-11-24：VEH 上限提高后进度到 3358 行；新前沿是"游戏决定自杀"的陷阱

### 一个被我低估的观测限制

VEH 里原本 `if (count++ >= 8) return`，只记前 8 个异常。真实运行里 first-chance
事件很多（C++ throw、可恢复 AV），额度很早就用完，**真正的最后一堵墙从来没被记录过**。
把上限提到 96 后，同一配置的进度从 2844 行跳到 **3358 行**（并看到了新的前沿）。

### 新前沿：`game+0x2474606`

对齐后反汇编（`0x24745C2` 起）：

```asm
call *0x190(%rax)          ; 设备槽 50
mov  rcx,[0x1475df048] / mov edx,3 / call *0x420(%rax)   ; 另一对象槽 132
call 0x145f3fda0           ; IAT thunk
mov  rcx,rsi
call *[0x1467939c8]        ; ★ IAT -> api-ms-win-core-errorhandling!SetUnhandledExceptionFilter
int3                       ; ★ 崩在这里
```

解析 IAT 得到：**`SetUnhandledExceptionFilter` 之后紧跟 `int3`** ——
这是"游戏主动决定终止"的陷阱路径（先装过滤器，再断点），**不是普通不变量断言**。

### 说明

- 之前把这种 `int3` 一律当"可 patch 的断言"是过于简化；这一处意味着游戏已经认定
  某个致命错误（很可能来自前面某个调用的返回值/对象不满足要求）。
- 前面的槽 50（`0x190`）和槽 132（`0x420`）是嫌疑点：槽 50 目前是通用桩，
  槽 132 也是未建模。

### 下一步

1. 查槽 132 的调用对象（全局 `0x1475df048` 是什么对象：队列？device？）
2. 槽 50 按参考实现的 `DEV_STORE3`（只存 3 个参数、无害）核对语义
3. 若两者都只是"记录型"，则真正的致命原因在更早处 —— 需要顺着
   `0x2474606` 的跳转来源（谁跳到这个陷阱）反查

## 26. 2026-11-24：致命路径的调用链挖到了

### 陷阱点的对象身份（VEH 定点采集）

```
陷阱采集：全局[game+0x75DF048]=0x21DDFE00990  对象 vtable=0x7FF9055BFD00
vtable[132] = 0x7FF90559CAD0 -> d3d12_xs.dll+0x4CAD0   （我们的通用桩）
设备全局[game+0x8759D00] 的 vtable=0x21DDFE00990        （与上面是同一个代理对象）
```

即：`0x1475df048` 与设备全局指向**同一个设备代理**，槽 132 打到我们的通用桩。

### 但槽 132 不是触发点

把槽 132 实现为返回 `S_OK` 后：**同样 3376 行、同一陷阱**。已撤回（保留函数与注释）。

### 致命上报器的真相

`game+0x2474500..0x247461E` 是"致命错误上报器"：
引用 `XMemGetWorkingSetStatistics` / `XMemAlloc` / `XMemFree` / `SetUnhandledExceptionFilter`，
最后 `int3`。它有 **31 个调用者**，还有个薄包装 `0x4E9B660(status)`。

崩溃栈：

```
#3 game+0x2474606   <- int3
#4 game+0x4E9B68D   <- 包装函数
#5 game+0x2476F9F   <- 真正调用点
#6 game+0x1D65B83
#7 game+0x247C828
```

`#5` 处的代码：

```asm
142476f91  call 0x140fa09f0        ; ★ 某个函数 -> eax（状态码）
142476f96  mov  %eax,%ecx          ; 把状态码当参数
142476f9a  call 0x144e9b660        ; 致命上报器（内部按状态码决定是否自杀）
142476f9f  test %ebx,%ebx
142476fa1  js   0x14247706f        ; 负值再走错误分支
```

所以：**`0x140FA09F0` 返回了负 HRESULT，这个值被送进上报器 -> 触发 int3**。
它才是要修的东西，而不是上报器或槽 132。

### 下一步

1. 在 shim 侧**记录所有返回负 HRESULT 的槽**（带槽号与调用方 RVA），
   与"最后一次负返回"对齐 —— 直接指出是哪个槽导致 `0x140FA09F0` 失败
2. 再反汇编 `0x140FA09F0` 看它内部调的是哪个设备槽

## 27. ★★★ 2026-11-24：致命原因 = **DXGI_ERROR_DEVICE_REMOVED**（GPU 设备被移除）

给"槽 16 转发"加了返回值日志后，直接看到了失败点：

```
槽16 转发(vt[16]) a1=0x1 a2=<blob> a3=0xa4 调用方=+0xC38B  -> hr=0x00000000
槽16 转发(vt[16]) a1=0x1 a2=<blob> a3=0x74 调用方=+0xFA0B3B -> hr=0x887a0005   ★
```

`0x887a0005` = **`DXGI_ERROR_DEVICE_REMOVED`**。

同时确证了**槽 16 = `CreateRootSignature`**（参数 `(nodeMask=1, pBlob, blobSize, IID, out)`，
与桌面签名逐项对应），我们绑的"消费端剥离"在参数映射上是正确的。

### 因果链

```
Intel 的着色器编译器 igc-default64.dll 崩（前面几轮反复看到）
    -> 驱动把设备标记为 REMOVED
    -> 之后所有 D3D12 调用（含 CreateRootSignature）返回 0x887a0005
    -> 游戏把负 HRESULT 交给致命上报器 -> SetUnhandledExceptionFilter -> int3
```

**所以 0x2474606 的 int3 是"设备已经没了"的下游后果，不是独立缺陷。**
之前一直把它当"可以 patch 的断言"是错的方向 —— patch 它只会掩盖问题。

### 两台机器的现状

| 适配器 | 进度 | 结局 |
|---|---|---|
| Intel（igc 崩） | 3376 行 | DEVICE_REMOVED -> 自杀陷阱 |
| RTX 5060 | 2719 行 | `0xC0000409` fail-fast / C++ 异常 |

### 这说明什么

到这一步，**我们的层已经把该给的都给了**：53 个 PSO 创建成功、根签名剥离、
描述符堆、资源、UAV 全部 OK。剩下的阻塞来自**驱动/GPU 侧**：
- Intel：编译器对这批 DXBC 崩（驱动 bug）
- NVIDIA：更早触发 fail-fast（原因待查）

### 下一步（方向性）

1. **确认 RTX 上的 fail-fast 点**（0xC0000409 不走 SEH，需要别的观测手段），
   如果它是游戏侧安全检查，那 NVIDIA 这条线可能更接近成功
2. **记录触发 Intel igc 崩溃的具体 shader**（长度/校验和），按特征跳过该 PSO
   以绕开驱动 bug —— 代价是画面缺一块，但能让流程继续
3. 重新评估**着色器转换层**的必要性：DXBC 能被接受，但驱动在编译/执行时崩，
   说明"格式对"不等于"语义对"（主机 ABI 相关的 root flags 我们剥了，
   但 shader 里可能依赖主机特有的布局）

## 28. ★★★ 2026-11-24：**着色器是 DXIL SM 6.0**（目标④ 的关键结论）

### 做法

给槽 62 加了字节码落盘（`XBGTASIX_DUMP_SHADERS=<目录>`），把游戏送进
`CreateComputePipelineState` 的 DXBC 存下来离线解析。

### 结果（前两个 shader）

```
cs_01_len5136.bin  chunk 数=9
  [0] 'SFI0' size=8     [1] 'ISG1' size=8      [2] 'OSG1' size=8
  [3] 'PSV0' size=104   [4] 'RTS0' size=96     [5] 'STAT' size=1976
  [6] 'ILDN' size=128   [7] 'HASH' size=20     [8] 'DXIL' size=2648  version=0x00050065
```

`version=0x00050065` = token 5（DXIL）| major 6 | minor 0 → **Shader Model 6.0 DXIL**。

### 结论（回答了目标④）

- **主机发的不是 RDNA2 原生 ISA，是标准的 DXIL SM 6.0 容器**。
  这推翻了我早前根据 `docs/shader-corpus.md` 得出的"必须反编译重建"的判断
  —— 至少在 compute PSO 这条路径上不成立。
- 但"格式对"不等于"能编"：实测 **Intel（igc-default64）与 NVIDIA 两个驱动
  都在编译这批 DXIL 时崩**，Intel 侧直接导致 `DEVICE_REMOVED`。
- 因此着色器转换层的必要性要重新表述为：**不是"格式转换"，而是"主机 ABI 对齐"**
  —— shader 的 `RTS0` 块声明的是主机 root signature（含 memory-based ABI 等位），
  与桌面驱动期望的 ABI 不一致。

### 已验证/已撤回的实验

| 实验 | 结果 |
|---|---|
| 落盘 + 解析 DXBC 分块 | ✅ 确认 DXIL SM6.0 |
| 把 shader 的 `RTS0` 也剥掉主机位 | ✗ 进度退步（3376 -> 2860），已改为 opt-in（`XBGTASIX_STRIP_SHADER_RTS0=1`） |

### 当前基线

Intel + `XBGTASIX_PATCH_ASSERT=247AF2D,19F4F58`：**3400 行**，止于 `DEVICE_REMOVED`
引发的自杀陷阱（`game+0x2474606`）。

### 下一步

1. 对比"游戏原始 RTS0"与"桌面期望的 RTS0"差在哪（能否只对齐 ABI 位而不破坏程序块一致性）
2. 或者：让驱动不崩 —— 例如先用同一 PSO 描述在 WARP（软件驱动）上试编译，
   若 WARP 能编过，说明 DXIL 本身合法，崩溃是硬件驱动的 bug

## 29. ★★★★ 2026-11-24：**WARP 编译成功 48/53** —— DXIL 合法，问题在硬件驱动

### 做法

1. 给槽 62 加 shader 落盘、给槽 16 加根签名落盘（`XBGTASIX_DUMP_SHADERS`）
2. 新测试 `tests/t49_pso_warp`：用 WARP 设备，把 dump 到的 53 个 shader
   与 17 个游戏真实根签名**逐个组合**试建 compute PSO

### 结果

```
[t49] dump 里的根签名可用 17 个
[t49] 汇总：编译成功 48 / 被拒 5 / 共 53
[ok]  至少有一个 shader 能被 WARP 编译（说明 DXIL 本身合法）
5 checks, 0 failures -> PASS
```

（被拒的 5 个返回 `0x887A0005` = DEVICE_REMOVED，是 WARP 设备在试错过程中被移除，
后续组合全部连带失败；前 48 个成功已足够定论。）

### 这条结论为什么重要

- **我们送进 `CreateComputePipelineState` 的字节码是合法、可编译的 DXIL SM6.0**：
  软件驱动能编 48/53
- 因此 Intel（igc-default64）与 NVIDIA 的崩溃**是硬件驱动面对这批 shader 的失败**，
  不是我们的格式/参数错误
- 也再次确认：**不需要"把主机 ISA 反编译成 HLSL"那一层**（目标④ 的原始设想作废）

### 对"出画面"的含义

剩下的选择变成两类（都需要权衡）：

| 方向 | 说明 |
|---|---|
| **绕开硬件驱动** | 用 WARP（软件渲染）跑游戏：能过编译，但性能极低、可能缺少某些特性；不过"能启动/出第一帧"的目标是可达的 |
| **逐个规避崩溃的 shader** | 记录哪几个 shader 触发驱动崩溃并按特征跳过（缺一块画面，但流程能继续） |

### 工具与测试

| 项 | 状态 |
|---|---|
| `XBGTASIX_DUMP_SHADERS=<dir>` | 落盘 `cs_*.bin`（shader）与 `rs_*.bin`（根签名） |
| `tests/t49_pso_warp` | 无 dump 时 SKIP；有 dump 时给出"能否被软件驱动编译"的判据 |
| 全套 | 51 PASS / 3 SKIP / 0 FAIL / 1 已知失败（共 55） |
