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
   我们还没实现这个 —— 这是真实的适配缺口。

## 现在的实现状态

| 能力 | 状态 |
|---|---|
| CPU 侧内存配额（XMem） | ✅ 已实现（64 KB 页语义） |
| GPU 显存预算查询 | ✅ 本测试（DXGI QueryVideoMemoryInfo） |
| 预算变化通知 + 降级 | ❌ 未实现 |
| Reserved 资源 + residency | ❌ 未实现（需要 `ID3D12DeviceX` 语义，NDA） |
