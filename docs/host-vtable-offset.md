# 主机设备 vtable 偏移：一个尚未定论的开放问题

写于 2026-11-24。这是本轮分析里最重要、但**证据互相矛盾**的一条，必须写下来避免以后重复踩。

## 主张：主机槽 = 桌面槽 + 4

静态反汇编找到 9 个交叉验证点（每个都由调用点的参数形态与 IID 确认）：

| 主机槽 | 语义 | 桌面槽 |
|---|---|---|
| 8 | `CreateCommandQueue` | 4 |
| 14 | `CreateDescriptorHeap` | 10 |
| 15 | `GetDescriptorHandleIncrementSize` | 11 |
| 19 | `CreateUnorderedAccessView` | 15 |
| 20 | `CreateRenderTargetView` | 16 |
| 21 | `CreateDepthStencilView` | 17 |
| 25 | `GetResourceAllocationInfo` | 21 |
| 26 | `GetCustomHeapProperties` | 22 |
| 36 | `CreateFence` | 32 |

证据样例（`game+0x2478E2F`）：

```asm
mov 0x62e0eca(%rip),%rcx      ; 设备
xor %edx,%edx                 ; HeapType = 0
call *0x78(%rax)              ; 0x78/8 = 15
mov %eax,0x1475e7000          ; ← 返回值存进全局，之后当描述符步进用
```

`game+0x1C7459E` 用同一个全局做 `add %rax,%rdi` 步进 => 槽 15 确实是"取描述符步进"，
而桌面 vtable 里这个函数在 11（不是 15）。

## 矛盾：按 -4 转发会让真机更早崩

把通用转发改成 `pc_vtable[N-4]`、并把槽 15 实现成返回主机 64 B 步进后，真机在
**更早的位置**就崩了：

```
[d3d12_xs] slot 8 -> CreateCommandQueue ... hr=0x887a0005   (DXGI_ERROR_DEVICE_REMOVED)
```

而同号转发（当前状态）能一路推进到 `game+0x247AF2D`。

所以两条证据链**不能同时成立**。可能的解释：

1. 主机 vtable 的形状**不是全局 +4**，而是"部分槽位插入/重排"。9 个点恰好都在 +4 的位置上，
   但别的槽（例如介于 8~14 之间的）可能不是。
2. 我们的代理对外暴露的 vtable 也是自建的：真机行为取决于**我们暴露的槽位**与游戏调用点的
   对应关系，而不是主机二进制里那张表的绝对位置。也就是说"+4"可能是我们*应该*在内部做的
   映射，但只有在**同时**修正其他按名调用（如 `custom_heap_properties`）之后才成立。
3. 槽 15 返回 64 B 步进后，标题的 arena 记账（802816×64）与桌面堆（32 B/项）不一致，
   可能让后续资源创建踩坏显存 —— 这与 `DEVICE_REMOVED` 吻合。

## 当前决定

**保持同号转发**（能跑通到 `0x247AF2D` 的状态），并把这条记成开放问题。
代码里的 `forward_slot<N>` 上方有同样内容的注释，引用本文。

## 怎么定论（下一步实验）

1. 给 `slot_probe_forward<N>` 加上**调用方 RVA**（现在只有槽号，无法把静态调用点与运行时对上）。
2. 对某个具体槽（建议先 9 / 15 / 36，它们各有唯一或极少的静态调用点）做"调用约定断言"：
   在桩里 dump 参数，与静态反汇编里该调用点准备的值逐字段比对 —— 参数对得上，说明映射正确。
3. 单独打开 -4（不改槽 15 的返回值），看是"更早崩"还是"推进"；再把槽 15 的返回值在
   0x20 / 0x40 之间切换，二分出是哪一个改动导致的 `DEVICE_REMOVED`。

## 相关

- 未建模槽位的完整清单与定性：`docs/host-slots-audit.md`（待补）
- 当前崩溃 `game+0x247AF2D` 的分析：它是游戏自身的不变量断言
  `(S.end - S.begin) + g_1475E6FF0 > 0x1FFFFF`，四个操作数全部来自游戏自己的立即数与 BSS，
  **没有任何 shim 返回值参与** —— 见本节"槽 15 把步进写成 0"的间接链路假设。
