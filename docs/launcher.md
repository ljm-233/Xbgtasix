# xbgtasix.exe —— 转译层的启动器

三个 shim 是**库**，不是程序。`xbgtasix.exe` 才是能双击运行的那个东西：
把 DLL 摆到位、设环境、拉起游戏、看它怎么死、告诉你下一步做什么。

**纯 C++、静态链接、零运行时依赖**（不需要 Python、不需要 bash、不需要 MSYS）。
测试也是 C++（`tests/t39_host_tool`），因为项目不引入 Python 依赖。

## 六条命令

```
xbgtasix run     <exe|包目录> [--dry-run] [--stop] [--timeout 秒] [--dll-dir 目录] [--log 文件]
xbgtasix check   <exe> [--dll-dir 目录]
xbgtasix verify  <包目录>
xbgtasix doctor
xbgtasix restore <包目录|exe路径>
xbgtasix firstrun <包目录|exe路径> [--dry-run] [--stop] [--timeout 秒]
```

| 命令 | 做什么 | 退出码 |
|---|---|---|
| `run` | 预检 → 放 DLL → 拉起 → 分类失败 → 给下一步 | 0=PASS，1=非 PASS，2=自身错误 |
| `check` | 解析目标 exe 的导入表，跟三个 shim 的导出表逐名字对表 | 0=全覆盖，1=缺符号，2=解析失败 |
| `verify` | 按包自带 `layout_*.xml` 的 `<FileGroup Include="…"/>` 核对文件 | 0=完整，1=有缺失，2=找不到清单 |
| `doctor` | OS / 显卡 / D3D12 / VC 运行库 / shim / 写权限 / 交互桌面 | 0=通过，1=有阻碍项 |
| `restore` | 撤销 `run` 的写入：有备份就还原，没备份就删掉我们放的 DLL | 0=还原干净，1=有文件被占用，2=路径不存在 |
| `firstrun` | `verify → doctor → check → run` 四步一次走完 | 同 `run`（最后一步的退出码），2=目标 exe 不存在 |

## check 与 `tools/analyze_exe.py` 的关系

`check` 是这个能力的**权威实现**：它解析导入表、跟三个 shim 的导出表逐名字对表，还会
真去解析每一个非 shim 依赖模块（`SearchPathW` + `LoadLibraryEx(LOAD_LIBRARY_AS_DATAFILE)`），
并能识别 shim 文件损坏、把按序号导入的符号排除在"缺失"之外。

**以 C++ 的 `check` 为准，`tools/analyze_exe.py` 只是历史遗留/补充**：
它是纯 Python 的同一件事，留着只是因为「不需要 Windows、也不需要先构建出 shim」
——拿一个 exe 在别的机器上做静态分诊时用得上。两者结论冲突时，信 `check`。

## check 能告诉你的三件事

1. **缺哪些导出名**（`需要 N 个，缺失 M 个` + 名字清单，每模块最多列 25 个）
2. **哪些依赖模块根本解析不到**（运行时就是 `0xC0000135` / `MISSING_DLL`）
3. **shim 自己是不是坏的**（0 字节、缺导出 → 直接 exit 2，不拿坏数据对表）

## run 的失败分类

| 退出码 | 分类 | 下一步 |
|---|---|---|
| 0 | `PASS` | 看画面；没画面就用 `--stop` 重跑 |
| 70 | `UNIMPL` | 上面的频次表就是待补的桩清单；**若里面只有 `XStore*` 名字，那是授权卡点而不是兼容层缺口** → [license-and-store](license-and-store.md) |
| 124 | `TIMEOUT` | 多半卡在等一个永不完成的异步 |
| `0xC0000135` | `MISSING_DLL` | 把包内同名 DLL 拷到 exe 同目录 |
| `0xC0000139` | `MISSING_EXPORT` | 用 `xbgtasix check` 看缺哪些名字 |
| `0xC0000005` | `ACCESS_VIOLATION` | 看日志最后一行（常见是空指针解引用） |
| `0xC000007B` | `BAD_IMAGE` | `check` 确认目标是不是合法 PE |

## 放置方式：只有一种，但会先备份

要跑起来，三个 DLL **必须**放在 exe 旁边——没有第二种办法（游戏按 exe 目录找依赖，
用 junction 把包挂到别处，DLL 实际还是写进原包，这点已实测确认）。
既然**要跑就必须把 DLL 放进包**，`run` 在覆盖同名文件前先把原件备份成
`<名字>.xbgtasix-backup`；之后要撤销就交给 `restore`。

| 方式 | 命令 | 行为 |
|---|---|---|
| 直接放入（唯一方式） | `run <包目录>` | 往包目录写 3 个 DLL；**覆盖同名文件前先备份**为 `*.xbgtasix-backup` |
| 只检查不写 | `run <包目录> --dry-run` | 预检 + 打印"将写入/备份/启动什么"，不写文件、不起进程 |

`--dry-run` 是**唯一**的"先看看"开关：它只做预检并逐条打印计划（目标 exe、三个 shim 的
来源与目的地、哪些会被备份、退出后用 `restore` 怎么恢复），不碰磁盘、不拉起进程。

> 曾经的 `--staging` 已删除：它用 junction 指向原包，所谓"临时目录"其实就是原包，
> 备份与 DLL 都落在包里，"原包不动"的承诺不成立。现在传 `--staging` 会明确报错并指向 `--dry-run`。

## restore：撤销 `run` 的写入

`run` 只写三样东西：备份同名 DLL、覆盖、写日志。`restore` 就是它的撤销，三个 shim 逐个走这张表：

| 目录里的状态 | 动作 | 输出 |
|---|---|---|
| 有 `<dll>.xbgtasix-backup` | 先删掉我们放的那份，再把备份改回原名 | `已还原 <dll>（原件已回到原位）` |
| 没备份、但有 `<dll>` | 这个位置原本是空的，现在这份是我们放的 → 直接删 | `已移除 <dll>（原来没有同名文件）` |
| 两个都没有 | 没事可做 | `<dll> 不在这个目录里，跳过` |

顺序不能反：`MoveFile` 到已存在的目标会失败，所以必须"先删 dst 再改名"。

收尾两件事：

- 失败项（文件被游戏/杀软占用）逐条打 `!` 行，最后 `FAIL：N 项没还原成功`，退出码 1；
- 目录里若还剩别的 `*.xbgtasix-backup`（不认识的、或刚才没还原成功的），会**列出来让人自己判断**
  ——里面可能才是原件，不能静默当"全好了"。

**不碰**包里其它文件，尤其是 `xbgtasix.log`：它是排障证据，不是我们放的 DLL。

## firstrun：四步串联

```
verify → doctor → check → run
```

拿到包/ exe 时最顺的一条命令，四步共用一次预检结果：

| 步 | 做什么 | 不通过时的态度 |
|---|---|---|
| 1/4 `verify` | 按 `layout_*.xml` 核对包完整性 | **只警告**：缺文件不等于不用试，继续往下 |
| 2/4 `doctor` | 环境自检（显卡/D3D12/VC 运行库/写权限） | **只警告**：critical 项会提示"下面可能起不来" |
| 3/4 `check` | 导入面 vs 导出面 | **只警告**：能启动也可能半路崩在缺符号上 |
| 4/4 `run` | 真跑（带 `--dry-run` 时只预检） | 返回码与 `run` 完全一致 |

唯一的硬失败是**目标 exe 找不到**（`layout` 声明了但文件不在，或传的路径不存在）：
没有它，后面三步全是空谈，直接 exit 2。
`--dry-run / --stop / --timeout / --dll-dir / --log` 原样透传给第 4 步。

## 实测输出（本机）

```
== 预检
目标程序      probe_ok.exe
shim          3/3 在 D:\\GTA6\\Xbgtasix\\build

== 准备运行环境
放置方式      直接放入游戏目录
已放置        3/3 个 DLL

== 结果
退出码        0x00000000
分类          PASS
```

故障注入（`--stop` + 一个真桩）：

```
退出码        0x00000046
分类          UNIMPL
日志          9 行，UNIMPLEMENTED 1 条
未实现频次（先补最多的）：
    1  XLaunchRestartOnCrash
```

## 设计边界

只做这六件事（run / check / verify / doctor / restore / firstrun）。
**不做**：UI 窗口、进程注入、hook、配置系统、自动更新。
理由见 [HANDOFF](HANDOFF.md) 的"非关键路径已冻结"——判断标准是
「这件事能让 GTA6 早一分钟启动吗」。

## 实现要点（踩过的坑）

| 坑 | 症状 | 处理 |
|---|---|---|
| 源和目标是同一个 DLL | `CopyFile` 返回 err=32（共享冲突） | 先比绝对路径，同一文件视为已完成 |
| 用相对裸名调 `CreateProcess` | err=123（非法名） | 目标与工作目录都换成绝对路径 |
| 测试进程自己占着日志文件 | 工具 `CREATE_ALWAYS` 撞共享冲突，读不到子进程日志 | 加 `--log` 参数，测试用独立日志 |
| 工具输出是 UTF-8 | 外部按本地代码页解码会中文乱码 | 工具内 `SetConsoleOutputCP(CP_UTF8)`，读的一方按 UTF-8 解 |
