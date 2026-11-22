# 交接摘要（下次开工先看这个）

> 状态时间：2026-11-22 深夜。等 `game_scarlett_master.exe` 落地。

## 一句话

兼容层和图形能力已经做完（L0–L21 全绿），数据面能自凿；**唯一缺口是游戏 exe**。

## 复跑（30 秒）

```bash
cd D:/GTA6/Xbgtasix && ./build.sh
./tools/run_suite.sh          # 21 项测试一条命令，输出 PASS/SKIP/FAIL 表 + TSV 报告
```

期望：`21 PASS / 0 SKIP / 0 FAIL`。
换机器时若某项返回 2（SKIP），说明该机不支持那项能力（如无光追），不算失败。

## exe 到手后（照这个顺序）

```bash
python tools/analyze_exe.py <exe>     # 看缺多少导出
./tools/run_target.sh <exe>           # 取第一个未实现调用
```

细节与预期三堵墙见 [exe-day-checklist.md](exe-day-checklist.md)。

## 现在有什么

| 部件 | 位置 | 说明 |
|---|---|---|
| 兼容层 | `src/`、`build/*.dll` | 512 导出；14 手写（含真任务队列/用户），其余是"一调用就报名字"的桩 |
| 测试 | `tests/t0…t9` | 9 个，全部离屏、无桌面可跑 |
| 数据面工具 | `tools/packaging/` | RPF 探针/雕刻、着色器统计、PE 扫描 |
| 结论文档 | `docs/` | 技术报告 + 4 份证据表 + 踩坑清单 |

## 关键结论（避免重复劳动）

- 包内**没有 exe**：`gameos.xvd` 348 MB 全量 0 个 PE，RPF 抽样 0 个，`Catalog000.bin` 是退化 PE
- RPF9 **目录表加密**（熵 7.994、16 字节对齐），公开工具都不解；但**载荷**可雕刻（`common.rpf` 挖出 120 个 XML）
- 21,522 条着色器：**34.8% mesh shader、64 个 BVH64、绑定槽 p99=98** —— 这三项已各自对应一个通过的测试
- `xmem.h` 不在公开 GDK（NDA）；`XMem` 的 13 条语义断言在 [xmem-evidence.md](xmem-evidence.md)

## 别做

- 不碰破解 exe、密钥、授权伪造
- 不一次铺满几百个桩再跑（默认 `stop` 策略就是用来一次只暴露一个缺口的）
- 不原地改 `src/generated/`（重跑 `gen_stubs.py` 会覆盖）
