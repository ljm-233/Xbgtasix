# Xbgtasix 状态与路线（2026-11-24 收口）

一页纸说明"做到哪、证明过什么、还差什么"。所有结论都指向可复查的日志或测试。

## 1. 已经能跑的部分（有真机证据）

真机 `game_scarlett_master.exe`（Intel 核显，`XBGTASIX_ADAPTER=0`）已经走过的阶段：

| 阶段 | 证据 |
|---|---|
| 加载 + 8 个 shim 全部生效 | `[d3d12_xs] 选卡 -> Intel(R) Graphics` |
| 主机盘符映射（G:/T:） | `[drives] 已映射 G: -> <游戏目录>` |
| 12 个资源包加载 | `appdata0.rpf` 等 |
| 24 GB + 1.14 GB 主机内存预留、物理页映射 | `[xmem] XMemVirtualAlloc ... gran=65536/2097152` |
| D3D12 设备 + DIRECT 队列 | `slot 8 -> CreateCommandQueue ... hr=0` |
| 描述符 arena（标题 0.18 GB 占位区里切 32 MB/64 B 步进） | `描述符 arena 切出 32000000 字节` |
| 4K 资源创建（槽 127 = CreateCommittedResource3） | `槽127 desc1: dim=3 3840x2160 ... hr=0` |
| 资源分配信息（槽 25，反向 ABI：RCX=输出缓冲） | `SizeInBytes=33488896 Alignment=0x10000` |
| UAV 创建 + 句柄翻译（槽 19 五参） | `dest=<arena> -> <真实堆地址>` |
| 帧节奏（槽 78/79） | `SetFrameIntervalX interval=33333` |
| 虚拟内存配置（16 GiB） | `主机虚拟内存配置: total=0x400000000 (16.00 GiB)` |

## 2. 当前唯一的硬阻塞

`game+0x247AF2D` 的断言：

```
if ((end - begin) + g > 0x1FFFFF) int3       // 2 MiB - 1
实测 begin=0   end=0x4000000(64MiB)   g=[game+0x75E6FF0]=0   无回绕
```

三项都是**游戏自己的立即数与 BSS**，没有 shim 返回值参与。

### 已排除的 11 条假设（每条都有实测）

1. 守卫可由 `g` 满足 —— 算术不可能（`g` 是加项）
2. 守卫可由机型门闩切分支满足 —— 64 MiB / 82 MiB 都 ≫ 2 MiB
3. 瞬时块机制经过我们的 shim —— `xmem` 探针 0 命中
4. 崩溃只因 `g == 0` —— 播种 2 MiB 后崩溃点/寄存器形态不变
5. 返回失败能让游戏换路径 —— 立刻在 `0x1E1CFE9` 断言
6. `XMemVirtualQuery` 会查主机 GPU VA —— 探针 0 次
7. 登记 GPU VA 后 `g` 会被写入 —— 登记后仍为 0
8. config 是 8 指针的区间表 —— ABI 修正后证明那 8 个是垃圾
9. 虚拟内存配置与瞬时块相关 —— 按主机语义预留 16 GiB 后崩溃点不变
10. 机型门闩查我们的导出 —— 整个运行只有 1 条未实现导出调用痕迹
11. 跳过守卫可绕过 —— 会打断"登记/注销"配对，必在 `0x5C8FA96` 链表遍历崩

## 3. 已完成的能力（不依赖上面那堵墙）

| 能力 | 状态 | 测试 |
|---|---|---|
| 根签名主机 flags 剥离（序列化端 + 消费端） | ✅ | `t46_rootsig`：31 checks |
| 窗口 + 交换链 + 连续 present | ✅ | `t48_present`：9 checks |
| 描述符句柄翻译（arena ↔ PC 堆） | ✅ | `t41` |
| 差分测试（shim vs 桌面逐像素） | ✅ | `t44_diff`：50 checks（RTX + WARP） |
| 合规护栏（静态白名单 + 落点闸门） | ✅ | `policy.py` 36 条 / `repo_guard.py` 23 条 |
| 导入解析阶梯 + census | ✅ | `t45_census`：53 checks |
| PSO 槽 60/62 取证桩 | ⚠️ 就位，游戏走不到 | `t47_pso`：72 checks |

全套：**53 PASS / 1 SKIP / 0 FAIL**（共 54）。

## 4. 要真正出画面，还差什么

1. **绕开或解决瞬间块机制**（当前硬阻塞，见第 2 节；穷举已到边际）
2. **主机 PSO 创建**（槽 60/62 目前恒 `E_NOTIMPL`）—— 没有 PSO 无法 draw
3. **着色器转换层**（完全不存在）：主机发的是 **RDNA2 原生 ISA**
   （`DXBC 外壳 → XDAT → zlib → FSSM → RDNA2`），必须先反编译成 HLSL 再用 DXC 重编译。
   参考实现为此有 64 KB 转换器 + 383 KB HLSL 生成器，验证 105,506 条。**这是最大的一块。**
4. 根签名剥离的消费端已做，但游戏还没走到（依赖 1–3）

## 5. 建议的下一步（按性价比）

| 优先级 | 动作 | 理由 |
|---|---|---|
| 高 | 用 `tools/disasm_rva.py` 完整还原 `0x1E1CF60` 主体 | 已确认它只引用两个导入，是唯一还没读完的关键函数 |
| 高 | 评估"绕开渲染初始化另一分支"的可行性 | 继续在瞬时块上加探针的边际收益已很低 |
| 中 | 把 `t47_pso` 的桩升级为真实实现（需先有 PSO 参数实测） | 依赖游戏走到那一步 |
| 低 | 着色器转换层立项 | 工程量最大，前置依赖多 |

## 6. 工具

| 工具 | 用途 |
|---|---|
| `tools/disasm_rva.py <exe> <rva> [len]` | 按 RVA 反汇编（只收 RVA，杜绝 RVA/VA 混用） |
| `python tools/xref.py <exe> --addr <rva>` | 函数边界 + 调用图 + 混淆字符串还原 |
| `XBGTASIX_VEH=1` | 崩溃现场：符号化栈 + 寄存器 + 对象内存 |
| `XBGTASIX_SLOTPROBE=1` | 设备/队列/命令列表槽位调用记录 |
| `XBGTASIX_SKIP_GAME_ASSERT=247AF2D` | 跳过那堵断言（会被大声记录，且已知会崩在后面） |
| `XBGTASIX_VMCFG_FAIL=1` / `XBGTASIX_TRANSIENT_BLOCK=1` | 因果检验开关（默认关） |
| `XBGTASIX_PRESENT_SELFTEST` | 见 `t48_present` |
