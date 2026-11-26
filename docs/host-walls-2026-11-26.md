# 2026-11-26 一整轮推进：从「278 ms 就死」到「12716 行 / 约 13 秒」

本文件记录这一天在 **Xbgtasix + vkd3d-proton** 路线上逐条拆掉的墙与对应证据。
每条都带"改了什么 / 证据 / 剩下的问题"，方便下次接手直接续。

起点：本地 `build/` 里那套 shim 是 11-24 的旧产物，缺四个 `InitializeApiImpl*` 导出，
一跑 278 ms 就死在 `game+0x5A91C21`（文档里第一堵墙）。
用当前 HEAD 交叉编译（`tools/build_linux_cross.sh`）后回到 6542 ms / 2614 行。

---

## 1. `XMemMapPhysicalPagesScatter` 把 PFN 当指针 memcpy（4388 → 10942 行）

`page_array` 里是**物理页号**，旧实现 `memcpy(va, (void*)pfn, 64KB)` 直接读垃圾地址：
实测崩在 `xmem+0x3550` 的 `memcpy`，`addr=0xb21000` 是 `MEM_FREE`。

改法：不读 PFN，目标页按 64 KB 清零，并记日志说明这是**语义缺口**（PC 上没有"按 PFN 取页"）。

## 2. 槽 25 / 108 的分配信息查询（desktop 值泄漏）

`GetResourceAllocationInfo` 的 desc 里有主机专有字段：实测 `layout=261(0x105)`
（桌面枚举只有 0..3），桌面判非法后返回 `SizeInBytes=0xFFFFFFFFFFFFFFFF`，
调用方紧跟 `cmpl $0,0x4(%rsi)` —— 高 32 位非 0 就 int3。

改法：`Layout > 3` 归一成 `UNKNOWN`；mips/depth/samples/w/h 的 0 补成桌面最小值；
仍被拒时打整张 desc 的 8 个 qword，并给 64 KiB/4 KiB 的兜底（明确记成兜底）。

## 3. 槽 41 = `CreateCommandSignature`（IID 确证）

IID `{C36A797C-EC80-4F0A-8985-A7B2475082D1}` 在 `vkd3d_d3d12.idl:3213` 明确是
`ID3D12CommandSignature`。调用点 `game+0x2479554/90/CC`，形状 `(device, desc, 0, riid, ppv)`，
desc 逐字段与桌面 `D3D12_COMMAND_SIGNATURE_DESC` 一致。

主机独有间接参数类型 **105**（`stride=12`）按 `DISPATCH(2)` 映射（12 字节正好是
ThreadGroupCountX/Y/Z），可用 `XBGTASIX_INDIRECT105=8` 做对照。

## 4. AMD 专用指令 `CLZERO`（0F 01 FC）

标题给 Zen 编译，清 cache line 用的是 AMD 独有的 `CLZERO`；本机 Intel 执行即 #UD
（`0xC000001D` @ `game+0x4E8AA70`，循环体只有这一条 3 字节指令）。

改法：VEH 里仿真 —— 清零 `[RAX & ~63]` 的 64 字节、`RIP += 3`。

## 5. 空调用仿真（主机槽 9 落在桌面短 vtable 上）

对象是 `ID3D12CommandAllocator`（用 `d3d12core.dll` 符号表反查确认：
`.data+0x4560 = d3d12_command_allocator_vtbl`，9 项），游戏按主机布局调槽 9 →
vtable 越界读到 0 → RIP=0。

包代理会破坏 vkd3d 的类型假设（分配器交回 `CreateCommandList/Reset` 时死得更早，
实测退到 236 行），所以改成**只在这次调用上当它返回 S_OK**：
VEH 判据四连（AV + 访问类型=执行 + 故障地址=0 + RIP=0）→ 弹栈、RIP 拨回调用之后、RAX=0。

## 6. 短跑（2643 行 / 退出码 3）的两个来源

- **vkd3d 断言**：`meta_commands.c:632` 的 `face->lpVtbl == &d3d12_meta_command_vtbl`
  失败会弹断言框（Wine 的 Assertion failed 对话框），进程随之结束。
  用 `meson configure -Db_ndebug=true` 重编 d3d12core 后消失（12683 → 12708 行）。
- **残留 wine 进程**：连跑时不清理，短跑概率显著升高。每次开跑前
  `pkill -f "[g]ame_scarlett"` 后就稳定回到长跑。

## 7. 命令列表上的描述符句柄翻译（槽 46–50）

桌面 `ClearUnorderedAccessViewUint` 换来的句柄必须翻译：
实测崩在 `d3d12_command_list_clear_uav_builtin+0x611` `movb $0x1,0x127(%rbx)`（rbx=0）。
现在槽 46 `OMSetRenderTargets` / 47 `ClearDSV` / 48 `ClearRTV` / 49 `ClearUAVUint` /
50 `ClearUAVFloat` 都先走 `translate_cpu` / `translate_gpu`。

## 8. 槽 80 `WaitFrameEventX` 必须返回非负

调用点 `game+0x2482448` 是 `call *0x280(%rax)` + `js -> int3`，只查返回值。
带 `flags=0x2` 的形态以前返回 `E_NOTIMPL` 直接被断言。
现在无 block/flags 时走本地帧事件实现，其余形态一律 S_OK（等待语义退化为立即返回）。

## 9. 命令列表槽位偏移的 A/B 结论

`XBGTASIX_LIST_SLOT_DELTA` 单变量对照（同一构建、同一份游戏）：

| delta | 结果 |
|---|---|
| 0（同号） | **12716 行**（最好） |
| -4 | 12687 行 |
| +4 | 12686 行 |

结论：命令列表这条**不需要**"主机槽 = 桌面槽 ± 4"的修正；同号转发是对的。
设备侧那个开放问题（`docs/host-vtable-offset.md`）不受此影响，仍待逐槽确认。

## 10. 当前前沿

```
12716 行 / 约 13 秒 / 退出码 0xC0000005
崩点：d3d12core.dll+0xCC084 = d3d12_command_list_CopyBufferRegion+0x64
现场：rsi=1（被当结构体指针解引用 0x1a4(%rsi)）
```

仍未建模、且调用频次最高的主机槽：**57（40 次）**、70（32 次）、86（6 次）。
其中 57 的调用点 `game+0x247757E` 是 `call *0x1c8(%rax)`，形参含 `.rdata` 里的 GUID 表，
最像"批量创建/查询管线对象"，下一步优先啃它。

---

## 11. 描述符记账：三次单点尝试全部失败（负面结论，别再重复）

槽 15 的调用点已确认：`game+0x2478E3D` 是 `xor %edx,%edx` + `call *0x78(%rax)`，
即 `(device, type=0)`。三种返回值对照（同一构建、同一份游戏）：

| 槽 15 返回 | 结果 |
|---|---|
| 同号转发（桌面 CreateUAV 的 S_OK = 0） | **12711 行**（当前基线，但步进其实是 0） |
| 0x40（标题的 64 B/条目） | 2617 行早退 |
| 驱动真值 0x20 | 2655 行早退 |

把 `translate_cpu` 改成"按索引换算（host 64 B -> 驱动 32 B）"，并同时接上
15/20/21（IncrementSize / CreateRTV / CreateDSV）后：**game+0x1F77023 早退，约 2900 行**。

结论：现在的 1.27 万行是**建立在"步进=0"这个错误前提上**的。描述符这套东西
（步进 / arena 记账 / 句柄翻译 / 视图创建）必须**整体一致**才有意义，单点修改
一定更早崩。下次动它之前，先把下面四件事作为一个整体设计：

1. 标题的 arena 记账：802816 项 × 64 B（已在 `kHostDescriptorStride` 里写成 0x40）
2. 驱动真实步进：NVIDIA 上 32 B
3. `translate_cpu` / `translate_gpu`：按索引换算，不是差值搬运
4. 视图创建（CBV/SRV/UAV/RTV/DSV/Sampler）与 `CopyDescriptors*` 全部走同一套换算

## 12. 主机槽 57 的探针（已实现，默认关）

`XBGTASIX_SLOT57=1` 时：记录参数 + 给一个 16 槽哑对象。实测返回哑对象后
行数从 12716 掉到 12439，且崩溃点不变 ⇒ 该槽拿到的对象会被真正使用，
不能用"给个非空对象"糊过去。默认关闭。

## 13. 崩点真正入口：`game+0x2482689 call *0x118(%rax)`（槽 35，裸对象）

长跑日志（12720 行）里的崩溃链：

```
#2 d3d12core+0x95847
#3 d3d12core+0xA8955
#4 d3d12core+0xCC076        ← 已确证在 CopyBufferRegion 那片
#5 game+0x248268F
#6 game+0x2482498
#7 game+0x2215ECA
```

反汇编 `game+0x2482660`：`142482689 call *0x118(%rax)` —— **槽 35**。
对象 vtable 在 d3d12core 里（裸桌面对象），而桌面这几个接口只有 9 项 vtable，
槽 35 直接越界取到函数指针 → 跳进无关代码 → 崩。

## 14. 槽 36 = CreateFence 已接（本轮）

按 `docs/host-vtable-offset.md` 的交叉验证，主机槽 36 = CreateFence（桌面 32）。
以前是"同号转发"，被当成桌面 `SetStablePowerState` 调用，游戏拿到的是**裸 fence**。
现在：真建 fence + 16 槽主机代理。实测三把 fence 都建出来了（
`槽36 CreateFence initial=0x0 flags=0x1 -> 主机代理 ...`），但崩点没变
（12720 → 12725 行，仍在 `d3d12core+0x95847`）。

⇒ 说明槽 35 那个裸对象**不是 fence**，还得继续找。下一步：把 `log_object_vt`
铺到所有创建型槽（资源/根签名/分配器/PSO/signature/heap/fence 全部），
崩溃时对着 vtable 偏移反查"这是谁给的"。

## 15. ★ 主机（XS）槽位权威表（来自 `scarlett_upload_patch` 的 PORTING.md）

来源：用户提供的 `scarlett_upload_patch-fix1.zip`，其 PORTING.md 是**对该 EXE 静态分析**
得出的槽位表。这张表解释了我们之前所有"同号转发"的错误根源。

```
Device  : 14 CreateDescriptorHeap     15 GetDescriptorHandleIncrementSize
          17 CreateConstantBufferView 27 CreateCommittedResource
          36 CreateFence              38 GetCopyableFootprints
GCL     : 15 CopyBufferRegion         16 CopyTextureRegion
          26 ResourceBarrier          146 ID3D12GraphicsCommandList7::Barrier（增强）
Queue   : 10 ExecuteCommandLists      16 Signal
          35 = 引擎提交入口（带 frameToken + flags）
Resource: 8 Map   9 Unmap   10 GetDesc   11 GetGPUVirtualAddress
Heap    : 9 GetCPUDescriptorHandleForHeapStart
Fence   : 8 GetCompletedValue
```

已按表改正并保留的接线：设备 **17 → CreateConstantBufferView**、命令列表 **146 → 增强 Barrier**。
已试但暂缓：设备 **27/38**（接上后稳定退到 2648 行且 27 一次未被调用 → 说明该阶段走别的路径）。

## 16. 清理残留进程的正确姿势

`pkill -x "game_scarlett_master.exe"` **无效**：pkill 用 comm 名，Linux 上限 15 字符，
长名永远匹配不到。之前所谓"已清理"其实没清。正确写法：

```bash
ps -eo pid=,comm= | awk '$2 ~ /^game_scar/ {print $1}' | xargs -r kill -9
ps -eo pid=,comm= | awk '$2 ~ /^wineserver/ {print $1}' | xargs -r kill -9
```

## 17. 短跑根因：Vulkan 着色器缓存 + 自递归钩子的坑

给 `kernel32!ExitProcess` 挂钩子（打印带模块名的调用栈）之后，短跑真相：

```
exit#0 winevulkan.dll+0x11050
exit#1 d3d12core.dll+0x53918
exit#2 d3d12core.dll+0x6E48
exit#3 d3d12_xs.dll+0x5AA67        ← 槽 62 创建 PSO
exit#4 XboxBvhBuilder.dll+0xC4E4   ← 游戏自带的 PSO 构建 DLL
```

即：**创建 PSO 时 vkd3d 内部的 Vulkan 组件抛致命错误，vkd3d 自己 ExitProcess(3)**。
清掉 GPU 驱动着色器缓存（`~/.cache/mesa_shader_cache*`，以前只清 `~/.cache/vkd3d`）
之后，行数从 2648 恢复到 **10545**。

### 钩子自身的坑（已修）

第一次实现把 `g_exit_orig` 直接指向**被打过补丁的入口**，于是每次 exit 都无限递归：
877 次 ExitProcess、栈里全是 `XCRDAPI.dll+0xB21F`、进程以 0x7C 退出。
正确做法是 trampoline：把原 12 字节拷到可执行内存，尾部加 `mov rax, target+12; jmp rax`。
修好之后 ExitProcess 只出现 4 次，真实调用链露出：

```
d3d12core → winevulkan → XCRDAPI → xgameruntime → d3d12_xs → ExitProcess(3)
```

## 18. 经验：测量前必须做的三件事

1. `ps -eo pid=,comm=` 按 comm 前缀杀残留（`pkill -x` 因 15 字符上限无效）
2. 清 `~/.cache/vkd3d` **和** `~/.cache/mesa_shader_cache*`
3. 确认可用内存 > 5 GB（游戏预留 24 GiB 并按页提交）

否则测出来的是环境噪声，不是代码进度。
