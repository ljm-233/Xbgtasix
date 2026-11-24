# 导入解析阶梯 + census（模式 C）

> 设计来源：`D:\GTA6\handoff\victory-reusable-designs.md` 第 3 节（模式 C）与第 5 节（验收标准）。
> 本文自包含：不读原项目也能照着实现/复核。
>
> 交付物：
> - [tools/census.py](../tools/census.py) —— 只读的符号归属分类器（纯标准库、纯静态）
> - [tests/t45_census/main.cpp](../tests/t45_census/main.cpp) —— 回归测试（含 3.3 的"当前目录同名 DLL 不被采纳"）
> - 本文 —— 阶梯定义、HostPolicy 白名单与理由、census 用法、实测结果

---

## 1. 问题：68 个模块、587 个符号，每一个该由谁提供

目标 exe（`game_scarlett_master.exe`，114.6 MiB，PE32+）导入 **68 个模块 / 587 个符号**。
其中 7 个模块是我们自己实现的（本项目的 shim），其余 61 个是系统或主机/标题自带的。

接管一个第三方二进制时，最难的不是实现某个 API，而是**决定每个 `dll!function` 的归属**：

| 错误做法 | 后果 |
|---|---|
| 全部自己实现 | 工作量爆炸（光 CRT 数学函数就是 76 个符号），还得跟着微软改 |
| 全部透传 | 语义静默出错：主机与 PC 在内存属性、线程/时间、包与用户身份上根本不是一回事 |
| 按目录里有什么就加载什么 | 第三方程序自带同名 DLL 就拿到代码执行（见第 4 节） |

现有的 `xbgtasix check <exe>`（[src/host/main.cpp](../src/host/main.cpp) 的 `cmd_check`）只回答
**"缺哪些符号"**，不回答 "**每个符号该由谁提供**"。这就是 census 的位置。

---

## 2. 阶梯（顺序即语义）

对每一个 `dll!symbol`，按顺序判定，**第一个负责的那一级胜出**：

| 顺序 | Provenance | 规则 | 判定输入 |
|---|---|---|---|
| 1 | `Hle` | 这个 dll 是**我们自己实现的**（第 2.2 节的 8 个名字），**且**该符号在我们的导出清单里 | `build/*.dll` 的 PE 导出表，或源码清单 |
| 2 | `Host` | `HostPolicy.allows(dll)` 通过 | [tools/census.py](../tools/census.py) 的 `POLICY_RULES` |
| 3 | `Title` | 目标二进制**在 exe 同目录自带了同名 DLL** | exe 目录的文件名（不加载、不解析） |
| 4 | `Stub` | 以上都不负责 | —— 掉进生成的 stub |

为什么是这个顺序：

1. **我们永远优先**。语义与宿主不同的（内存、文件、线程、时间、包/用户身份）**必须**在这一级覆盖，
   否则第 2 级会把主机语义当成 PC 语义用。
2. **宿主只放行"两边语义一致"的**。白名单默认拒绝，每加一条都要写理由（第 3 节）。
   API Set、CRT、crypto、socket、HTTP 这类"不管在哪个 Windows 上都是一套契约"的东西才配进表。
3. **标题自带的 DLL 排在我们的和宿主的之后**。这一级的现实用途是 `Bink2Scarlett.dll`、
   `libHttpClient.GDK.dll`、`XboxBvhBuilder.dll` 这类**只有标题才有的第三方库**：
   它们在 exe 目录里，系统里没有，我们也不打算实现。
   顺序放这么后，是为了**防止第三方库顶掉我们**：标题自带的 `AcpHal.dll`、`d3d12_xs.dll`
   就在同一个目录里（见 6.5 的第 3 条），绝不能让它抢走我们的模块。
4. **stub 是"故意不给"的显式表达**。调用时打印 `dll!name`，行为可切换（`stop` / `log`）——
   比"猜一个返回值让它继续跑"安全得多。

### 2.1 两条补充规则（census 与文档、测试必须一致）

**a) 注册表否决权（registry veto）：我们的模块永远不从 `Host` 拿。**
第 2 级只对"不归我们"的模块生效。理由很实在：PC 上可能**真的**存在同名 GDK DLL
（例如 GDK for PC 的 `xgameruntime.dll`），但那份的语义是"PC 上的 Xbox 服务"，不是主机语义；
一旦让它接管，我们等于把第 1 级白写了。所以 `dll ∈ OUR_DLLS` 时，`HostPolicy` 连问都不问。

**b) `gap`（缺口）是"我们的模块缺这个符号"，不是"别人提供"。**
第 1 级要**两个条件同时成立**（是我们的模块 + 我们导出了这个符号）。只满足前一半时，
census 会在 `GAP` 列打标，并按阶梯继续往 2/3/4 级走 —— 因为**加载器实际会那样走**。
但修法只有一个：**补我们的导出**，不是换个来源。`--only missing` 只列这些行。

> 这条很关键：census 描述的是"**加载器会怎么解析**"，不是"我们希望它怎么解析"。
> 两者不一致的地方，就是 P0 缺口。

### 2.2 我们自己的 8 个模块（`Hle` 的唯一名单）

名单**写死在** [tools/census.py](../tools/census.py) 的 `OUR_DLLS` 里，不从目录里猜：
目录里多一个 DLL 不等于我们负责它；少一个也不等于它不归我们（那要报"导出清单缺失"）。

| 模块 | 职责 | 当前导出数 |
|---|---|---|
| `xgameruntime.dll` | GDK 运行时主 API 面：task queue / async / user / storage / package | 461 |
| `xmem.dll` | 主机内存：物理页、虚拟地址映射、工作集统计 | 15 |
| `d3d12_xs.dll` | Xbox D3D12 变体（device ABI / 描述符堆）；转发到 PC d3d12 的部分见 `forward.def` | 20 |
| `AcpHal.dll` | 音频/计算 HAL：APU 堆、shape context、APU 虚拟地址映射 | 9 |
| `xapu.dll` | APU 命令通道 | 5 |
| `chad.dll` | DSP/CHAD 客户端（`XDsp*`） | 5 |
| `xg_xs.dll` | 图形辅助：swizzle 建议、texture computer | 2 |
| `XCRDAPI.dll` | DirectStorage 的 Xbox 侧导出面 | 1 |

### 2.3 遮蔽（shadowed）：`Host` 与 `Title` 同名时的现实后果

本次实测发现：标题**在 exe 同目录自带了** `msvcp140.dll` / `vcruntime140.dll` / `vcruntime140_1.dll`
（Xbox 版 CRT），而这三者又在 HostPolicy 白名单里 → 按阶梯是 `Host` 赢。

这正是第 4 节安全细节的用武之地：**`Host` 透传一律只从 `System32` 加载**，
所以 exe 目录里那三份 Xbox CRT **不会被采纳**，拿到的是 PC 的 VC 运行时（同一 ABI）。
census 把这种情况标成 `shadowed`（本次 137 个符号 / 3 个模块），并在 `note` 里写明，
免得有人看到"目录里明明有 msvcp140.dll"就以为它是被加载的那份。

### 2.4 只分类、不绑定、不运行目标

`classify(dll, symbol) -> Provenance` 是**纯静态**的：只读目标 PE 的导入表、shim 的导出表、
以及"exe 同目录有没有这个文件名"。它**不加载、不绑定、不运行目标**。

好处有三个：

1. 拿一个 exe 在任何机器（包括 Linux）上都能先做分诊；
2. 崩溃排查时可以直接回答"这个环境里它为什么会缺那个符号"；
3. 它是**移植工作量的度量**（第 6.4 节的 `--no-host` 缺口清单）。

同一套词汇（`Hle` / `Host` / `Title` / `Stub`）应该被日志、census、崩溃报告共用 ——
这样看到一行日志就知道是"我们实现的"、"宿主透传的"，还是"掉进 stub 了"。

---

## 3. HostPolicy：默认拒绝的白名单

### 3.1 语义（三条硬性约束）

```python
class HostPolicy:
    def allows(self, dll):
        low = dll.lower()
        for pat, why, risk in self.rules:
            if pat.endswith("*"):          # 通配**只支持尾部** '*'
                if low.startswith(pat[:-1]): return True
            elif low == pat:               # 没通配就是整体相等（不是前缀！）
                return True
        return False                       # 默认拒绝
```

1. **白名单 + 默认拒绝**：不在表里的模块一律不透传（会落到 `Title` 或 `Stub`）。
2. **通配只支持尾部 `*`**：`api-ms-win-*` 合法；`api-*-core` 这种中缀通配**不解释**，
   按字面量比（写错了就匹配不上，而不是悄悄扩大范围）。这是刻意的：白名单的每一格都该是
   人读得懂的，不要发明正则。
3. **每条都有理由**：`--policy` 会把 `pattern` + `risk` + 理由整张表打印出来。
   **没有理由的条目不许进表**。

`risk` 是给读者的提示，不是给程序的开关：

| risk | 含义 |
|---|---|
| `none` | 两边语义/二进制契约一致，透传是"显然正确"的选择 |
| `low` | 函数契约一致，但**环境**差异（编解码器可用集、证书链、代理、驱动）可能让行为不同 |

`medium` / `high` 的条目**不该出现在白名单里** —— 那种应该由 `Hle` 实现或 `Stub` 明确拒绝。

### 3.2 白名单全表（20 条，每条带理由）

`python tools/census.py --policy` 的原文见 6.6；这里是同一张表（**改任何一条要三处同步**：
`census.py:POLICY_RULES`、`tests/t45_census/main.cpp:kPolicy`、本节）。

| # | pattern | risk | 为什么允许透传 |
|---|---|---|---|
| 1 | `api-ms-win-*` | none | **API Set 契约**：名字本身就是"转发到系统实现"的稳定别名（Win10 起由 apisetschema 解析）。主机与 PC 在 file/thread/synch/heap 这些原语上都直接落到各自的 `Nt*`，函数契约、参数、返回码一致；这类导入没有"主机专有语义"，自己实现只会引入偏差 |
| 2 | `ext-ms-win-*` | none | **ext API Set**（rtcore/ntuser 的窗口消息面）：同样是"转发别名"，指向 user32/ntdll。主机上的窗口消息语义与 PC 一致（`GWLP_*`/`WM_*` 数值共用） |
| 3 | `kernel32.dll` | none | 基础 Win32 面。真正"主机语义不同"的部分（内存属性、线程调度、时间源）在 `xmem`/`xgameruntime` 里由我们覆盖，**轮不到** kernel32；剩下的是 `CreateFileW`/`HeapAlloc` 这类契约 |
| 4 | `kernelbase.dll` | none | kernel32 的实现层，契约相同；新版工具链会直接导入它（省一次转发） |
| 5 | `ucrtbase.dll` | none | Universal CRT：微软承诺的**二进制稳定 ABI**（VC redist 就是它）。`printf`/`malloc`/`locale` 的行为由 CRT 版本决定，主机与 PC 是同一套 UCRT 源码 |
| 6 | `msvcp140.dll` | low | MSVC C++ 标准库：v140 ABI 跨小版本稳定是官方承诺。**标题自带的 Xbox 版 msvcp140.dll 反而依赖主机 CRT 内部**，PC 上加载不了 —— 所以这里必须透传宿主那份（且只从 System32，见第 4 节） |
| 7 | `msvcp140_1.dll` | none | msvcp140 的拆分件（C++17 新符号），同一 ABI 承诺 |
| 8 | `msvcp140_2.dll` | none | 拆分件（并行/特殊数学），同上 |
| 9 | `msvcp140_atomic_wait.dll` | none | 拆分件（atomic wait/notify），同上 |
| 10 | `msvcp140_codecvt_ids.dll` | none | 拆分件（codecvt id 常量），同上 |
| 11 | `vcruntime140.dll` | none | MSVC C 运行时（异常展开、CRT 初始化）：同一份 v140 契约；主机版的 SEH 展开表与 PC 版布局一致，透传不需要翻译 |
| 12 | `vcruntime140_1.dll` | none | vcruntime140 的拆分件（x64 C++ EH 展开），同上 |
| 13 | `d3d12.dll` | low | PC 的 D3D12。主机侧标题导入的是 `d3d12_xs.dll`（**那份归我们，Hle 优先**），而直接导入 `d3d12.dll` 的符号是跨平台共有面；device/command list 的 ABI 已由 [tests/t41_d3d12_device_abi](../tests/t41_d3d12_device_abi/main.cpp) 验过一致 |
| 14 | `dxgi.dll` | low | 适配器枚举、交换链、output 描述：PC 与主机共用同一套 DXGI 契约。主机专有的 present 路径走 `d3d12_xs` 的 `PresentX`，在这里不会被调用到 |
| 15 | `bcrypt.dll` | none | BCrypt 加密原语（哈希/AES/RNG）：算法与参数契约由 CNG 定义，两边实现同源；主机上多出的策略（密钥隔离）不在标题会调的面里 |
| 16 | `advapi32.dll` | low | 注册表/令牌/服务控制：PC 与主机都是同一套 Win32 注册表语义（主机是只读子集，**读取路径一致**） |
| 17 | `ws2_32.dll` | low | Winsock 2：TCP/UDP socket 契约与 BSD 兼容层一致；主机与 PC 走同一个内核网络栈，差异只在可达地址（主机只连 Xbox 服务），不是 API 语义 |
| 18 | `winhttp.dll` | low | WinHTTP 客户端：HTTP/1.1、TLS、代理配置的 API 契约两边一致；主机版同样有 WinHTTP。差异在证书链与代理策略（**环境问题，不是函数语义**） |
| 19 | `mfplat.dll` | low | Media Foundation 平台层：Xbox 系统同样自带 MFPlat（GDK 的视频播放就走它），attribute / 媒体类型 / 拓扑的 API 契约一致 |
| 20 | `mfreadwrite.dll` | low | MF 的读写层（SourceReader / SinkWriter）：同上；差异只在可用**编解码器集合** |

> 表是**最小集合**：只放"这次真需要 + 明显该透传"的。`d3d11.dll`、`user32.dll`、`ole32.dll`
> 这次没有导入，就不放 —— 真要加的时候连理由一起加。

### 3.3 明确**不**放进白名单的（附理由）

| 模块 | 为什么拒绝 | 结果 |
|---|---|---|
| `xgameruntime.dll` / `xmem.dll` / `d3d12_xs.dll` / `AcpHal.dll` / `xapu.dll` / `chad.dll` / `xg_xs.dll` / `XCRDAPI.dll` | 归我们。PC 上可能有同名 GDK DLL，但语义是 PC 版服务；注册表否决权连问都不问 | `Hle`（或 `GAP` → 后两级） |
| `GameInput.dll` | GDK 的输入汇流面。PC 上虽有 GameInput v1，但**设备枚举/力反馈/玩家绑定**的语义与主机不同，接错会让输入静默失效 | `Stub`（本次实测唯一一个，且还是按序号导入的 `#1`） |
| `Bink2Scarlett.dll` | 标题自带的 Bink2 主机版解码器，系统里没有、我们也不打算实现 | `Title`（23 符号） |
| `libHttpClient.GDK.dll` | 标题自带的 GDK HTTP 客户端静态库产物，PC 上没有同名系统库 | `Title`（38 符号） |
| `XboxBvhBuilder.dll` | 标题自带的 BVH 构建器（主机 XDXR），实现它=重写一个 BVH 库 | `Title`（7 符号） |
| `amd_fidelityfx_loader_xbox.dll` | FidelityFX 的 Xbox 侧 loader，随标题分发 | `Title`（4 符号） |
| `dstorage.dll` / `dstoragecore.dll` | DirectStorage 的 PC 侧实现在系统里，但**主机侧导出面**是 `XCRDAPI.dll`（归我们）。同名遮蔽在这里会变成语义错位 | 不透明透传（真需要时单独写理由） |

### 3.4 加一条白名单的纪律

1. 在 `census.py:POLICY_RULES` 加 `(pattern, reason, risk)`；
2. 在 `tests/t45_census/main.cpp:kPolicy` 加同名 pattern（**测试会逐条比对两边**，少了就红）；
3. 在本节表格加一行（理由写清楚"为什么两边语义一致"）；
4. 跑 `python tools/census.py <exe> --policy` 和 `build/t45_census.exe` 确认没漂移。

**例外表条目数就是技术债计数器**：这次是 0 条例外、20 条允许。

### 3.5 `HostPolicy::none()` 不是摆设

`--no-host` 把白名单清空，一个都不透传。它回答的是另一个问题：
**"如果完全不借宿主的力，我们真正要实现的 API 面有多大？"**

本次答案（见 6.4）：**341 个符号 / 54 个模块**必须由 `Hle` 或 `stub` 负责。
这个数字才是移植工作量的下限 —— 它同时也告诉你**哪一天该停止透传**：
当某个模块的 `Hle` 覆盖率上来后，可以从白名单里把它删掉，用 `--no-host` 继续盯缺口。

---

## 4. 安全：透传宿主 DLL 只从 System32 加载

### 4.1 攻击模型

按名字透传 `LoadLibraryA("msvcp140.dll")` 时，Windows 的**默认搜索顺序**包含
可执行文件目录与**当前目录**。第三方程序（游戏、插件、老 SDK 宿主）只要在 exe 目录或
工作目录里放一个同名 DLL，我们就在**替它加载代码**。

这个项目尤其危险，因为：

- 我们**明知** exe 目录里有 `msvcp140.dll`、`vcruntime140.dll`、`AcpHal.dll`、`d3d12_xs.dll`（26 个 DLL）；
- 标题还会用自己的文件覆盖我们的 shim（`AcpHal.dll.xbgtasix-backup` 就是证据）；
- 我们跑的目标是从不可信来源拿到的（解包游戏），目录内容完全由它决定。

### 4.2 正确写法（三条）

```cpp
// 1) 只认模块名：带路径分隔符的名字直接拒绝
if (name.find_first_of(L"\\/") != std::wstring::npos) return nullptr;

// 2) 只搜 System32：exe 目录 / 当前目录 / PATH 全部不参与
HMODULE h = ::LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);

// 3) 句柄按小写名缓存（同一模块别重复 Load，FreeLibrary 也就不容易漏）
cache.emplace(lowercase(name), h);
```

第 1 条不是多余：`LoadLibraryExW` 收到**全路径**时会直接用那个路径，`SEARCH_SYSTEM32` 管不着 ——
所以"名字里不许有分隔符"这一层过滤是必须的。

`LOAD_LIBRARY_SEARCH_SYSTEM32` 需要 KB2533623（Win7 SP1+ / Win8+ 的补丁，Win10/11 自带）。

### 4.3 回归测试（刻意构造，[tests/t45_census/main.cpp](../tests/t45_census/main.cpp) 的 C 段）

```text
1. 装置：CopyFileW(C:\Windows\System32\version.dll, <tmp>\msvcp140_xbgtest.dll)
        SetCurrentDirectoryW(<tmp>)
2. 安全做法（必须先做）：LoadLibraryExW(L"msvcp140_xbgtest.dll", 0, SEARCH_SYSTEM32)
        → 断言 NULL，且 GetLastError == ERROR_MOD_NOT_FOUND(126)
        → 断言 GetModuleHandleW 里没有任何该名字的映射
3. 朴素做法：LoadLibraryW(L"msvcp140_xbgtest.dll")
        → 断言非 NULL，且 GetModuleFileNameW 指向 <tmp>（劫持成立）
        → 断言路径不在 System32 下
4. 正向对照：LoadLibraryExW(L"version.dll", 0, SEARCH_SYSTEM32)
        → 断言成功且路径在 System32 下（证明 flag 是"只搜 System32"而不是"永远失败"）
5. 封装层：HostLoader 同样拒绝 <tmp> 的同名 DLL；拒绝 "C:\...\version.dll" 与 "..\version.dll"
6. 收尾：恢复工作目录 → 删文件 → 删目录 → 断言工作目录已恢复
```

**顺序陷阱（写测试时最容易踩）**：必须先做第 2 步、再做第 3 步。
`LoadLibrary` 会先在"已加载模块表"里按**基名**查 —— 如果先做了朴素加载（劫持成功），
后面带 `SEARCH_SYSTEM32` 的调用会直接返回那个**被劫持的句柄**，测试就变成自证失败。

实测输出（6.6.2）里两条断言的细节值：安全做法 `GetLastError=126`、
朴素做法拿到的路径是 `...\build\xbgtasix_t45_<pid>\msvcp140_xbgtest.dll`、
正向对照是 `C:\Windows\SYSTEM32\version.dll`。

---

## 5. census 用法

### 5.1 命令

```bash
python tools/census.py <exe>                      # 完整报告（模块分布 + 明细前 40 行）
python tools/census.py <exe> --csv out.csv        # 明细落盘（utf-8-sig，Excel 直接开）
python tools/census.py <exe> --only stub          # 只看掉进 stub 的
python tools/census.py <exe> --only missing       # 只看"我们的模块缺符号"（P0 缺口）
python tools/census.py <exe> --only shadow        # 只看 Host/Title 同名遮蔽
python tools/census.py <exe> --only hle|host|title
python tools/census.py <exe> --no-host            # HostPolicy::none()：缺口清单
python tools/census.py <exe> --no-title-ours      # 我们的模块缺符号时不映射到 Title（规划视角）
python tools/census.py <exe> --policy             # 打印白名单全表（含理由）
python tools/census.py <exe> --limit 0            # 明细全打（默认 40 行）
python tools/census.py <exe> --no-modules         # 不打印 68 行模块表
python tools/census.py <exe> --shim-dir <dir>     # 我们自己 DLL 的位置（可多次）
python tools/census.py --self-test                # 纯逻辑自测（不需要 exe；23 项）
```

退出码：`0` = 报告成功；`2` = 用法/解析错误。
**census 不对缺口判 FAIL** —— 它是只读度量；要在 CI 里卡缺口，用 `--only missing` + `grep`。

### 5.2 输出怎么读

```text
== census of <exe>                     ← 目标、exe 目录、生效的 policy、目录里有多少 DLL
-- 我们的导出清单来源                    ← 8 个模块各自的导出数 + 来源（PE 导出表 / 源码清单）
-- 归属分布                            ← 模块级 4 类计数 + 符号级 4 类计数 + 缺口 + 遮蔽
-- 模块明细                            ← 68 行：模块级归属、符号数、每类符号数
-- 明细                                ← 逐符号：PROV / DLL / SYMBOL / GAP / 依据或备注
```

- **模块级归属**（`module_provenance`）看整个模块归谁：ours → `Hle`，白名单 → `Host`，
  目录里有 → `Title`，否则 `Stub`。"68 个模块的归属分布"用的是这个。
- **符号级归属**是阶梯的逐条结果（一个模块可能跨类，例如我们缺导出时）。
- `GAP` 列非空 = 我们的模块缺这个符号（第 2.1 节的 b）。

### 5.3 导出清单从哪来（权威顺序）

1. `--shim-dir`（默认 `build/`）里同名 DLL 的 **PE 导出表** → 这就是加载器看到的东西，权威；
2. 构建产物不存在时，退回**源码清单**：`src/generated/xgameruntime_exports.inc`、
   `src/xgameruntime/exports*.txt`、`src/xmem/exports_more.txt`、`src/d3d12_xs/{exports.txt,forward.def}`、
   `src/console_hw/*.cpp` 里的 `__declspec(dllexport)`。这样在没有构建产物的机器上也能出报告
   （报告里会标出来源，别把它当产物等价物）；
3. 两者都没有 → 报告里打 `!! 找不到导出清单`，并且 stderr 警告（否则它的导入会被误报成缺口）。

**一致性实测**：把 `build/` 换成空目录、让 census 只走源码清单，8 个模块的导出数与
PE 导出表**逐个相等**（461 / 15 / 20 / 9 / 5 / 5 / 2 / 1）。所以"没有构建产物的机器上"
给出的报告同样可信（来源会标成"源码清单"）。

### 5.4 和 `xbgtasix check` 的分工

| 工具 | 回答的问题 | 特点 |
|---|---|---|
| `xbgtasix check <exe>` | **我们缺哪些符号**（对我们 3+5 个 shim 逐名字对表） | 权威、会真去探测依赖模块能否解析、能判 shim 损坏 |
| `tools/analyze_pe.py` | 目标 PE 的**体检**（节表、保护特性、PDB、TLS、延迟导入） | 纯静态，不依赖构建产物 |
| `tools/census.py` | **每个 dll!symbol 该由谁提供**（Hle/Host/Title/Stub） | 纯静态、不运行目标、能测缺口与遮蔽 |

---

## 6. 实测：对真机 exe 跑一次

```bash
python tools/census.py "D:/GTA6/GTA6.Xbox.v1.0.2.1-CYBERLEEK/game_scarlett_master.exe"
```

### 6.1 计数

```text
-- 归属分布
   模块 68 个：  Hle 7  Host 56  Title 4  Stub 1
   符号 587 个：  Hle 37 (6.3%)  Host 477 (81.3%)  Title 72 (12.3%)  Stub 1 (0.2%)

   缺口（我们的模块缺符号）    0 个符号 / 0 个模块   <- 补我们的导出，不是换来源
   遮蔽（Host 与 exe 同目录同名）137 个符号 / 3 个模块   <- 阶梯让 Host 赢；透传只认 System32
```

7 个我们的模块 / 37 个符号**全部命中**（缺口 0），与 `xbgtasix check` 的结论一致。

### 6.2 模块分布（68 个）

| 归属 | 模块数 | 符号数 | 是哪些 |
|---|---|---|---|
| `Hle` | 7 | 37 | `AcpHal` 9 / `xmem` 12 / `chad` 5 / `d3d12_xs` 3 / `xapu` 5 / `xg_xs` 2 / `XCRDAPI` 1 |
| `Host` | 56 | 477 | 43 个 `api-ms-win-*` + 4 个 `ext-ms-win-*` + `ADVAPI32` 3 / `bcrypt` 3 / `MSVCP140` 113 / `VCRUNTIME140` 23 / `VCRUNTIME140_1` 1 / `WS2_32` 25 / `WINHTTP` 15 / `MFPlat` 6 / `MFReadWrite` 1 |
| `Title` | 4 | 72 | `libHttpClient.GDK` 38 / `Bink2Scarlett` 23 / `XboxBvhBuilder` 7 / `amd_fidelityfx_loader_xbox` 4 |
| `Stub` | 1 | 1 | `GameInput.dll`（按序号导入 `#1`） |

`xgameruntime.dll`（461 导出）**没有被这个 exe 直接导入** —— 68 个模块里没有它，
也没有延迟导入目录。它属于"标题的其它 DLL 或后续加载路径会用到"的模块，
census 只报"这个 exe 真正导入的"，不替它编数据。

### 6.3 明细前 30 行（原始输出）

```text
#    PROV      DLL                                          SYMBOL                                         GAP   依据/备注
1    Host      api-ms-win-core-processthreads-l1-1-2.dll    SetProcessPriorityBoost                              HostPolicy: api-ms-win-*
2    Host      api-ms-win-core-processthreads-l1-1-0.dll    CreateThread                                         HostPolicy: api-ms-win-*
3    Host      api-ms-win-core-processthreads-l1-1-0.dll    GetCurrentProcess                                    HostPolicy: api-ms-win-*
4    Host      api-ms-win-core-processthreads-l1-1-0.dll    GetCurrentProcessId                                  HostPolicy: api-ms-win-*
5    Host      api-ms-win-core-processthreads-l1-1-0.dll    GetCurrentThreadId                                   HostPolicy: api-ms-win-*
6    Host      api-ms-win-core-processthreads-l1-1-0.dll    GetProcessId                                         HostPolicy: api-ms-win-*
7    Host      api-ms-win-core-processthreads-l1-1-0.dll    GetStartupInfoW                                      HostPolicy: api-ms-win-*
8    Host      api-ms-win-core-processthreads-l1-1-0.dll    GetThreadPriority                                    HostPolicy: api-ms-win-*
9    Host      api-ms-win-core-processthreads-l1-1-0.dll    OpenThread                                           HostPolicy: api-ms-win-*
10   Host      api-ms-win-core-processthreads-l1-1-0.dll    ResumeThread                                         HostPolicy: api-ms-win-*
11   Host      api-ms-win-core-processthreads-l1-1-0.dll    SetThreadPriority                                    HostPolicy: api-ms-win-*
12   Host      api-ms-win-core-processthreads-l1-1-0.dll    SuspendThread                                        HostPolicy: api-ms-win-*
13   Host      api-ms-win-core-processthreads-l1-1-0.dll    TerminateProcess                                     HostPolicy: api-ms-win-*
14   Host      api-ms-win-core-libraryloader-l1-2-0.dll     FreeLibrary                                          HostPolicy: api-ms-win-*
15   Host      api-ms-win-core-libraryloader-l1-2-0.dll     GetModuleFileNameA                                   HostPolicy: api-ms-win-*
16   Host      api-ms-win-core-libraryloader-l1-2-0.dll     GetModuleFileNameW                                   HostPolicy: api-ms-win-*
17   Host      api-ms-win-core-libraryloader-l1-2-0.dll     GetModuleHandleA                                     HostPolicy: api-ms-win-*
18   Host      api-ms-win-core-libraryloader-l1-2-0.dll     GetModuleHandleW                                     HostPolicy: api-ms-win-*
19   Host      api-ms-win-core-libraryloader-l1-2-0.dll     GetProcAddress                                       HostPolicy: api-ms-win-*
20   Host      api-ms-win-core-libraryloader-l1-2-0.dll     LoadLibraryExA                                       HostPolicy: api-ms-win-*
21   Host      api-ms-win-core-libraryloader-l1-2-0.dll     LoadLibraryExW                                       HostPolicy: api-ms-win-*
22   Host      api-ms-win-core-profile-l1-1-0.dll           QueryPerformanceCounter                              HostPolicy: api-ms-win-*
23   Host      api-ms-win-core-profile-l1-1-0.dll           QueryPerformanceFrequency                            HostPolicy: api-ms-win-*
24   Host      MSVCP140.dll                                 ??0?$basic_ios@DU?$char_traits@D@std@@@st...         HostPolicy: msvcp140.dll；exe 同目录也有同名 DLL（msvcp140.dll）—— 按阶梯 Host 优先，且 Host 只从 System32 加载
25   Host      MSVCP140.dll                                 ??0?$basic_iostream@DU?$char_traits@D@std...         HostPolicy: msvcp140.dll；exe 同目录也有同名 DLL（msvcp140.dll）—— 按阶梯 Host 优先，且 Host 只从 System32 加载
26   Host      MSVCP140.dll                                 ??0?$basic_iostream@_WU?$char_traits@_W@s...         HostPolicy: msvcp140.dll；exe 同目录也有同名 DLL（msvcp140.dll）—— 按阶梯 Host 优先，且 Host 只从 System32 加载
27   Host      MSVCP140.dll                                 ??0?$basic_istream@DU?$char_traits@D@std@...         HostPolicy: msvcp140.dll；exe 同目录也有同名 DLL（msvcp140.dll）—— 按阶梯 Host 优先，且 Host 只从 System32 加载
28   Host      MSVCP140.dll                                 ??0?$basic_ostream@DU?$char_traits@D@std@...         HostPolicy: msvcp140.dll；exe 同目录也有同名 DLL（msvcp140.dll）—— 按阶梯 Host 优先，且 Host 只从 System32 加载
29   Host      MSVCP140.dll                                 ??0?$basic_streambuf@DU?$char_traits@D@st...         HostPolicy: msvcp140.dll；exe 同目录也有同名 DLL（msvcp140.dll）—— 按阶梯 Host 优先，且 Host 只从 System32 加载
30   Host      MSVCP140.dll                                 ??0?$basic_streambuf@_WU?$char_traits@_W@...         HostPolicy: msvcp140.dll；exe 同目录也有同名 DLL（msvcp140.dll）—— 按阶梯 Host 优先，且 Host 只从 System32 加载
...（还有 557 行没打印；--limit 0 打印全部，或 --csv 落盘）
```

注意第 24 行起：**MSVCP140 是 `Host`，但 exe 目录里也有 `msvcp140.dll`**（遮蔽）。
这正是第 4 节"只从 System32 加载"要保护的场景。

### 6.4 `--no-host`：真实 API 面的缺口清单

```text
   模块 68 个：  Hle 7  Host 0  Title 7  Stub 54
   符号 587 个：  Hle 37 (6.3%)  Host 0 (0.0%)  Title 209 (35.6%)  Stub 341 (58.1%)
   --no-host 缺口清单：341 个符号 / 54 个模块必须由 Hle 或 stub 负责
```

即：**一个都不透传时，还有 341 个符号（54 个模块）没有归属**，必须实现（`Hle`）或显式拒绝（`stub`）。
前几名就是实现的优先级：

| 模块 | 缺口符号数 |
|---|---|
| `api-ms-win-crt-math-l1-1-0.dll` | 41 |
| `api-ms-win-crt-string-l1-1-0.dll` | 31 |
| `api-ms-win-crt-stdio-l1-1-0.dll` | 29 |
| `WS2_32.dll` | 25 |
| `api-ms-win-crt-runtime-l1-1-0.dll` | 25 |
| `api-ms-win-core-file-l1-1-0.dll` | 22 |
| `api-ms-win-core-synch-l1-1-0.dll` | 17 |
| `WINHTTP.dll` | 15 |
| `api-ms-win-core-processthreads-l1-1-0.dll` | 12 |
| `api-ms-win-crt-convert-l1-1-0.dll` | 12 |

再看 `Title` 那 209：其中 137 个是 `msvcp140`/`vcruntime140` 的 CRT 符号，
**它们只是"目录里恰好有同名 DLL"**（Xbox 版 CRT，PC 上根本加载不了），
不构成真正的实现路径 —— 这就是为什么 `--no-host` 必须配合 `--only stub` 一起看。
`--only stub` 的 341 里去掉这些才是"要自己动手"的数量。

### 6.5 手工核对 5 个符号

| # | 符号 | census 判定 | 我的依据（可复核） |
|---|---|---|---|
| 1 | `xmem.dll!XMemAlloc` | **`Hle`** | ① `objdump -p build/xmem.dll` 的 `[Ordinal/Name Pointer] Table` 第 1 项就是 `XMemAlloc`（ordinal 1）；② 源码 [src/xmem/xmem.cpp:135](../src/xmem/xmem.cpp) 是 `__declspec(dllexport) void* __cdecl XMemAlloc(uint64_t, size_t)`；③ 目标导入表里 `xmem.dll` 的 12 个符号都在我们的 15 个导出里 |
| 2 | `d3d12_xs.dll!D3D12XboxCreateDevice` | **`Hle`** | ① `objdump -p build/d3d12_xs.dll` → `[ 17] +base[ 18] 0011 D3D12XboxCreateDevice`；② [src/d3d12_xs/exports.txt:5](../src/d3d12_xs/exports.txt) 手写清单里有它；③ `forward.def` 只转发 `D3D12*` 的 PC 共有面，它由 `d3d12_xs.cpp` 自己实现 |
| 3 | `AcpHal.dll!ApuCreateHeap` | **`Hle`**（**而标题的自带版也导出同名符号 —— 我们赢了**） | ① `objdump -p build/AcpHal.dll` → `[ 4] +base[ 5] 0004 ApuCreateHeap`；② 反证：`AcpHal.dll.xbgtasix-backup`（标题原版）解析出来的 9 个导出 **与我们的 9 个完全同名**，其中就有 `ApuCreateHeap` —— 如果第 1 级不优先，第 3 级会让标题的 DLL 顶掉我们的实现；③ 源码 [src/console_hw/acphal.cpp](../src/console_hw/acphal.cpp) 里 `ApuCreateHeap` 是真实现 |
| 4 | `MSVCP140.dll!??0?$basic_ios@DU?$char_traits@D@std@@@std@@IEAA@XZ` | **`Host`**（+ `shadowed`） | ① `POLICY_RULES` 第 6 条 `msvcp140.dll`（v140 ABI 稳定）命中；② exe 目录里**确实有** `msvcp140.dll`（`ls` 可见，Xbox 版），按阶梯第 2 级在 Title 之前 → Host，但标记 `shadowed`；③ `C:\Windows\System32\msvcp140.dll` 存在 → 透传能成功，且只从 System32 取（第 4 节） |
| 5 | `GameInput.dll!#1` | **`Stub`** | ① 不在 `POLICY_RULES`（第 3.3 节写了拒绝理由：设备枚举/力反馈语义不同）；② exe 目录**没有** `GameInput.dll`（`ls` 报 No such file）；③ 不归我们 → 阶梯走到底；④ 它是**按序号导入**（`#1`），note 明确写"无法按名字核对"—— 这种不能假装覆盖了 |

补充两个对照（不进上表，但值得记）：

- `libHttpClient.GDK.dll!HCInitialize` → `Title`（38 个符号）：目录里 `libHttpClient.GDK.dll` 975,856 字节，
  系统里没有同名库，我们也不实现 → 第 3 级。
- `WS2_32.dll` 的导入是 **ordinal**（`#111`/`#115`/...）：模块级仍判 `Host`（白名单），
  但明细里 `note` 标"按序号导入，无法按名字核对" —— census 不假装核对过。

### 6.6 复现全部证据的命令

```bash
python tools/census.py --self-test                                  # 23 checks, 0 failures
python tools/census.py <exe>                                        # 6.1 / 6.2 / 6.3
python tools/census.py <exe> --no-host                              # 6.4
python tools/census.py <exe> --only stub --limit 0                   # 唯一一条 GameInput #1
python tools/census.py <exe> --only missing                          # 0 行（缺口 0）
python tools/census.py --policy                                      # 3.2 的原表
python tools/census.py <exe> --csv build/census.csv                  # 587 行明细落盘
./build/t45_census.exe                                              # 53 checks, 0 failures
```

---

## 7. 模式 C 验收自查（对 handoff 文档第 5 节）

| 验收项 | 状态 | 证据 |
|---|---|---|
| `classify`/`census` 能在不运行目标的情况下给出完整符号归属 | ✅ | 6.1–6.3：68 模块 / 587 符号全部有归属；`classify()` 只吃 `Facts`，不加载目标 |
| `HostPolicy` 是白名单、默认拒绝、每条有理由 | ✅ | 第 3.2 节 20 条全带理由 + `risk`；3.1 的 `allows()` 语义；`--policy` 打印全表 |
| 透传 DLL 只从 System32 加载，并有"当前目录同名 DLL 不被采纳"的回归测试 | ✅ | 第 4 节；C 段 19 条断言（含"朴素做法中招"与 System32 正向对照），53 checks / 0 failures |
| stub 的调用日志包含 `dll!name`，行为可切换（trap/zero） | ⚠️ 部分 | 行为可切换已有（[include/xbgtasix/trace.h](../include/xbgtasix/trace.h) 的 `XBGTASIX_UNIMPL=stop|log`，`stop` 默认：打印 + 栈回溯 + 退出码 70）；**日志目前是 `UNIMPLEMENTED: <name>`，还没有 `dll!` 前缀** —— 见第 8 节的接线项（本次不改 `src/`） |

另外两条自查（本次刻意做的）：

- **`--no-host` 能给出缺口清单**：341 符号 / 54 模块（6.4），并且能说出"其中 137 个只是目录里
  恰有同名 CRT，不算真实现路径"。
- **文档/代码/测试三者不漂移**：`t45` 的 D 段会把 `census.py:POLICY_RULES` 与 `main.cpp:kPolicy`
  逐条比对（本次 20/20），并实际跑一次 census 验证它可用。

---

## 8. 后续接线项（本次不碰 `src/`，交给 lead）

1. **把 `HostLoader` 落进 `src/host`**：`LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32)` +
   拒绝带路径的名字 + 小写名缓存（[tests/t45_census/main.cpp](../tests/t45_census/main.cpp) 的
   `HostLoader` 就是可直接搬的参考实现）。
2. **把 `classify()` 的词汇接进日志/崩溃报告**：`Hle` / `Host` / `Title` / `Stub`，
   别在日志里写"大概是 stub"。
3. **stub 日志补 `dll!name`**：现在 `XB_UNIMPL("XAsyncBegin")` 只带名字；
   生成器 [tools/gen_stubs.py](../tools/gen_stubs.py) 知道每个桩属于哪个 shim，
   把模块名前缀一起生成即可（模式 C 的最后一条验收就补齐了）。
4. **`Title` 级的加载方式**：必须用 `LoadLibraryExW(<exe 目录>\<name> 全路径, ...)`，
   并**不**参与 System32 搜索；这一级只对"我们没接管 + 白名单拒绝 + 文件确实在 exe 目录"的模块生效。
5. **接 `tools/run_suite.sh`**：把 `t45_census` 加进套件（它按 `<N> checks, <M> failures` 判定，
   与套件的 `has_evidence` 规则兼容）。
6. **per-title 的 policy 覆盖**：不同标题的导入面不一样（这个 exe 没有 `xgameruntime.dll`，
   别的 exe 会有）。`HostPolicy` 支持从文件/参数加载额外条目是自然的下一步，
   但**每条仍然必须带理由**（第 3.4 节的纪律不变）。

---

## 9. 已知边界（诚实的部分）

- census 只看**静态导入表**。通过 `GetProcAddress` 动态解析的符号不在里面
  （这个 exe 的 68 个模块都是静态导入，`延迟导入` 目录为空，所以本次没有盲区）。
- `Title` 级只判"**文件名存在**"，不判那个 DLL 能不能被加载（架构、依赖、签名都不看）。
  它回答的是"如果按阶梯走，会落到这份文件"，不是"这份文件一定可用"。
- 按序号导入（`#N`）无法按名字核对：census 会如实标注，不猜。
- `--no-host` 的 `Title` 计数会**虚高**：目录里恰好有的同名 CRT 也算 `Title`，
  但那不是可用的实现路径（6.4 已解释）。看缺口时以 `--only stub` 为准。
- census 的"我们的导出清单"在缺构建产物时退回源码清单。本次实测两边**逐个相等**（见 5.3），
  但机制上仍不保证：手写清单里可能有本次没编进去的符号（`--shim-dir` 指向旧产物时同理），
  报告会标出来源，别把两者混为一谈。
