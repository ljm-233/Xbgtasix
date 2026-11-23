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
