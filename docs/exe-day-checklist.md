# exe 到手后：启动流程

前提：`./build.sh` 能过、`./tools/run_suite.sh` 是 38 PASS / 1 SKIP（共 39 步）。
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
python tools/analyze_exe.py "$XBGTASIX_PKG/game_scarlett_master.exe"
```

- **看到"缺失导入 0"** → 我们 461 个导出覆盖了它要的全部符号，可以进第 2 步
- **看到缺失列表** → 那些名字就是要补的桩，先用 `gen_stubs` 补，别急着跑

## 2. 第一次运行：log 模式（默认，不会因为一个未实现 API 就死）

```bash
./tools/launch.sh "$XBGTASIX_PKG/game_scarlett_master.exe" --timeout 180
```

启动器会做四件事：建 staging、把 3 个 shim 拷进去、设好 `XBGTASIX_*` 环境变量、
跑起来并按退出码分类。

**判读表**：

| 看到 | 含义 | 下一步 |
|---|---|---|
| `分类: PASS` | 正常退出 | 进第 3 步 |
| `分类: UNIMPL` + 一堆 API 名 | 撞到未实现（仅 `--stop` 模式） | 那些名字就是待办清单 |
| `分类: TIMEOUT` | 卡住 | 看"日志末尾"，通常卡在等一个永不完成的 async |
| `0xC0000135` | 缺依赖 DLL | 把 package 里同名 DLL 拷进 staging |
| `0xC0000139` | 缺导出（入口点未找到） | 回第 1 步，用 `analyze_exe` 对表 |
| `0xC0000005` | 访问冲突 | 看最后一条日志：多半是未实现接口返回空指针后被解引用 |
| 窗口出现但黑屏 | 渲染未出画面 | 参考 [graphics-pitfalls.md](graphics-pitfalls.md) 的 9 条 |

日志里每条 `UNIMPLEMENTED` 后面都带**调用栈地址**——用 `addr2line` 或
Visual Studio 的符号加载可以定位到是哪个模块调的。这就是排查的起点。

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
