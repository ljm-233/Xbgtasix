# Arch Linux 支持

## 先分清两件事

| 问题 | 状态 |
|---|---|
| **能不能编译**（在 Arch 上产出可用的 DLL） | ✅ 已验证：交叉编译脚本跑通，导出数与 Windows 版一致（461） |
| **能不能运行**（在 Wine/Proton 里跑起来） | ⚠️ 未验证：需要一台 Linux 机器；下面列出逐项风险 |

这个区分很重要：编译只是"产物能生成"，运行取决于 Wine 对 D3D12 / XAudio2 / 输入 的覆盖度。

## 路线选择：为什么不做原生 ELF 移植

三条路，代价差一个数量级：

| 路线 | 做法 | 代价 |
|---|---|---|
| **A. PE + Wine（推荐）** | 同一份源码，MinGW 交叉编译出 Windows DLL，Arch 上用 Wine/Proton 加载 | 小：改构建脚本 + 验证 |
| B. 原生 ELF + 自建 thunk 层 | 把 shim 编成 `.so`，自己实现 PE 加载与 SysV↔MS x64 调用约定转换 | 大：等于重写半个 Wine，且要处理 SEH/TLS |
| C. 静态重编译（Leonida 那种） | 反汇编 exe 重新生成 C 代码 | 极大 |

**选 A**，理由是：项目的价值在"461 个导出的行为"，而不在"用什么容器装它们"。
Wine 已经提供了容器，我们不该重造。B 路线的调用约定转换（Microsoft x64 前四个参数走
RCX/RDX/R8/R9，SysV 走 RDI/RSI/RDX/RCX）是所有麻烦的根源，让 Wine 处理即可。

## 构建（Arch 上）

```bash
sudo pacman -S mingw-w64-gcc python

git clone https://github.com/ljm-233/Xbgtasix && cd Xbgtasix
python tools/fetch_gdk.py --out ../gdk          # 从 nuget.org 取官方 GDK 头/库
./tools/build_linux_cross.sh                     # 产物在 build/linux/
```

实测（在 Windows 上用同一套 mingw 交叉编译，等价路径）：

```
0.60 MB  xmem.dll
0.76 MB  d3d12_xs.dll
3.11 MB  xgameruntime.dll        ← 导出数 461，与本机 Windows 构建一致（构建时快照，随代码变化；当前 Windows 构建为 3.32 MB）
```

### 依赖极窄，这是关键

三个 DLL 的运行时依赖只有：

```
xgameruntime.dll   KERNEL32.dll  ole32.dll  SHELL32.dll
xmem.dll           KERNEL32.dll
d3d12_xs.dll       dxgi.dll  KERNEL32.dll
```

`-static` 把 MinGW 运行库全链进去了，所以没有 `libwinpthread-1.dll` 之类的额外依赖。
Wine 对这几个 DLL 的实现是成熟的。

## 运行（Wine / Proton）

```bash
# 最简：把三个 DLL 与 exe 放同一目录
cp build/linux/*.dll "/path/to/game/"
cd "/path/to/game"
WINEPREFIX=~/.wine-gta6 wine ./game_scarlett_master.exe
```

`WINEPREFIX=~/.wine-gta6` 单独建前缀，避免污染默认前缀；也方便回滚。

D3D12 走 **vkd3d-proton**（Proton 自带，或 `vkd3d-proton` 包 + `VKD3D_CONFIG`）。
**这是最大的未知数**，见下表。

## 逐项风险表

| 能力 | Windows | Wine/Proton | 风险 |
|---|---|---|---|
| `KERNEL32`（文件/线程/内存/VirtualAlloc） | ✅ | ✅ 成熟 | 低 |
| `XMem`（我们自己的实现，只用 VirtualAlloc） | ✅ | ✅ | 低 |
| `d3d12` 基础管线（PSO/root sig/draw） | ✅ | ✅ vkd3d-proton 覆盖良好 | 低 |
| **mesh shader**（语料占 34.8%） | ✅ | ⚠️ vkd3d-proton 支持，但取决于 Vulkan 驱动与 `VKD3D_FEATURE_LEVEL` | **中高** |
| **DXR 1.1**（64 个 BVH64） | ✅ | ⚠️ vkd3d-proton 支持 ray query / RTPSO，覆盖面随版本变化 | **中高** |
| bindless（98 槽描述符表） | ✅ | ✅ 需要 `VK_EXT_descriptor_indexing`，现代驱动都有 | 中 |
| `dxgi` 交换链（t31/t32 验的路径） | ✅ | ✅ Wine 提供 `dxgi.dll` | 低 |
| `XInput`（手柄） | ✅ | ✅ Wine 通过 SDL 映射 | 低 |
| `GameInput.dll` | ✅ 系统自带 | ❌ **Wine 没有** | 中（`t35` 已按 SKIP 处理，不是崩溃） |
| `XAudio2_9`（音频） | ✅ | ⚠️ Wine 有 `xaudio2_7`，`_9` 覆盖不完整 | 中 |
| SAPI（我们实现的语音合成） | ⚠️ 部分 | ❌ Wine 的 SAPI 很薄，`CoCreateInstance(SpVoice)` 大概率失败 | 中（已按"取不到就报不可用"处理） |
| `XGameSave` / `XPackage`（我们自己的实现） | ✅ | ✅ 纯文件操作 | 低 |
| 显存预算查询（`IDXGIAdapter3`） | ✅ | ⚠️ vkd3d-proton 的实现可能返回值有限 | 中（`t14/t15` 已有 SKIP 分支） |

**结论**：基础渲染和平台层风险低；**mesh shader 与光追是真正的未知数**，
而这两项恰好是 GTA6 着色器语料里占比最高的两块（34.8% + 64 个光追着色器）。

## 代码里已经为 Linux 做的准备（不是临时加的）

这些设计让同一份二进制在 Wine 上能降级而不是崩溃：

1. **可选组件运行时解析**：`t35_gamepad` 用 `LoadLibrary + GetProcAddress` 找
   `GameInput.dll` / `XInput`，找不到就 SKIP。静态导入会变成 `0xC0000135` 起不来。
2. **能力门不虚报**：`XGameRuntimeIsFeatureAvailable` 对没实现的模块返回 false，
   游戏走降级路径而不是撞未实现桩。
3. **SAPI 失败即降级**：语音合成拿不到 `SpVoice` 就返回 `E_NOTIMPL` 并让
   `GetStreamData*` 明确报不可用，不会假装有音频。
4. **显存查询有 SKIP 分支**：拿不到 `IDXGIAdapter3` 时 `t14/t15` 返回 2。

## 还没做的（明确列出）

| 项 | 说明 |
|---|---|
| 在真 Linux 上验证 | 本文所有"已验证"仅限交叉编译；运行侧需要一台 Arch 机器 |
| CI 的 Linux 交叉编译 job | ✅ 已加：`linux-cross` job 在 `ubuntu-latest` 上装 `g++-mingw-w64-x86-64`、拉 GDK、跑 `build_linux_cross.sh`，并校验三个 DLL 的导出数为 461/4/18 |
| Proton 版本与 vkd3d-proton 参数 | 需要实机调 `VKD3D_CONFIG` / `PROTON_USE_WINED3D` 等 |
| 音频（XAudio2_9） | Wine 覆盖不完整，可能需要映射到 `xaudio2_7` 或直接静音降级 |

## 复现（交叉编译部分）

```bash
./tools/build_linux_cross.sh
# 或指定编译器/头文件位置
CXX=x86_64-w64-mingw32-g++ GDK_INC=/opt/gdk/include_clean ./tools/build_linux_cross.sh
```

---

## 在哪恢复（下次换到 Linux 时从这里开始）

**当前进度**：构建已验证（CI 的 `linux-cross` job 在真实 Ubuntu 上跑过，
三个 DLL 导出数 461/4/18 与 Windows 构建一致）。**运行侧完全未验证。**

换到 Linux 后按这三步走，不要重新调研：

```bash
# 1) 装环境 + 交叉编译（约 3 分钟，GDK 包 280MB 会缓存在 build/gdk/_pkg）
sudo pacman -S mingw-w64-gcc python wine
git clone https://github.com/ljm-233/Xbgtasix && cd Xbgtasix
python tools/fetch_gdk.py --out ../gdk
./tools/build_linux_cross.sh

# 2) 先验"能不能装进 Wine"（不含渲染）：跑最小的那个测试
WINEPREFIX=~/.wine-test wine build/t0_hello.exe --smoke
#    成功判据：打印 "smoke done with 0 failure"

# 3) 再验渲染（这一步才暴露 vkd3d 的真实覆盖度）
WINEPREFIX=~/.wine-test wine build/t1_triangle.exe
WINEPREFIX=~/.wine-test wine build/t5_mesh.exe      # mesh shader：高风险项
WINEPREFIX=~/.wine-test wine build/t6_rt.exe        # 光追：高风险项
```

**注意**：`build/*.exe` 是 Windows PE，Wine 能直接跑——这才是最快的探针，
不需要先等游戏。t0/t1 过了说明链路通，t5/t6 的结果直接回答"mesh 与光追在
vkd3d-proton 上到底行不行"。

**预期**（基于风险表）：

| 测试 | 预期 |
|---|---|
| t0 / t1 / t2 / t3 / t4（基础渲染与内存） | 大概率 PASS |
| t5 mesh / t6 光追 | **可能 FAIL 或 SKIP** —— 这就是要量出来的东西 |
| t14 / t15（显存预算） | 可能 SKIP（vkd3d 的 `IDXGIAdapter3` 覆盖有限） |
| t28 语音 | 大概率 SKIP（Wine 的 SAPI 很薄） |
| t35 手柄 | XInput 走 SDL 应该 PASS；GameInput.dll 不存在会 SKIP |

跑完把 `build/suite/report.txt` 存下来，那就是 Wine 覆盖度的第一份实测矩阵。
