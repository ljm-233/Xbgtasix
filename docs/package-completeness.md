# 这份泄露包到底缺什么：只缺 1 个文件

用包自带的 `layout_*.xml`（makepkg 的清单，共 104 条）逐条核对：

```bash
python tools/packaging/verify_package.py <包目录>
```

```
清单条目: 104（来自 25 个 Chunk）
实际存在: 103 个，共 224.96 GB
**缺失: 1 个**
  Chunk 1001  game_scarlett_master.exe   （清单说来自 .\..\..\content\）
MicrosoftGame.Config 声明的可执行文件: game_scarlett_master.exe  **不存在**
```

## 这意味着什么

- 资产、着色器、音频、导航、UI **全部齐全**（103/104，共 224.96 GB）
- 唯一缺的是**可执行文件**，而且清单明确它本该来自 `.\..\..\content\`
- 也就是说：这不是"残缺的包"，是**刻意只留了代码之外的一切**

这解释了之前几个观测：

| 现象 | 现在的原因 |
|---|---|
| 全包扫不到 PE（`gameos.xvd` 348 MB 全量 0 个） | 唯一的 PE 就是那个 exe，而它不在 |
| `packs.rpf` 里 0 个 TXD | 贴图字典在 `data\ttdb\*.ttpf` 里，而那是**另一批文件**（也在清单内，已存在） |
| 服装贴图按名字匹配不到 | 服装贴图嵌在 `.#dr` 资源内，不在 rpf 字典里 |

## 顺带修正一个自查 bug

第一版校验器把 68 个文件误报成缺失——因为它按清单里的**打包后根路径**去找，
而泄露包是按**源目录结构**摊开的（例如 `packs/story000/story000.rpf`）。
改成"按 basename 建递归索引再查"之后，缺失数从 68 降到 1。

> 教训：校验工具自己也要被校验。68 与 1 的差别，就是"包不完整"和"缺一个 exe"的差别。

## 复现

```bash
python tools/packaging/verify_package.py "D:/GTA6/GTA6.Xbox.v1.0.2.1-CYBERLEEK"
```
