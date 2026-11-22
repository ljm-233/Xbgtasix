# 交接摘要

写这份文档的目的：任何人（包括三个月后的自己）拿到这个仓库，能在十分钟内知道
**现在到哪一步了、下一步做什么、哪些事不要再重复做**。

---

## 一、当前状态（最后更新：2026-11-23）

| 项 | 值 |
|---|---|
| 提交 | 71 |
| 测试 | **33 PASS / 1 SKIP / 0 FAIL**（34 项，约 12 秒） |
| 导出面 | 461 = 265 手写 + 146 生成桩 + 50 个 GUID 数据 |
| 文档 | 20 份 |
| 代码 | 14,882 行 |
| CI | 三个 job 全绿；`windows-build` 从 NuGet 拉官方 GDK，完整构建 + 全套测试（约 4 分钟） |
| 干净克隆 | `git clone` → `./build.sh` → `./tools/run_suite.sh` 全部通过（已验证） |

那个 SKIP 是 `t28_speech`：语音合成器创建/枚举正常，但本机会话没有音频输出，`Speak` 失败。

---

## 二、已完成的能力（都有测试）

- **加载层**：三个 shim 与 Windows 加载器配合；16/16 真实游戏侧 DLL 加载成功
- **图形**：D3D12 管线、mesh shader、DXR 1.1、bindless 98 槽、窗口交换链、`ResizeBuffers`
- **内存**：XMem 64 KB 页语义、ABA 防护（隔离区）、泄漏可见
- **显存**：预算查询、变化通知 + 降级、`Evict`/`MakeResident` 实测闭环
- **异步/队列**：`XAsyncRun`、条件变量等待（54 µs）、waiter/monitor、复合队列 —— 家族桩清零
- **平台**：包挂载、存档（目录式 + 容器式）、用户、UI 面板、串流/捕获/网络、语音、
  `XLaunchUri`（真 `ShellExecute`）、无障碍字幕
- **启动链路**：`tools/launch.sh` 分类失败原因；`t30_fakegame` 排练整条启动序列

---

## 三、唯一缺口：exe

[docs/package-completeness.md](package-completeness.md) 用包自带 layout 清单核对：
**104 个条目只缺 `game_scarlett_master.exe` 一个**。

### exe 到手后的操作顺序

```bash
cd D:/GTA6/Xbgtasix
./build.sh                                   # 约 90 秒
python tools/analyze_exe.py <exe>            # 10 秒：导入面 vs 我们的导出面
./tools/launch.sh <exe> --timeout 180        # log 模式跑，拿 UNIMPLEMENTED 清单
```

然后按 [exe-day-checklist.md](exe-day-checklist.md) 的六步走：
静态核对 → log 模式跑 → 收敛未实现面（`grep UNIMPLEMENTED | sort | uniq -c`）→
`--stop` 模式暴露硬门槛 → 黑屏排查 → 记录三件套。

---

## 四、明确不做的事（不要再讨论）

| 不做 | 原因 |
|---|---|
| `XStore` 购买/授权链路（77 个桩） | 不伪造授权，是设计决定 |
| 需要 XBL token 的功能 | token 由微软服务按用户签发，离线不可能 |
| 主机统一内存 / residency 精确语义 | `ID3D12DeviceX` 在 NDA 内；已确认 `XMem*` 在两个公开 lib 里都是 0 符号 |
| 获取/破解 exe 或密钥 | 法律红线，且不解决"能不能跑"的问题 |
| 分发游戏资产 / 微软 GDK 二进制 | 许可问题（GDK 由 CI 从 NuGet 官方源取） |

---

## 五、不要再重复踩的坑

看这两份就够，都是实测记录：

- [graphics-pitfalls.md](graphics-pitfalls.md) —— **9 个"不报错只黑屏"的坑**
  （stream padding、IA flag、row_major、RH 视图、绕序、Z-up、flip 交换链、fence 重置）
- [env-gotchas.md](env-gotchas.md) —— **7 个环境/工具链坑**
  （沙箱按进程放行、`CreateDirectory` 不建中间层、bash 里能用 Win32 不能用的路径、
  git TLS 坏但 `gh api` 能用、mingw 头文件与运行时不符、生成桩混入非 API 符号、mkdtemp 不可写）

还有一条元教训（写在 [audit-2026-11.md](audit-2026-11.md)）：
**这个项目 5 次因为"凭记忆写签名"出错**（`XTaskQueueCreate` 少参数、
`XSystemGetDeviceType` 返回类型、`XPackageEnumerateChunkAvailability` 参数顺序、
`XGameUiTextEntryGetState` 少两参、`XErrorSetOptions` 参数个数）。
规则：**签名一律抄 `gdk/include_clean` 里的头文件**。

---

## 六、复跑与验证

```bash
./build.sh                    # 90 秒
./tools/run_suite.sh          # 34 项，12 秒
./tools/ci_build.sh           # CI 同款（无 GDK 也能验 xmem 子集）
python tools/selftest_tools.py  # Python 工具自检
python tools/check_docs.py      # 文档相对链接
bash tools/ci_watch.sh 240 --jobs  # 查 CI，最多等 4 分钟
```

## 七、目录速查

| 想做什么 | 看这里 |
|---|---|
| 加一个测试 | [CONTRIBUTING.md](../CONTRIBUTING.md) |
| 补一个导出 | `src/xgameruntime/*.cpp` + `exports*.txt`，然后 `rm -rf src/generated && ./build.sh` |
| 了解某条结论的依据 | `docs/*-evidence.md`（每条标 `[事实]/[推断]/[自定]`） |
| 知道还剩哪些桩 | `grep -c '=' src/generated/xgameruntime_exports.inc` + `docs/audit-2026-11.md` |
