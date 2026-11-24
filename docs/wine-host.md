# 在 Linux / Wine 下运行：宿主差异、482 ms 那一枪的根因、以及多系统支持做到哪一步

写于 2026-11-25。所有结论都带可复跑的命令与原样输出；区分 **实测** 与 **推断**。

---

## 0. 一句话结论

Wine 上 482 ms 就死的那个断言（`0x80000003` at `game+0x5A91C21`）**不是 Wine 的 API 行为问题**，
是 **shim 少导出四个名字**：标题的 GDK 引导按名字 `GetProcAddress` 主机运行时的内部入口点
`InitializeApiImpl` / `InitializeApiImplEx` / `InitializeApiImplEx2` / `UninitializeApiImpl`，
我们一个都没有导出，于是引导函数返回 `0x89240101`，标题自己 `js` 跳到 `int3`。

补上这四个导出之后，**这条断言不再出现**，主线程前进到 `game+0x55DE020`：

```
1455de012: call 0x1455de048          ; 取 API 对象（内部先做引导，再调 QueryApiImpl）
1455de01b: mov  0x40(%rsp),%rcx      ; rcx = QueryApiImpl 写出来的对象指针
1455de020: mov  (%rcx),%rax          ; ← rcx=0，读 vtable 空指针 → 0xC0000005
```

也就是说下一堵墙是 **`QueryApiImpl` 要返回一个真的 COM 式对象**（标题会用
`vtable[0x10]` 当释放、`vtable[0xE8]` 当实际工作），这条还没实现。见第 5 节。

---

## 1. 先说环境：不补 dxvk/vkd3d-proton，连断言都到不了

实测（本机，Arch + ROG 天选7 Pro）：

```bash
P="/home/link/.local/share/proton/runners/DW-Proton Latest/files"
cd /mnt/winbot-software/GTA6/Xbgtasix
WINEPREFIX=~/Games/gta6-x11 PATH="$P/bin:/usr/bin:/bin" DISPLAY=:0 \
  wine build/xbgtasix.exe run 'D:\GTA6\GTA6.Xbox.v1.0.2.1-CYBERLEEK'
```

结果 **不是** 482 ms 的断言，而是更早的：

```
err:module:import_dll Library libvkd3d-shader-1.dll (which is needed by L"C:\\windows\\system32\\wined3d.dll") not found
err:module:import_dll Library libvkd3d-1.dll (which is needed by L"C:\\windows\\system32\\wined3d.dll") not found
err:module:import_dll Library wined3d.dll (which is needed by L"C:\\windows\\system32\\dxgi.dll") not found
err:module:import_dll Library dxgi.dll (which is needed by L"D:\\GTA6\\GTA6.Xbox.v1.0.2.1-CYBERLEEK\\d3d12_xs.dll") not found
err:module:loader_init Importing dlls for ... game_scarlett_master.exe failed, status c0000135
== 结果
退出码        0xc0000135     分类 MISSING_DLL     耗时 218 ms
```

原因：直接调 Proton 的 `wine` 二进制时，**wine 自带的 builtin `dxgi.dll` 静态依赖 `wined3d.dll`，
`wined3d` 又依赖 `libvkd3d-*.dll`**；而这几份 DLL 在 Proton 里单独放在
`files/lib/vkd3d/`、`files/lib/vkd3d-proton/`、`files/lib/wine/dxvk/`，
**不在 wine 的默认 PE 搜索路径里**（Proton 自己的启动脚本才补）。

注意两条走不通的路（都实测过）：

| 尝试 | 结果 |
|---|---|
| 把它们加进 unix `PATH` | 不变，仍 `c0000135`（PE 依赖不走 PATH） |
| 设 `WINEDLLPATH=$P/lib/vkd3d/x86_64-windows:$P/lib/wine/x86_64-windows` | 不变，仍 `c0000135`（这个变量在本次 wine 11.0 构建里没改变结果） |

**走通的做法**：把 `dxgi`/`d3d12` 让给 dxvk / vkd3d-proton（Proton 的正常组合）：

```bash
export WINEDLLPATH="$P/lib/wine/dxvk/x86_64-windows:$P/lib/wine/vkd3d-proton/x86_64-windows:$P/lib/wine/x86_64-windows"
export WINEDLLOVERRIDES="dxgi=n;d3d11=n;d3d10core=n;d3d12=n;d3d12core=n"
```

这套已经封进 [`tools/run_wine.sh`](../tools/run_wine.sh)（跑 `./tools/run_wine.sh --dll-dir build/wine-bridge` 即可）。

顺带：前缀里其实**已经有** dxvk 的 `dxgi.dll`（5.3 MB, 11-24 22:41）和 vkd3d-proton 的
`d3d12.dll`/`d3d12core.dll`，但 wine 默认优先 builtin，所以不设 override 等于没装。

---

## 2. 482 ms 那一枪：反汇编读出来的因果链

调用栈（原样，来自 `GTA6.Xbox.v1.0.2.1-CYBERLEEK/xbgtasix.log`）：

```
[xbgtasix] FATAL 异常 0x80000003 at game_scarlett_master.exe+0x5A91C21
[xbgtasix] 现场寄存器 rax=0000000089240101 rbx=000000000000000a rcx=00006ffffffabbc8 ...
[xbgtasix]   #2  game_scarlett_master.exe+0x5A91C21
[xbgtasix]   #3  game_scarlett_master.exe+0x56A1E5A
[xbgtasix]   #4  kernel32.dll+0x11739
```

`#4 = kernel32+0x11739` 是 `BaseThreadInitThunk`，`#3 = game+0x56A1E5A` 是 CRT 线程入口 ——
**是主线程**（PE 的初始线程也走 `BaseThreadInitThunk`），不是竞态。

### 2.1 `game+0x5A91C21` 只是一个 `FAILED(hr) → int3` 的陷阱

```
145a91bd0: push %rsi / sub $0x20,%rsp / mov %rcx,%rsi
145a91bd8: call 0x1455dcf74
145a91bdd: test %eax,%eax
145a91bdf: js   0x145a91c21        ← 返回值 < 0 就跳去 int3
...
145a91c21: int3                    ← 断言点（0x80000003）
145a91c22: jmp  0x145a91be1
```

`rax` 在 `int3` 时就是那个返回值：`0x89240101`（**有符号为负**）。
所以这一行日志本身不含语义，语义在 `0x1455dcf74` 里：

```
1455dcf74: xor %edx,%edx
1455dcf76: xor %ecx,%ecx
1455dcf78: jmp 0x1455e2a70        ; 等价于 Init(NULL, NULL)
```

### 2.2 `0x1455e2a70` 是标题自己的 GDK 引导：`LoadLibraryExW` + 六个 `GetProcAddress`

关键几段（`0x146e04e80` 是临界区，`0x146e04ea0` 是模块句柄，`0x146e04e*b0..d8` 是六个函数指针槽）：

```
1455e2aac: call 0x1455e29dc                   ; 没有句柄就去拿
...
1455e2ada: lea 0x11aadd7(%rip),%rdx           ; "QueryApiImpl"
1455e2ae1: call *0x146793530                  ; GetProcAddress(句柄, 名字)
1455e2aee: mov %rax,0x146e04eb0               ; 存进槽 1
1455e2aff: lea 0x11aadda(%rip),%rdx           ; "InitializeApiImpl"
1455e2b06: call *0x146793530                  ; 存进 0x146e04eb8（槽 2）
1455e2b26: ...                                ; "InitializeApiImplEx"   -> 0x146e04ec0（槽 3）
1455e2b4d: ...                                ; "UninitializeApiImpl"   -> 0x146e04ec8（槽 4）
1455e2b7e: ...                                ; "XErrorReport"          -> 0x146e04ed0（槽 5）
1455e2bb6: ...                                ; "InitializeApiImplEx2"  -> 0x146e04ed8（槽 6）
```

判定（缺一个就 `0x89240101`）：

```
1455e2be2: cmpq $0x0,0x146e04eb0   ; QueryApiImpl
1455e2bea: je   0x1455e2d53
1455e2bf0: cmpq $0x0,0x146e04eb8   ; InitializeApiImpl
1455e2bf8: jne  0x1455e2c08
1455e2bfa: test %rax,%rax          ; InitializeApiImplEx
1455e2bfd: jne  0x1455e2c08
1455e2bff: test %r10,%r10          ; InitializeApiImplEx2
1455e2c02: je   0x1455e2d53
1455e2c08: test %r8,%r8            ; XErrorReport
1455e2c0b: je   0x1455e2d53
1455e2c11: test %rdx,%rdx          ; UninitializeApiImpl
1455e2c14: je   0x1455e2d53
...
1455e2d53: mov $0x89240101,%ebx    ; ← 就是日志里 rax 的那个值
```

同一个函数里还有两个**明文字符串**，把语境钉死了：

- `0x1455e2c24` 分支用 `0x89240100` 报：`"XGameRuntimeInitialize must be called before making this call."`
- `0x1455e2c64` 分支用 `0x8924010a` 报：`"XGameRuntime is outdated and does not support these initialization options."`

也就是说：**这段就是标题对“主机 GDK 运行时够不够新”的自检**，而它认的门票是那四个内部导出。

### 2.3 加载器侧的直接证据（`WINEDEBUG=+module`）

补丁前，原样输出：

```
0173:warn:module:LdrGetProcedureAddress "InitializeApiImpl" (ordinal 0) not found in L"D:\\GTA6\\GTA6.Xbox.v1.0.2.1-CYBERLEEK\\xgameruntime.dll"
0173:warn:module:LdrGetProcedureAddress "InitializeApiImplEx" (ordinal 0) not found in L"D:\\GTA6\\GTA6.Xbox.v1.0.2.1-CYBERLEEK\\xgameruntime.dll"
0173:warn:module:LdrGetProcedureAddress "InitializeApiImplEx2" (ordinal 0) not found in L"D:\\GTA6\\GTA6.Xbox.v1.0.2.1-CYBERLEEK\\xgameruntime.dll"
0173:warn:module:LdrGetProcedureAddress "UninitializeApiImpl" (ordinal 0) not found in L"D:\\GTA6\\GTA6.Xbox.v1.0.2.1-CYBERLEEK\\xgameruntime.dll"
```

四个名字，一条不多一条不少；`QueryApiImpl` 与 `XErrorReport` **没有** warn（我们的 shim 有这两个）。
这与 2.2 的槽位顺序对得上：槽 1、5 拿到，槽 2/3/6 全空 → `0x89240101`。

**这里可以明确否掉两个替代假设**：

| 假设 | 判据 |
|---|---|
| 「Wine 的 `LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32)` 找不到 xgameruntime.dll」 | 不成立：`+module` 里 `xgameruntime.dll` 只加载了一次，来源是 **D 盘游戏目录**（静态导入），并且拿到了句柄 —— 后面才会去 `GetProcAddress` |
| 「Wine 的 `GetProcAddress` 行为不对」 | 不成立：它老老实实把 461 个导出里的两个查到了，只对真不存在的四个报 not found |

（`0x1455e29dc` 里那句 `LoadLibraryExW(L"xgameruntime.dll", 0, 0x800 或 0xA00)` 仍值得留意：
`0x800 = LOAD_LIBRARY_SEARCH_SYSTEM32`，`0xA00 = 那个 + APPLICATION_DIR`，由
`0x146e04ea8 == 2`（是否为打包应用）决定 —— 但目前这条路径**没有**被走到，日志可以证明。）

### 2.4 真机（Windows）为什么没死在这里 —— 推断，未验证

同一份 shim 在 Windows 上按同样的搜索顺序也会从游戏目录加载，所以按理该同样缺这四个名字。
两个说得通的解释，都**没有**在本机验证过（没有 Windows 环境）：

1. 当时 Windows 上那次 3400 行的跑用了**更全的导出集**（历史构建），之后被重新生成的 `.def` 缩回 461 条；
2. Windows 的 `LoadLibraryExW`/模块去重按**解析后的全路径**做，System32 里那份微软 GDK 运行时
   （`0x800` 只搜 System32）被当成另一个模块加载，补齐了这四个入口 —— 而 Wine 的
   `load_dll` 先按**基名**命中已加载模块，直接把我们这份返回了。

第 2 条能解释「Windows 过、Wine 不过」，但也意味着**补导出是两边都该做的事**（本项目本来就不该依赖
系统里装没装 GDK）。这条留给有 Windows 机器的下一次实测。

---

## 3. 修法（两层）

### 3.1 源码层（正路，本机已打通）

- `src/xgameruntime/exports.txt` 追加四个名字（手写实现清单，不属于生成桩）；
- 新增 `src/xgameruntime/api_impl.cpp`：四个 `extern "C"` 入口，返回 `S_OK`，
  并各自记一行 `[xgamert] ...` 日志（带宿主与 Wine 版本），方便下次一眼看出“引导走到哪了”。

### 3.2 应急层：给现成 DLL 补导出表（现在不需要了，留作工具）

本机（Arch）的 mingw-w64 不在 PATH 里，但在 `~/toolchain-dl/mingw/usr/bin`：

```bash
export PATH="$HOME/toolchain-dl/mingw/usr/bin:$PATH"
x86_64-w64-mingw32-g++ --version   # GCC 16.2.0，target x86_64-w64-mingw32
CXX=x86_64-w64-mingw32-g++ GDK_INC=/mnt/winbot-software/GTA6/gdk/include_clean \
    ./tools/build_linux_cross.sh    # 8 个 shim，全从源码编，导出 465
```

`build.sh` 本身在 Linux 上跑不了：它要 `dxc`（只用于 shader 测试，核心 DLL 不需要），
且里面的 `/d/mingw64/bin` 是 git-bash 的路径。
在 mingw 到手之前，为了不等环境写了 [`tools/add_exports.py`](../tools/add_exports.py)：给现成的 PE **追加**导出名，
原有 461 个一个字节都不动 ——

做法：文件末尾加一节 `.edata2`（`0x60000040`，可读可执行），里面放 4 个 4 字节函数体
（`33 C0 C3 90` = `xor eax,eax; ret`，返回 `S_OK`）、重建的导出目录 / 地址表 / 名字指针表 /
序号表（旧名字的字符串按原 RVA 复用），再补一个节头（PE 头区尾部正好有 `0x14C` 字节空位），
改 `NumberOfSections` / `SizeOfImage` / 数据目录[0]。名字表按字节序**排序**，
因为 `GetProcAddress` 对导出名做二分查找。

```bash
python3 tools/add_exports.py build/xgameruntime.dll -o build/wine-bridge/xgameruntime.dll \
    --add InitializeApiImpl --add InitializeApiImplEx --add InitializeApiImplEx2 --add UninitializeApiImpl
# 写出 build/wine-bridge/xgameruntime.dll: 导出 461 -> 465（.edata2 @RVA 0x17a000，5120 字节）
```

第一次忘了把新节标成可执行，结果 `0xC0000005 访问类型=8 地址=…17A008`（取指违例）——
这条弯路也留在工具注释里了。

### 3.3 验证结果

```bash
./tools/run_wine.sh --dll-dir build/wine-bridge --timeout 60
```

| | 补丁前 | 补丁后 |
|---|---|---|
| 结局 | `0x80000003` at `game+0x5A91C21`（482 ms） | `0xC0000005` at `game+0x55DE020`（248 ms） |
| `LdrGetProcedureAddress` 四条 warn | 有 | **无** |
| `UNIMPLEMENTED` | 0 条 | 1 条（`QueryApiImpl`） |

`#5 game+0x5A91CAF`、`#6 game+0x5A91BE6` 出现在新栈里 —— 正是 2.1 那段 `int3` 之后继续往下走的地址。
**那一枪确实过去了。**

---

## 4. 现在这堵墙：`QueryApiImpl` 的对象被要"真东西"了（哑对象已上，露出第 3 堵）

```
1455de012: call 0x1455de048          ; 内部：必要时先做引导，然后 call [0x146e04eb0] = QueryApiImpl
1455de01b: mov  0x40(%rsp),%rcx      ; rcx = 出参（对象指针）
1455de020: mov  (%rcx),%rax          ; ← 崩在这里：rcx = 0
1455de023: mov  0xe8(%rax),%rax      ; vtable[0xE8/8 = 29]
1455de02a: call 0x1456a2070          ; 经 CFG 派发
1455de02f: ...
1455de037: mov  0x10(%rax),%rax      ; vtable[0x10/8 = 2]（看着像 Release）
```

而 `0x1455de048` 里调 `QueryApiImpl` 之前只做了一件事：

```
1455de069: xor  %edx,%edx
1455de06b: lea  0x1(%rdx),%ecx       ; ecx = 1
1455de06e: call 0x1455e2a70          ; 引导（rcx=1, rdx=0）
1455de077: mov  0x146e04eb0,%rax     ; QueryApiImpl
1455de07e: mov  %rbx,%r8             ; r8 = 出参
1455de084: mov  %rsi,%rcx
1455de087: call 0x1456a2070
1455de08e: cmp  $0x80040154,%eax     ; 特别认 E_NOINTERFACE
```

结论（推断）：`QueryApiImpl(<id>, <id2>, void** out)` 是个 COM 式的“取实现对象”，
标题要求的是主机 `XblApi`/API 客户端的实现对象，`vtable` 至少 30 个槽（`0xE8`）。
我们的桩返回 `NULL`（`S_OK` 但不写出参）→ 立刻空指针。

要往下走，需要：

1. 判定 `QueryApiImpl` 的入参（`rcx`/`rdx` 是什么：GUID 的两半？API 编号？）；
2. 造一个最小 COM 对象：`vtable[2]` = `Release`（返回 0），`vtable[29]` = 实际工作入口（先返回 `S_OK`）；
3. 用“第一次调用记录 + 返回哑对象”的方式跑一轮，看标题接着要什么。

### 4.1 入参是**两个 GUID 指针**，不是整数

哑对象实现（`src/xgameruntime/api_impl.cpp`）把每次调用都记了下来，实测四种：

```
[xgamert] QueryApiImpl(id=0000000145F41040 ver=0000000145F41040)
[xgamert] QueryApiImpl(id=0000000145F41170 ver=0000000145F41180)
[xgamert] QueryApiImpl(id=0000000145F41110 ver=0000000145F41110)
```

三个值都落在 `.rdata` 的 GUID 表里（每 16 字节一个），按小端读出来是：

| 地址 | GUID |
|---|---|
| `0x145F41040` | `073B7DCB-1FCF-4030-94BE-E3C9EB623428` |
| `0x145F41110` | `01ACD177-F199-4763-A38E-CCBB55CE32E0` |
| `0x145F41170` | `37E56907-2F10-41E8-B72F-36EDB185331A` |
| `0x145F41180` | `BF2346B2-39AF-4658-B5EA-44713C7E83B3` |

**修正第 4 节的旧推断**：`QueryApiImpl` 不是“按 API 编号取对象”，是
`QueryApiImpl(const GUID* id, const GUID* iid, void** out)` —— COM 式的“按 IID 要实现”。

### 4.2 哑对象跑出来的槽位地图

实现：静态 256 字节对象（只给 8 字节会被标题写坏后面的全局量），vtable 40 槽，
每槽一个独立函数体（只有这样才能从里面知道自己是谁），槽 2 = `Release` 返回 0，其余槽
**首次**被调用时记一行再返回 `S_OK`（每次都记会被刷屏）。

实测被调到的槽（`vtable[N]` 的 off = `N*8`）：

| 槽 | off | 入参（`this` 之后） | 备注 |
|---|---|---|---|
| 29 | `0xE8` | `a=0x370720` | 第 4 节那处 `mov 0xe8(%rax),%rax` 打的就是它 |
| 2 | `0x10` | `a=0x370720` | 紧接着调用 —— 与“Release”一致 |
| 12 | `0x60` | `a=1 b=1 c=0x1471B90A0` | |
| 15 | `0x78` | `a=0x10FC90` | |
| 33 | `0x108` | `a=0 b=0x149AB3020 c=0x142211C80` | |

### 4.3 第 3 堵墙：`game+0x5925915` —— 其实是 **Wine 的 GameInput**，已用一个环境变量越过

```
[xbgtasix] FATAL 异常 0x80000003 at game_scarlett_master.exe+0x5925915
#5  game+0x81B253    #6  game+0x53EC04F   #7  game+0x53EBF3A
#8  game+0x661F0     #9  game+0x1D109C9   #10 game+0x5A91C0D   ← 还在那条 GDK 引导里
#11 game+0x56A1E5A   #12 kernel32+0x11739  #13 ntdll+0x10E2F
```

`0x5925915` 又是一个 `FAILED(hr) → int3` 陷阱；它所在的那段在做一个 **单例 + 引用计数**的动作：

```
1459258e2: mov (%rsi),%rax
1459258e5: mov %rsi,%rcx
1459258e8: call *0x8(%rax)          ; vtable[1]，像 AddRef
1459258eb: mov 0x149ab15e8,%rcx     ; 全局单例
1459258f2: test %rcx,%rcx
1459258f5: jne 0x145925918
1459258f7: mov %rsi,0x149ab15e8     ; 存进去
145925909: call *0x10(%rax)         ; vtable[2]，像 Release
```

也就是说：标题拿到我们那个哑对象之后，把它当**带引用计数的主机接口**在用，
并且要把某个“结果”存进全局单例 `0x149AB15E8`；某一步的返回值不合格就 int3。


### 4.4 这个 int3 的调用链：`GameInput.dll` 序号 1

把 `call 0x145F3FED0` 追到 IAT，是 **`GameInput.dll` 的序号 1**（`GameInputCreate`）；
崩溃时 `rax = 0x80004001`（**E_NOTIMPL**）。再打开 Wine 自己的 `gameinput.dll`：

```
1800014f9: lea  0x54b0(%rip),%rcx      # "SteamGameId"
180001500: call *%rbx                  ; getenv
180001505: test %rax,%rax
180001508: je   0x180001700
...
180001709: mov  $0x80004001,%esi       ; E_NOTIMPL —— getenv 为空时就是这条
```

也就是说：**Wine 的 GameInput 只在设了 `SteamGameId` 时才肯干活**，否则 `GameInputCreate`
直接 `E_NOTIMPL`；标题在 GDK 引导尾段必须建一个 `IGameInput` 单例（全局 `0x149AB15E8`），
失败就 int3。这是一个典型的**宿主差异**（真 Windows 上装了 GameInput Redistributable 就正常）。

`tools/run_wine.sh` 已经默认带上它：

```bash
export SteamGameId=${SteamGameId:-1771300}
```

实测效果：`game+0x5925915` 这条断言**消失**，流程再往前走到

### 4.5 第 4 堵墙：`game+0x2237178`（布尔断言）

```
142237167: call 0x142213030
14223716c: mov  0xc(%rsi),%ecx
14223716f: call 0x140006170        ; 返回 bool
142237174: test %al,%al
142237176: jne  0x2237179
142237178: int3                    ; ← 第 4 堵墙
```

`0x140006170` 拿 `[rsi+0xC]` 当参数，返回假 → int3。不是失败码陷阱，是**布尔断言**，
得先弄清 `0x140006170` 判的是什么（`rsi` 指向的对象、`+0xC` 那个字段）。
这一层的输入还是我们给的哑对象（这轮日志里槽 3、4 也被调了：`vtable[3] a=0xD2F8E4`、
`vtable[4] a=0x10 b=0xD2F900 c=0xD2FA00`）。

**下一步要的**：把槽 29/12/15/33/3/4 的**入参语义**定下来（它们显然在传不同的 IID/结构），
让至少一个槽返回“真东西”而不是 `S_OK`。这一点需要读 `0x5925800..0x5925915`、
`0x140006170` 和几个调用者，本轮到此为止。

---

## 5. 多系统支持：现在有什么

### 5.1 “当前平台”的概念

[`include/xbgtasix/platform.h`](../include/xbgtasix/platform.h)（头文件内联实现，exe 与 8 个 shim
都能直接用，不用改各自的目标文件列表）：

| 接口 | 作用 |
|---|---|
| `xbgtasix::host_platform()` | `Windows` / `Wine`。取值优先级：`XBGTASIX_PLATFORM=windows\|wine` → 探测 `ntdll!wine_get_version` |
| `xbgtasix::host_is_wine()` | 布尔快捷 |
| `xbgtasix::host_platform_name()` | `"windows"` / `"wine"`，写日志用 |
| `xbgtasix::host_wine_version()` | `wine_get_version()` 的字符串（真 Windows 上为空） |
| `xbgtasix::host_probe_gpus()` | Wine 下读 `Z:\sys\class\drm\cardN\device\{vendor,device,subsystem_*,boot_vga}`，拿到**真实**硬件；真 Windows 返回空 |

为什么用 `wine_get_version`：这是 Wine 自己的规范标识（`ntdll` 导出），比查注册表/文件路径稳。
为什么读 `Z:`：Wine 把 `Z:` 映射到 `/`，于是不用任何 Linux 专属 API（也就不会在 Windows 上编不过）。

已接入的地方：

- `xbgtasix doctor`：新增 `宿主` 一行 +（Wine 下）`真实 GPU` 若干行，并提示
  “DXGI 报的适配器可能是 vkd3d 编的，跟下面 DXGI 的对比着看”；
- `xbgtasix run` 的运行横幅：多一行 `宿主`，日志事后可追溯。

> **这两个 C++ 改动没有编译过**（本机没有 mingw-w64，`sudo` 需要密码，装不了）。
> 同一份信息在 Linux 侧已经用 [`tools/host_probe.py`](../tools/host_probe.py) 跑出来了（见 5.2），
> 所以逻辑本身是验证过的，只有 C++ 那一份没进过编译器。

### 5.2 `tools/host_probe.py`（本机已验证）

```bash
$ python3 tools/host_probe.py --doctor
宿主          linux（Wine 之外）
wine          wine-11.0 (CachyOS)
WINEPREFIX    /home/link/Games/gta6-x11
真实 GPU      card0: NVIDIA 10DE:2D59 (sub 1043:3B49)  GB206M [GeForce RTX 5060 Max-Q / Mobile]
真实 GPU      card1: Intel 8086:7D67 (sub 1043:3B49)  [boot_vga]  Arrow Lake-S [Intel Graphics]
--- wine 下 doctor 报的（DXGI/vkd3d 视角，可能是假卡）
  dxgi.dll      有
  GPU[0]        NVIDIA GeForce RTX 5060 Laptop GPU（AMD 1002:73DF，显存 8.0 GB，1 个输出）
  GPU[1]        Intel(R) Graphics (ARL)（Intel 8086:7D67，显存 0.5 GB，0 个输出）
  适配器        物理显卡 2 张（2 个硬件适配器）
info:  DXGI: Hiding actual GPU, reporting:
```

三个可用的观察：

1. 真实硬件是 **10DE:2D59（RTX 5060 Max-Q）+ 8086:7D67（ARL 核显，boot_vga）**；
2. 只要给了 dxvk / vkd3d-proton（`tools/run_wine.sh` 那套），**doctor 报的就是真卡**，
   `8086:7D67` 与 sysfs 完全对上；
3. 但 `GPU[0]` 的厂商号被报成 **`1002`（AMD）**，还打了 `DXGI: Hiding actual GPU, reporting:`
   —— 说明前缀里有东西在**改适配器描述**（dxvk-nvapi 或 vkd3d-proton 的 spoofing），
   所以“不要信 DXGI 报的型号”这条依然成立，只是假的程度随环境不同。
   之前看到的 `Intel(R) HD Graphics 4000（8086:0162，1.5 GB）` 属于 builtin wined3d 那条路径的
   占位适配器；这一轮用 `WINEDLLPATH`+`WINEDLLOVERRIDES` 都复现不出来，**来源没能确认**，
   记为未解。

### 5.3 跑法（Linux）

```bash
./tools/run_wine.sh                       # doctor 之外最常用：run 整个包
./tools/run_wine.sh --dll-dir build/wine-bridge --timeout 180
python3 tools/host_probe.py --doctor       # 先看宿主与真实 GPU
```

`run_wine.sh` 干的事：补 dxvk / vkd3d-proton 的搜索路径与 override、把包路径转成 `Z:\...`、
带上 `XBGTASIX_VEH=1`（异常钩子，否则 `int3` 只表现成退出码 0x80000003、日志里什么都没有）。

---

## 6. 还没做的

1. `QueryApiImpl` 对象的槽语义（第 4 节）——当前这堵墙：哑对象能过 10 次调用，
   然后游戏在 `game+0x5925915` 拒绝我们给的返回值；
2. `d3d12_xs` / `xmem` 里真正需要“按宿主分叉”的点还没挑出来（本轮只把地基和 doctor 接上）。
   已知候选：`XMemVirtualAlloc` 的大块预留（Wine 的地址空间布局与 Windows 不同，24 GB×2 都成功了，
   但**保留区语义**是否一致没有验证）、`PresentX` 家族在 vkd3d-proton 下的槽位行为；
3. 假适配器的来源（5.2 第 3 条）；
4. Windows 上这四个导出到底怎么过去的（2.4）——需要一台真机复跑 `WINEDEBUG` 等价物。

---

## 7. 槽位地图（真机复跑时直接对表）

哑对象（`src/xgameruntime/api_impl.cpp`）的 vtable 40 槽，实测被调到的槽与**首次**调用参数
（`this` 之后那三个；abi：rcx=this，rdx/r8/r9=参数）：

| 槽 | off | 首次参数 | 出现时机 |
|---|---|---|---|
| 29 | `0xE8` | `a=0x370750` | `0x55DE020` 那处 `mov 0xe8(%rax),%rax` 打的就是它 |
| 2 | `0x10` | `a=0x370750` | 紧跟 29 之后，形状与 `Release` 一致 |
| 12 | `0x60` | `a=1 b=1 c=0x1471B90A0` | `c` 是游戏全局（vtable 那类结构） |
| 15 | `0x78` | `a=0x10FC90` | `a` 看着像对象偏移/大小 |
| 33 | `0x108` | `a=0 b=0x149AB3020 c=0x142211C80` | `b` = **GameInput 单例**（vtable `0x146187BB0`）；`c` 与 `0x592581A` 的 `lea` 精确吻合 |
| 3 | `0x18` | `a=0xD2F8E4` | 过了 GameInput 之后才出现 |
| 4 | `0x20` | `a=0x10 b=0xD2F900 c=0xD2FA00` | 紧接槽 3 |
| 0 | `0x0` | — | 未观测到（标题不从槽 0 打） |
| 1 | `0x8` | — | 未观测到（游戏自己对象的 `AddRef` 在 `*0x8(%rax)`，但我们的对象没被这样用） |

入参语义（`QueryApiImpl`）：**两个 GUID 指针**，不是编号。见 4.1 的四种组合。

> **风险（已知未收敛）**：所有槽现在一律返回 `S_OK`，属于"假成功"。它能把未知暴露出来，
> 但也会把"其实该失败"的地方掩盖成继续跑。等某堵墙卡住不动，再来给已知槽返回有意义的失败码。

## 8. 第 4 堵墙的判据（读出来了，但对象不是我们的）

`game+0x2237178` 的判据其实只有三条指令 —— `0x140006170` 是：

```
140006170: 85 c9        test %ecx,%ecx
140006172: 0f 95 c0     setne %al
140006175: c3           ret
```

**`return (int)ecx != 0;`** —— 一个裸的"非零"谓词。调用点：

```
14223716c: 8b 4e 0c     mov 0xc(%rsi),%ecx
14223716f: e8 fc ef dc fd call 0x140006170
142237174: 84 c0        test %al,%al
142237176: 75 01        jne  ok
142237178: cc           int3
```

所以第 4 堵墙 = **"`[对象+0xC]` 必须是非零"**。

### 8.1 更正一行前提：这个对象**不是**我们给的哑对象

- 崩溃时 `rsi=0x2339B00`，落在 `allocBase=0xE00000 size=0x47EC7000`（那个 1.14 GB 的 xmem 大块）
  里 —— 见日志的"断言对象所在区域"行；我们的哑对象在 `xgameruntime.dll` 的 `.data`。
- 这个对象是 `operator new(0x28)` + 构造函数 `0x14582A610` 造出来的：

```
142236d85: mov $0x28,%ecx
142236d8f: call 0x144d9c160        ; new(0x28)
142236d9a: call 0x14582a610        ; 构造函数：vmovups ymm0,(rcx) + [rcx+0x20]=0 —— 整块清零
142236da2: mov %rsi,0x147453E58    ; 存全局
142236da9: call 0x142236E00        ; ← 崩的那个函数
```

  也就是说 **`+0xC` 生下来就是 0**，指望后面有人填。

### 8.2 `0x2236E00` 是什么形状

（函数边界由 `.pdata` 给出：`0x142236E00..0x1422371BD`，长度 `0x3BD`。）

- 全函数对 `(rsi)` 只有读（`0x2236F3B` 的 `cmpq $0x0,(%rsi)`、`0x2236F80` 的 `mov (%rsi),%rdi`），
  **没有任何 store** → `+0xC` 不是它填的；
- 中段反复 `lea rdx,<字符串表>` + `call 0x1422371F0(rcx=对象, rdx=串, r8=rsp)`，
  很像**诊断/报告构造器**；
- `0x1422372C0(对象)` 里也是先 `0x140FC21C0(&iface, <串>, 0)` → `call *0x8(%rax)` →
  `call *0x3b8(%rax)`（槽 119！）——在从一个接口里取东西；
- 最后回到 `mov 0xc(%rsi),%ecx` 断言。

### 8.3 读法（推断，未验证）

两个可能，倾向第二个：

1. **我们该填 `+0xC`**：那得知道它是什么字段。但 8.1/8.2 表明这个对象是标题自己 new 出来、
   自己清零、自己不填的 —— 除非"填"发生在 `0x1422371F0` 里（还没读）。
2. **这是错误路径的症状**：引导更早的地方已经失败，标题进来构造一段诊断报告，
   而报告构造器要求 `+0xC`（像错误码/句柄/字符串 id）非零。真凶在被我们 `S_OK` 哑对象
   掩盖的那一段。

### 8.4 这个对象其实是个 **vector**，断言等于"列表不能为空"

读了 `0x1422371F0(obj, key, value)`（它在 `0x2236E00` 里被反复调用，参数是对象 + 字符串 + 值）：

```
1422371f0: ...   mov $0x118,%ecx / call new / 构造 0x118 大小的条目
142237227: call 0x1428cb210        ; 给条目设 key（字符串）
142237232: call 0x141fb2210        ; 给条目设 value
142237237: mov  0xc(%rsi),%eax     ; count
14223723a: cmp  0x8(%rsi),%eax     ; 与 capacity 比
14223723d: jne  0x14223729c        ; 满了就扩容
142237260: call 0x144d9c140        ; 重新分配
142237281: mov  0x10(%rsi),%rcx    ; data 指针
```

所以对象布局是 **`+0x8` = capacity，`+0xC` = count，`+0x10` = data**，`0x1422371F0` 就是
`push_back(条目)`。**`[obj+0xC] != 0` 的真实语义 = "这个列表不能是空的"。**

### 8.5 为什么是空的：一个 `je` 把所有 `push_back` 全跳过了

```
142236e58: call 0x140643cd0            ; 取线程本地上下文（%gs:0x58 -> [+0x7F0]）
142236e5d: lea  0x1474539F6,%rdx       ; 参数 2（字符串/标识）
142236e64: lea  0x14678D1BB,%r8        ; 参数 3
142236e6b: mov  %rax,%rcx
142236e6e: call 0x14056eb10
142236e73: test %al,%al
142236e75: je   0x14223716c            ; ← 返回 false 就直接去断言，push_back 一个都不做
```

`0x14056EB10` 尾巴跳进 `0x14056EB30`，它干的第一件事是：

```
14056eb3f: cmpl $0x0,0xc04(%rcx)       ; 上下文里的一个计数
14056eb46: jle  0x14056ebf0
14056ebf0: xor  %eax,%eax              ; return false
```

**结论**：第 4 堵墙是**症状**，真因是这个计数（`ctx+0xC04`）是 0 ——
一个"分发/上报/注册表"里一条都没有，于是"取条目"直接失败、列表留空、断言炸。

### 8.6 这正好是"假成功"的账

`InitializeApiImpl*` 现在返回 `S_OK` 但**什么都没注册**；`QueryApiImpl` 也只是发个哑对象。
`ctx+0xC04` 是 0，与我们这些"应答了但没做事"的实现高度吻合（**推断，未验证**）。

验证办法（下一轮）：项目里 `trace.cpp` 已经有"全局写断点"机制（日志里那条
`★ g 写断点命中！`），把它挪来盯 `ctx+0xC04` 的写入，就能看到**到底谁该把它加上去**，
而不是继续猜。
