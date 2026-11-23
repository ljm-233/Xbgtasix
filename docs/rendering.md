# 渲染：主机呈现模型与 PC 侧的移植要点

这份文档记录我们在渲染面上已经确认的事实，以及接下来必须补的三块。

## 1. 主机上没有窗口，也没有交换链

Xbox Series 的呈现模型和 PC 完全不同：

| 项 | 主机（GDK / d3d12_xs） | PC（桌面 D3D12） |
|---|---|---|
| 窗口 | 没有 HWND | 必须有 |
| 交换链 | **没有 `IDXGISwapChain`** | `IDXGISwapChain3/4` |
| 呈现 | **命令队列的方法 `PresentX`** | `IDXGISwapChain::Present` |
| 帧缓冲 | `D3D12ConfigureVirtualMemoryX` 配置出来的显示内存 | 交换链后台缓冲 |
| 帧节奏 | `SetFrameIntervalX` / `ScheduleFrameEventX` / `WaitFrameEventX` | `Present(syncInterval)` + 帧延迟 |

所以**窗口必须由兼容层创建**。游戏代码里没有"创建窗口"这个概念，它只会说
"我要 present 这一帧"。

## 2. 我们的链路现状

```
游戏: D3D12ConfigureVirtualMemoryX()      → 已拦，返回 S_OK（并记录主机内存配置）
游戏: D3D12XboxCreateDevice()             → 已拦，转发到桌面 D3D12CreateDevice
游戏: 设备/队列方法调用                    → **主机槽位在 PC 对象上不存在 → 崩在 D3D12Core**
游戏: queue->PresentX(...)                → 未实现（窗口/交换链还没有接到这里）
```

实测崩溃证据（`XBGTASIX_VEH=1`）：

```
FATAL 异常 0xC0000005 at D3D12Core.dll+0x2A0EE
  #6 game_scarlett_master.exe+0x2478713   ← 调用方是游戏自己
```

说明设备确实建出来了，崩在"用设备"这一步。

## 3. 必须补的三块

### 3.1 设备/队列/命令列表的 vtable 代理（最重要）

主机接口是 `ID3D12DeviceX` / `ID3D12CommandQueueX` / `ID3D12GraphicsCommandListX`，
它们在 PC 的对应接口**之后**追加了主机专属方法：

| 接口 | 主机专属方法 |
|---|---|
| 设备 | `WaitFrameEventX`、`ScheduleFrameEventX`、`SuspendX`、`ResumeX`、内存配置类 |
| 队列 | `PresentX`、`SetFrameIntervalX`（帧节奏） |
| 命令列表 | 主机专有的寄存器写/状态设置槽位（实测游戏会调到 slot 131/132） |

做法：把 PC 对象包一层，主机 vtable 的槽位逐个映射——PC 有对等方法的直接转发，
主机独有的由我们实现（`PresentX` → 我们自建的窗口 + 交换链）。

参考实现（[omonkeyz/xbox-emulator](https://github.com/omonkeyz/xbox-emulator)，GPL-3.0）
的 `d3d12_xs` 有 151 个文件，其中 `device_wrap.rs` 单个 **121 KB** —— 说明这块是
整个渲染兼容层的主体，不是几十行能糊过去的。

### 3.2 Root signature 的主机标志转换

主机 root signature 里有 PC 不认的标志位：

| 位 | 含义 |
|---|---|
| `0x01000000` | 内存型 ABI / 光追 |
| `0x02000000` | compute-2 spill 区 |
| `0x80000000` | 驱动丢弃（driver-discarded） |

直接透传给桌面 `D3D12SerializeVersionedRootSignature` 会被拒。参考实现的做法是：
**序列化前剥掉这些位 → 交给桌面 d3d12 序列化 → 再把原始标志写回 blob**，
这样主机侧的 deserializer 仍然看到原始的 desc。

我们目前的实现是**直接转发**，这一块还没做。

### 3.3 着色器与纹理平铺

- `shaders.rpf` 411 MB，里面是主机预编译着色器。PC 的 D3D12 是否照收需要实测；
  主机 DXIL 若带 RDNA2 专属指令，需要重新编译（参考实现专门有 `dxbc2dxil_xs` 这个 crate）。
- `xg_xs.dll` 负责纹理 swizzle（主机平铺模式）。我们已经导出
  `XGSuggestSwizzleMode`（返回 0 = 线性）与 `XGCreateTextureComputer`（返回 E_NOTIMPL）。
  `XGCreateTextureComputer` 的返回值是主机 XG 资源布局二进制 blob，布局未公开，
  参考实现靠反推 swizzle 方程做的——我们暂时诚实失败。

## 4. 当前状态

| 项 | 状态 |
|---|---|
| `D3D12XboxCreateDevice` 转发 | ✅ 已实现（选卡 + feature level 11_0） |
| `D3D12ConfigureVirtualMemoryX` | ✅ 已拦（返回 S_OK，PC 由驱动管显存） |
| 设备/队列/命令列表 vtable 代理 | 🚧 **进行中**（`src/d3d12_xs/device_abi.h`） |
| Root signature 标志转换 | ❌ 未做 |
| `PresentX` + 窗口/交换链 | ❌ 未做 |
| 着色器格式验证 | ❌ 未做 |
| 纹理 swizzle | ⚠️ `XGSuggestSwizzleMode` 返回线性；`XGCreateTextureComputer` 诚实失败 |

`tests/t41_d3d12_device_abi` 是这套代理的回归测试，**目前崩溃**
（`D3D12Core` 内 AV，代理转发槽位仍有错位），因此在套件里按
"已知失败（KNOW）"统计：照样运行、如实打印，但不计入红绿门槛。
修好后会自动转成 PASS。

## 5. 为什么这条路成立

1. `game_scarlett_master.exe` 是**标准 Windows PE**（PE32+，GUI 子系统，68 个导入模块
   全部可解析）——Windows 加载器能直接加载，不需要模拟 CPU。
2. 设备**确实建出来了**：崩溃发生在设备方法调用，而不是设备创建。
3. "建窗口 + 建 D3D12 交换链 + 呈现"这个能力本仓库**已经验证过**：
   系统侧 UI（`t37_system_ui` / `t38_xboxui`）就是在进程内建 Win32 窗口 + flip 交换链
   并做像素级断言。缺的只是把游戏的 `PresentX` 接过去。

## 6. 复跑

```bash
# 看渲染相关调用与崩溃点
XBGTASIX_VEH=1 XBGTASIX_LOG=build/render.log ./game_scarlett_master.exe
python tools/summarize_run.py build/render.log

# 只跑设备 ABI 测试
build/t41_d3d12_device_abi.exe
```
