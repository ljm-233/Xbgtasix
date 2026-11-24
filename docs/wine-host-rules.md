# Wine 上的两条通则（跨项目可复用）

写于 2026-11-25。这两条不是本项目的私有知识，任何"Windows PE 经 Wine 跑起来"的活儿都会撞上，
所以单独成文。原始证据、反汇编与日志在 [wine-host.md](wine-host.md) 第 9、10 节。

---

## 通则一：这版 Wine 不投递硬件断点事件 —— 用 int3 探针

### 现象（实测，可复现）

在 shim 里用 `GetThreadContext / SetThreadContext` 给**当前线程**装 Dr2/Dr3 执行断点：

```
[xbgtasix] 已装执行断点：Dr2=game+0x56EB10 Dr3=game+0x56EB3F Dr7=0x50
```

`SetThreadContext` 返回成功、`Dr7 = 0x50`（L2/L3 使能位都对），目标是**必经的函数入口**，
结果**零命中**。写断点（Dr1，4 字节写，盯一个后来确实被读的计数器）同样零命中。
`Dr7` 的位算得没问题（L1 = bit2、RW1 = bits20-21、LEN1 = bits22-23）。

**结论：不要把时间花在调 Dr0..Dr3 上。** 至少 "Proton/DW-Proton 11.0 这套 wine" 是这样。

### 替代方案：一次性 int3 探针

做法（本仓库 `src/trace.cpp` 的 `install_int3_probes`）：

1. 把目标 RVA 的首字节改成 `0xCC`（`VirtualProtect` + 写 + `FlushInstructionCache`）；
2. VEH 收到 `EXCEPTION_BREAKPOINT` 且地址匹配 → 打 `rax/rbx/rcx/rdx`，并把 `rbx`（第 1 个点）
   或 `rdi`（第 2 个点）当 C 字符串读出来（非可打印字符替换成 `?`，最多 512 字节）；
3. **还原原字节**，把 `Rip` 拨回该指令开头，`EXCEPTION_CONTINUE_EXECUTION` 继续跑。

要点：只改内存、不碰磁盘文件；一次性（不重复触发）；一次最多两个点；两个点的
`rbx`/`rdi` 分工与 Dr2/Dr3 的槽位约定一致，换工具时不用改命令。

### 用法

```bash
# 包了层命令行的版本（本仓库）
python3 tools/probe_at.py 56EBA8 56EBBD
```

输出样例（真实运行，`0x56EBA8` 是 `call 0x140968480` 的下一条，`0x56EBBD` 是 `call *0x168(%rax)` 的下一条）：

```
[xbgtasix] 已装 int3 探针 0：game+0x56EBA8（原字节 0x48）
[xbgtasix] 已装 int3 探针 1：game+0x56EBBD（原字节 0x89）
[xbgtasix] 探针0 @game+0x56EBA8  rax=0x1478F5E80 rbx=0x10FA80 rcx=0x6FFFFFFAB7C0 rdx=0xFFFFFFFF
[xbgtasix] 执行断点：int3 探针0 rbx -> "G:/appdata0.rpf"（长度 15）
[xbgtasix] 探针1 @game+0x56EBBD  rax=0xFFFFFFFF rbx=0x10FA80 rcx=0xF rdx=0xED1D97704B9104D6
[xbgtasix] 执行断点：int3 探针1 rdi -> "appdata0.rpf"（长度 12）
```

这两行直接给出"它在找 `G:/appdata0.rpf`、lookup 成功、方法返回 `-1`"，
比读三个函数体的汇编快一个数量级。

### 还有一个更好用的变体：直接在探针里问系统

探针命中时顺手用 Win32 自检同一个路径/对象（`GetFileAttributesA` + `CreateFileA` + `GetLastError`），
一步就能把"路径拼错 / 变量没展开 / 文件真不在"三个假设全杀掉。实测输出：

```
Win32 侧自检 G:/appdata0.rpf        -> attr=无 open=失败 GetLastError=2
Win32 侧自检 G:\appdata0.rpf        -> attr=无 open=失败 GetLastError=2
Win32 侧自检 D:\GTA6\...\appdata0.rpf -> attr=有 open=成功
```

**"文件明明在、GetLastError=2"** 这一行就是通则二的入口。

---

## 通则二：Wine 的盘符只在进程启动时从 `$WINEPREFIX/dosdevices/` 建

### 现象（实测）

进程内调用 `DefineDosDeviceW(DDD_RAW_TARGET_PATH, L"G:", L"\\??\\D:\\path")`：
**返回 TRUE，但 `G:\` 不存在**。三种写法都试过，全部"成功但不生效"：

| 写法 | 结果 |
|---|---|
| raw DOS 目标：`\??\D:\GTA6\...` | 调用成功，`GetFileAttributesW(L"G:\\")` = 失败 |
| raw unix 目标：`\??\unix\mnt\...`（路径由 `wine_get_unix_file_name` 取得） | 同样失败 |
| 非 raw：`DefineDosDeviceW(0, L"G:", L"D:\GTA6\...")` | 同样失败 |
| 注册表 `HKCU\Software\Wine\Drives` 写 `G:` = `Z:\...` | `if exist G:\appdata0.rpf` → 否 |

**真正管用的**（必须在起进程**之前**做）：

```bash
ln -sfn /path/to/target "$WINEPREFIX/dosdevices/g:"
wine cmd /c "if exist G:\\appdata0.rpf (echo G_YES) else (echo G_NO)"   # -> G_YES
```

### 所以该由谁做

**宿主侧脚本/启动器**（本仓库：`tools/run_wine.sh` 的 `setup_drive g "$PKG"` / `setup_drive t …`），
**不是进程内的 shim**。原因：shim 跑起来的时候，这个进程的盘符表已经从 `dosdevices/` 建完了，
再调 `DefineDosDeviceW` 加不进来。

进程内那份代码（本仓库 `src/fstrace/drives.cpp`）保留的职责只有两条：

1. **映射完立刻自检**（`GetFileAttributesW(L"G:\\")`），不要相信 `DefineDosDeviceW` 的返回值；
2. **失败时大声报**：`!! G: 三种写法都没能生效 —— 依赖这个盘符的路径会全部 404`。

删掉启动前那两行 `ln -sfn`，游戏立刻回到"文件明明在、`GetLastError=2`（ERROR_FILE_NOT_FOUND）"，
而且**不会报错**，只会表现成某个上层断言炸掉 —— 这正是本项目 482 ms → 6868 ms 之间卡了一整轮的原因。

---

## 一条附带的经验

**先装眼睛，再看汇编。** 本项目最后一堵墙（`game+0x2237178`）先后被推断成
"注册表计数为 0"、"分发列表为空"，两次都被实测推翻；真正的原因（盘符假成功）
是探针打出 `"G:/appdata0.rpf"` + 三行 Win32 自检之后**五分钟内**确定的。
反汇编适合回答"它是怎么写的"，不适合回答"运行时到底是什么值"。
