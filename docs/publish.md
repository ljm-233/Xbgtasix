# 发布现状与重新发布

仓库已经在 GitHub 上，CI 也在跑。这份文档只回答两件事：**现在发布到哪一步**、
**下次怎么发**。

## 一、现状（2026-11-23 核对）

```bash
git remote -v      # origin  https://github.com/ljm-233/Xbgtasix.git
git rev-parse --short HEAD   # 85215e3
gh api repos/ljm-233/Xbgtasix --jq '{default_branch,pushed_at,private}'
```

| 项 | 值 |
|---|---|
| 远端 | `https://github.com/ljm-233/Xbgtasix.git`（公开，默认分支 `main`） |
| 本地提交 | 94 个，分支 `main`（工作区里可能有未提交的改动，`git status --porcelain` 自己确认） |
| CI | `.github/workflows/ci.yml` 四个 job；Actions 上已有 **49 次运行**，最近一次 `success` |
| 轮询工具 | `bash tools/ci_watch.sh 240 --jobs`（`gh` 已装在本机） |

**一个必须知道的细节**：远端 `main` 的 head 是 `0fdc671`，本地是 `85215e3`，
两者 SHA 不同源 —— 当初 git 的 TLS 后端报 `AcquireCredentialsHandle failed`，
历史是用 `tools/push_via_api.py` 走 GitHub REST API 逐条重放上去的（提交信息、
作者时间都保留，但 SHA 变了）。核对内容要按 tree 比，不要按 SHA 比：

```bash
git diff --name-only 0fdc671 HEAD    # 当前只有 1 个文件：src/xgameruntime/xgameruntime.cpp
```

也就是说：远端内容 = 本地 `ee8e8f1`（tree 完全相同），本地 HEAD 比它多改了 1 个文件，
**这条还没推上去**。正因为 SHA 不同源，普通 `git push` 会报 non-fast-forward，
要么 `git push --force-with-lease`（确认 diff 只有上面那 1 个文件），
要么沿用 `tools/push_via_api.py` 重放。

## 二、重新发布（正常有网环境）

```bash
cd D:/GTA6/Xbgtasix
./tools/run_suite.sh             # 40 步，38 PASS / 2 SKIP 才算能发
python tools/check_docs.py       # 文档相对链接
git add -A && git commit -m "<type>(<scope>): <一句话>"
./tools/push.sh ljm-233/Xbgtasix   # export GITHUB_TOKEN=... 后执行，token 只经 stdin
bash tools/ci_watch.sh 240 --jobs  # 等 CI，最多 4 分钟
```

`tools/push.sh` 的约定：token 只从 `GITHUB_TOKEN` 环境变量读，不落盘、不写进
`git config`，推完把 remote 重置成不带凭据的 `https://` 地址。
用完后到 GitHub 上 revoke 那个 token。

历史不同源时（见上节），改用：

```bash
git push --force-with-lease origin main          # 前提：diff 只有预期的那几个文件
# 或
python tools/push_via_api.py ljm-233/Xbgtasix    # 走 API 逐条重放，避免动 TLS
```

## 三、认证方式二选一

| 方式 | 命令 / 说明 |
|---|---|
| HTTPS + PAT | `git push` 时用户名填账号、密码填 **Personal Access Token**（需 `repo` 权限） |
| SSH | 先生成 `ssh-keygen -t ed25519`，把 `~/.ssh/id_ed25519.pub` 贴到 GitHub → Settings → SSH keys，再把 remote 换成 `git@github.com:ljm-233/Xbgtasix.git` |

## 四、推送前自查（2026-11-23 实测值）

```bash
git ls-files | wc -l          # 143 个文件
git ls-files -z | xargs -0 ls -la | sort -k5 -rn | head   # 最大 81,698 字节（docs/gdk_api_surface.md，约 80 KB）
git ls-files | grep -icE '\.(dll|exe|rpf|ttpf|pfx)$|keys' # 0
du -sh .git                   # 7.2 MB（块占用；du -sb 视 3.6 MB 为实际字节）
```

- 入库内容：源码、测试、工具、文档
- **不入库**：微软 GDK 头/库、游戏资产、任何密钥、构建产物（见 `.gitignore`）

## 五、离线搬运（换一台没网的机器时）

```bash
git bundle create ../Xbgtasix-<日期>.bundle --all   # 单文件仓库快照
```

到有网的机器上 `git clone Xbgtasix-<日期>.bundle Xbgtasix`，再 `set-url` 到目标仓库、
`push -u origin main`。bundle 自带全部提交历史，和直接 clone 等价。
（`../Xbgtasix-20261122.bundle` 就是这样来的，是 11-22 的快照，可直接当备份。）

## 六、仓库里没有的东西（很重要）

这个仓库**不含**：

- 游戏文件（`*.rpf` / `*.ttpf` / `*.dvd` / exe）
- 微软 GDK 的头文件与导入库
- 解密密钥、破解或 DRM 绕过（`tools/packaging/` 里的工具只做格式解析，不破解）

它包含的是：自写的兼容层、可断言的测试、以及**对公开数据的观测结论**。
复现任何数据面结论都需要使用者自己的游戏副本。

## 七、CI 说明

`.github/workflows/ci.yml` 四个 job：

| job | 内容 | 需要的条件 |
|---|---|---|
| `python-tools` | 用合成样本自检 5 个 Python 工具 | 无（纯标准库） |
| `docs-links` | 检查 md 里的相对链接 | 无 |
| `linux-cross` | `ubuntu-latest` 交叉编译三个 shim，校验导出数 461 / 4 / 18 | 无 |
| `windows-build` | 从 NuGet 拉官方 GDK → 完整构建 + 跑 40 步测试 | 需要 GDK 头/库；`STRICT=1`，拿不到 GDK 直接判 job 失败 |

`windows-build` **不再** `continue-on-error`：GDK 头/库不能入库，但"拉不到就降级成无 GDK 子集"
会让这个 job 假装覆盖了完整构建。现在走 `STRICT=1`：找不到 GDK 就失败并打印原因
（本地想只跑第 1 层，用不带 `STRICT` 的 `bash tools/ci_build.sh`）。
