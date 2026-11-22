# 着色器语料统计（21,522 条）

数据来源：`shaders.rpf` 内的 `PS\x00\x09` 运行时缓存，用 PercyStar 1.3.6 导出索引：

```
PercyStar.exe --disasm-shaders shaders.rpf out/ 1
Loaded PS\x00\x09 (0x65F41EDD, drv 930.048.020): 21522 shaders, 25471 Gfx PSOs, 2693 Compute PSOs
```

统计脚本：[tools/packaging/shader_stats.py](../tools/packaging/shader_stats.py)

## 总量

| 指标 | 值 |
|---|---|
| 着色器数 | 21,522 |
| 字节码 | 238.8 MB |
| 指令总数 | 43,560,444 |

## 阶段分布

| 阶段 | 数量 | 占比 | 字节数 | 字节占比 |
|---|---|---|---|---|
| PS（像素） | 9,535 | 44.3% | 94.2 MB | 39.4% |
| **NGG（mesh shader）** | **7,498** | **34.8%** | 40.1 MB | 16.8% |
| CS（计算） | 3,082 | 14.3% | 97.5 MB | 40.8% |
| VS（顶点） | 1,343 | 6.2% | 3.7 MB | 1.6% |
| **RT（光追 BVH64）** | **64** | **0.3%** | 3.4 MB | 1.4% |

注意 CS 只占 14.3% 的数量，却吃掉 40.8% 的字节——计算着色器单个体积大。

## 主机特性依赖

| 特性 | 命中 | 含义 |
|---|---|---|
| mesh shader（NGG） | 7,498 / 21,522 = **34.8%** | 三分之一的着色器是 mesh shader，不是传统 VS/IA 管线 |
| 硬件光追（BVH64） | 64 个 | 光追不是可选装饰，是管线的一部分 |

## 描述符绑定

| 指标 | 值 |
|---|---|
| 中位数 | 14 |
| p90 | 46 |
| p99 | 98 |
| 最大 | 109 |

**p99 接近 100 个绑定槽**，说明是典型的 bindless / 大描述符表设计。PC 侧需要大容量
descriptor heap + 根签名的描述符表数组，不能按"每个资源一个 root parameter"来做。

## 指令规模

| 分位 | 指令数 |
|---|---|
| min | 2 |
| 中位 | 707 |
| p90 | 4,955 |
| p99 | 21,468 |
| max | 46,723 |

## Compute 线程组（top 8）

| [numthreads] | 数量 | 占 CS |
|---|---|---|
| 32x1x1 | 1,229 | 39.9% |
| （空） | 462 | 15.0% |
| 8x8x1 | 412 | 13.4% |
| 64x1x1 | 293 | 9.5% |
| 16x16x1 | 187 | 6.1% |
| 8x4x1 | 119 | 3.9% |
| 1x1x1 | 85 | 2.8% |
| 128x1x1 | 74 | 2.4% |

`32x1x1` 占主导，与 RDNA wave32 的宽度一致。

## 对"移植到 PC"的含义

1. **必须支持 mesh shader**：34.8% 的着色器走 NGG 路径。桌面 D3D12 侧需要
   `D3D12_FEATURE_D3D12_OPTIONS7::MeshShaderTier`，Vulkan 侧需要 `VK_EXT_mesh_shader`。
   vkd3d-proton 已支持 mesh shader 翻译；Turing（RTX 20 系）及以上原生支持。
2. **必须支持 DXR**：64 个 BVH64 着色器意味着光追是渲染管线的一部分，不能降级成光栅。
3. **描述符容量要按 bindless 设计**：p99 = 98 个槽，需要 GPU 可见的大堆 + 索引式访问。
4. **计算占比高**：CS 吃掉 40.8% 的字节码，说明有大量 GPU 驱动的后处理/剔除/降噪。


## 导出样本的质量（实测）

导出 80 条（CS/PS/VS 混合）与 3,660 条（覆盖到 NGG 区间，index 3443 起）后抽样验证：

| 检查 | 结果 |
|---|---|
| SPIR-V 校验（`spirv-val --target-env vulkan1.2`） | 80/80 通过；NGG 抽样 25/25 通过 |
| SPIR-V 版本 | 1.5，入口点类型正确（Fragment / GLCompute / MeshEXT） |
| HLSL 形态 | **寄存器级转写**，不是语义化反编译 |
| 字节码容器 | 明文，`PS\x00\x09` 头含 PSO hash、驱动版本（930.048.020） |

HLSL 的真实形态（像素着色器）：

```hlsl
Texture2D<float4> Tex_0 : register(t0); // slot=0xD off=0x0 dw=128
void PSMain(float4 pos : SV_Position, out float4 mrt0 : SV_Target0)
{
    float v[64]; uint s[64]; // RDNA2 Wave32 VGPR / SGPR register file
    s12 = exec_lo;
    v[4..7] = Tex_s24.SampleLevel(Samp_s20, float2(v2, v3), 0); // image_sample dmask=0xf
    ...
```

mesh shader（NGG）能正确识别成 `[outputtopology("triangle")]` + `MSMain`，
但函数体同样是寄存器级转写。

**怎么理解这个质量**：它保住语义、可被重新编译，但**不是可读源码**。
所以"自动翻译"足以支撑"能编译、能跑"，不足以支撑"能改"。

## 与 shim 的对应

`tests/t4_bindless` 用 98 槽描述符表 + 动态索引验证了语料里 p99=98 的绑定需求，
顺带确认 65,536 个描述符的 GPU 可见堆能建出来。

`tests/t5_mesh` 验证了 34.8% 那部分所依赖的能力：本机 `MeshShaderTier = 10`（Supported），
用 `D3D12_PIPELINE_STATE_STREAM_DESC` 建 mesh PSO、`ID3D12GraphicsCommandList6::DispatchMesh`
出图，中心像素 (105,130,146) 与清屏色 (5,5,5) 区分开。

`tests/t6_rt` 验证了 64 个 BVH64 所依赖的能力：本机 `RaytracingTier = 11`（1.1），
BLAS(384 B) + TLAS(448 B) + 状态对象 + SBT + `DispatchRays` 全通，中心像素绿（命中）、
角落像素蓝（未命中）。

> 踩坑记录：手写 stream 子对象时，类型字段（4 字节）后面必须按载荷自身对齐补 padding，
> 否则指针落在 +4 位置，D3D12 运行时用对齐指令读会直接 AV（0xC0000005）。

## 局限

- 只统计了 `PS\x00\x09` 缓存里的着色器；SGD5 着色器组（`--extract-sgd5`）另外算
- 索引不含名称（RDNA2 缓存按 PSO hash 索引，没有着色器文件名）
- 这是"能跑需要什么能力"的输入，不是"移植已完成"的证据——仍然缺 exe
