# XMem 语义证据表

`xmem.h` 不在公开 GDK 里（[官方参考页](https://learn.microsoft.com/en-us/gaming/gdk/docs/reference/system/xmem/functions/xmemalloc?view=gdk-2610)
明确要求 Xbox 开发者协议）。我把 `GDK_2610.0.9661.zip` 里五个 MSI 的 File 表全查过，
`xmem` 零命中 —— 所以下面每一条都得标来源和可信度。

等级：**[事实]** 有公开原文支撑 · **[推断]** 由原文合理推导 · **[自定]** 本项目选定，只由测试锁住

| # | 断言 | 依据 | 等级 |
|---|---|---|---|
| 1 | XMem 系列以 **64 KB 粒度**分配/映射物理页 | devdocs《内存》概览：“以 64 KB 粒度进行物理页分配和映射” | 事实 |
| 2 | GDK 用 `MEM_64K_PAGES` / `MEM_2MB_PAGES` 表示页大小，2 MB 是更大的页 | devdocs《XBOX Series X\|S 图形内存概述》：“原生 64 KB 页大小，以及更大的 2 MB 页大小” | 事实 |
| 3 | `XMemVirtualAlloc` 是新 API，替代 Xbox One ERA 的扩展 `VirtualAlloc` | 同上 | 事实 |
| 4 | XMem 属于“Win32 Memory APIs 的 Xbox 扩展”，文档锁在 NDA | [GDK 介绍页](https://learn.microsoft.com/en-us/gaming/gdk/docs/gdk-dev/intro/introduction?view=gdk-2610) 编程模型表 | 事实 |
| 5 | `XMemAlloc`/`XMemFree` 的**默认实现源码随 GDK 安装** | devdocs《内存》概览 | 事实（但公开 GDK 包内没有，说明只随 NDA 版分发） |
| 6 | 主机把系统进程内存隔离到固定分区，**不计入游戏预算**，所以游戏可用内存可预测 | GDK 介绍页“memory model on Xbox” | 事实 |
| 7 | 标题内存预算由 `MicrosoftGame.Config` 指定（`XboxLockhartTitleMemory` / `XboxAnacondaTitleMemory` 等） | 你包里的 `MicrosoftGame.Config`：`<XboxLockhartTitleMemory>Advanced</…>` | 观察 |
| 8 | 存在 `XMemSize`（查询分配大小） | 第三方排障文章提及“XMemAlloc、XMemFree 和 XMemSize” | 推断（弱，未加进导出） |
| 9 | 签名顺序是 `XMemAlloc(attributes, size)` | 无公开原文 | **推断** |
| 10 | `attributes` 低 32 位若是 ≥64 KB 的 2 的幂，视为请求对齐 | 无公开原文；由 #1/#2 的“页大小是属性”推导 | **推断** |
| 11 | `XMemVirtualQueryInfo` / `XMemWorkingSetStatistics` 的字段布局 | 无公开原文；按 Windows `MEMORY_BASIC_INFORMATION` 同构设计 | **自定** |
| 12 | `size == 0` 返回 `nullptr` | 无公开原文 | **自定** |
| 13 | 二次释放/野指针 → 忽略 + 记日志 | 主机上是 undefined；移植期崩在这里没有诊断价值 | **自定** |

## #9 和 #10 是最大的风险

签名顺序错了，参数会被当成另一个值读 —— **不会崩，只会算出离谱的结果**。
`attributes` 的编码错了，对齐就会退化，表现为偶然的 GPU 崩溃或性能塌陷。

这两条只能靠两种方式消掉：

1. 拿到 NDA 版 GDK，把 `xmem.h` 原文抄进 `src/xmem/xmem.cpp`（改动很小，只有 `alignment_from` 和四个函数签名）
2. 等真程序跑起来，从调用现场反推（tracer 已经会把每次调用记进 `xbgtasix.log`）

## 一条实测发现（值得记下来）

最初 `XMemVirtualQuery` 对 1 字节分配报 `RegionSize = 4096`，不是 64 KB。
原因不是对齐错，而是：`VirtualAlloc(MEM_COMMIT|MEM_RESERVE, 65561)` 只把前 17 页提交了，
指针落在其中一段，`VirtualQuery` 自然只看到剩下那一页。

修法：把整段**向上取整到 64 KB 并全部提交**（`committed = ceil(total/64K)*64K`）。
现在报 `RegionSize = 65536`，与 #1 的口径一致。代价是 1 字节分配也占 64 KB ——
这恰好也是主机上的真实代价，所以真游戏不会拿 `XMemAlloc` 做小对象分配。

## 拿到 `xmem.h` 后要改哪

- `XMemAlloc` / `XMemFree` / `XMemVirtualQuery` / `XMemGetWorkingSetStatistics` 的签名
- `XMemVirtualQueryInfo` / `XMemWorkingSetStatistics` 的字段与顺序
- `alignment_from()`：换成官方的 `XMemAllocAttributes` 位定义
- 补上 `XMemSize` 等缺失导出（用 `tools/gen_stubs.py` 从 NDA 版的 `.lib` 生成，再逐个实现）

改完后跑 `t3_xmem`：**测试会立刻告诉你哪些断言被破坏**。如果官方语义和我们的不同，
就改测试而不是改实现——测试是规格，实现是猜测。
