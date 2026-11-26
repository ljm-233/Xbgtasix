# 下一轮从哪里接（2026-11-26 收尾盘点）

## 现状一句话

GTA6（Xbox 版）在 Xbgtasix 路线上能跑 **约 13 秒 / 12725 行日志**，
造出 **54 个 compute PSO**，但**还没有窗口、交换链、Present**。
当前阻塞点是 vkd3d 内部（`clear_uav_builtin` / `CopyBufferRegion`）拿到空句柄。

## 必须先做的三件事（否则测的是环境噪声）

```bash
# 1) 按 comm 前缀杀残留（pkill -x 因 15 字符上限无效）
ps -eo pid=,comm= | awk '$2 ~ /^game_scar/ {print $1}' | xargs -r kill -9
ps -eo pid=,comm= | awk '$2 ~ /^wineserver/ {print $1}' | xargs -r kill -9
# 2) 两份着色器缓存都要清
rm -rf ~/.cache/vkd3d ~/.cache/mesa_shader_cache*
# 3) 确认可用内存 > 5 GB（游戏预留 24 GiB 并按页提交）
free -m
```

跑法（2~3 次取长跑，短跑 2648 行属于环境噪声）：

```bash
XBGTASIX_PATCH_ASSERT=247AF2D,19F4F58 XBGTASIX_PSO62_RET=hr \
  ./tools/run_wine.sh --dll-dir build/linux > LOG 2>&1
```

## 下一步的技术优先级

1. **描述符模型整体重做**（见 `host-walls-2026-11-26.md` §11）
   现在是"步进=0"这个错误前提支撑着 1.27 万行；步进/arena/翻译/视图创建必须一起改。
2. **按权威槽位表替换"同号转发"**（§15）
   已改：设备 17（CBV，默认关）、命令列表 146（增强 Barrier）、设备 36（Fence）。
   待改：设备 27 `CreateCommittedResource`、设备 38 `GetCopyableFootprints`、
   命令列表 16 `CopyTextureRegion`、队列 16 `Signal`、队列 35 提交入口（带 frameToken）。
3. **裸对象泄漏**：凡是 vkd3d 会强转类型的对象（分配器、资源、命令列表、堆），
   要么包 16 槽主机代理，要么在交给 vkd3d 前换回它自己的对象。
   已知反例：给分配器包代理会让启动直接死在 236 行。
4. **清空"空调用仿真"的历史包袱**：VEH 里"调用目标为 0 就当返回 S_OK"是权宜之计，
   真正的修法是让对象 vtable 足够长（主机布局），不要靠吞异常。

## 诊断工具（都已留在树里）

- `docs/host-walls-2026-11-26.md`：全部墙与证据（18 节）
- `XBGTASIX_SLOTPROBE=1`：设备/命令列表槽位首次调用记录
- `XBGTASIX_EXIT_HOOK=1`：ExitProcess 调用栈（默认关，挂上会引入 `<unknown>+0x820008` 崩溃）
- `XBGTASIX_SLOT57=1`、`XBGTASIX_CBV17=1`、`XBGTASIX_PHYS_LAZY=1`：单变量实验开关
- `tools/xref.py --addr`：反汇编 + 调用者定位（本轮定位 `FrameSubmissionQueue_Consume` 用的就是它）
