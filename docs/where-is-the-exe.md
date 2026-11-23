# 包里到底有没有 exe？—— 扫描结论：没有

`MicrosoftGame.Config` 声明的可执行文件是 `game_scarlett_master.exe`，但包内不存在。
为了排除"藏在某个容器里"的可能，用结构校验（`MZ` + 有效 `e_lfanew` + `PE\0\0`）
扫了全部文件（工具：[tools/packaging/find_pe.py](../tools/packaging/find_pe.py)）。

## 结果

| 对象 | 扫描范围 | PE 命中 |
|---|---|---|
| 16 个 `.dll` | 全文件 | 16（都在偏移 0，即平台 DLL 本身） |
| `gameos.xvd`（348 MB） | **全文件** | **0** |
| `levels.rpf`（76 GB） | 8 个采样点 × 16 MB | 0 |
| `packs.rpf`（4.8 GB） | 8 个采样点 × 16 MB | 0 |
| `data.rpf`（482 MB） | 8 个采样点 × 16 MB | 0 |
| `common.rpf` / `script.rpf` / `appdata0.rpf` | 全文件 | 0 |
| `audio.rpf` / `movies.rpf` / `anim.rpf` / `textures.rpf` | 前 48 MB | 0 |
| `Catalog000.bin` | 全文件 | 1（退化 PE，见下） |

## Catalog000.bin 不是可执行文件

它确实有一个 PE 头，但是退化的：

```
machine=0x8664 (x64)  sections=1  magic=0x20b (PE32+)  entry=0x0
section (无名字)  vsize=0  vaddr=0x1000  rsize=811008  raddr=0x1000
```

入口点为 0、段表无名字、`vsize=0` —— 这是一个**用 PE 外壳装的二进制目录**，
不是能执行的代码。

## 结论

**没有 exe，所以"反编译 exe"在当前材料下没有输入。**

`gameos.xvd` 也帮不上忙：它是 Xbox Game OS 的容器镜像（348 MB 全扫无 PE），
不是游戏本体，也不是可直接读取的 PE。

## 如果将来拿到 exe，分析路径是什么

1. **先看导入表**（最省力、收益最大）：`xgameruntime` / `d3d12_xs` / `xmem` 的具体函数清单，
   直接变成 shim 的待办列表 —— 现在的 143 个桩就是按"名字"铺的，拿到真导入表可以精确排序。
2. **找 RPF9 TOC 解码函数**：搜索 AES 常量与密钥派生逻辑，拿到它就能解目录
   （我们对 TOC 的独立观测：熵 7.994、16 字节对齐、长度是 16 的倍数，见
   [rpf9-findings.md](rpf9-findings.md)）。
3. **看启动序列**：`XGameRuntimeInitialize` → `XUserAddAsync` → `XTaskQueue` →
   `D3D12CreateDevice` 的真实调用顺序与参数，用来校准 shim 的默认返回值。
4. **静态重编译**（Leonida 路线）：机器码 → C，需要 CFG 恢复与间接跳转还原。

## 现在能"反编译"的其实是着色器，而且已经做了

GPU 侧 21,522 条 RDNA2 着色器已导出反汇编（`.asm`）、lift 的 HLSL 与 SPIR-V，
抽样 105 条全部通过 `spirv-val`。见 [shader-corpus.md](shader-corpus.md)。
