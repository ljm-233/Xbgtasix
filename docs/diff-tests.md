# 差分测试（模式 A）：d3d12_xs 的 ABI 翻译

> 落地自 `handoff/victory-reusable-designs.md` 第 1 节（模式 A：同源双编译 + 同机双绘制 + 逐像素比对）。
> 交付物：`tests/t44_diff/main.cpp`（C++20 / MinGW，HLSL 以字符串常量内嵌，无二进制夹具）。
> 本文自包含：说清它证明什么、怎么跑、以及**证明不了什么**。

---

## 1. 它证明什么

同一个 HLSL、同一块 GPU、同样的资源，两条路径各画一帧，读回内存**逐像素按位比对**：

| 路径 | 是谁的实现 |
|---|---|
| **A（被测）** | `d3d12_xs.dll` 的 `D3D12XboxCreateDevice` → **设备代理**，再走它建模过的**主机 ABI 槽位** |
| **B（标准答案）** | `LoadLibrary("C:\Windows\System32\d3d12.dll")` 的 `D3D12CreateDevice` → 桌面 `ID3D12Device`，标准 C++ 接口 |

标准答案来自**桌面厂商实现**（Windows 自带 `d3d12.dll` → `d3d12core.dll` → 驱动），不是我们的第二份代码。
否则"我们的两个 bug 恰好互相抵消"会被判成通过。

它真正覆盖的是翻译层里最容易"能跑但结果错"的部分：

- 主机 `GetResourceAllocationInfo`（槽 25）把输出缓冲放在 RCX、设备 this 放在 RDX，桌面是隐藏返回缓冲；返回值还要**剥掉 PC 不认的主机资源标志位**；
- 主机 `CreateUnorderedAccessView`（槽 19）是**五参**（多一个 `pCounter`，目标句柄落到栈上第 5 个参数），桌面是四参；
- 主机 `CreateCommittedResource3`（槽 127）用 `D3D12_RESOURCE_DESC1` + 主机资源标志（实测 `0x20000`）+ 主机 layout，桌面拿到的必须是剥干净的等价描述；
- 主机 `CreateCommandListX`（槽 73）用 `{type,2,1,0}` 描述符并返回命令列表代理，代理的 `SetDescriptorHeaps` 会把描述符堆代理解包成真堆；
- 其余槽位（8/9/10/16/18/20/27/34…）是"同形转发"，必须保证 **栈上参数不被截断**（`CreateCommittedResource` 有 8 个参数）。

**判据**：两侧读回的 256 个 32 位像素值相等（见第 5 节的容差规则）。

---

## 2. 两条路径具体走什么

路径 A 的调用序列（`XBGTASIX_SLOTPROBE=1` 实测打印出 9/10/15/16/18/20/27/36 这些转发槽位，
8/14/19/25/73 是建模槽位，走的不是探针）：

| 步骤 | 路径 A（shim） | 路径 B（桌面参照） |
|---|---|---|
| 设备 | `D3D12XboxCreateDevice` → 代理（`d3d12_xs` 导出） | `d3d12.dll!D3D12CreateDevice` → 原生 |
| RootSignature 序列化 | `d3d12_xs!D3D12SerializeVersionedRootSignature` | `d3d12.dll!D3D12SerializeVersionedRootSignature` |
| 队列 | 槽 8（主机形参，返回队列代理） | `CreateCommandQueue` |
| 描述符堆 | 槽 14（主机形参，返回堆代理） | `CreateDescriptorHeap` |
| 堆 CPU/GPU 句柄 | 堆代理虚表槽 9/10（**句柄在 RAX 返回**） | `GetCPUDescriptorHandleForHeapStart` 等 |
| UAV 描述符 | 槽 19（**五参**） | `CreateUnorderedAccessView`（四参） |
| 资源 | 槽 27（`CreateCommittedResource`，8 参转发） | `CreateCommittedResource` |
| 分配尺寸 | 槽 25（输出缓冲在 RCX / 设备 this 在 RDX） | `GetResourceAllocationInfo` |
| 命令列表 | 槽 73（命令列表代理） | `CreateCommandList` |
| 追加 ABI 用例 | 槽 127（`CreateCommittedResource3`） | `CreateCommittedResource`（等价描述） |

两侧**每个用例**都用同一份 HLSL、同一套资源参数、同一块适配器（NVIDIA 独显；`XBGTASIX_WARP=1` 时是 WARP）：

- 3 个顶点的顶点缓冲（裁剪空间全屏三角形，坐标由公式给出）；
- 常量缓冲（`gOne = 1.0f`、`gHalf = 0.5f`，运行期值，防止编译器把 `x*1.0` 折成 mov）；
- 256 项的输入表（`StructuredBuffer<uint>`，见第 4 节为什么不放常量缓冲里）；
- **16×16 `R32_UINT` render target**（按位保存 f32 结果，NaN payload / denormal / ±0 都能原样读回）；
- 256 项 `R32_UINT` UAV（同一个像素着色器同时写 RT 与 UAV：UAV 那条路径才真正用上槽 19 建出来的描述符）；
- READBACK 缓冲（RT 用 `CopyTextureRegion`，UAV 用 `CopyBufferRegion`，一个栅栏等完再 `Map`）。

---

## 3. CASES 表

一条用例 = 一个最小像素着色器片段（`kRenderCases`）+ 一个"测什么/为什么期望两侧一致"的断言。
像素 `(x,y)` 取输入表的第 `y*16+x` 项，4 个 `op` 按 `y>>2` 分组，所以每个 op 都能看到全部 16 个
"难搞语义"家族（NaN/±Inf/±0/±denormal/1-1ulp/2^23/2^31/127…）。

| 用例名 | 测什么 | 期望两侧一致的理由 / 不一致说明什么 | 容差 |
|---|---|---|---|
| `nan_propagation` | NaN 经 `add`/`mul`/`min`/`max` 的传播与符号 | 两侧执行同一份 DXIL、同一块 GPU 的同一组指令；shim 只翻译资源与 ABI，不碰着色器。不一致 ⇒ 我们建资源/绑定（UAV 视图、CBV、SRV）与桌面不同，而不是"NaN 语义不同" | ≤1 ULP（NaN payload 可差 1） |
| `denormal_flush` | denormal 输入是否被 flush 成 0（`x*1.0`、`x+x`、`x*0.5`、`x*2.0`） | denormal 处理是驱动/硬件行为，两条路径在同一设备上应完全相同。不一致 ⇒ 格式/描述被改，或两侧根本不是同一块设备 | ≤1 ULP |
| `signed_zero` | `+0` 与 `-0` 的位模式（`1/x` 的符号、`x*1.0`、`x+0.0`、`min(x,+0)`） | IEEE-754 里最容易被优化掉的部分；两侧同 DXIL 必然一致。不一致 ⇒ 资源被翻译成不同格式（例如 `R32_UINT` 变成 float RT）或绑定错位读到了别的像素 | **位必须完全相同** |
| `round_sat` | 浮点→8 位整数的舍入与饱和边界（`round`+clamp、clamp 后截断、unorm8 编码、有符号饱和），含 NaN/Inf/-1 | NaN 走 clamp/round/转整数都是"实现定义"，所以只要求**两侧一致**，不要求等于某个值。不一致 ⇒ RTV 格式被改（例如 `R32_UINT` 被换成 UNORM） | **位必须完全相同** |
| `bitfield_oob` | `(x >> 40) & 0xFFFF`、算术右移 33、`(x << 40)`、范围内 `(x>>8)&0xFF` | HLSL 把"移位量 ≥ 位宽"留成未定义，实际由指令决定；两侧同 DXIL 必须一样。不一致 ⇒ 输入数据被翻译错（stride/偏移不对），而不是"移位语义不同" | **位必须完全相同** |
| `precision` | 大数相加丢精度（`(x+2^23)-2^23`）、`mad`（可融合）vs 分步乘加、`mad(x,3,1e-8)-3x` | 两侧同一 DXIL、同一驱动 JIT；`dxc 1.4.363.1` 把两条乘加都编成 `fmul fast + fadd fast`，最终是否收缩由驱动决定 —— 这条用例记录的就是那个决定。不一致 ⇒ 常量缓冲/SRV 绑定被翻译错 | ≤1 ULP |
| `abi_alloc_info` | 槽 25 返回值：三组描述（干净 buffer / 带 `0x20004` 的 buffer / 带 `0x20001` 的 16×16 R32_UINT） | shim 剥掉 `0x20000` 后问桌面，等于参照侧直接问"剥干净的描述"。不一致 ⇒ 剥位漏了/剥多了，或参数位置拿错（输出缓冲与 this 交换） | 精确相等 |
| `abi_host_flag_strip` | 槽 127 用主机 `desc1 + 0x20004 + layout 3` 建资源 | 建出来的资源 `GetDesc()` 必须与参照侧逐字段相同。建不出来或字段不同 ⇒ 剥位/描述 ABI 坏了 | 精确相等 |

另外还有 6 项"比对器自检"断言（相同位必须相等、1 ULP 容忍、2 ULP 拒绝、±0 拒绝、`+Inf vs NaN` 拒绝、
整数用例不容差），以及 2 项设备断言（`D3D12XboxCreateDevice` 建出代理、
`d3d12_xs!D3D12CreateDevice` 仍返回**原生**桌面设备——用 `QI(ID3D12Device8)` 成功来区分）。

### 本机实测（2026-11-24，RTX 5060 Laptop，驱动见系统信息）

| 观察 | 值 |
|---|---|
| denormal | **被 flush 成 0，符号保留**：`+min_denormal*1.0 → 0x00000000`、`-min_denormal*1.0 → 0x80000000`、`max_denormal → 0`，而 `1-1ulp` 原样保留（不是"把小数都清零"） |
| ±0 | `1/+0 → 0x7F800000`、`1/-0 → 0xFF800000`；`1/+Inf → +0`、`1/-Inf → -0` |
| 移位越界 | `(x >> 40)` 实际等价于 `(x >> 8)`（移位量按 5 位掩码，`40 & 31 = 8`） |
| 槽 25 剥位 | 带 `0x20004` 的原始描述直接给桌面得到 `SizeInBytes = 0xFFFFFFFFFFFFFFFF`（描述被拒）；剥位后是 `65536 / Alignment 0x10000`，与参照侧一致 ⇒ **剥位这步是必需的，不是装饰** |
| 槽 127 | `flags 0x20004 → 0x4`，资源建成，`GetDesc()` 与参照侧逐字段相同 |

---

## 4. 夹具：全部由代码生成

- 输入表 `input_word(i)`：16 个"难搞语义"家族的位模式（`0x7FC00000` / `0xFFC00001` / `0x7F800000` /
  `0xFF800000` / `0x00000000` / `0x80000000` / `0x00000001` / `0x80000001` / `0x007FFFFF` /
  `0x3F7FFFFF` / `0x4B000001` / `0x4F000000` / `0x3FC00000` / `0xBF800000` / `0x42FE0000` / `0x42FF0000`），
  奇数行再乘 `1 + (y&7)*0.25`，制造"同一家族不同量级"。
- 顶点：`{(-1,-1), (3,-1), (-1,3)}`（全屏三角形）。
- 常量：`gOne = 1.0f`、`gHalf = 0.5f`（运行期写入，避免被折叠）。
- **仓库里没有任何二进制夹具**：HLSL 是字符串常量，`.cso` 是运行时 `dxc` 编译到临时目录的产物。

> **踩过的坑（必须写在文档里，否则下一个人会重犯）**
> 输入表一开始放在常量缓冲里（`uint gIn[260]`）。结果读到的输入全错位：**HLSL 常量缓冲的数组元素
> 各自独占 16 字节槽位**（`cbufferLoadLegacy` 的寄存器索引 = 元素下标），可变下标 `gIn[i]` 实际读到的是
> 第 `4i` 个 dword。这个 bug 在"两侧一致"的判定下**不会报错**（两边都错），只会让用例变得毫无意义。
> 现在输入表走 `StructuredBuffer<uint>`（stride 就是 `StructureByteStride = 4`），并且用一次性探针
> 断言过 `gIn[y*16+x] == input_word(y*16+x)`（0 处不符）。常量缓冲仍保留，用来放运行期常量。

---

## 5. 判定：位模式、ULP、±0、浮点类别

`bits_close(got, expect, allow_ulp)`：

1. 位相同 → 通过。
2. `allow_ulp == false`（整数/位模式用例）→ 必须完全相同。
3. 出现 `±0` → 拒绝（`±0` 的符号有语义：`1/x` 的符号、`min/max` 的取舍靠它区分）。
4. 浮点**类别**不同（有限数 / ±Inf / NaN）→ 拒绝。理由：`+Inf` 加 1 ULP 就变成 NaN，
   拿 ULP 距离去"容忍" `Inf vs NaN` 会把最严重的偏差当噪声放过去。
5. 否则按 `f32` 的 ULP 距离 ≤ 1 通过（跨符号用有序 key 计算）。

不一致时打印**用例名 + 前 4 个不同像素**，例如：

```
[t44]   signed_zero/rt pixel (5,0) got=0xbf800000 expect=0xff800000  (-1 vs -inf)
[t44] FAIL render target 逐像素比对不一致
[t44]   signed_zero: rt 1/256 像素不同（容差 位必须相同）
```

（上面这段是故意注入 `^0x40000000` 造差异时实测到的输出；注入 `+1 ULP` 时，`nan_propagation`
仍 PASS、`signed_zero` 报 FAIL —— 容差策略确实在起作用。）

---

## 6. 怎么跑

### 编译（不跑 `build.sh`，用已有的 `build/d3d12_xs.dll` + `build/libd3d12_xs.dll.a`）

```sh
/d/mingw64/bin/g++ -std=c++20 -O2 -D_WIN32_WINNT=0x0A00 -DNOMINMAX -DWIN32_LEAN_AND_MEAN \
  -static-libgcc -static-libstdc++ -Iinclude -I"D:/GTA6/gdk/include_clean" \
  tests/t44_diff/main.cpp -Lbuild -ld3d12_xs -ldxguid -ldxgi -ld3d12 -o build/t44_diff.exe
```

- 加 `-Wall -Wextra` 也是零警告（写这套测试时按这个标准自查过）。
- **`t44_diff.exe` 必须和 `d3d12_xs.dll` 在同一个目录**（`build/`），否则 `STATUS_DLL_NOT_FOUND`（0xC0000135）。
  临时目录按 `%TEMP%` → exe 所在目录 → 当前目录 的顺序试，第一个能建目录的胜出（本机 `%TEMP%` 在沙箱里
  被拒，实际落在 `build/xbgtasix_t44_<pid>/`，跑完自动清理）。

### 环境变量

| 变量 | 作用 |
|---|---|
| `XBGTASIX_DXC=<path>` | `dxc.exe` 路径（支持 MSYS 风格 `/c/...`，测试内部会转成 `C:\...`）。缺省扫 `C:\VulkanSDK\*\Bin\dxc.exe`（取版本号最大），再查 `PATH` |
| `XBGTASIX_DIFF_ONLY=<case>` | 只跑一条（名字见第 3 节表格）。名字不存在 → 打印可用清单并 **FAIL**（rc=1） |
| `XBGTASIX_EMIT_DUMP=<dir>` | 把两侧 DXIL（`<case>.{shim,desktop}.{vs,ps}.cso`）与读回的像素（`<case>.<side>.<rt|uav>.txt`，`x y 0xXXXXXXXX` 文本）写进该目录 |
| `XBGTASIX_WARP=1` | 用 WARP 设备（CI 无 GPU） |

### 退出码与输出约定

- `0` = PASS，`2` = SKIP，其它 = FAIL。
- **SKIP 的两个条件**：① 找不到 `dxc`；② 建不出桌面 D3D12 设备 / 没有适配器 / 没有可写临时目录。
  都会先打印一行说明并 `return 2`。
- **例外（有意为之）**：桌面设备能建、但 **`D3D12XboxCreateDevice` 建不出来**算 **FAIL**（rc=1），
  不算 SKIP —— 那正是我们要抓的 shim 回归，而不是"这台机器没这个能力"。
- 最后两行固定为：

  ```
  [t44] 50 checks, 0 failures
  [t44] done failures=0 -> PASS
  ```

  （`failures=0 -> PASS` / `checks, 0 failures` 都能被 `tools/run_suite.sh` 的既有 grep 认出来。）

### 本机实测

| 场景 | 结果 |
|---|---|
| 默认（NVIDIA GeForce RTX 5060 Laptop GPU） | `50 checks, 0 failures` · `done failures=0 -> PASS` · rc=0 |
| `XBGTASIX_WARP=1` | `50 checks, 0 failures` · rc=0 |
| `XBGTASIX_DIFF_ONLY=bitfield_oob` | `14 checks, 0 failures` · rc=0 |
| `XBGTASIX_DIFF_ONLY=abi_host_flag_strip` + `XBGTASIX_DXC=/c/VulkanSDK/1.4.363.1/Bin/dxc.exe` | `11 checks, 0 failures` · rc=0（顺便验证 MSYS 路径转换） |
| `XBGTASIX_DIFF_ONLY=nope` | FAIL，列出可用用例名 · rc=1 |
| 模拟"机器上没有 dxc" | `[t44] SKIP: 找不到 dxc…` + `done failures=0 -> SKIP` · rc=2 |

计时：整轮约 1 秒（6 次 dxc×2 次编译 + 12 帧 16×16）。WARP 也一样，因为画面只有 16×16。

---

## 7. 它**证明不了**什么（诚实边界）

1. **证明不了"主机编译器 vs PC 编译器"的等价性。** 这台机器上没有 Xbox 着色器编译器（GDK 的 `dxc`/`fxc`
   变体不在本机），所以两侧用的都是同一个 `dxc 1.4.363.1`。用例里"两侧 DXIL 逐字节相同"这条断言，
   证明的是"两次编译拿到的是同一份源"，**不是**"两家编译器对同一份源给出一致语义"。
   真正的"同源双编译"要在有主机工具链的机器上换掉路径 A 的编译器，本测试的结构已经留好了位置
   （`compile_shader()` 是唯一入口）。
2. **证明不了着色器算术语义是"我们翻译对的"。** 本项目的 shim 不重写 DXIL、不碰着色器执行；
   两侧跑的是同一段 DXIL、同一个驱动。所以 `nan_propagation`/`denormal_flush`/… 这些用例抓到的是
   **"资源/描述符/绑定被翻译坏了，导致 shader 看到的东西不同"**，而不是"我们把 NaN 规则实现错了"。
   它们同时是一份**本机数值语义基线**：将来若真的加 DXIL 重写、格式转换、精度降级，
   这些基线就是"改之前长这样"的对照物。
3. **只覆盖建模过的槽位，且只覆盖一种管线形状。** 图形管线固定为"全屏三角形 + `R32_UINT` RT + 一个
   structured UAV"；没有采样器/纹理/`TYPELESS` 格式、没有 MSAA、没有 bundle、没有 compute、
   没有 `Present`/swapchain/residency/`MakeResident`、没有描述符 arena 翻译（进程里没加载 `xmem.dll`，
   所以 `virtual_cpu_base` 恒为 0；arena 句柄换算这条路**没被测到**）。
4. **证明不了失败路径的语义。** 只有 `abi_host_flag_strip` 里隐含了一次"剥位后能建"的检查；
   "主机传了 PC 不支持的东西时该返回什么 HRESULT"（槽 8/48/73 的 `E_INVALIDARG`/`E_NOINTERFACE` 等）
   由 `t41` 覆盖，不在这套差分测试的范围里。
5. **单机结论不能外推。** WARP 与 NVIDIA 都通过，只能说明这两个适配器上一致；AMD/Intel 的
   denormal、FMA 收缩、移位越界行为都可能不同。用例的设计是"记录实际行为并要求两侧一致"，
   所以换硬件不会假红，但**也不会告诉你哪个才是主机行为**。
6. **槽 127 的"初始状态"没被验证。** shim 建那个资源时总是先试 `UNORDERED_ACCESS` 再退 `COMMON`，
   测试拿不到真实初始状态（D3D12 没有查询接口），所以 `abi_host_flag_strip` 只比 `GetDesc()` 与能否建成，
   没有把该资源拿去做渲染。渲染用例的 RT/缓冲走的是槽 27（同形转发），初始状态两侧都显式指定。
7. **`bitfield_oob`/`precision` 的"未定义行为"只是记录，不是规范。** 换驱动可能变，用例会跟着变，
   这是设计意图（`记录实际行为，两侧必须一致`），但不要把表里的数值当成"HLSL 保证"。
8. **比对器的容差本身是判断。** ≤1 ULP + 浮点类别必须相同 + ±0 不容差，是一个工程折中；
   它能放过"硬件给 `+0` 而我给 `±8.7e-8`"这类差（交接文档 1.4 明确接受），但也会放过
   "1 ULP 但语义完全不同"的极少数情形（除 ±0/类别外）。

---

## 8. 以后改这套测试要注意

- **不要在常量缓冲里做可变下标取数组**（见第 4 节的坑）。
- **不要用 MinGW 的 C++ 接口去调代理的"返回聚合"方法**：`d3d12_xs` 的堆代理按主机 ABI 把
  `GetCPUDescriptorHandleForHeapStart` 的句柄放在 **RAX**，而 MinGW 的 `d3d12.h` 走
  `WIDL_EXPLICIT_AGGREGATE_RETURNS`（隐藏返回缓冲，sret）。直接 `heap->GetCPUDescriptorHandleForHeapStart()`
  会读到 0。测试里对 shim 侧一律用裸虚表（槽 9/10），桌面侧才用 C++ 接口。
- **不要把堆代理直接交给原生命令列表**：`SetDescriptorHeaps` 需要真堆，测试用 shim 的槽 73 命令列表代理
  （它会 `unwrap_descriptor_heap`）。给原生列表塞代理堆在 `D3D12Core` 里会炸。
- 加用例只需在 `kRenderCases` 里加一条（`body` 里定义 `Calc` + `PSMain`），并在文档第 3 节表格里写清
  "测什么/为什么期望一致/不一致说明什么"。单例跑：`XBGTASIX_DIFF_ONLY=<name>`。
- 套件接线（`build.sh` / `tools/run_suite.sh`）由维护者负责；本文件不改它们。
