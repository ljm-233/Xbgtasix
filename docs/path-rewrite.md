# 路径重写：把"造盘符"换成"文件 API 层换字符串"

写于 2026-11-25。所有结论都是本机（Arch + Wine/Proton）实测，命令与日志原样贴。

---

## 0. 一句话

游戏里写死了 `G:`（`"G:/appdata0.rpf"`，int3 探针打出来的原文）。以前靠在 Wine 前缀里建
`dosdevices/g:` 符号链接；现在改成 **shim 在文件 API 转发之前把 `G:\...` / `G:/...` 换成真实目录**。
两种模式都实测到同一个位置（`game+0x247AF2D`，那堵 64 MiB 瞬时块的真问题墙），
**重写模式在 `g:`/`t:` 符号链接被移走的前提下跑通**。

---

## 1. 为什么换

| 做法 | 结果（实测） |
|---|---|
| `DefineDosDeviceW` raw DOS 目标 | 返回 TRUE，`G:\appdata0.rpf` 仍 `ERROR_FILE_NOT_FOUND(2)` |
| `DefineDosDeviceW` raw unix 目标（`\??\unix/...`） | 同上，假成功 |
| `DefineDosDeviceW` 非 raw | 同上，假成功 |
| 注册表 `HKCU\Software\Wine\Drives` | 无效 |
| `$WINEPREFIX/dosdevices/g:` 符号链接 | **有效**，但必须在起进程之前建好 |

也就是说盘符这条路只在"宿主提前摆好符号链接 + 这一份 Wine 认 dosdevices"这个组合下成立。
换成路径重写之后：Windows 与 Wine 走同一条代码路径，Wine 的进程启动时序、注册表、dosdevices
全都不参与，只剩"查表 + 拼字符串"。

---

## 2. 实现（改在哪三个地方）

### 2.1 表：`src/fstrace/drives.cpp`

表还是原来那张，只是用途从"建盘符"变成"查表换字符串"：

| 盘符 | 默认落地目录 | 覆盖变量 |
|---|---|---|
| `G:` | shim 自己所在目录（= 包目录，`appdata0.rpf` 等就在这里） | `XBGTASIX_DRIVE_G` |
| `T:` | `%LOCALAPPDATA%\Xbgtasix\T` | `XBGTASIX_DRIVE_T` |
| `D:` | 只有显式指定时才进表（否则会遮掉真实 D 盘） | `XBGTASIX_DRIVE_D` |

新增对外三个函数（声明在 `include/xbgtasix/trace.h`）：

```cpp
bool path_rewrite_enabled();                                // 开关
bool rewrite_console_path(const wchar_t* in, std::wstring& out);  // 命中就替换并返回 true
long long path_rewrite_count();                             // 命中次数
```

重写规则（每条都有自检日志兜着，见第 3 节）：

- 只认 `字母 + ':'` 开头；**盘符字母大小写不敏感**（`G:` / `g:` 都命中）；
- 盘符后面的根分隔符**正斜杠反斜杠都吃**（`G:\x`、`G:/x`、`G:\`、只有 `G:` 四种写法都对）；
- 尾部原样保留（其中的 `/` 不转，Windows 和 Wine 都接受）；
- UNC（`\\server\share`）、设备路径（`\\?\`、`\\.\`）、相对路径、表外盘符（`C:`、`Z:`）
  **一律原样放行**，返回 false；
- 命中日志去重 + 上限 200 行（游戏会成千上万次调这些 API，日志只留"哪些路径被换了"）。

### 2.2 转发层：`src/fstrace/fstrace.cpp`（IAT 钩子）

这一层原来只管追踪（`XBGTASIX_FSTRACE=1`）。现在共有两个开关，**装钩子的条件是并集**：

```cpp
bool tracing() { ... }   // XBGTASIX_FSTRACE=1，只有它才写日志
bool active()  { return tracing() || xbgtasix::path_rewrite_enabled(); }  // 决定要不要接管 IAT
```

每个 thunk 结构：`active()` → 重写路径（`RwPath::apply`）→ 转发 → 只在 `tracing()` 时记日志。
`apply` 不命中就返回**原指针**，与从前完全一致（零拷贝放行）。

拦了的 API（分两步，按任务书要求）：

| 步 | API | 说明 |
|---|---|---|
| 1（读） | `CreateFileW` `CreateFile2` `GetFileAttributesW` `GetFileAttributesExW` `FindFirstFileW` `FindFirstFileExW` | 让 `appdata0.rpf` 落地就够用 |
| 2（写/属性/ANSI） | `GetFileAttributesA` `CreateDirectoryW` `RemoveDirectoryW` `DeleteFileW` `MoveFileExW` `SetFileAttributesW` | 这 6 个是本作 exe **导入表里真实存在**的（`objdump -p` 查的），所以都补上 |
| 不拦 | `CreateFileMappingW` `GetFullPathNameW` `ReadFile`/`FindNextFileW` | 前两个：本作 exe **根本没导入**；mmap 收的是 HANDLE 不是路径，天然不用换。后两个只拿 HANDLE |

`GetFileAttributesA` 单独处理：ACP → wide → 重写 → ACP，任何一步转换失败就原样转发
（宁可 404 也不拿猜出来的路径去开文件）。

### 2.3 我们自己的 DLL 也得换：`src/console_hw/xcrdapi.cpp`

**这是第一轮重写失败的原因，值得单独记。** 第一版只改主 exe 的 IAT，结果：

```
[rewrite] #1 G:\appdata0.rpf  ->  Z:\mnt\...\GTA6.Xbox.v1.0.2.1-CYBERLEEK\appdata0.rpf
[xcrdapi] IDStorageFactoryX::OpenFile(slot4) 打不开 'G:\appdata0.rpf'（尝试过原路径与 game_root 回退）hr=0x80070002
[xbgtasix] FATAL 异常 0x80000003 at game_scarlett_master.exe+0x2237178     ← 还是第 4 堵墙，799 ms
```

`xcrdapi.cpp` 的 `open_path_candidates` 用 `CreateFileW` 开游戏递进来的路径 —— 那是**我们自己的
DLL**，它的导入表不在 xmem 的 patch 范围里，所以 `G:` 照旧打不开。修法：在候选列表最前面插一条
`rewrite_console_path()` 的结果（`build.sh` / `tools/build_linux_cross.sh` 把 `drives.cpp` 也编进
`XCRDAPI.dll`）。补上之后 6 个 rpf 全部打开：

```
[xcrdapi] IDStorageFactoryX::OpenFile(slot4) 'G:\appdata0.rpf'    -> hr=0x00000000 实际打开路径='Z:\mnt\...\appdata0.rpf'
[xcrdapi] IDStorageFactoryX::OpenFile(slot4) 'G:\common.rpf'      -> hr=0x00000000 ...
[xcrdapi] IDStorageFactoryX::OpenFile(slot4) 'G:\movies.rpf'      -> hr=0x00000000 ...
[xcrdapi] IDStorageFactoryX::OpenFile(slot4) 'G:\shaders.rpf'     -> hr=0x00000000 ...
[xcrdapi] IDStorageFactoryX::OpenFile(slot4) 'G:\anim.rpf'        -> hr=0x00000000 ...
[xcrdapi] IDStorageFactoryX::OpenFile(slot4) 'G:\data.rpf'        -> hr=0x00000000 ...
（后面还有 levels/navmeshes/packs/textures/audio/script.rpf）
```

顺带查出来一件事：那条日志里的 "game_root 回退" 是**死代码** —— `xbgtasix::set_game_root()`
全仓库**没有任何调用点**，`game_root()` 恒为空串，所以回退那两级永远不触发（`grep -rn
set_game_root src` 只有定义处）。这也是为什么第一版里明明有回退还是 0x80070002。

---

## 3. 怎么开关

```bash
# 默认：路径重写（不建任何盘符）
./tools/run_wine.sh --dll-dir build/wine-bridge-rewrite

# 回退：盘符模式（脚本会自己建 dosdevices/g:、t: 符号链接）
XBGTASIX_PATH_REWRITE=0 ./tools/run_wine.sh --dll-dir build/wine-bridge-rewrite

# 只想追踪、不想改路径（纯观察 G:\ 的原始调用）
XBGTASIX_PATH_REWRITE=1 XBGTASIX_NO_PATH_REWRITE=1 XBGTASIX_FSTRACE=1 ./tools/run_wine.sh
```

`XBGTASIX_NO_PATH_REWRITE=1` 优先于 `XBGTASIX_PATH_REWRITE=1`（"反向开关"更明确）。
`tools/run_wine.sh` 只在回退模式下建那两行符号链接。

---

## 4. 实测证据

编译：

```bash
export PATH="$HOME/toolchain-dl/mingw/usr/bin:$PATH"
OUT=build/linux-rewrite CXX=x86_64-w64-mingw32-g++ \
  GDK_INC=/mnt/winbot-software/GTA6/gdk/include_clean ./tools/build_linux_cross.sh
# -> OK：三个 shim 交叉编译完成 -> build/linux-rewrite
cp build/linux-rewrite/*.dll build/wine-bridge-rewrite/     # --dll-dir 隔离，不动包内文件
```

跑之前把 `g:`/`t:` 从 dosdevices 里移走（**这是判据**）：

```bash
mv ~/Games/gta6-x11/dosdevices/{g:,t:} ~/Games/gta6-x11/dosdevices-moved/
```

自检（每次重写模式启动都打，前 15 行原样）：

```
[rewrite] 路径重写模式：不建任何盘符，文件 API 层查表换路径
[rewrite] 表 G: -> Z:\mnt\winbot-software\GTA6\GTA6.Xbox.v1.0.2.1-CYBERLEEK
[rewrite] 表 T: -> C:\users\steamuser\AppData\Local\Xbgtasix\T
[rewrite] 自检 "G:\appdata0.rpf" 重写 -> "Z:\mnt\...\GTA6.Xbox.v1.0.2.1-CYBERLEEK\appdata0.rpf"
[rewrite] 自检 "G:/appdata0.rpf" 重写 -> "Z:\mnt\...\GTA6.Xbox.v1.0.2.1-CYBERLEEK\appdata0.rpf"
[rewrite] 自检 "g:\Common.RPF" 重写 -> "Z:\mnt\...\GTA6.Xbox.v1.0.2.1-CYBERLEEK\Common.RPF"
[rewrite] 自检 "G:" 重写 -> "Z:\mnt\...\GTA6.Xbox.v1.0.2.1-CYBERLEEK"
[rewrite] 自检 "\\server\share\x.rpf" 原样放行
[rewrite] 自检 "appdata0.rpf" 原样放行
[rewrite] 自检 "C:\windows\notepad.exe" 原样放行
[fstrace] 已接管主 exe 的 11 个 IAT 槽：CreateFileW ReadFile GetFileAttributesW
          GetFileAttributesExW FindFirstFileW GetFileAttributesA CreateDirectoryW
          RemoveDirectoryW DeleteFileW MoveFileExW SetFileAttributesW
```

三种跑法的对照（同一台机、同一份包、`--dll-dir build/wine-bridge-rewrite`）：

| 跑法 | 退出码 | 耗时 | 停在哪 | 日志行数 |
|---|---|---|---|---|
| 盘符模式（`dosdevices/g:` 在） | 0x80000003 | 6964 ms | `game+0x247AF2D` | 2577 |
| 盘符模式（第二次，缓存热） | 0x80000003 | 9109 ms | `game+0x247AF2D` | 2577 |
| **重写模式（无 `g:`/`t:`）** | 0x80000003 | **3014 ms** | **`game+0x247AF2D`** | 2598 |
| 重写模式（补完写侧钩子后） | 0x80000003 | 2964 ms | `game+0x247AF2D` | 2608 |

断言现场两边一模一样（这才是"同一个位置"的硬证据，不是看毫秒）：

```
[xbgtasix] 断言操作数: begin=[rsp+0x70]=0x0(可读) end=[rsp+0x78]=0x4000000(可读) g=0x0(可读)
[xbgtasix] 减法 end-begin = 0x4000000 （无回绕）；加 g 后 = 0x4000000；阈值 0x1FFFFF -> 超出（触发断言）
[xbgtasix]   #2 game+0x247AF2D  #3 game+0x2479430  #4 game+0x19F4F54  #5 game+0x580437E
             #6 game+0x43A4911  #7 game+0x224070B  #8 game+0x23CF00A  #9 game+0x2215ECA
```

日志逐行对比（把地址与路径归一化后 `diff`）：两种模式**只差两类行** ——
`[rewrite]` 行，以及 `OpenFile` 那行的 `实际打开路径=`（盘符模式是 `G:\x`，重写模式是真实路径）。
其余 2570 行完全一致，说明游戏走的是同一串步骤、没有跳过任何东西。这也解释了为什么
"耗时"不能当判据：同一模式的两次跑分别是 6964 / 9109 ms（差 2 s），host_stall / d3d12 初始化
本来就在抖；比位置只能比 RVA 与断言现场。

---

## 5. 边界与已知缺口

- **`CreateFileMappingW` / `GetFullPathNameW` 没拦**：本作 exe 的导入表里没有这两个名字
  （`objdump -p ... | awk '$1 ~ /^0/ {print $4}'` 查过）。mmap 走的是 `CreateFileW` 拿到的
  HANDLE，路径在那一刻已经换过，所以不需要额外处理。将来换标题时先查导入表再加。
- **游戏完全绕开 Win32 走 `NtCreateFile`**：本作没出现。真出现的时候，IAT 这条路就断了，
  那时只能退回"盘符 + 重写并用"（回退开关就是为这个留的）。
- **我们自己的 shim 若再开游戏给的路径**：必须像 `xcrdapi.cpp` 那样自己调
  `rewrite_console_path`，并把 `src/fstrace/drives.cpp` 加进那个 DLL 的编译行（一行）。
  xmem 的 IAT 钩子只管主 exe。
- **每个 DLL 各有一张表**：表是函数内静态，`G:` 默认指向"该 DLL 自己所在目录"。当前所有 shim
  都被启动器拷进包目录，所以各表一致。若以后把 shim 放到别处（`--dll-dir` **只影响拷贝源**，
  运行期仍在包目录），要用 `XBGTASIX_DRIVE_G` 统一指定。
- **DWARF 体积**：`build_linux_cross.sh` 用当前 toolchain（mingw 16.2.0）编出来的
  `xgameruntime.dll` 有 14 MB（带 `.debug_info`），而 `build/wine-bridge` 里那份 3.59 MB 是
  mingw 16.1.0（Brecht Sanders）编的。功能上等价（导出数都是 465 条，新版含 50 个 `_GUID_*`
  数据导出），只是体积差别，不影响加载或本文的结论。

---

## 6. 改动的文件

```
include/xbgtasix/trace.h        + 路径重写三个声明
src/fstrace/drives.cpp          表 + 查表重写 + 自检；盘符模式降级为回退
src/fstrace/fstrace.cpp         两个开关（tracing/active）、11 个 path 钩子、重写后转发
src/console_hw/xcrdapi.cpp      OpenFile 候选表第 0 级：自己查表重写
build.sh / tools/build_linux_cross.sh   XCRDAPI 也编进 drives.cpp
tools/run_wine.sh               按模式决定要不要建 dosdevices 符号链接
```
