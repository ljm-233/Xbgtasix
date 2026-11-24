# 资产与派生物政策（模式 B：静态闸 + 运行时闸）

这个仓库要处理**别人拥有的数据**：200+ GB 的主机游戏包、主机 DLL、微软 GDK 的头文件与
导入库、从包里解出来的纹理/着色器。这类东西一旦进了 git 历史，删一个 commit 是**不够的**
——必须 `git filter-repo` 重写历史 + force-push + 所有人重新 clone。所以"不要提交"不能只是
文档里的一句呼吁，它必须是两处**代码闸门**：

| 闸门 | 文件 | 管什么 |
|---|---|---|
| 静态闸 | [tools/policy.py](../tools/policy.py) | 什么能**进 git**（pre-commit + CI + 仓库自测) |
| 运行时闸 | [tools/repo_guard.py](../tools/repo_guard.py) | 运行时**往哪写盘**（缓存/解包/转码产物不许落回源码树） |
| 钩子 | [.githooks/pre-commit](../.githooks/pre-commit) | 只有一行 `exec`，调用静态闸 |
| 根标记 | [.xbgtasix-root](../.xbgtasix-root) | 让任意 clone / 拷贝 / 符号链接都被运行时闸认出来 |

一句话政策：**仓库只收"我们写的、小的、文本的"文件；别人的数据及其派生物，既不进 git，
也不许落在源码树里（`build/` 除外，见下）。**

---

## Allowed（白名单，默认拒绝）

不在下表里的东西**一律拒绝**——白名单不是黑名单，"没想到的格式"自动被拦。

| 类别 | 条目 |
|---|---|
| C/C++ 源码与头 | `c` `h` `hpp` `cpp` `inc` |
| 着色器（文本） | `hlsl` |
| 构建描述 | `cmake` `def`（`CMakeLists.txt` 走 `txt`） |
| 工具链脚本 | `py` `sh` `ps1` |
| 文本数据/配置 | `md` `txt` `json` `yml` `yaml` |
| 无扩展名的白名单文件名 | `.gitignore` `.gitattributes` `.editorconfig` `.xbgtasix-root` `.xbgtasix-ignore` `LICENSE` `COPYING` `NOTICE` `pre-commit` |

**唯一权威是 [tools/policy.py](../tools/policy.py) 里的 `ALLOWED_EXTENSIONS` / `ALLOWED_NAMES`**，
本表是同一个人的镜像，改一处要改两处（`--selftest` 会校验例外表与本文档一致，扩展名靠 code review 对齐）。
要加新格式：先在 `SELF_TESTS` 加用例，再加白名单，最后改本表——顺序反了很容易漏掉边界。

文本文件还必须同时满足：

| 约束 | 值 | 为什么 |
|---|---|---|
| 大小 | ≤ **512 KiB** | 源码/文档不会这么大；大了就是想夹带东西 |
| NUL 字节 | 不允许 | 有 NUL 就不是文本，是二进制 |
| 连续 hex/base64 候选段 | ≤ **512 字符** | 抓"把数据塞进字符串常量"这条路 |
| 首字节魔数 | 不得命中禁用表 | 见下一节；**魔数优先于扩展名** |

扩展名白名单是按本仓库**实际用到的**集合定最小集：`tools/policy.py` 里有注释说明每一组的
出处。每多一个扩展名，就是多一条"可以夹带数据"的通道，所以不预埋用不上的格式。

## Not allowed

**（一）任何命中禁用魔数的文件**——不管它叫什么名字。一个叫 `notes.txt` 的 PE 也会被抓：

| 首字节 | 格式 | 本项目的典型来源 |
|---|---|---|
| `MZ` | PE executable | 游戏 exe、主机 DLL、`xbgtasix.exe` |
| `DXBC` | shader container | 从包里解出的主机着色器 |
| `DDS ` | DDS texture | 解包出的 `.dds`/平铺纹理 |
| `RIFF` | RIFF（音频/视频） | 语音、过场音频 |
| `BC C0 DE` | LLVM bitcode | 编译中间产物 |
| `Microsoft C/C++ MSF` | PDB | 调试符号 |
| `!<arch>` | 库归档 | `.lib`/`.a` |
| `PK 03 04` | zip | GDK nupkg、打包产物 |
| `7z BC AF` | 7-zip | 压缩包 |
| `MSCF` | cabinet | 安装包 |
| `D0 CF 11 E0` | MSI/OLE | 安装包、Office 文档 |

**（二）按品类明确禁止**（`.gitignore` 也据此排除）：

- 游戏文件：`*.rpf` / `*.ttpf` / `*.dvd` / 任何原版 exe / 解密或未解密的主机包体
- 微软 GDK 头文件与 `.lib`（`include_clean/`、`xgameruntime.lib`）——需要时各自本地
  `python tools/fetch_gdk.py` 解出，不入库
- 构建产物：`build/`、`*.cso`、`*.dll`、`*.exe`、`*.dll.a`、`*.log`
- 调试/抓帧产物：`*.pdb`、PIX capture、shader cache、crash dump
- 图片/音频/视频等一切二进制媒体（见"例外"一节的唯一一条）
- 任何密钥、解密工具、DRM 绕过
- 把数据编码进字符串常量的"文本"（512 字符阈值就是拦这个）

**（三）改扩展名没用。** 魔数检查在扩展名检查之前；`notes.txt`、`README.md`、`src/x.cpp`
只是容器名，内容说了算。

## 例外

例外表在 [tools/policy.py](../tools/policy.py) 的 `EXCEPTIONS` 里。**条目数就是技术债
计数器，目标是 0 条。**

当前 **1 条**：

| 路径 | 理由 |
|---|---|
| `docs/images/xboxui.png` | 文档配图：`t38_xboxui` 自绘 shell 窗口的效果图（本项目代码渲染，非微软素材、非游戏素材）。二进制图片违反白名单，是本表唯一一条技术债；消除办法是让 CI 跑 t38 现场生成、不入库，删掉该文件与 `EXCEPTIONS` 里对应的一行即可回到 0 条例外 |

加一条新例外的规则（缺一条 `--selftest` 就红）：

1. `EXCEPTIONS` 里写 `路径 -> 理由字符串`，理由 ≥ 20 字，必须写清"为什么不能删/不能生成"；
2. **同一条必须写进本文件的表格**（`--selftest` 会读本文件校验路径出现过）；
3. PR 说明里写明期限与消除计划；
4. 季度复核一次：能删就删，能代码生成就代码生成。

例外是**整条跳过**检查的（不是"仅放宽大小"），所以路径要写得越具体越好——不要写目录通配。

## 泄露了怎么办（按顺序做，不要跳步）

这是事故，不是小失误。**只删文件再 commit 一遍没有用**：对象还在历史里，clone 过的人还有。

1. **先止血**：停掉会产生更多派生物的流水线；别急着 push，先想清楚范围。
2. **定范围**：`git log --all --oneline -- <path>`、`git rev-list --objects --all | grep <name>`；
   用 `git rev-list --objects --all | git cat-file --batch-check` 找大对象，确认是哪个提交引入的、
   有没有进过 tag/release/PR 分支。
3. **重写历史**：用 `git filter-repo --path <file> --invert-paths`（多文件就多写几个 `--path`；
   整个品类用 `--path-glob '*.rpf'`）。只想去掉大对象也可以
   `git filter-repo --strip-blobs-bigger-than 1M`。不要再用 `git filter-branch`。
4. **强制推送 + 通知所有人重新 clone**：`git push --force --all` 与 `--force --tags`；所有人
   （包括 CI 机器、fork）必须**重新 clone**，不能 pull/merge——否则旧对象会回来。
   让托管平台收敛：删掉 fork/PR 引用、清 Actions 缓存与 artifact，GitHub 还需要让 support
   或"删除并重建仓库"才能彻底回收不可达对象。
5. **轮换**：如果泄露的是密钥/令牌/证书，立刻作废重发；如果涉及厂商数据，按许可要求处理
   （不扩散、不再贴到 issue 里）。
6. **复盘**：把这次漏掉的形状补成 `tools/policy.py` 的用例（`SELF_TESTS`），再补 `.gitignore`
   规则；如果是从写盘工具漏出去的，给它接上 `ensure_outside_repo`。

## 运行时闸：为什么 `build/` 放行

静态闸只管"进 git"；运行时产物（解包结果、转码缓存、prepared image、crash dump）照样会
落进仓库目录。`ensure_outside_repo(dest)` 用"`.xbgtasix-root` 标记 + 祖先检查"识别任意
clone，因此对**任何**位置的仓库副本都成立（含符号链接/junction、不同盘符、被整棵拷走的树）。

**放行规则**：`<仓库根>/build/` 整棵子树放行，其余仓库内路径一律拒绝。

为什么必须这么放：

- [build.sh](../build.sh) 里 `OUT=build`，这个仓库的构建产物**就在仓库目录内**；
- [tools/fetch_gdk.py](../tools/fetch_gdk.py) 的默认落点是 `build/gdk`；
- 不放行的话，整条构建链、GDK 解包、签名全都会被自己拦住，闸门第一天就会被 `--no-verify`
  或被删掉——闸门必须能长期开着才有意义。

这是**显式取舍，不是遗忘**，代价与补偿：

- 代价：`build/` 里确实可能有派生物（解出来的 GDK 头/库、`.cso`）。
- 补偿 1：`build/` 已被 [.gitignore](../.gitignore) 排除，静态闸的 `--staged` 只认真实进索引
  的文件，`git add -f` 也会被魔数/白名单抓；
- 补偿 2：放行按**路径分量**比较，`build-old/`、`build2/` 不会被误放行，`--selftest` 有专门用例；
- 补偿 3：除 `build/` 外的仓库内落点照拦（`tools/../not-yet/out` 这种绕法也一样拦）。

如果你不想承担这个取舍：把构建目录整体挪到仓库外（`OUT=../xbgtasix-work/build`），
然后用 `ensure_outside_repo(dest, allow_build=False)` 关掉放行即可。当前选择是"构建就地、
闸门认 build/"。

更推荐的新写入点落位：**仓库外的工作目录**，例如 `D:/GTA6/xbgtasix-work/`（与仓库同级）。

## 闸门怎么用

```bash
# 静态闸
python tools/policy.py --selftest        # 内置单测（纯函数，不需要 git 状态）
python tools/policy.py --check-repo      # 全仓扫描，CI/自测跑这条：0 违规才算过
python tools/policy.py --staged          # pre-commit 用：只读 git 索引里的 blob
python tools/policy.py --install-hooks   # 把 core.hooksPath 指到 .githooks

# 运行时闸
python tools/repo_guard.py --selftest
python tools/repo_guard.py ../xbgtasix-work/cache    # 0 = 可写，1 = 被拦
python tools/repo_guard.py --print-root
```

`--staged` 走 `git diff --cached --name-only --diff-filter=ACMR -z` 拿文件列表
（`-z` 防止文件名里塞空格/换行做手脚），内容走 `git show :<path>` 读**索引里的 blob**，
不读工作区——否则"提交前先改回干净版、把脏版留在索引里"就能绕过。

**每个写盘的工具都必须先调 `ensure_outside_repo`**（这是纪律，不是可选装饰）：

```python
from repo_guard import ensure_outside_repo
dest = ensure_outside_repo(dest)      # 返回规范化后的绝对路径；在仓库内会抛 InsideRepoError
```

### 建议接线位置（本次未改动这些文件）

以下三处已确认是真实写盘点，接线方式已经定好，改动留给仓库维护者：

1. **[tools/fetch_gdk.py](../tools/fetch_gdk.py)**（解包 GDK 到 `build/gdk`）：在
   `out = os.path.abspath(a.out)` 之后、`if ready(out)` 之前插入

   ```python
   sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
   try:
       from repo_guard import ensure_outside_repo, InsideRepoError
       out = ensure_outside_repo(out)          # 默认放行 build/；其余仓库内落点拒绝
   except ImportError:                         # 独立拷出去用时没有 repo_guard，不阻塞
       pass
   except InsideRepoError as e:
       print("拒绝：%s" % e)
       return 2
   ```

   注意行为变化：`--out gdk`（仓库根）会被拦；请用默认的 `build/gdk`，或用 build.sh 期望的
   仓库外路径 `../gdk`（`gdk_inc_default` 就是 `../gdk/include_clean`）。

2. **[tools/sign_local.ps1](../tools/sign_local.ps1)**：签名前守一次 `$Exe`
   （在 `$Exe = Join-Path $repo $Exe` 之后），防止 `-Exe` 被指到仓库里别的位置：

   ```powershell
   & python "$PSScriptRoot\repo_guard.py" $Exe
   if ($LASTEXITCODE -ne 0) { throw "落点在仓库内（且不在 build/），拒绝签名: $Exe" }
   ```

   正常路径 `build/xbgtasix.exe` 在放行区，所以不会误拦。

3. **[build.sh](../build.sh) 的签名段**（`SIGN=1` 时第 603 行调用 sign_local.ps1）：
   **不改**。若将来要在这里加断言，建议放在 `OUT=build` / `mkdir -p "$OUT"` 之后：
   `"$PY" tools/repo_guard.py "$OUT" >/dev/null || { echo "错误：构建目录不在允许位置" >&2; exit 1; }`
   ——它在当前布局下恒真，作用是"如果有人把 `OUT` 改成仓库内的别处，构建直接拒绝"。

## 已知取舍（诚实记录）

- 例外表 1 条（见上），目标 0 条。
- 阈值 512 KiB / 512 字符是经验值，不是密码学边界：真正的防线是"白名单 + 魔数 + 人工 review"，
  超长编码段只拦"偷懒的路"。
- `--check-repo` 只扫工作区里**未被 `.gitignore` 排除**的文件（不入库的东西不算违规）；
  强行 `git add -f` 进来的仍由 `--staged` 兜住。目录层面只跳过 `.git/`、`build/`、`.dsh/`、
  `__pycache__/`：如果本地还有别的巨大忽略目录（例如仓库根的 `gdk/`），建议挪到仓库外，
  否则全仓扫一遍会慢。
- 不经 git、直接 `scp` 上服务器或贴进 issue/PR 附件的派生物，闸门管不到——那要靠 review 纪律。
- 运行时闸保护的是"写盘落点"，不限制运行时**读取**游戏文件（读是兼容层的本职）。

## 与其它文档的关系

本文件是 [CONTRIBUTING.md](../CONTRIBUTING.md) 里「不要提交」那张表的**机器执行版**：
表里的每一行在这里都能找到对应的检查项；两边改一边时记得同步另一边。
