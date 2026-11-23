# XStore（商店 / 授权）：为什么它是桩，跑了会看到什么

> **一句话结论**：`XStore*` 共 **78 个导出**（77 个生成桩 + 1 个手写），全部**不伪造授权**——
> 桩只做两件事：往日志里打一行 `UNIMPLEMENTED: <API 名>`，然后返回失败码。
> 这是**设计决定，不是 bug**；游戏若因此起不来，那说明它卡在"查授权"这一步，
> 而不是兼容层没实现某个图形/内存接口。

本文所有数字都是当场用下面的命令数出来的（口径写在命令旁边），不引用旧文档。

---

## 一、有多少个 XStore，它们在哪

| 口径 | 数量 | 命令 |
|---|---|---|
| `.inc` 里的 `XStore*` 桩（生成物） | **77** | `grep -cE '^XStore' src/generated/xgameruntime_exports.inc` |
| 手写的 `XStore*`（在 `exports*.txt` 里，生成时被 `--exclude` 跳过） | **1** | `grep -h '^XStore' src/xgameruntime/exports*.txt` |
| 编出来的 `xgameruntime.dll` 里真实的 `XStore*` 导出 | **78** | `objdump -p build/xgameruntime.dll \| sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p' \| grep -c XStore` |

那个手写的是 `XStoreQueryProductForCurrentGameAsync`（`src/xgameruntime/xgameruntime.cpp`），
它和桩的唯一区别是"有人负责"，行为完全一样：`XB_UNIMPL` + `return E_NOTIMPL`。

## 二、桩到底返回什么（贴代码）

桩由 `tools/gen_stubs.py` 生成，返回类型从 GDK 头文件推断，三种模板：

```python
# tools/gen_stubs.py:258-273
kind = rettype_index.get(sym, "other")
if kind == "void":   ... void <id>(void) { XB_UNIMPL("X"); }
elif kind == "bool": ... bool <id>(void) { XB_UNIMPL("X"); return false; }
elif kind == "other":... uintptr_t <id>(void) { XB_UNIMPL("X"); return 0; }
else:                # HRESULT
    f.write('long %s(void) { XB_UNIMPL("%s"); return (long)0x80004001; }\n' ...)
```

实际生成出来的 XStore 桩（`src/generated/xgameruntime_stubs.cpp`）：

```c
long xbgtasix_stub_0033(void) { XB_UNIMPL("XStoreAcquireLicenseForPackageAsync"); return (long)0x80004001; }
uintptr_t xbgtasix_stub_0058(void) { XB_UNIMPL("XStoreIsLicenseValid"); return 0; }
```

统计结果：

- **69 个返回 `0x80004001`**（`E_NOTIMPL`）——所有 `*Async` / `*Result` / 查询类（`CreateContext`、`Register*` 等）
- **8 个返回 `0`**（生成器拿不到确切返回类型，按 `other` 处理）：
  `XStoreCloseContextHandle` / `XStoreCloseLicenseHandle` / `XStoreCloseProductsQueryHandle` /
  `XStoreIsAvailabilityPurchasable` / `XStoreIsLicenseValid` /
  `XStoreProductsQueryHasMorePages` / `XStoreUnregisterGameLicenseChanged` /
  `XStoreUnregisterPackageLicenseLost`
  —— 其中 `IsLicenseValid` / `IsAvailabilityPurchasable` 的 `0` 语义上就是 **false = 没有有效授权**

复跑：

```bash
grep 'XB_UNIMPL("XStore' src/generated/xgameruntime_stubs.cpp | wc -l                                  # 77
grep 'XB_UNIMPL("XStore' src/generated/xgameruntime_stubs.cpp | grep -vc 'return (long)0x80004001'     # 8
```

### 一定会打日志

`XB_UNIMPL(api)` 展开成 `xbgtasix::unimpl(api)`（`include/xbgtasix/trace.h`），它会：

1. 往 **stderr 和 `xbgtasix.log`**（默认在 exe 同目录，`XBGTASIX_LOG=` 可改）各写一行
   `[xbgtasix] UNIMPLEMENTED: <API 名>`；**同一个 API 只报第一次**，避免刷爆日志。
2. 看 `XBGTASIX_UNIMPL`（默认 `stop`）：
   - `stop`（默认）：打印调用栈 + 主模块基址，然后 `TerminateProcess(70)`。
     这就是 `xbgtasix run` 报 `分类 UNIMPL / 退出码 70` 的来源。
   - `log`：只记一行然后**继续跑**，把失败码交回给游戏。

所以"有没有打日志"不是问题——**打没打日志反而是判断卡点的唯一可靠线索**。

## 三、游戏大概率会调用它们查授权，预期现象

游戏启动时的授权检查通常是一条链：`XStoreCreateContext` → `XStoreQueryGameLicenseAsync`
（或 `XStoreQueryProductForCurrentGameAsync`）→ `XStoreQueryLicenseTokenAsync` →
`XStoreIsLicenseValid`。按现在的实现，会看到下面三种现象之一：

| 现象 | 发生条件 | 判据 |
|---|---|---|
| **进程直接消失，`run` 报 `UNIMPL`（退出码 70）** | 默认 `stop` 模式 | 日志最后几行是 `UNIMPLEMENTED: XStore…` + 栈 |
| **弹"无法验证授权 / 请购买 / 登录失败"后退出或回主菜单** | `XBGTASIX_UNIMPL=log`，且游戏先把 `E_NOTIMPL` 当错误处理 | 日志里该 API 只有 1 行，游戏自己的日志/画面提示授权失败 |
| **卡住不动，`run` 报 `TIMEOUT`（124）** | `log` 模式，且游戏对 `*Async` 调 `XAsyncGetStatus(wait=true)` 等回调 | 日志停在某个 `XStore*Async` 名字上，之后再没有新行 |

第三种最容易被误判成"兼容层没实现"。机制在这里（`src/xgameruntime/xgameruntime.cpp`）：

- 桩对一个 `*Async` 只返回 `E_NOTIMPL`，**不会去完成那个 `XAsyncBlock`**；
- 我们自己的 `XAsyncGetStatus(wait=true)` 等 5 秒后返回 `E_PENDING`
  （`src/xgameruntime/xgameruntime.cpp` 的 `XAsyncGetStatus`，当前在第 906 行附近），
  调用方若循环重试就等于永久等下去；
- 代码里把这条写成铁律注释（`xgameruntime.cpp` 的 UI 段，当前在第 1405 行附近）：
  "`*Async` 必须让 `XAsyncBlock` 完成。返回 `E_NOTIMPL` 却不完成，
  调用方的 `XAsyncGetStatus(wait=true)` 会一直重试 …… 引擎若轮询或断言 `S_OK` 就等于永久卡死。"

**这是设计决定：我们不伪造授权。** 一条假的 `S_OK` + 一个伪造的 license token 能让游戏往下走，
但它同时也是一份伪造的购买凭证；而且游戏后续用这个 token 去连微软服务时照样会失败，
只是把"卡在启动"换成"卡在联网"。所以宁可让它在这里明确地停下，留下确切的 API 名。

## 四、卡住了怎么判断"是授权，不是兼容层"

按这个顺序看：

1. `build/xbgtasix.exe check <exe>` 是否 **PASS**（导入面全覆盖）？
   PASS 说明所有导出都在，剩下的只可能是语义/授权问题。
2. `grep UNIMPLEMENTED build/launch/xbgtasix.log | sort | uniq -c | sort -rn`
   ——**只有 XStore 名字**，就是卡在授权。
3. 如果里面**还混着非 XStore 的 API**，那才是兼容层的缺口，先补那些（见
   [exe-day-checklist.md](exe-day-checklist.md) 第 3 步）。

授权链路上最可能出现的名字（按调用概率排序）：

| API 名 | 含义 |
|---|---|
| `XStoreCreateContext` | 建立商店上下文，几乎必然是第一个 |
| `XStoreQueryGameLicenseAsync` | **查本游戏的授权** —— 最关键的一个 |
| `XStoreQueryLicenseTokenAsync` | 取 license token；这是"离线不可能过"的那一环 |
| `XStoreIsLicenseValid` | 判授权是否有效（我们返回 false） |
| `XStoreQueryProductForCurrentGameAsync` | 查当前游戏的商品信息（唯一手写的那一个） |
| `XStoreCanAcquireLicenseForPackageAsync` | 判能否取得包授权 |
| `XStoreRegisterGameLicenseChanged` | 注册授权变化回调（常和上一条一起出现） |

一句话判据：**日志里出现 `XStoreQueryGameLicenseAsync` / `XStoreQueryLicenseTokenAsync`
而 `check` 是 PASS ⇒ 卡在授权，不是兼容层没实现。**

## 五、明确不要做的事

| 不要做 | 为什么 |
|---|---|
| 伪造 license token / 伪造 `XStoreQueryLicenseTokenResult` 的内容 | 那就是伪造购买凭证；且线下无法生成微软服务签名，假 token 在下一次校验就露 |
| 让 `XStoreIsLicenseValid` 一律返回 `true` | 等于无条件声称"已购买"，性质同上，还会让游戏走进我们完全没实现的后续链路 |
| patch 游戏跳过授权检查 / hook 校验函数 | 越过红线，也把"能不能跑"这个技术问题变成别的问题 |
| 把 `E_NOTIMPL` 改成 `S_OK` 来"让它先跑起来" | 调用方会去读我们根本没填的 `_Out_` 参数，得到的是栈垃圾 → 更难的 AV |
| 从别人的仓库/工具里搬"授权绕过" | 同上；本仓库只做 API 兼容 |

**该做的是**：把日志里那个 `XStore` 名字记下来，如实写进结论
（"卡在授权校验" ≠ "兼容层缺东西"），然后继续验证剩下的非授权路径。

## 六、复跑（都是只读命令）

```bash
# 数量
grep -cE '^XStore' src/generated/xgameruntime_exports.inc        # 77 个桩
grep -h '^XStore' src/xgameruntime/exports*.txt                 # 1 个手写
objdump -p build/xgameruntime.dll | sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p' | grep -c XStore  # 78

# 桩行为
grep -n 'XB_UNIMPL("XStoreIsLicenseValid")' src/generated/xgameruntime_stubs.cpp
grep -n "void unimpl" -A30 src/trace.cpp   # unimpl(): 打日志 + 可选栈 + TerminateProcess(70)

# 现场判断
build/xbgtasix.exe check <exe>
grep UNIMPLEMENTED build/launch/xbgtasix.log | sort | uniq -c | sort -rn
```
