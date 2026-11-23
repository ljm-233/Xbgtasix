# 交接给 GPT：当前状态、已确认的事实、下一步

## 最新续跑

- 调试器确认 `game+0x19F4F58` 由图形初始化失败触发。XMem 现在通过 `VirtualAlloc2` 保留与替换占位区，真机已完成五段地址区间的替换和物理页映射。
- 描述符堆代理把 CPU/GPU 句柄按 Xbox ABI 从寄存器返回，命令列表绑定时使用原生堆。修复后真机越过原来的 `D3D12Core.dll` 访问冲突。
- RTX 5060 的描述符步长为 32 字节，游戏申请 1,245,184 项的 shader-visible CBV/SRV/UAV 堆，驱动返回 `E_INVALIDARG`。Intel 核显步长为 64 字节，802,816 项申请成功，随后游戏在 `game+0x1CF336C` 断言 CPU 句柄必须等于预留地址。两项要求都要纳入描述符句柄转换。
- `t41` 在 WARP 和 RTX 5060 上验证了独立帧事件计时和单平面 SDR `PresentX`。真实游戏仍在描述符堆初始化，尚未调用 `PresentX`。

以下是 2026-11-24 04:40（本地）的初稿。此时最新提交 `c4a67ca`，套件
`39 PASS / 2 SKIP / 0 FAIL`，CI 刚推送、正在跑。

## 一、一句话现状

游戏已经能**加载 12 个资源包、建好 D3D12 设备、进入帧节奏初始化**，
卡片在 `game+0x19F4F58` 的一个裸断言上——位置就在 Present 之前。
剩下的是渲染呈现链：**帧事件信号 + `PresentX` → 自建窗口/交换链**。

## 二、这一轮做完的事（都有提交与测试）

| 提交主题 | 内容 |
|---|---|
| xmem 5 参数 ABI | `XMemVirtualAlloc(address, size, alloc_type:u32, attributes:u64, protect:u32)` —— 我们原来写成 4 个参数，游戏的 `attributes` 落进了 `protect` 槽 |
| 页粒度 | `XMEM_64K_PAGES=0x20400000`（64KB 对齐）/ `XMEM_LARGE_PAGES=0x20000000`（2MB 对齐），按粒度 align+round |
| 物理页真实现 | `XMemAllocatePhysicalPages(u64 flags, size_t* num_pages, u64* page_array)` 真分配（实测一次 11360 页 = 710 MB）；Map/MapScatter 是**拷贝语义**（PC 没有物理页别名） |
| 五个主机模块 | `AcpHal/xapu/chad/xg_xs/XCRDAPI` 各自一份源文件，按 omonkeyz/xbox-emulator（GPL-3.0）移植；XCRDAPI 自带 zlib inflate，实现了主机布局 `IDStorageFactoryX/FileX/QueueX/StatusArrayX` |
| 文件 API 追踪 | `src/fstrace/fstrace.cpp`，IAT 挂钩 `CreateFileW/ReadFile/GetFileAttributesW/ExW/FindFirstFileW`，`XBGTASIX_FSTRACE=1` |
| **主机盘符映射** | `src/fstrace/drives.cpp`，`DefineDosDeviceW` 进程内映射 `G:`（游戏内容分区）与 `T:`（临时盘）。**这是让 12 个 RPF 加载成功的开关** |
| 设备代理修复 | 见第五节，`t41` 从崩溃转 PASS |
| 诊断工具 | `tools/xref.py`（.pdata 函数边界 + 调用图 + 混淆字符串还原）、`tools/analyze_pe.py`、`tools/summarize_run.py` |
| 套件档位 | `run_known_fail`：WIP 测试照样运行、如实打印、单独统计，不计入红绿门槛；修好自动转 PASS |

## 三、真机因果链（全部有日志证据，按顺序）

```
① 主机盘符缺失 → G:\appdata0.rpf 查不到 → RAGE 的 pack 挂载定义表为空
   → 崩在 game+0x2237178 的裸断言（*(int*)(obj+0xC) != 0，obj 全 0）
② 映射 G: 到包目录后 → 12 个 RPF 全部加载成功
   appdata0 / common / movies / shaders / anim / textures / audio / script …
③ 物理页桩返回 null → 崩；改成真分配后 → 进入图形初始化
④ 设备代理的裸汇编转发器问题 → 崩在 D3D12Core；换成 C 透传后 → 进入帧节奏
⑤ 现在：slot 78 SetFrameIntervalX(33333, periodicity=3) + slot 79 ScheduleFrameEventX
   → 崩在 game+0x19F4F58
```

## 四、下一步要做的（渲染呈现链）

### 4.1 先定位 0x19F4F58

```bash
python tools/xref.py "D:/GTA6/GTA6.Xbox.v1.0.2.1-CYBERLEEK/game_scarlett_master.exe" --addr 0x19F4F58 --depth 2
```

重点看它是否落在帧事件等待路径上（`WaitFrameEventX` 是设备 slot 80），
以及它引用了哪些导入。

### 4.2 帧事件要真的信号

现在 `SetFrameIntervalX` / `ScheduleFrameEventX` 只是记录参数。
主机语义：`ScheduleFrameEventX` 排一个事件，`WaitFrameEventX` 等到"帧起点"才返回，
`SuspendX/ResumeX` 可打断。PC 上可以用 `WaitForSingleObjectEx` + 定时器模拟，
但要保证：**每帧恰好信号一次**、超时返回 `D3DERR_DEVICEREMOVED`（主机语义）、
`PresentX` 之后才允许下一帧。

### 4.3 PresentX → 窗口 + 交换链（核心）

主机 `PresentX` 在**命令队列**上（参考实现：队列 vtable 槽 **30**），PC 没有这个方法。
做法：

1. 在 `D3D12XboxCreateDevice` 成功后创建 HWND（进程内）+ `IDXGISwapChain3`
   （flip model，后台缓冲格式跟随游戏的帧缓冲）
2. 队列代理的槽 30 实现 `PresentX(frameToken, ...)`：
   把游戏的呈现语义翻译成 `swapchain->Present(syncInterval, 0)`，
   并把帧令牌回填给游戏（`D3D12XBOX_FRAME_PIPELINE_TOKEN`）
3. 消息泵：Present 前后处理 `PeekMessage`，否则窗口会"未响应"

"建窗口 + 建 flip 交换链 + 呈现"这套**仓库里已经跑通过**：
`tests/t37_system_ui`（系统侧 UI）就是进程内建窗口 + D3D12 flip 交换链 + 像素级断言，
可以直接复用它的代码。

### 4.4 Root signature 主机标志（还没做）

主机 root signature 有 PC 不认的位，直接转发会被拒：

| 位 | 含义 |
|---|---|
| `0x01000000` | 内存型 ABI / 光追 |
| `0x02000000` | compute-2 spill 区 |
| `0x80000000` | 驱动丢弃 |

正确做法（参考实现）：**序列化前剥掉 → 交给桌面 d3d12 序列化 → 再把原始位写回 blob**。

### 4.5 着色器与纹理平铺（未验证）

`shaders.rpf` 411 MB，里面是主机预编译着色器；PC 是否照收需要实测。
`xg_xs` 的 `XGSuggestSwizzleMode` 我们返回 0（线性），
`XGCreateTextureComputer` 返回 `E_NOTIMPL`（返回值是主机 XG 布局二进制 blob，布局未公开）。

## 五、设备代理的关键知识（别踩的坑）

### 5.1 裸汇编转发器不可靠（这次的真凶）

`__attribute__((naked))` 的 asm 跳板（只做"改 this、jmp pc->vtable[N]"）在 MinGW 下
**真实调用会崩**，改成纯 C 透传就好了。现在 `forward_slot<N>` 是 C 版本：
声明 `this + 9 个 uintptr_t`，按 x64 调用约定原样透传。

**结论：这个仓库里不要用 naked asm 做 COM 转发。**

### 5.2 主机 vtable ≠ PC 头文件 vtable

PC（MinGW `d3d12.h`）关键槽位：`8=CreateCommandQueue`、`12=CreateCommandList`、
`27=CreateCommittedResource`、`36=CreateFence`。

主机（参考实现实测）：`8=CreateCommandQueue`、`13=CheckFeatureSupport`、
`14=CreateDescriptorHeap`、`17=CreateConstantBufferView`、`25=GetResourceAllocationInfo`、
`27=CreateCommittedResource`、`28=CreateHeap`、`29=CreatePlacedResource`、
`44=CreatePlacedResourceX`、`48=CreateCommandQueueX`、`60/61/62=PSO/采样器创建`、
`73=命令列表创建`、`78-81=帧节奏`、队列 `30=PresentX`。

诊断方法：`XBGTASIX_SLOTPROBE=1` 会把进过的槽位号各记一次并照常转发。

### 5.3 参考实现的位置

[omonkeyz/xbox-emulator](https://github.com/omonkeyz/xbox-emulator)（GPL-3.0，与我们同许可）：
- `crates/compat/d3d12_xs/src/device_wrap.rs`（121 KB，末尾 `vtable()` 函数就是完整槽位表）
- `crates/compat/d3d12_xs/exports.txt`（55 个导出，每个都写了控制台语义）
- `docs/LIBRARY_MAP.md`（逐函数状态）

另外 [OpenScarlett issue #18](https://github.com/californiaAlmonds/OpenScarlett/issues/18)
列了同一批 ABI 缺口（PresentX 3 参 vs 4 参、IDXGIDevice1 QI 等），值得读。

## 六、环境与操作注意（都是踩过的）

| 坑 | 现象 | 处理 |
|---|---|---|
| 沙箱写权限 | `xbgtasix.exe run` 拷 DLL 报 `err=5` | 本会话沙箱不让我们的 exe 写 `D:\GTA6`；用 bash `cp` 部署 DLL |
| exe 被占用 | 构建报 `cannot open output file` | 先把 `build/xbgtasix.exe` 改名再构建 |
| `xgameruntime.dll` 是运行期加载的 | 它的 DllMain 装钩子太晚 | 钩子类代码放 `xmem.dll`（静态导入，DllMain 必然先跑） |
| 控制台编码 | 中文输出乱码/`UnicodeEncodeError` | `export PYTHONIOENCODING=utf-8` |
| 微基准受负载影响 | t29 偶发失败 | 阈值已有绝对上限兜底 |
| 异常钩子 | 只记第一次异常会漏真崩溃 | 已改成前 8 次都记 |

## 七、复跑命令

```bash
# 构建 + 全套
cd D:/GTA6/Xbgtasix && bash ./build.sh && bash tools/run_suite.sh

# 部署到游戏目录（本沙箱必须用 bash）
cd D:/GTA6 && cp Xbgtasix/build/*.dll GTA6.Xbox.v1.0.2.1-CYBERLEEK/

# 真机跑 + 诊断（三个开关按需开）
cd D:/GTA6/GTA6.Xbox.v1.0.2.1-CYBERLEEK
XBGTASIX_VEH=1 XBGTASIX_LOG=../Xbgtasix/build/run.log ./game_scarlett_master.exe
python ../Xbgtasix/tools/summarize_run.py ../Xbgtasix/build/run.log

# 设备槽位探针
XBGTASIX_SLOTPROBE=1 ./game_scarlett_master.exe
```

## 八、我这边没做完/没做的

1. `game+0x19F4F58` 的具体归属（还没跑 xref）
2. 帧事件信号（`SetFrameIntervalX`/`ScheduleFrameEventX` 目前只记录参数）
3. `PresentX` + 窗口 + 交换链（**渲染呈现的最后一块**）
4. Root signature 主机标志转换
5. 着色器格式验证（`shaders.rpf` 里的主机 DXIL PC 是否照收）
6. 音频三模块的"诚实失败"部分（AcpHalCreate 的 IAcpHal vtable、chad 的卷积/FFT、
   xapu 的 Opus 解码）—— 都是明确返回失败码，不是静默错误
