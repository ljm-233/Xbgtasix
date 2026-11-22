# 显卡适配：选对卡 + 能力断言

## 发现的问题

这台机器有 **3 个适配器**，而 `D3D12CreateDevice(nullptr, ...)` 默认选中的是 **Intel 核显**：

```
[0] Intel(R) Graphics                     Intel      0.1 GB
[1] NVIDIA GeForce RTX 5060 Laptop GPU    NVIDIA     7.7 GB
[2] Microsoft Basic Render Driver         Microsoft  (软件)
```

默认 LUID `00014A65`（Intel） vs 独显 LUID `00014FB2`（NVIDIA）。

**在修之前，本仓库所有图形测试都跑在核显上** —— 也就是说之前那些 "PASS" 是在 iGPU 上得到的，
独显路径根本没被验证过。这类问题在笔记本上极其常见（游戏莫名其妙只有 30 帧）。

## 修法：在 shim 里替换 adapter

`d3d12_xs.dll` 的 `D3D12CreateDevice` 现在会在调用方传 `nullptr` 时替它挑卡：

- 默认 `best`：非软件适配器里**显存最大**的那块
- `XBGTASIX_ADAPTER=default`：保持系统默认
- `XBGTASIX_ADAPTER=0/1/2…`：按 DXGI 枚举下标强制指定

```
[d3d12_xs] 选卡 -> NVIDIA GeForce RTX 5060 Laptop GPU（7.7 GB）
[t0] device ok, adapter luid=00000000:00014fb2
```

## 跨 GPU 验证结果

同一套测试，在两块卡上各跑一遍（`XBGTASIX_ADAPTER=0` 强制核显）：

| 测试 | RTX 5060（默认） | Intel 核显 |
|---|---|---|
| `t1_triangle` | PASS | PASS |
| `t2_textured` | PASS | — |
| `t4_bindless`（98 槽） | PASS | — |
| `t5_mesh`（MeshShaderTier） | PASS（10） | PASS（10） |
| `t6_rt`（RaytracingTier） | PASS（**12**） | PASS（**11**） |
| `t9_async` | PASS | — |

**两个厂商、两种能力等级，全部通过。** 特别是：
- 光追等级不同（RTX 12 / Intel 11），测试两种都能过 —— 说明用例没有硬编码某一代的假设
- 现代 Intel 核显也支持 mesh shader（Tier 10），所以"核显一定跑不了"是过时的判断

## 能力断言（t13_caps）

`t13_caps` 把 `docs/shader-corpus.md` 里的需求变成可复跑的检查：

| 检查项 | 需求来源 | RTX 5060 结果 |
|---|---|---|
| 描述符表容量 Tier ≥ 2 | 绑定槽 p99 = 98 | PASS（Tier 3） |
| Wave 内建操作 | wave 级着色器 | PASS |
| 光追 DXR 1.0 | 64 个 BVH64 | PASS（Tier 12） |
| Mesh Shader | 34.8% 是 NGG | PASS（Tier 10） |
| 实际创建 98 槽 GPU 可见堆 | 同上 | PASS |

## 用法

```bash
./t13_caps.exe                       # 打印适配器清单 + 能力 + 对照结论
XBGTASIX_ADAPTER=0 ./t6_rt.exe       # 强制核显跑
XBGTASIX_ADAPTER=default ./t6_rt.exe # 用系统默认
```

## 还没做的适配

| 场景 | 现状 |
|---|---|
| 多卡（独显 + 核显） | ✅ 自动选独显，可强制指定 |
| 无光追的卡 | `t6_rt` 返回 2（SKIP）而不是失败 |
| 无 mesh shader 的卡 | `t5_mesh` 返回 2（SKIP） |
| 显存不足 | 未处理（真实游戏需要 residency 管理） |
| 不同驱动的着色器缓存 | 未处理（t6 首次运行 ~30s 是驱动编译光追管线） |
