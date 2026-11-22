# exe 到手当天：10 分钟出结论的操作顺序

目标：**拿到 `game_scarlett_master.exe` 后，用最短路径判断"能不能启动、缺什么"。**

## 0. 先验文件（1 分钟）

```bash
file game_scarlett_master.exe        # 必须是 PE32+ x64
python tools/analyze_exe.py game_scarlett_master.exe | head -30
```

看三件事：

- **架构** 必须是 x64（主机是 x86-64，32 位就没戏）
- **导入模块数** 与 `MicrosoftGame.Config` 是否自洽
- **主机专有导入** 有多少、我们缺多少

分析器会直接给出「需要 shim 提供 N 个符号，已覆盖 M，**缺 K**」。

## 1. 补齐缺失导出（3 分钟）

```bash
EXCL=$(grep -v '^;' src/xgameruntime/exports.txt | grep . | paste -sd, -)
python tools/gen_stubs.py ../gdk/lib/xgameruntime.lib --dll xgameruntime \
    --out src/generated --exclude "$EXCL"
./build.sh
```

> 拿到真实导入表后，优先实现"每次启动都会调"的那批（运行时初始化、任务队列、
> 用户、内存），其余按 tracer 日志的顺序来。**不要一次铺满 500 个。**

## 2. 试跑，看第一个缺口（1 分钟）

```bash
./tools/run_target.sh /path/to/game_scarlett_master.exe
```

输出会给出 `第一个未实现调用` + 日志尾部。
默认策略是"未实现即退出"，所以第一次跑**一定**会停在某个 API 上 —— 那就对了。

## 3. 按调用顺序推进（按小时算）

| 现象 | 说明 |
|---|---|
| 报某个 `XGame*` / `XUser*` 没实现 | 补那一个，重跑 |
| 报 `d3d12_xs!X` 找不到 | 我们的 `forward.def` 缺这个导出，补一条转发 |
| 进程直接无输出退出 | 可能是 CRT/TLS 或入口点问题，用 `XBGTASIX_UNIMPL=log` 再跑一次 |
| 卡住不动 | 异步等待没被满足（`XTaskQueue`/`XAsyncBlock` 路径），查 `XTaskQueueSubmitCallback` 是否被调用 |

## 4. 预期会撞到的三堵墙

### 4.1 RPF9 目录表（最可能）

游戏启动后第一个要读的就是资源包。目录表是加密的，**但游戏自己的代码里就有解码器** ——
所以这堵墙不影响"能不能跑"，只影响"我们能不能独立读资产"。

判断方法：日志里出现 `CreateFile` 打开 `.rpf` 之后停在某个读取函数，说明它已经在解目录了。

### 4.2 主机专有 D3D12 接口

游戏大概率会 `QueryInterface` 一个 `ID3D12DeviceX` 之类的接口。我们没有 IID，
只能返回 `E_NOINTERFACE`，然后看它是否降级——很多引擎会降级，有些不会。

### 4.3 商店/授权

`XStore*` 一律返回桩。如果游戏在启动早期硬要求 entitlement，会卡在这里；
那就只能继续往深处补（`XStoreQueryProductForCurrentGameAsync` 等），不要试图伪造授权。

## 5. 如果它真的跑了

优先级立刻变成：

1. **抓第一帧**：改 `t8_model` 的相机/管线不合适，直接给 shim 加 hook 记录所有 `Present`
2. **导入表驱动的实现排序**：按调用次数从高到低补
3. **录一份完整调用序列**（`XBGTASIX_LOG`）作为回归基准

## 6. 不要做的事

- 不要去破解或伪造授权（法律红线，且技术上也是死路）
- 不要在没有备份的情况下原地改 `src/generated/`（它是生成物，重跑脚本会覆盖）
- 不要一次实现几十个 API 再跑 —— 每实现一个就跑一次，失败点最清晰
