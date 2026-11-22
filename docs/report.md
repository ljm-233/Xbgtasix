# 技术报告：从一份 Xbox GDK 包到能跑起来的 API 层

> 本文是 [README](../README.md) 的叙事版，给不打算读代码的人看。
> 所有数字都来自本机实测，命令可复跑。

## 1. 目标与结论

**目标**：让 Xbox Series 上的 GDK 版 GTA6 在 PC 上启动。

**结论**：在当前材料下不可达，卡点只有一个 —— **缺 `game_scarlett_master.exe`**。
但围绕它做出来的东西是可交付的：

| 产物 | 状态 |
|---|---|
| Xbox GDK API 兼容层（3 个 DLL，512 个导出） | 跑通 L0–L9 |
| 可断言测试（9 个，全部离屏、无桌面可跑） | 全绿 |
| 数据面结论（RPF9 / 着色器语料 / 资产） | 有量化结论与工具 |

## 2. 为什么"缺 exe"是致命的

1. shim 提供的是 API，不是程序；没有调用者，它永远不会被触发
2. RPF9 的目录表加密（熵 7.994、16 字节对齐），公开工具都不解
3. 全包扫描确认无 PE：`gameos.xvd`（348 MB 全量）0 个，`levels/packs/data.rpf`
   抽样各 8×16 MB 共 0 个，`Catalog000.bin` 是退化 PE（entry=0）

## 3. 兼容层怎么做的

Windows 加载器本身就是 PE loader。把同名 DLL 放在 exe 旁边，导入解析/重定位/TLS/SEH
全部由系统完成 —— 要写的只是"这些 DLL 里该有什么"。

| DLL | 内容 | 导出数 |
|---|---|---|
| `xgameruntime.dll` | 14 个手写（含**真任务队列**与模拟用户）+ 478 个生成桩 | 492 |
| `xmem.dll` | `XMemAlloc/Free/VirtualQuery/GetWorkingSetStatistics`，64 KB 页语义 | 4 |
| `d3d12_xs.dll` | 4 个手写（带日志）+ 14 个原生转发到系统 `d3d12.dll` | 18 |

生成桩来自 GDK 官方导入库：`tools/gen_stubs.py` 自己解 COFF archive（MS 大端 / GNU 小端都处理），
剥 `__imp_`、丢弃 C++ 装饰名。任何未实现的调用会打印名字并退出 —— **日志就是待办清单**。

`XTaskQueue` 是真的：`ThreadPool` 模式起工作线程、按端口分派、`Terminate` 等待收尾。
`XUser` 是本地模拟用户，跑通 `AddAsync → AddResult → GetId → Close`。

## 4. 图形能力：每一条都由语料统计驱动

先统计 21,522 条 RDNA2 着色器，再按统计结果逐项验证：

| 能力 | 语料依据 | 验证 | 结果 |
|---|---|---|---|
| 基础管线 | — | `t1_triangle` | 像素 (106,131,143) |
| 纹理 + CBV + 深度 | — | `t2_textured` | 采样值逐位等于源数据 |
| **bindless** | 绑定槽 p99 = 98 | `t4_bindless` | 98 槽动态索引命中正确；64K 堆可建 |
| **mesh shader** | 34.8% 是 NGG | `t5_mesh` | `MeshShaderTier=10`，DispatchMesh 出图 |
| **光追 DXR** | 64 个 BVH64 | `t6_rt` | `Raytier=11`，命中绿/未命中蓝 |
| 真实资产 | — | `t8_model` | 导出模型占屏 26.9%，带光照 |

## 5. 数据面：能拿到什么

| 类型 | 结论 |
|---|---|
| 贴图 | 1354 个 TXD 可枚举导出，**名字来自载荷内部**（不是目录） |
| 着色器 | 21,522 条明文容器，可反汇编、lift 成 HLSL/SPIR-V（抽样 105 条全部通过 `spirv-val`） |
| 文本/XML | **自己实现的雕刻器**从 `common.rpf` 解出 200 个 zlib 载荷，其中 120 个是 XML |
| 视频/音频 | 明文的 Bink/ADAT 可定位（`data.rpf` 前 200 MB 内有 20 处 Bink） |

雕刻器（`tools/packaging/rpf_carve.py`）不依赖任何第三方工具：扫 `0x78` zlib 头、
解压、按魔数分类。解出来的 XML 里能看到 `$(common)/movies/...` 这样的路径 ——
**路径信息存在于载荷内部**，这是目录表之外的第二条线索。

## 6. 踩过的坑（都不报错，只黑屏或 AV）

1. mesh stream 子对象少插 padding → `0xC0000005`
2. root signature 缺 `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` → PSO `E_INVALIDARG`
3. HLSL 常量矩阵默认列主序 → 顶点塌成一点
4. 右手系视图矩阵第三行是 `-f` → 全被裁掉
5. 归一化矩阵平移项漏乘缩放 → 模型偏出画面
6. `list->Reset(nullptr, …)` → AV

完整清单与诊断顺序见 [graphics-pitfalls.md](graphics-pitfalls.md)。

## 7. 复跑

```bash
./build.sh
cd build && ./t0_hello.exe --smoke && ./t1_triangle.exe && ./t2_textured.exe \
  && ./t3_xmem.exe && ./t4_bindless.exe && ./t5_mesh.exe && ./t6_rt.exe && ./t9_async.exe
./t8_model.exe <model.obj> "" model.png
```

## 8. 还剩什么

| 缺口 | 性质 |
|---|---|
| 游戏 exe | 决定性，无法绕过 |
| `xmem.h` / `ID3D12DeviceX` | NDA，签名与 IID 靠推定 |
| RPF9 目录表 | 加密；但游戏自带解码器，不阻塞运行 |

**一句话**：API 层和图形能力已经"能跑真实 GDK 程序"的水平（缺的只是那个程序本身），
数据面能自己凿出内容，剩下的两个缺口都不是靠再努力能补的。
