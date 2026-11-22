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


## 结构探测（工具：tools/packaging/rpf9_probe.py）

对五个包扫头部与零填充区，得到一个可复核的布局模型：

| 文件 | f1 | f2 | f3 | 第一个零填充起点 | f1 == 起点 |
|---|---|---|---|---|---|
| script.rpf | 608 | 576 | 128 | 608 | 是 |
| shaders.rpf | 34368 | 34336 | 160 | 34368 | 是 |
| textures.rpf | 1936 | 1728 | 544 | 1936 | 是 |
| appdata0.rpf | 864 | 832 | 64 | 1360 | 否 (+496) |
| common.rpf | 38272 | 34080 | 14864 | 39968 | 否 (+1696) |

```
0x00  64 B   头部：magic(0x52504639) + f1 + f2 + f3
0x40  ?      目录区（高熵，无压缩容器特征）
f1    ...    零填充或一小段附加数据
0x1000 ...   载荷区（4 KB 对齐）
```

- `magic = 0x52504639`，小端写作字节 `39 46 50 52` —— 确认是 RPF9
- `f1` 在三个包里精确等于"零填充起点"，说明它是**目录区的结束偏移**
- `f2 = f1 - 32` 在 appdata0 / script / shaders 成立（common 与 textures 不成立）→ 疑似 32 字节 MAC
- 目录区（0x40..f1）**熵 7.4–7.6、无 zlib 流** → 加密，不是压缩
- 载荷区按 4 KB 对齐；audio.rpf 里能扫到明文 Bink 头 → 载荷侧并非全部加密

结论不变但更精确：**目录区加密，载荷是"部分明文 + 部分自定义压缩"**。
所以能雕刻出载荷，拿不到名字与路径。


## 公开进展（2026-11 检索）

比我们独立探测走得更远的两个开源工具：

**PercyStar**（rpatty-dev，Rust）v1.3.2 changelog 原文：

> **9FPR Header Parser:** Directly parses `entry_count` (+0x24), `data_offset` (+0x04), and
> `names_len` (+0x0C) in the RPF9 64-byte header

> … and proved **static-key fixed-IV 9FPR TOC encryption** across 4 archives

> **500 lifted PS_0x09 RDNA2 -> SPIR-V 1.5 shaders** validated … (500/500 vkCreateShaderModule)

还提供 `--dump-textures`（20 种 DXGI 格式 + Scarlett RDNA2 4KB/64KB 反 swizzle）
和 `data.rpf` 内的 GXT 文本表查看。

**SixNG**（holdmysocks）：

> sixng audio … finds the sound banks by scanning the archive: 7,160 banks, 1.6 M streams …
> **Stream names are 29-bit hashes and are not recovered**

它能导出 navmesh、道路图（63,808 节点）、音频（Opus 无损转封装），
但明确说明**名字无法还原**。

用公开字段重新读我们的包（`tools/packaging/rpf9_probe.py` 的读数）：

| 文件 | data_offset | names_len | entry_count@0x24 | TOC 长度 | 每条约 |
|---|---|---|---|---|---|
| appdata0.rpf | 864 | 64 | 2 | 800 | 400.00 |
| script.rpf | 608 | 128 | 2 | 544 | 272.00 |
| common.rpf | 38272 | 14864 | 418 | 38208 | 91.41 |
| shaders.rpf | 34368 | 160 | 3 | 34304 | 11434.67 |
| textures.rpf | 1936 | 544 | 22 | 1872 | 85.09 |
| data.rpf | 69728 | 22640 | 699 | 69664 | 99.66 |
| navmeshes.rpf | 1920 | 1152 | 29 | 1856 | 64.00 |

`navmeshes.rpf` 正好 64.00 B/条，其余不是整数 → 记录**变长**（内嵌名字）。
`entry_count` 看起来是**顶层条目数**，不是递归总数。

## 结论修订

之前写"目录无法解出"要收紧为：

- 目录区确实是加密的（我们独立验证过：熵 7.994、16 字节对齐、零重复块）
- 但**加密方案已被公开证明是"静态密钥 + 固定 IV"**，不是每份 exe 一个密钥
- 名字/路径仍未公开还原（SixNG 明说哈希不可逆，PercyStar 靠扫描）
- 所以：内容可拿（贴图、音频、navmesh、文本表、甚至着色器），**文件名仍是缺口**


## 对项目定位的影响

原始目标（让泄露版跑在 PC 上）在缺 exe + 目录加密的双重约束下不可达。
可交付的部分是一个**通用的 Xbox GDK 兼容层骨架**：加载、内存、D3D12 入口、测试。
它的价值不依赖那个游戏，也不受 DMCA 影响。
