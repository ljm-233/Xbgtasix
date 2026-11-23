# 显存预算：账面额度 vs 真能拿到

工具：`tests/t14_vram`（DXGI `QueryVideoMemoryInfo` + 实测分配）

## 本机实测（RTX 5060 Laptop，7.7 GB）

```
架构: UMA=0（独显，非统一内存）
本地显存: budget=7.0 GB  usage=0.0 GB  可预留=3.6 GB
非本地  : budget=8.1 GB  usage=0.0 GB          <- 溢出到系统内存的额度
实测可分配: 6144 MB（6.0 GB，测试上限）  = 账面的 86.2%
```

三个数各有含义：

| 量 | 意义 |
|---|---|
| `budget` 7.0 GB | 驱动/OS 认为这个进程"应该"用的显存上限（WDDM 会随系统负载浮动） |
| `AvailableForReservation` 3.6 GB | 用于 **reserved 资源**（`CreateReservedResource`）的额度，不是自由显存 |
| 实测 6.0 GB | 真按 committed 资源分配能拿到的量；到 6 GB 仍成功，说明还能再多 |

## 对移植的意义

GTA6 的资源是 **224.96 GB**，而这块卡只有 **7 GB 预算** —— 比例约 1:32。

这意味着：

1. **必须有流式加载**（residency）。整包一次性驻留是不可能的。
2. **`XMemAlloc` 的 GPU 侧语义**（主机上显存与内存统一管理）在 PC 上要拆成两套：
   CPU 用 `VirtualAlloc`（我们已实现），GPU 用 `CreateReservedResource` + `MakeResident`。
3. **budget 会浮动**：WDDM 会按系统负载调低预算，游戏必须监听
   `IDXGIAdapter3::RegisterVideoMemoryBudgetChangeNotificationEvent` 并做降级。
   已实现并验证（见下文 t15_budget）。

## 预算变化通知 + 降级（t15_budget 实测）

```
起始: budget=6.96 GB usage=0.01 GB
注册成功，开始猛分配（上限 8192 MB）
… 分配过程中持续收到预算变化事件 …
降级前: budget=6.96 GB usage=8.01 GB
降级: 释放 10 块（每块 256 MB），usage 8.01 GB -> 5.51 GB，已回到预算内
done notifications=8 -> PASS
```

三个可观察的事实：

1. **通知能收到**：实测 8~40 次 budget-change 事件（每次运行不同），测试只断言 `> 0`
2. **可以超预算分配**：usage 到 8.01 GB 仍成功，说明 WDDM 允许"借"显存，直到驱动开始拒绝
3. **降级有效**：按"释放到 budget 的 80%"策略释放 10 块后，usage 回到 5.51 GB

### mingw 的接口声明与运行时不符（又一个坑）

`IDXGIAdapter3` 在 mingw 头里是 `Register(HANDLE, DWORD*)`（出参），
但 MS 的真实语义是 `Register(HANDLE, DWORD)`（入参）。

x64 上两者落在同一个寄存器，所以做法是：传一个 `DWORD` 变量的地址，
运行时就把它当 cookie 的 32 位值；Unregister 时传同一指针的低 32 位。

直接按"MS 顺序"手写 vtable 反而会 AV（头文件里方法顺序也不同），不要走那条路。

## 现在的实现状态

| 能力 | 状态 |
|---|---|
| CPU 侧内存配额（XMem） | ✅ 已实现（64 KB 页语义） |
| GPU 显存预算查询 | ✅ 本测试（DXGI QueryVideoMemoryInfo） |
| 预算变化通知 + 降级 | ✅ 已实现并验证（`t15_budget`） |
| Reserved 资源 + residency | ❌ 未实现（需要 `ID3D12DeviceX` 语义，NDA） |
