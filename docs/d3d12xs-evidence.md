# D3D12.x（Xbox）与 D3D12（桌面）差异表

目标：搞清楚 `d3d12_xs.dll` 到底要提供什么，以及哪些差异靠"转发"就能解决、哪些不能。
等级：**[事实]** 公开原文 · **[观察]** 从包内文件实测 · **[推断]** 合理推导 · **[未知]** 查不到

## 已确认的事实

| # | 事实 | 依据 | 等级 |
|---|---|---|---|
| 1 | Xbox 上 GDK **只支持 D3D12.x**，不支持 D3D11.x；PC 上两者都支持 | [GDK 介绍页](https://learn.microsoft.com/en-us/gaming/gdk/docs/gdk-dev/intro/introduction?view=gdk-2610) | 事实 |
| 2 | D3D12.x 是"D3D12 + 主机扩展"，且 GDK 的 D3D12.x 与 Xbox One ERA 的 D3D12.x **只有很小差异** | 同上 | 事实 |
| 3 | 面向 Xbox 编译时链接的是 `d3d12_x.lib` + `xgameplatform.lib` + `xgameruntime.lib`，并加 `/NODEFAULTLIB` | 同上 | 事实 |
| 4 | D3D12.x 的参考文档在 `xbox-nda-docs` 下 | 介绍页链接指向 `/gaming/xbox-nda-docs/.../d3d12_x/` | 事实 |
| 5 | 图形内存按 **64 KB / 2 MB 页**管理，`XMemVirtualAlloc` 是新的显存分配 API | devdocs《XBOX Series X\|S 图形内存概述》 | 事实 |
| 6 | 包内 16 个 DLL 只从 `d3d12_x.dll` 导入 **2 个**函数：`D3D12SerializeVersionedRootSignature`、`D3D12CreateRootSignatureDeserializer` | [gdk_api_surface.md](gdk_api_surface.md) | 观察 |
| 7 | 系统 `d3d12.dll` 只有 **19 个**导出（真正实现在 `d3d12core.dll`） | 实测 dump | 观察 |

## 差异面清单

| 面 | 差异 | 我们的处理 | 风险 |
|---|---|---|---|
| **导出表** | `d3d12_x.dll` 应是 `d3d12.dll` 的超集（同名 + `X` 变体） | **全量转发**（见下） | 若真有 `X` 独有导出，名字对不上就解析失败 |
| **设备创建** | 主机上 `D3D12CreateDevice` 的 adapter 参数/返回接口可能不同 | 手写转发 + 日志，参数原样透传 | 若 xs 版签名不同，参数会错位 → 静默错乱 |
| **Residency** | 主机内存是固定物理分区，`MakeResident`/`Evict` 大概是记账而非真实换页 | 未处理（转发到桌面实现） | 桌面驱动会按自己的预算驱逐，可能导致纹理丢失/花屏 |
| **Bindless / 描述符上限** | 主机的描述符堆上限未公开 | 未处理 | 超过桌面上限时 `CreateDescriptorHeap` 返回失败 |
| **着色器 blob** | 主机可能发行**非标准 DXIL** 的预编译着色器 | 未处理 | 转发给桌面驱动会直接拒绝 → 这一步是硬墙 |
| **特性查询** | `CheckFeatureSupport` 的返回值在主机上不同 | 未处理 | 游戏可能走进它以为存在、实际没有的路径 |
| **内存分配** | 显存通过 `XMemAlloc` / `XMemVirtualAlloc` 走 64KB/2MB 页 | `xmem.dll` 已实现（见 [xmem-evidence.md](xmem-evidence.md)） | 对齐/页大小口径 |

## 已完成：全量转发

`d3d12_xs.dll` 现在有 **18 个导出**：

- **4 个手写**（带日志）：`D3D12CreateDevice`、`D3D12SerializeVersionedRootSignature`、
  `D3D12CreateRootSignatureDeserializer`、`D3D12GetDebugInterface`
- **14 个转发**：其余全部原样转发到 `d3d12.dll`，例如
  `D3D12CoreCreateLayeredDevice`、`D3D12CreateVersionedRootSignatureDeserializer`、
  `D3D12EnableExperimentalFeatures`、`D3D12GetInterface`

转发由 PE 加载器原生解析（`Forwarder RVA`），零运行时开销、零手写代码。
生成方式：`python tools/forward_shim.py /c/Windows/System32/d3d12.dll --dll-name d3d12_xs --target d3d12 --out src/d3d12_xs/forward.def --exclude <手写的4个>`。

这样任何 GDK 程序对 `d3d12_xs.dll` 的导入都能解析到，不会因为"缺导出"而加载失败 ——
这是"先让它能启动"的必要条件。

## 转发解决不了的：需要拦截方法调用

`D3D12CreateDevice` 返回的是 **COM 接口指针**，之后所有调用都走虚表，转发层看不见。
所以下面这些只能靠**包装设备对象**（自己实现 `ID3D12Device`/`ID3D12DeviceX` 的代理，
把方法转发给真对象并顺手做记账）：

- Residency 记账（`MakeResident` / `Evict` / `SetResidencyPriority`）
- 描述符堆创建时的上限检查（超过桌面限制就提前报错，而不是等崩溃）
- `CheckFeatureSupport` 的取值改写（把主机特性表映射到桌面）

这条路的前提是知道 `ID3D12DeviceX` 的 **IID 和虚表布局** —— 那是 NDA 内容。
没有它，代理只能实现到 `ID3D12Device` 为止，游戏 `QueryInterface` 一个 X 接口就会拿到
`E_NOINTERFACE`。**这就是下一堵墙。**

## 结论

- **能做的已做完**：导出面全覆盖，加载不会因缺符号失败
- **需要观察的**：真程序启动后会依次撞上 residency → 描述符上限 → 着色器格式 → `ID3D12DeviceX`
- **最先要预判的**：如果泄露包里的着色器是主机专有格式而非标准 DXIL，那么即使前面全过，
  `CreateGraphicsPipelineState` 也会失败。

### 已做的一次探测（2026-11-22）

对 `shaders.rpf`（411 MB）做原始字节扫描，找 DXBC/DXIL 容器魔数：

```
DXBC  count=0    DXIL  count=0    SHDR  count=0    RDEF  count=0
```

同时测了四个 RPF9 包的统计：

| 文件 | 熵（bits/byte） | 可打印字符占比 |
|---|---|---|
| common.rpf | 6.42 | 0.255 |
| script.rpf | 7.83 | 0.334 |
| shaders.rpf | 7.59 | 0.327 |
| appdata0.rpf | 6.36 | 0.258 |

参考值：随机数据熵 ≈8.00、可打印 ≈0.37；纯文本熵 ≈4.5、可打印 ≈0.99。

**结论**：找不到任何明文的着色器魔数，也找不到明文的文件名 TOC。这与社区说法
（"缺失的 exe 里带着解密密钥"）一致 —— 数据很可能是加密/压缩存储的。
但这只是**字节级旁证**，不是定论：RPF9 的 TOC 可能在压缩后的块里。

下一步验证方式：用 `rpf9-explorer`（第三方工具，不在本仓库内） 打开 `common.rpf` 试列目录。
列得出来 → 只是压缩；列不出来 → 加密，资产路线也得等密钥。
