# 环境与工具链的坑（每条都有实测证据）

这些坑的共同点：**不报错、或者报一个指向错误方向的错**，很容易得出错误结论。

## 1. 沙箱限制的是"进程身份"，不是"路径权限"

现象：我们编译的 exe 在 `%LOCALAPPDATA%` 下建目录返回 `ERROR_ACCESS_DENIED(5)`，
于是很容易判成"权限不够"。

对照实验（同一个 exe，2026-11-23）：

| 目标 | mkdir | 建文件 |
|---|---|---|
| `D:\GTA6\Xbgtasix\build\from_exe` | ✅ | ✅ |
| `C:\Users\link\AppData\Local\dsh_probe_dir\from_exe` | ❌ err=5 | ❌ err=3 |
| `C:\Users\link\AppData\Local\Xbgtasix\7952987A\from_exe` | ❌ err=5 | ❌ err=3 |

同两个 `%LOCALAPPDATA%` 路径：**bash 的 mkdir 成功、Python 的 os.makedirs 成功**。

排查掉的替代解释：

```
icacls: SYSTEM / Administrators / link 三条均为 (I)(OI)(CI)(F)   → 完全控制，不是 ACL 问题
Defender 受控文件夹访问 = 0，无 1123/1124 事件                     → 不是杀软拦截
```

结论：沙箱放行 bash/Python 这类工具，限制编译产物正常工作区外的路径。
**测试统一用 `XBGTASIX_PLS_ROOT` 指到 `build/` 下**，就能在受限环境里跑通。

## 2. `CreateDirectoryA` 不建中间层，而且失败时返回值常被忽略

真 bug（已修）：逐级建目录的循环从 `base + 1` 开始扫，跳过了最后一级分隔符，
导致 `%LOCALAPPDATA%\Xbgtasix` 这一层从未被创建 → `ERROR_PATH_NOT_FOUND(3)`。

更糟的是当时没检查返回值，日志照样打印"PLS 目录: ..."，看起来像成功。

**规则**：建目录后必须 `GetFileAttributes` 验证，失败要带上 `GetLastError()` 记录。

## 3. 传进 Windows API 的路径必须是 Windows 形式

`/d/GTA6/...` 在 MSYS 的 bash 里合法，传给 Win32 API 就是非法路径。

踩过三次：g++ 的 `-I` 路径、`XBGTASIX_PLS_ROOT`、`tools/run_suite.sh` 里的默认值。

**规则**：脚本里凡是给原生程序用的路径，先过 `cygpath -m`。

## 4. git 的 TLS 后端可能坏，但 `gh api` 还能用

现象：

```
git push  -> schannel: AcquireCredentialsHandle failed (SEC_E_NO_CREDENTIALS)
git -c http.sslBackend=openssl -> "Ignoring unavailable SSL backend 'openssl'"
```

但同一台机器上 `gh api user` 正常（gh 用 Go 自己的 TLS 栈）。

**绕法**：[tools/push_via_api.py](../tools/push_via_api.py) 用 REST API 逐提交重放
（blob → tree → commit → PATCH ref），完整保留历史。长推送要带重试——
`stream error: CANCEL` 在网络抖动时很常见。

## 5. minGW 的头文件声明可能与真实运行时不一致

三例：

| 接口 | mingw 声明 | 真实语义 |
|---|---|---|
| `RegisterVideoMemoryBudgetChangeNotificationEvent` | `(HANDLE, DWORD*)` 出参 | `(HANDLE, DWORD)` 入参 |
| `UnregisterVideoMemoryBudgetChangeNotification` | `(DWORD)` | 同左（与 Register 反了） |
| `device->QueryInterface(IID_IDXGIDevice)` | 编译通过 | 运行期失败 |

按"MS 顺序"手写 vtable **会 AV 崩溃**（头文件里方法顺序也不同）。

**规则**：遇到 `E_INVALID_CALL` 先怀疑头文件，用 ABI 层面规避（传指针地址当 DWORD），
不要手搓 vtable。

## 6. 生成桩会混进非 API 符号

从 `.lib` 里解导出名时，会带出 CRT/编译器内部件：
`RtlAlternateDosPathSeperator`、`HRESULT_FROM_WIN32`、`_XBLD_GRDK_*`、`PFX*` 私有前缀。

31 个这种符号曾让导出计数虚高（在 461 之外再多出 31 个非 API 名）。现在 `gen_stubs.py`
按前缀过滤。

## 7. `tempfile.mkdtemp` 建出来的目录可能与 `os.makedirs` 不可写

实测：`mkdtemp(dir="build")` 建的目录，往里写文件直接 `PermissionError`；
自己 `os.makedirs` 同样的路径则正常。

**规则**：受限环境里不要用 `mkdtemp`，自己 `makedirs` 一个带名字的目录。
