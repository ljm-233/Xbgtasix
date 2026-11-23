# 音频与视频：依赖面与预期

结论先说：**这两块都不需要兼容层实现**，但都需要机器上有对应的系统组件。
下面每条都有实测依据（包内 DLL 的导入表 + 本机探测）。

## 视频（Bink）

包内只有 `Bink2Scarlett.dll` 一个视频相关模块。用 `objdump -p` 看它的导入表：

```
VCRUNTIME140.dll
xmem.dll              <- 我们提供
XAudio2_9.dll         <- 系统组件
api-ms-win-core-*     <- 系统
```

**没有**导入 `d3d12.dll` / `dxgi.dll` / `mfplat.dll` / 任何主机视频解码库（XVA/XVD）。
也就是说：

| 结论 | 依据 |
|---|---|
| 解码在 CPU 侧、由 Bink 自己完成 | 导入表里没有任何 GPU / 媒体基础 API |
| 画面上屏由游戏负责（把 Bink 输出的帧当纹理上传） | 同上，Bink 不碰 D3D12 |
| **不存在"主机专用视频解码器"这个坎** | 没有 XVA/XVD 之类导入 |

视频路径的风险因此集中在两点：`xmem.dll`（我们有）与 `XAudio2_9.dll`（系统），
以及游戏自己的上屏逻辑——后者归 D3D12 那套（见 graphics-pitfalls.md）。

## 音频（XAudio2_9）

`Bink2Scarlett.dll` 从 `XAudio2_9.dll` **只导入一个函数**：

```
DLL Name: XAudio2_9.dll
  Ordinal 1                       <- XAudio2Create
```

序号 1 对应 `XAudio2Create`（用 `objdump -p C:\\Windows\\System32\\XAudio2_9.dll` 核对过，
序号基为 1，名称表第 0 项即序号 1）。这是标准 PC 音频入口，不是主机专有接口。

| 项 | 本机实测 |
|---|---|
| `XAudio2_9.dll` | 有（602 KB，位于 System32） |
| `XAudio2Create` 可解析 | 是 |
| 波形输出端点 | 1 个 |

### 缺 XAudio2_9 会怎样

游戏在**加载 Bink 那一步**就失败：`0xC0000135`（找不到模块）。表现是"启动瞬间退出"，
不是黑屏、也不是没声音。`xbgtasix doctor` 现在会直接报出来：

```
音频          XAudio2_9 没有，波形输出端点 0 个
[!]           缺少 XAudio2_9.dll —— 包内 Bink2Scarlett.dll 导入它，
              游戏会在加载视频/音频模块时 0xC0000135
```

`xbgtasix check <exe>` 也会把它列进"依赖模块找不到"（它探测非 shim 依赖）。

### 没有音频端点会怎样

`waveOutGetNumDevs() == 0` 时 doctor 只警告、不判失败：游戏通常仍能跑，只是没声音，
或者卡在初始化音频（这种情况日志会停在音频相关 API 上）。

## 兼容层自己实现了什么

| 项 | 状态 |
|---|---|
| XAudio2 / WASAPI | **不实现**，用系统组件 |
| Bink 视频解码 | **不实现**，由包内 `Bink2Scarlett.dll` 自己做 |
| 语音合成（`XSpeechSynthesizer*`，16 个导出） | 实现过，走系统 SAPI；本机无音频输出时 `t28` SKIP |
| 串流/捕获（77 个导出） | 桩，返回 `E_NOTIMPL`；不涉及音视频编解码 |

## 已知未验证项（需要 exe）

1. **Bink 的 Scarlett 版本在 PC 上是否走软件路径**：导入表支持这个判断，但没有实跑过。
2. **5.1 / 空间音频**：主机游戏常按 5.1 混音；PC 端 XAudio2 的声道掩码若不同，
   可能出现声道错位。只能等真游戏播声音时才看得出来。
3. **视频播放与渲染的同步**：Bink 出帧节奏与游戏的 D3D12 present 节奏是否匹配，
   属于运行时行为。

## 复跑

```bash
build/xbgtasix.exe doctor          # XAudio2_9 + 端点
build/xbgtasix.exe check <exe>     # 依赖模块是否都能解析
```
