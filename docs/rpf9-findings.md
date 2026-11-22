# RPF9 可读性调查（结论：目录解不开）

决定项目上限的一个问题：**能不能读出 RPF9 里的文件目录**。三条独立证据指向同一个结论。

## 证据 1：专杀工具自己承认解不开目录

`rpf9-explorer 1.0`（专门为 RPF9 写的工具）的 README 原文：

> **RPF9 support is partial. The original archive directory is not decoded**, and the app cannot
> rebuild a complete RPF9 archive.

> If an RPF9 archive has few results, **Scan entire archive** searches the full file for supported
> payloads and saves a local index. **It does not decode the original directory.**

也就是说它靠"在全文件里扫描可识别的载荷特征"（纹身、模型、音频）来凑出内容列表，
而不是解析目录表。这直接否定了"用第三方工具列目录"这条路。

## 证据 2：文件里没有明文目录，也没有明文着色器

| 检查 | 对象 | 结果 |
|---|---|---|
| `DXBC` / `DXIL` / `SHDR` / `RDEF` 魔数 | `shaders.rpf`（411 MB 全文件） | **全部 0 次** |
| 末尾若干长度区间的可读字符串 | `common.rpf` / `shaders.rpf` | 只有零散噪声，没有路径/文件名 |
| 熵 / 可打印字符占比 | 四个包 | 熵 6.4–7.8，可打印 0.25–0.33（纯文本应为 ≈4.5 / ≈0.99） |

## 证据 3：载荷里有明文，但只在少数位置

在 `audio.rpf` 前 210 MB 扫到 34 个 Bink 特征（`BIK`/`BK2`），`textures.rpf` 前 105 MB 扫到 6 个。
随机数据里 4 字节特征在 210 MB 中的期望命中约 0.05 次，所以这些是**真实载荷**。

但同时 `OggS` / `RIFF` / `DDS` / `PNG` 全是 0 —— 说明大部分内容不是裸格式，
而是压缩或加密后的块，只有少数容器头落在明文里。

## 结论

**RPF9 的目录表（TOC）无法从公开渠道解出**，与社区说法一致（密钥在缺失的
`game_scarlett_master.exe` 里）。这意味着：

1. 我们**无法重建游戏期望的虚拟文件系统** —— 没有路径到偏移的映射
2. 所以即使 shim 做到完美，游戏也会在"读第一个资源"处停住
3. `Xbgtasix` 当前的能力上限就是 **L0–L4 的 API 层 + 一套可断言的测试**，
   这部分是通用的、可复用的，不依赖任何游戏文件

## 还有什么能做

- **载荷雕刻（carving）**：按已知魔数扫描，能把 Bink 视频、部分贴图抠出来看。适合研究，
  但拼不回原始目录结构，也不能让程序跑起来
- **反向工程 RPF9 目录格式**：需要大量样本 + 已知明文对比。没有密钥的情况下，
  只能靠"同一文件不同版本形态"做差分 —— 成本极高，收益是"能看资产"，不是"能跑游戏"
- **把项目转成通用 GDK 兼容层**：这是唯一不依赖这条死路的产出方向

## 对项目定位的影响

原始目标（让泄露版跑在 PC 上）在缺 exe + 目录加密的双重约束下不可达。
可交付的部分是一个**通用的 Xbox GDK 兼容层骨架**：加载、内存、D3D12 入口、测试。
它的价值不依赖那个游戏，也不受 DMCA 影响。
