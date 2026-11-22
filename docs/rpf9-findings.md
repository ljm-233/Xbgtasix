# RPF9 资源包：能读到什么，读不到什么

结论先行：

| 层 | 状态 |
|---|---|
| 目录表（TOC，含文件名） | **加密，公开工具不解** |
| 载荷（贴图、着色器、视频、音频…） | **可扫描定位，多数可解码** |
| 名字 | 仅当载荷内部自带名字时可得（贴图有，音频流是哈希） |

对项目的影响：**不阻塞"让游戏跑"**（游戏自己带 TOC 解码器），只阻塞我们的工具链。

---

## 一、TOC 是加密的（我们独立验证的三条证据）

### 1. 头字段与布局

`magic = 0x52504639`，小端写作 `39 46 50 52`，即 `9FPR` → RPF9。
64 字节头的字段位置由公开工具 PercyStar 的发布说明给出：

```
+0x04  data_offset    目录区结束 / 数据区开始
+0x0C  names_len
+0x24  entry_count    顶层条目数
```

实测五个包：

| 文件 | data_offset | names_len | entry_count | 目录区长度 |
|---|---|---|---|---|
| appdata0.rpf | 864 | 64 | 2 | 800 |
| script.rpf | 608 | 128 | 2 | 544 |
| common.rpf | 38272 | 14864 | 418 | 38208 |
| shaders.rpf | 34368 | 160 | 3 | 34304 |
| navmeshes.rpf | 1920 | 1152 | 29 | 1856 |

`navmeshes.rpf` 正好 64.00 B/条，其余不是整数 → 记录**变长**（内嵌名字）。

### 2. 目录区是高熵且 16 字节对齐

```
common.rpf   TOC 长度=34016  16 的倍数? 是  块数=2126  重复块=0
             字节熵=7.994 bits/byte（随机=8.0）
```

长度是 16 的倍数 + 熵接近 8.0 + 零重复块 → **AES 一类的现代分组密码**，
不是压缩。同时 16–4096 偏移范围内没有任何可解 zlib 流，排除了"只是压缩"。

### 3. 所有公开工具都绕开目录

- `rpf9-explorer` README：**"The original archive directory is not decoded"**，
  它靠 "Scan entire archive" 在全文里找可识别载荷
- PercyStar README：**"Zero Circumvention / Zero Keys … Encrypted data is skipped untouched"**，
  配套脚本是 `rpf9_carve.py`（carver 而非 parser），面板只报 "Found 1354 TXDs" 而不是全部条目
- SixNG 对音频明说：**"Stream names are 29-bit hashes and are not recovered"**

## 二、载荷是可读的

| 类型 | 证据 |
|---|---|
| 贴图（TXd） | PercyStar `--dump-textures` 在 `textures.rpf` 枚举出 **1354 个 TXD**，导出 PNG 0 失败；文件名是真实资源名（`ffx_armor01_light01`、`pistol_glass_crack_decals`…），图像内容与名字吻合 |
| 着色器 | 容器 `PS\x00\x09` 明文，**21,522 条**可完整枚举与反汇编（见 [shader-corpus.md](shader-corpus.md)） |
| 视频 | `audio.rpf` 前 210 MB 扫到 34 个 Bink 特征（随机期望 0.05 次） |
| 音频 | SixNG 扫出 7,160 个音库 / 1.6 M 流（Opus 可转封装，名字是哈希） |
| 导航 | SixNG 解出 navmesh 与道路图（63,808 节点） |

贴图带名字 ≠ 目录解开了：**名字来自 TXd 载荷内部**。

## 三、结论修正记录（保留过程，避免重复踩坑）

1. 最初判断"目录加密、一切不可读"——对 TOC 正确，对载荷过于悲观
2. 看到 PercyStar 导出带名字的贴图后，误判为"目录可解"——错，那是载荷内嵌名
3. 读到 PercyStar 的 "Zero Keys / Encrypted data is skipped untouched" 与 `rpf9_carve.py` 后回到正确判断

教训：**"能导出带名字的资源"和"能解目录"是两件事**，判断前先看工具的导入方式和它的自我声明。

## 四、复现方式

```bash
python tools/packaging/rpf9_probe.py <archive.rpf>          # 头字段 + 熵图 + 零填充 + 魔数 + zlib 探测
PercyStar.exe --dump-textures <archive.rpf> <out> 40        # 贴图（需自备工具）
```
