# exe 到手后：启动流程

前提：`./build.sh` 能过、`./tools/run_suite.sh` 是 38 PASS / 2 SKIP（共 40 步）。
下面每一步都有明确的"看到什么算成功、看到什么看哪里"。

## 0. 先备好环境（30 秒）

```bash
cd D:/GTA6/Xbgtasix
./build.sh                    # 生成 3 个 shim
export XBGTASIX_PKG="D:/GTA6/GTA6.Xbox.v1.0.2.1-CYBERLEEK"
```

exe 应该在包目录（`layout_*.xml` 里 Chunk 1001 声明的 `game_scarlett_master.exe`）。
如果它在别处，直接传绝对路径给启动器即可。

## 1. 静态核对（10 秒，不用跑）

```bash
build/xbgtasix.exe check "$XBGTASIX_PKG/game_scarlett_master.exe"
# 没有 Windows 侧构建产物时（例如在别的机器上分诊），用 Python 版顶着：
python tools/analyze_exe.py "$XBGTASIX_PKG/game_scarlett_master.exe"
```

- **看到 `PASS`** → 我们 461 个导出覆盖了它要的全部符号、其它依赖模块也都找得到，可以进第 2 步
- **看到缺失列表** → 那些名字就是要补的桩，先用 `gen_stubs` 补，别急着跑
- **看到 `!! 找不到` 的模块** → 那是 `MISSING_DLL` 的预兆，把包内同名 DLL 拷到 exe 同目录

以 `check` 的结论为准（它能真去解析依赖模块、能判 shim 损坏）；`analyze_exe.py`
只是"没构建产物时也能跑"的补充，详见 [launcher.md](launcher.md)。

## 2. 第一次运行：log 模式（不会因为一个未实现 API 就死）

```bash
./tools/launch.sh "$XBGTASIX_PKG/game_scarlett_master.exe" --timeout 180
```

`tools/launch.sh` 会做四件事：建 `build/launch/` staging、把 3 个 shim 和目标 exe 拷进去、
设好 `XBGTASIX_*` 环境变量（`XBGTASIX_UNIMPL=log`，这是它的默认）、跑起来并按退出码分类。

零依赖的等价路径是 `build/xbgtasix.exe`（要注意它**默认是 stop 模式**，撞到第一个未实现就退出 70）：

```bash
build/xbgtasix.exe run "$XBGTASIX_PKG" --dry-run     # 只看计划：写什么、备份什么、启动什么
build/xbgtasix.exe run "$XBGTASIX_PKG" --timeout 180 # 真跑（覆盖前备份为 *.xbgtasix-backup）
build/xbgtasix.exe restore "$XBGTASIX_PKG"           # 撤销：有备份→还原，没备份→删掉我们放的
```

**要跑就必须把 3 个 DLL 放进包**（没有第二种办法），所以 `run` 一定先备份；
`--dry-run` 是唯一"不写文件只看计划"的开关。

**判读表**：

| 看到 | 含义 | 下一步 |
|---|---|---|
| `分类: PASS` | 正常退出 | 进第 3 步 |
| `分类: UNIMPL` + 一堆 API 名 | 撞到未实现（仅 `--stop`/默认 stop 模式） | 那些名字就是待办清单；**若只有 `XStore*` 见下方"卡在授权"** |
| `分类: TIMEOUT` | 卡住 | 看"日志末尾"，通常卡在等一个永不完成的 async |
| `0xC0000135` | 缺依赖 DLL | 把 package 里同名 DLL 拷到 exe 同目录 |
| `0xC0000139` | 缺导出（入口点未找到） | 回第 1 步，用 `check` 对表 |
| `0xC0000005` | 访问冲突 | 看最后一条日志：多半是未实现接口返回空指针后被解引用 |
| 窗口出现但黑屏 | 渲染未出画面 | 参考 [graphics-pitfalls.md](graphics-pitfalls.md) 的 9 条 |
| 弹"无法验证授权"/登录失败/直接退 | **卡在授权校验，不是兼容层缺口** | 见 [license-and-store.md](license-and-store.md) |

日志里每条 `UNIMPLEMENTED` 后面都带**调用栈地址**——用 `addr2line` 或
Visual Studio 的符号加载可以定位到是哪个模块调的。这就是排查的起点。

### 若卡在授权（`XStore`）

- 授权链路是 **78 个 `XStore*` 导出**（77 个生成桩 + 1 个手写），它们**一律不伪造授权**，
  只打一行 `UNIMPLEMENTED` 并返回失败码——**这是设计决定，不是 bug**；
- 判据：`check` 是 PASS、日志里 `UNIMPLEMENTED` **只有 `XStore` 名字** ⇒ 卡在授权；
  若还混着别的 API 名，那才是兼容层缺口，先补那些；
- **不要**去伪造 license token、让 `XStoreIsLicenseValid` 返回 true、或 patch 游戏跳过校验。
  完整说明见 [license-and-store.md](license-and-store.md)。

## 3. 收敛未实现面

把第 2 步日志里的 `UNIMPLEMENTED` 名字去重，按出现顺序排：

```bash
grep "UNIMPLEMENTED" build/launch/xbgtasix.log | sed 's/.*: //' | sort | uniq -c | sort -rn
```

**先补出现次数最多的**，因为它们多半在启动/主循环里；一次只补一批，补完重跑，
观察日志里 `UNIMPLEMENTED` 的总数是否单调下降。下降停了就说明卡在别的地方，而不是"桩的问题"。

## 4. 切到 stop 模式，暴露第一个真正的拦路虎

```bash
./tools/launch.sh <exe> --stop --timeout 60
```

`stop` 模式在**第一个**未实现调用处停下（带栈），这样你能确认"程序跑到哪一步为止"。
这是判断"能不能跑"最快的手段：如果它停在某个**必然会被调用**的 API 上，那就是硬门槛。

## 5. 若出现窗口但无画面

按顺序排查（都在 [graphics-pitfalls.md](graphics-pitfalls.md)）：

1. 设备是不是选对了卡（日志里有 `选卡 ->`，没选独显会慢很多）
2. 有没有 `D3D12CreateDevice` 失败但被游戏忽略
3. 交换链相关：主机不用 swapchain，PC 必须有 —— 这是**最可能的结构性缺口**

## 6. 记录结果

不管成不成，把这三样记下来：`build/launch/xbgtasix.log`、退出码、以及
`grep UNIMPLEMENTED | sort | uniq -c` 的结果。这三样决定了下一轮该补什么。

## 崩了怎么看日志

一次真机运行的日志有几千行，`tail` 只能看到最后一条 FATAL。用：

```bash
python tools/summarize_run.py build/first_run.log --tail 15
```

它给出：各子系统调用计数、未实现 API 清单、**第一次致命异常**及其符号化调用栈
（`模块+RVA`，可直接 `objdump -d` 定位）、以及断言前最后 15 个调用。

跑的时候建议带上这三个开关：

| 环境变量 | 作用 |
|---|---|
| `XBGTASIX_VEH=1` | 致命异常处 dump 调用栈（模块+RVA） |
| `XBGTASIX_TRACE=1` | 记录所有 shim 调用（含主机硬件层） |
| `XBGTASIX_UNIMPL=log` | 撞到未实现 API 时继续跑而不是退出 |
