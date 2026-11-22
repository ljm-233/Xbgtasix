# 发布到 GitHub（本机没网络，做完只差 push）

## 为什么我不能替你推送

当前环境没有出网能力，实测：

```
Resolve-DnsName github.com  ->  127.0.0.1
curl https://github.com     ->  000（连不上）
gh                          ->  未安装
~/.ssh/*.pub                ->  无
git remote                  ->  未配置
```

所以最后一步需要你在有网的环境执行。

## 方式一：直接在这台机器上推（最省事）

在有网的时候（比如关掉代理限制、或换到正常网络），两条命令：

```bash
cd D:/GTA6/Xbgtasix
git remote add origin https://github.com/<你的账号>/Xbgtasix.git
git push -u origin main
```

建仓时**不要**勾选 "Add a README / .gitignore / license"——本地已经有一份完整的，
远程初始化反而要合并。

## 方式二：用 bundle 搬运（本机永远没网时）

我已经打了一个单文件仓库快照：

```bash
# 本机生成（已执行）
git bundle create ../Xbgtasix-<日期>.bundle --all
```

到有网的机器上：

```bash
git clone Xbgtasix-<日期>.bundle Xbgtasix
cd Xbgtasix
git remote set-url origin https://github.com/<你的账号>/Xbgtasix.git
git push -u origin main
```

bundle 自带全部提交历史，和直接 clone 等价。

## 认证方式二选一

| 方式 | 命令 / 说明 |
|---|---|
| HTTPS + PAT | `git push` 时用户名填账号、密码填 **Personal Access Token**（需 `repo` 权限） |
| SSH | 先生成 `ssh-keygen -t ed25519`，把 `~/.ssh/id_ed25519.pub` 贴到 GitHub → Settings → SSH keys，再把 remote 换成 `git@github.com:<账号>/Xbgtasix.git` |

## 推送前自查（已在本机执行过）

```bash
git ls-files | wc -l          # 85 个文件
git ls-files -z | xargs -0 ls -la | sort -k5 -rn | head   # 最大 80 KB
git ls-files | grep -icE '\.(dll|exe|rpf|ttpf|pfx)$|keys' # 0
du -sh .git                   # 2.7 MB
```

- 入库内容：源码、测试、工具、文档
- **不入库**：微软 GDK 头/库、游戏资产、任何密钥、构建产物（见 `.gitignore`）

## 仓库里没有的东西（很重要）

这个仓库**不含**：

- 游戏文件（`*.rpf` / `*.ttpf` / `*.dvd` / exe）
- 微软 GDK 的头文件与导入库
- 任何解密密钥或绕过手段

它包含的是：自写的兼容层、可断言的测试、以及**对公开数据的观测结论**。
复现任何数据面结论都需要使用者自己的游戏副本。

## CI 说明

`.github/workflows/ci.yml` 三个 job：

| job | 内容 | 需要的条件 |
|---|---|---|
| `python-tools` | 用合成样本自检 5 个 Python 工具 | 无（纯标准库） |
| `docs-links` | 检查 md 里的相对链接 | 无 |
| `windows-build` | 构建 + 跑 21 项测试 | 需要 GDK 头/库，缺则打印说明后跳过 |

`windows-build` 设为 `continue-on-error`：因为 GDK 头不能入库，绝大多数 CI 环境都会走"跳过"分支，
这属于预期行为，不应该让 CI 变红。
