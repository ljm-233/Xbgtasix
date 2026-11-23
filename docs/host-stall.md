# 主机侧停顿：为什么标题会在 30 秒后"自己崩溃"

## 现象

真实 EXE 跑到图形初始化后，进程在某处**无声消失**或停在一个裸断言上，耗时总是
30 秒上下；日志里没有异常、没有 HRESULT、没有 D3D12 报错。看上去像崩溃，其实是
**标题自己的看门狗把它杀了**。

## 机制（[事实] 参考实现 Leonida PR #7858 的实测）

标题有一个开机 hang check 线程：

```
loop {
    Sleep(1000);
    如果主线程进度标记这一轮没变化 -> rounds++
    否则 rounds = 0
    如果这一轮实际超过 2 s（QueryPerformanceCounter）-> rounds = 0   // 模拟主机挂起
    if (rounds >= 30) 往地址 0 写 -> 进程被杀
}
```

主机上图形管线是**原生代码**建的，一瞬间；到 PC 上每条 PSO 变成一次**驱动编译**，
冷缓存下 25–60 s 都发生在标题的主线程里。看门狗看不到"进展"（驱动编译不在它的
可见范围内），30 轮后动手。

**这不是 bug，是它的设计**：它在防主机挂起。所以正确的做法不是绕过它，而是让它
的规则按本意生效。

## 做法

| 件 | 作用 |
|---|---|
| `xbgtasix::host_stall::scope` | 把我们自己的长操作标出来：这段时间不算"guest 醒着的时间" |
| `src/host_stall/host_stall.cpp` | 帐本（QPC 纳秒，**含进行中的停顿**）+ 主模块 IAT 里的 `Sleep` thunk |
| `include/xbgtasix/host_stall.h` | 客户端用的一张调用表（`XbgtasixHostStallApi`，让 xmem.dll 只多一个导出） |

`Sleep(n)`（n ≥ 1 s）的语义变成：**在停顿之外累计满 n ms 才返回**。于是被停顿盖住的
那一轮实际耗时 = 停顿时长 + 剩余醒着的时间，> 2 s，标题自己那条"超过 2 s 就重数"
的规则把计数清零，进程活下来。

边界：`Sleep(n < 1 s)` 原样转发（看门狗每轮是 1000 ms，改短睡眠只会引入差异）；
xmem.dll 没加载时 `scope` 是空操作；thunk 里不写日志、不分配、不加锁。

## 装在哪、谁负责调

- **钩子装在 xmem.dll**：它是主 exe 的静态导入，`DllMain` 最早执行，能赶在标题的
  watchdog 线程起来之前改好 IAT（`src/fstrace/fstrace.cpp` 的 DllMain 里调
  `xbgtasix::host_stall::install()`）。
- **今天已接的点**：`D3D12CreateDevice`（笔记本上枚举 + 初始化可以几秒）。
- **必须接的点（还没实现）**：主机 PSO 创建（设备槽 60–67 那一段）。见
  [device_abi.h](../src/d3d12_xs/device_abi.h) 里 `device_extension_stub_n` 上方的警告——
  谁实现那段，谁负责把驱动编译包进 `scope`。不包就是 30 秒后被杀。

### 接线骨架（照抄即可）

```cpp
#include "xbgtasix/host_stall.h"

// 主机 PSO 创建（设备槽 60–67 里 graphics/compute 的那个）
HRESULT WINAPI create_graphics_pipeline_state_x(DeviceProxy* self, /* 主机参数 */) {
    // 这一句是全部工作：把"这段是驱动在干活，不是标题主线程卡死"告诉账本。
    // xmem.dll 没加载时它是空操作，不影响任何现有行为。
    xbgtasix::host_stall::scope host_stall;

    return static_cast<ID3D12Device*>(self->pc)->CreateGraphicsPipelineState(/* ... */);
}
```

三条纪律：

1. **只包"驱动真在干活"的那一句**，不要包整个函数、更不要包循环——包大了会把
   正常的等待也算成停顿，标题的计数被无谓重置，反而更晚发现问题；
2. `scope` 可重入/可嵌套，但**不要跨线程**生存（账本是单调累加的，跨线程会重复计入）；
3. 别在 `Sleep` thunk 或短调用上加 —— 只对 `n >= 1 s` 的 guest 睡眠有意义。

### 真机上怎么确认它生效

```bash
grep "\[host_stall\]" run.log
# 期望两条以上：
#   [host_stall] Sleep 钩子已装：主机侧停顿不计入 guest 的睡眠预算
#   [host_stall] 主机侧停顿 12.34 s（累计 12.34 s）—— 已从 guest 的 Sleep 预算里扣除
```

只看到第一条、没有第二条 → 我们没有任何长停顿被记账（要么 PSO 还没走到，要么
`scope` 忘了加）。第一条都没有 → 主 exe 没导入 `Sleep` 或 xmem.dll 没被执行，
机制根本没生效，此时游戏活过 30 秒只能算运气。

## 验证

`tests/t43_host_stall`（离屏、无 GPU、无游戏，7 项断言）：

```
[ok]   xmem.dll 已加载
[ok]   xmem.dll 提供了 host_stall 调用表
[ok]   主模块 IAT 里的 Sleep 已被接管
[ok]   停顿期间 watchdog 一轮都没数完（0，期望 0..0 ms）
[ok]   被盖住那一轮的实际时长 > 停顿时长（标题会重数）（4002，期望 3400..6000 ms）
[ok]   没有停顿时的 Sleep(1000) 仍约 1 s（1014，期望 850..1800 ms）
[ok]   Sleep(50) 原样转发（63，期望 40..300 ms）
7 checks, 0 failures
```

**反向验证**（把 thunk 改成永远直通）：停顿期间 watchdog 数完 3 轮、被盖住那轮
只有 1001 ms → `2 failures`。所以这个测试确实在测机制，不是在测运气。

## 与 Leonida 的关系

同一条机制、两个独立实现。对方的版本更早落地（PR #7858，冷启动扛过 56 s 编译、
D3D12 调用从 1,641 推进到 1,661），并在 `runtime/src/host_stall.h` +
`runtime/shims/host_stall.cpp` 里把 `vkCreateComputePipelines` 标成停顿。
[leonida-crossref](leonida-crossref.md) 第四节记了它提供的其它实测事实。
