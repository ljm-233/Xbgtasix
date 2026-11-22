# 图形侧踩过的坑（每条都真实崩过或静默画不出东西）

记录这些是因为它们**都不会报错**，只会让画面全黑或进程 AV，排查成本极高。

## 1. mesh PSO 的 stream 子对象少插 padding → `0xC0000005`

手写 `D3D12_PIPELINE_STATE_STREAM_DESC` 时，每个子对象是 `[4 字节类型][载荷]`。
指针类载荷（`ID3D12RootSignature*`、`D3D12_SHADER_BYTECODE`）要求 8 字节对齐，
**C++ 编译器对 `alignas(void*)` 结构会自动插 4 字节 padding**，手写字节流必须复刻：

```
[类型:4][padding:4][载荷:8]     <- 正确
[类型:4][载荷:8]                <- 指针落在 +4，反序列化时 AV
```

`tests/t5_mesh` 的 `StreamBuilder` 里按 `alignof(T)` 补齐，注释写明原因。

## 2. root signature 没加 `ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT` → PSO `E_INVALIDARG`

带 `D3D12_INPUT_ELEMENT_DESC` 的图形 PSO，如果 root signature 的
`Flags` 里没有 `D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT`，
`CreateGraphicsPipelineState` 直接返回 `0x80070057`，且不告诉你哪一项错了。

> 现象：调试层不可用时只能看到 E_INVALIDARG；**先把不使用的子对象逐个去掉做二分**最快。

## 3. C++ 行主序矩阵送进 HLSL 要写 `row_major`

HLSL 的常量缓冲里矩阵默认按**列主序**解释。CPU 侧按行主序填的 `float4x4`
如果声明成普通 `float4x4`，GPU 会把它当转置矩阵用 —— 顶点全部塌到一个点，
**画面纯背景、无任何报错**。

```hlsl
cbuffer Cam : register(b0) { row_major float4x4 g_mvp; };
```

## 4. 右手系视图矩阵第三行是 `-f`

行向量约定下，视图矩阵：

```
[ s.x  s.y  s.z  0 ]
[ u.x  u.y  u.z  0 ]
[ -f.x -f.y -f.z 0 ]
[ -dot(s,eye) -dot(u,eye) dot(f,eye) 1 ]
```

写成 `+f` 会让可见点落到相机背后：`w = -z_view` 变负，被裁剪掉，
表现同样是**画面全空**。诊断方法：把顶点投到 NDC 打印 `w`，`w<0` 就是这个问题。

## 5. 归一化矩阵的平移项也要乘缩放

`p' = (p - center) * s`，行向量矩阵里平移行必须写 `-center * s`。
漏乘 `s` 不会崩，只是模型偏出去一点 —— 小模型看不出来，大模型就飞出画面。

## 6. 顺时针绕序 → 面法线朝内 → 整体发暗

RAGE 导出的网格是**顺时针**绕序。用 `cross(b-a, c-a)` 求面法线会得到朝内的方向，
半兰伯特光照下 `N·L ≈ 0`，整模型发暗、像背光。修法二选一：

- 法线取反（`n = -cross(b-a, c-a)`）
- 或者把 `FrontCounterClockwise` 设 TRUE 并开背面剔除

`tests/t8_model` 两者都做了：默认平直着色 + 法线翻转 + `CULL_BACK`，
`XBGTASIX_T8_SHADING=smooth` / `XBGTASIX_T8_CULL=0` 可切回对照。

## 7. Z-up → Y-up 转错了 90° 的方向 → 模型上下颠倒

行向量约定下绕 X 轴旋转：

```
y' = y*cosθ + z*sinθ
z' = -y*sinθ + z*cosθ
```

要求把 Z 轴朝上转成 Y 轴朝上（即 `y' = z`），必须取 **θ = +90°**：

| θ | 结果 | 现象 |
|---|---|---|
| +90° | y' = z | 正确（Z 朝上 → Y 朝上） |
| −90° | y' = −z | 模型**上下颠倒** |

后者不会报错、也不会空屏，只是整个模型倒过来 —— 和"绕序反了导致发暗"一样，
属于**看着像渲染成功、其实方向错了**的静默错误。


## 8. flip 模型下永远写 buffer 0 → 第二帧 DEVICE_REMOVED

`CreateSwapChainForHwnd` 用 `DXGI_SWAP_EFFECT_FLIP_DISCARD` 时，back buffer 会轮换。
每帧必须 `swap3->GetCurrentBackBufferIndex()` 取当前索引，用对应的资源与 RTV；
一直写 buffer 0 会在第二帧 Present 时拿到 `0x887A0005 (DXGI_ERROR_DEVICE_REMOVED)`。

**症状很像"驱动崩了"**，实际是资源状态用错。

## 9. 重置 allocator 前必须等 GPU 用完

`ID3D12CommandAllocator::Reset()` 在 GPU 仍在执行引用它的命令列表时调用，
同样会得到 `DEVICE_REMOVED`（不是断言、不是崩溃）。

**正确姿势**：每帧 `Signal(fence, ++fv)` + `SetEventOnCompletion` + `WaitForSingleObject`，
再 `alloc->Reset()`。`t31_swapchain` 里两帧之间就是走这个流程。

## 诊断顺序（省时间的那个）

1. 先用已知能画的 shader（`SV_VertexID` 三角形）跑同一套 RT/PSO → 确认管线通
2. 用"顶点直出"变体（位置×3、uv 当颜色）→ 确认 VB/IB/输入布局通
3. 用"硬编码矩阵"变体 → 确认是不是常量缓冲的问题
4. 最后再怀疑矩阵数学

`tests/t8_model` 里保留了 `XBGTASIX_T8_MODE=1..7` 这几档诊断开关，就是按这个顺序排的。
