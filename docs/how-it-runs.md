# Xbox 程序怎么跑在 PC 上（机制说明）

## 一句话

**不写 PE loader、不模拟主机。** Xbox Series 的产物就是标准 Windows x64 PE，
我们用"同名 DLL 放在 exe 旁边"让 Windows 加载器自己完成绑定——我们只负责那些 DLL
里面该有什么。

## 证据：这些确实就是普通 PE

包内 16 个 DLL 逐个解析头：

| 文件 | machine | 格式 | 子系统 | DllCharacteristics |
|---|---|---|---|---|
| `Bink2Scarlett.dll` | `0x8664`（x64） | PE32+ | GUI (2) | `0x0160` |
| `XCurl.dll` | `0x8664` | PE32+ | Console (3) | `0x4160` |
| `XboxBvhBuilder.dll` | `0x8664` | PE32+ | GUI (2) | `0x0160` |
| `amd_fidelityfx_upscaler_xbox.dll` | `0x8664` | PE32+ | Console (3) | `0x0160` |

- **machine `0x8664`** = x86-64，和 PC 一样
- **PE32+** = 标准 64 位 PE
- 子系统是 GUI / Console，都是 Windows 合法值
- `DllCharacteristics` 里只有 `TERMINAL_SERVER_AWARE(0x4000)` 之类，没有加载器会拒绝的位

也就是说：**Windows 加载器不需要任何改造就能接受它们。**

## 依赖是怎么被满足的

```
game_scarlett_master.exe
 ├─ KERNEL32 / USER32 / d3d12 / dxgi / XAudio2_9 …     ← Windows 自带
 ├─ concrt140 / msvcp140 / vcruntime140 …              ← 包内自带（VC 运行库）
 ├─ Bink2Scarlett.dll                                  ← 包内自带
 ├─ amd_fidelityfx_*.dll                               ← 包内自带
 ├─ xgameruntime.dll   ← 我们提供（461 导出）
 ├─ xmem.dll           ← 我们提供（4 导出）
 └─ d3d12_xs.dll       ← 我们提供（18 导出，14 个转发系统 d3d12）
```

四个环节全部由**操作系统的加载器**完成，我们不做重定位、不做导入修补：

1. 读 PE 头、映射节区
2. 按名字递归解析依赖（同目录优先）
3. 修 IAT：每个导入名在对应 DLL 的导出表里查；**查不到就是 `0xC0000139`**
4. 跑 TLS 回调与 DllMain

我们能控制的只有第 3 步的**名字集合**——所以"兼容层"的实质是
**461 个名字 + 它们的行为**。

### 一个反直觉的实测结果

包内 **16 个 DLL 里没有任何一个导入 `xgameruntime.dll`**。
也就是说：`xgameruntime` 的 461 个导出，消费方**只有那个缺失的 exe**。
这也是为什么"缺 exe"这件事无法用别的文件替代——它不是缺一个组件，是缺唯一的调用方。

## exe 到手后的确切流程

```bash
# 0) 把三个 shim 编出来
cd D:/GTA6/Xbgtasix && ./build.sh

# 1) 静态对表：它要的符号我们有吗（10 秒，不用跑）
build/xbgtasix.exe check "<包>/game_scarlett_master.exe"
#    权威实现；没有构建产物时可用纯 Python 的 python tools/analyze_exe.py <exe> 顶替
#    看到 PASS 就可以往下；否则先按列出的名字补桩

# 2) 拉起：建 staging、拷 shim、设环境、跑、按退出码分类
./tools/launch.sh "<包>/game_scarlett_master.exe" --timeout 180

# 3) 拿到待办清单
grep UNIMPLEMENTED build/launch/xbgtasix.log | sort | uniq -c | sort -rn
```

第 2 步会把 exe 复制进 `build/launch/`，同目录放三个 shim——
因为 Windows 的依赖搜索**先看可执行文件所在目录**，这是整套办法成立的关键。

## 五个检查点，每处都可能失败

| # | 检查点 | 失败表现 | 判据 / 对策 |
|---|---|---|---|
| 1 | PE 头合法 | `0xC000007B`（映像格式无效） | `check` 能解析即可 |
| 2 | 依赖模块都能找到 | `0xC0000135`（找不到模块） | 把包内同名 DLL 拷进 staging |
| 3 | 导入符号都存在 | `0xC0000139`（入口点未找到） | `check` 的缺失列表；补桩 |
| 4 | DllMain / TLS 不炸 | 启动瞬间崩溃 | 日志最后一行 + 栈 |
| 5 | 行为正确（不崩但不干活） | 窗口出现但黑屏 / 卡在异步 | `graphics-pitfalls` 9 条 + `UNIMPLEMENTED` 清单 |

前三个是**机械问题**，有工具能在十秒内定性；后两个才是真正耗时的。

## 这套办法会在什么情况下失效

| 情况 | 说明 |
|---|---|
| 产物不是 PE（如 `.xex` / `.xvc` 包） | 那就要先解包；本包 layout 明确写的是 `.exe`，所以按 PE 处理 |
| 签名强制校验 | 本机运行时一般不做；若游戏自己校验自身签名，那需要另外处理 |
| 依赖主机专有接口且无法降级 | 例如 `ID3D12DeviceX`（NDA）；目前没证据说 exe 用了它 |
| 反调试/完整性校验 | 未知，只能跑了才知道 |

## 一句话总结

**容器不用造（Windows 的加载器就是），要造的是容器里的东西。**
461 个导出、16/16 真实 DLL 可加载、40 步测试，都是在证明"容器里的东西够用"——
但"够不够用"的最终判据只有一个：那个 exe 跑起来。
