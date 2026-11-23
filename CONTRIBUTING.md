# 贡献指南

这个项目有两条线，改动前先确认你动的是哪一条：

| 线 | 目录 | 判断标准 |
|---|---|---|
| 兼容层 | `src/`、`tests/`、`include/` | 改完必须能 `./build.sh` 并通过 `run_suite.sh` |
| 数据面研究 | `tools/packaging/`、`docs/` | 改完必须能 `python tools/selftest_tools.py` 通过 |

提交前请跑：

```bash
./build.sh                 # 兼容层
./tools/run_suite.sh       # 39 步测试
python tools/selftest_tools.py   # Python 工具自检
python tools/check_docs.py       # 文档链接
```

---

## 加一个测试

**命名**：`tests/t<N>_<名词>/`，编号接在现有最大号之后（当前到 `t39`）。
`t7` 与 `t16` 是历史空号，不要再占（`t16` 这一步是 Python 的 `tools/check_real_dlls.py`）。

**必须遵守的两条约定**：

1. **离屏 + 断言**。不要依赖窗口或桌面会话 —— 渲染到离屏 target 再回读像素，
   这样在没有显示器的 CI 里也能跑。参考 `t1_triangle`（像素）、`t3_xmem`（字节级）。
2. **返回码语义**：`0` = PASS、`2` = SKIP（本机不支持该能力）、其它 = FAIL。
   例如 `t5_mesh` / `t6_rt` 在缺能力时返回 `2`，CI 不会因此变红。

**注册**：在 `build.sh` 里加一行编译、在 `tools/run_suite.sh` 里加一行 `run_one`。

**断言要"双向"**。只断言"画出了东西"会被"全屏同色"骗过；参考 `t6_rt`：
中心必须是绿（命中）、角落必须是蓝（未命中）。

---

## 补一个 shim 导出

```bash
# 1) 把名字加进手写清单（整行匹配，注意别用子串判断，GetPath 是 GetPathSize 的子串）
echo XSomeApi >> src/xgameruntime/exports.txt

# 2) 重新生成桩（排除已手写的）
EXCL=$(grep -v '^;' src/xgameruntime/exports.txt | grep . | paste -sd, -)
python tools/gen_stubs.py ../gdk/lib/xgameruntime.lib --dll xgameruntime \
    --out src/generated --exclude "$EXCL"

# 3) 在 src/xgameruntime/xgameruntime.cpp 里实现（签名抄 GDK 头文件，别猜）

# 4) 加测试，跑 run_suite.sh
```

**签名一律以 `../gdk/include_clean/` 里的头文件为准**。手写签名出错时编译器不会报，
只会静默破坏 ABI —— `XTaskQueueCreate` 少一个参数、`XSystemGetDeviceType` 返回值类型写错，
这两个坑都真实发生过。

`src/generated/` 是生成物，**不要手改**，重跑脚本会覆盖。

---

## 证据等级标注

结论性文档（`docs/*-evidence.md`、`*-findings.md`）里每条断言都要标来源：

| 标记 | 含义 | 例子 |
|---|---|---|
| `[事实]` | 有公开原文支撑，附链接 | "Xbox 上 GDK 只支持 D3D12.x" |
| `[观察]` | 从本地文件实测得到 | "包内 16 个 DLL 只有 3 个主机专有" |
| `[推断]` | 由公开资料推导，无逐字定义 | "`attributes` 低 32 位是对齐请求" |
| `[自定]` | 本项目选定，只由测试锁住 | "二次释放 → 忽略 + 记日志" |

不确定就标 `[推断]`，别伪装成事实。**结论被推翻时要留下修正记录**（见
[rpf9-findings.md](docs/rpf9-findings.md) 里的"结论修正记录"），这比藏起来有用。

---

## 文档约定

- 每份文档开头给**一句话结论**，再展开论证
- 给**可复跑的命令**，不要只写"我试过了"
- 相对链接必须能打开 —— `tools/check_docs.py` 会在 CI 里检查
- 数字要写清楚口径（"读前 64 MB 采样" vs "全量扫描"）

---

## 提交信息

`<type>(<scope>): <一句话>`，正文写"为什么"和实测数字。

```
feat(gpu): 显卡适配 —— 默认选卡修正 + 两块 GPU 交叉验证
fix(t8): 修正 Z-up -> Y-up 的旋转方向（模型上下颠倒）
docs: README 重写 —— 面向公开仓库的单一入口
test: 全量重跑（干净构建 + 9 个测试）
```

类型：`feat` / `fix` / `docs` / `test` / `refactor` / `chore`。

---

## 不要提交

| 东西 | 原因 |
|---|---|
| 游戏文件（`*.rpf` / `*.ttpf` / `*.dvd` / exe） | 版权；仓库只含自写内容 |
| 微软 GDK 头文件与 `.lib` | 微软自有许可，需各自本地解出 |
| 任何密钥、解密工具、DRM 绕过 | 法律红线，且不解决真正问题 |
| 构建产物（`build/`、`*.cso`、`*.dll`） | `.gitignore` 已排除，别 `-f` 强加 |
| 手改 `src/generated/` | 会被生成脚本覆盖 |

---

## 环境

| 依赖 | 说明 |
|---|---|
| MinGW-w64 | 默认 `D:/mingw64`，`CXX=` 可覆盖 |
| Microsoft GDK | 头放 `../gdk/include_clean/`，库放 `../gdk/lib/` |
| Vulkan SDK | 提供 `dxc.exe`，`DXC=` 可覆盖 |

没有 GDK 时 `build.sh` 会跳过相关步骤，`run_suite.sh` 里依赖 GDK 的项会失败 ——
这是环境问题，不是代码问题。CI 的 `windows-build` job 就是走的"缺 GDK → 跳过"分支。
