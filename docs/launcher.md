# xbgtasix.exe —— 转译层的启动器

三个 shim 是**库**，不是程序。`xbgtasix.exe` 才是能双击运行的那个东西：
把 DLL 摆到位、设环境、拉起游戏、看它怎么死、告诉你下一步做什么。

**纯 C++、静态链接、零运行时依赖**（不需要 Python、不需要 bash、不需要 MSYS）。
测试也是 C++（`tests/t39_host_tool`），因为项目不引入 Python 依赖。

## 四条命令

```
xbgtasix run    <exe|包目录> [--dry-run] [--stop] [--timeout 秒] [--dll-dir 目录] [--log 文件]
xbgtasix check  <exe> [--dll-dir 目录]
xbgtasix verify <包目录>
xbgtasix doctor
```

| 命令 | 做什么 | 退出码 |
|---|---|---|
| `run` | 预检 → 放 DLL → 拉起 → 分类失败 → 给下一步 | 0=PASS，1=非 PASS，2=自身错误 |
| `check` | 解析目标 exe 的导入表，跟三个 shim 的导出表逐名字对表 | 0=全覆盖，1=缺符号，2=解析失败 |
| `verify` | 按包自带 `layout_*.xml` 的 `<FileGroup Include="…"/>` 核对文件 | 0=完整，1=有缺失，2=找不到清单 |
| `doctor` | OS / 显卡 / D3D12 / VC 运行库 / shim / 写权限 / 交互桌面 | 0=通过，1=有阻碍项 |

## run 的失败分类

| 退出码 | 分类 | 下一步 |
|---|---|---|
| 0 | `PASS` | 看画面；没画面就用 `--stop` 重跑 |
| 70 | `UNIMPL` | 上面的频次表就是待补的桩清单 |
| 124 | `TIMEOUT` | 多半卡在等一个永不完成的异步 |
| `0xC0000135` | `MISSING_DLL` | 把包内同名 DLL 拷到 exe 同目录 |
| `0xC0000139` | `MISSING_EXPORT` | 用 `xbgtasix check` 看缺哪些名字 |
| `0xC0000005` | `ACCESS_VIOLATION` | 看日志最后一行（常见是空指针解引用） |
| `0xC000007B` | `BAD_IMAGE` | `check` 确认目标是不是合法 PE |

## 放置方式：只有一种，但会先备份

要跑起来，三个 DLL **必须**放在 exe 旁边——没有第二种办法（游戏按 exe 目录找依赖，
用 junction 把包挂到别处，DLL 实际还是写进原包，这点已实测确认）。

| 方式 | 命令 | 行为 |
|---|---|---|
| 直接放入（唯一方式） | `run <包目录>` | 往包目录写 3 个 DLL；**覆盖同名文件前先备份**为 `*.xbgtasix-backup` |
| 只检查不写 | `run <包目录> --dry-run` | 预检 + 打印"将写入/备份/启动什么"，不写文件、不起进程 |

> 曾经的 `--staging` 已删除：它用 junction 指向原包，所谓"临时目录"其实就是原包，
> 备份与 DLL 都落在包里，"原包不动"的承诺不成立。现在传 `--staging` 会明确报错并指向 `--dry-run`。

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

只做这四件事。**不做**：UI 窗口、进程注入、hook、配置系统、自动更新。
理由见 [HANDOFF](HANDOFF.md) 的"非关键路径已冻结"——判断标准是
「这件事能让 GTA6 早一分钟启动吗」。

## 实现要点（踩过的坑）

| 坑 | 症状 | 处理 |
|---|---|---|
| 源和目标是同一个 DLL | `CopyFile` 返回 err=32（共享冲突） | 先比绝对路径，同一文件视为已完成 |
| 用相对裸名调 `CreateProcess` | err=123（非法名） | 目标与工作目录都换成绝对路径 |
| 测试进程自己占着日志文件 | 工具 `CREATE_ALWAYS` 撞共享冲突，读不到子进程日志 | 加 `--log` 参数，测试用独立日志 |
| 工具输出是 UTF-8 | 外部按本地代码页解码会中文乱码 | 工具内 `SetConsoleOutputCP(CP_UTF8)`，读的一方按 UTF-8 解 |
