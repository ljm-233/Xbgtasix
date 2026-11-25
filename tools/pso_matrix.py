#!/usr/bin/env python3
"""pso_matrix —— "Vulkan 路线能不能吃下这批游戏 shader"的体检单。

它做三件事，重复跑就是回归：

 1. 从落盘的 53 个 `cs_*.bin` 里读出各自的 RTS0（内嵌根签名），剥掉主机标志位，
    与 17 个 `rs_*.bin` 逐字节比对，得出 cs → rs 的确定映射（不猜、不遍历全组合）。
 2. 用 mingw 编出 `tools/pso_matrix/pso_matrix.cpp`（Linux 上经 wine 跑编译器）。
 3. 在**指定的 d3d12.dll**（默认盘上那份 vkd3d-proton）上逐个建 PSO，
    每一条**单独开一个进程**，把 HRESULT、失败原因、耗时写成 TSV。

为什么一个 shader 一个进程
--------------------------
驱动在 `vkCreateComputePipelines` 里崩（SIGILL/SIGSEGV）会把整个进程带走 ——
这正是 `~/.local/lib/vkguard` 想拦的那种事故。一个进程跑 53 条的话，第一条崩掉
之后剩下的全是"连带失败"，而且拿不到名单（`docs/rts0-analysis.md` §1 就是这么丢的）。
分离进程后：崩了只记 `crash`，原因取 stderr，其余 52 条照常出结果。

用法：
    python3 tools/pso_matrix.py                    # 两颗 GPU 各跑一遍
    python3 tools/pso_matrix.py --icd intel        # 只跑 Intel/Mesa
    python3 tools/pso_matrix.py --diff             # 与上次结果对比，只打印变化的行
    python3 tools/pso_matrix.py --rebuild          # 强制重编 pso_matrix.exe

环境上两个硬前置（本机实测踩出来的，脚本已自动处理）：
  * `VK_LAYER_VKGUARD_compute`（`~/.local/lib/vkguard`）在 instance 创建之前把
    instance 级函数的查询一律返回 NULL，任何 Vulkan 程序都会 core dump —— 关掉它
    （`VK_LOADER_LAYERS_DISABLE`）。
  * `VkLayer_LSFGVK_frame_generation` 的 .so 与本机 loader 不兼容（只是告警），
    用包自带的 `DISABLE_LSFGVK=1` 关掉，免得日志被噪声淹没。
"""

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DUMPS = ROOT / "build" / "dumps"
OUTDIR = ROOT / "build" / "pso_matrix"
MINGW = Path("/mnt/winbot-software/mingw64/bin/x86_64-w64-mingw32-g++.exe")
VKD3D_D3D12 = Path("/mnt/winbot-software/GTA6/vkd3d-src/build/libs/d3d12/d3d12.dll")

# 单条 shader 的上限：正常 1–3 s，卡住就是驱动在等（别让一条拖住整张表）
PER_SHADER_TIMEOUT = 60

ICDS = {
    "intel": "/usr/share/vulkan/icd.d/intel_icd.json",
    "nvidia": "/usr/share/vulkan/icd.d/nvidia_icd.json",
}

# RTS0 头部：+20 是 Flags，主机标志位 0x03000000（docs/rts0-analysis.md §3）
RTS0_FLAGS_OFFSET = 20
PC_KNOWN_FLAGS = 0x7F          # src/d3d12_xs/root_sig.h 的 kPcKnownFlags

# stderr 里的原因归类：先匹配到哪条就用哪条，都不中则保留原始尾行
REASONS = [
    (re.compile(r"dxil-spirv:\s*(.+)"), "dxil-spirv: %s"),
    (re.compile(r"(Required WaveSize[^\n]*)"), "%s"),
    (re.compile(r"(Unsupported[^\n]*)"), "%s"),
    (re.compile(r"(Invalid[^\n]*)"), "%s"),
]


def wpath(p):
    """POSIX 绝对路径 → Wine 的 Z: 形式（PE 程序只认 Windows 路径）。"""
    return "Z:" + str(Path(p).resolve()).replace("/", "\\")


def parse_dxbc(blob):
    """返回 {fourcc: payload}；只认 DXBC 容器（rs_*.bin 与 cs_*.bin 都是）。"""
    if len(blob) < 32 or blob[:4] != b"DXBC":
        raise ValueError("不是 DXBC 容器")
    count = int.from_bytes(blob[28:32], "little")
    if count > 64:
        raise ValueError("chunk 数离谱")
    chunks = {}
    for i in range(count):
        off = int.from_bytes(blob[32 + 4 * i: 36 + 4 * i], "little")
        size = int.from_bytes(blob[off + 4: off + 8], "little")
        chunks[blob[off:off + 4].decode("ascii", "replace")] = blob[off + 8: off + 8 + size]
    return chunks


def strip_host_flags(rts0):
    """把主机标志位从副本上抹掉，剩下的才是桌面那套会接受的形状。"""
    if len(rts0) < RTS0_FLAGS_OFFSET + 4:
        return rts0
    flags = int.from_bytes(rts0[RTS0_FLAGS_OFFSET: RTS0_FLAGS_OFFSET + 4], "little")
    out = bytearray(rts0)
    out[RTS0_FLAGS_OFFSET: RTS0_FLAGS_OFFSET + 4] = (flags & PC_KNOWN_FLAGS).to_bytes(4, "little")
    return bytes(out)


def build_mapping(dumps):
    cs_files = sorted(dumps.glob("cs_*.bin"))
    rs_files = sorted(dumps.glob("rs_*.bin"))
    if not cs_files or not rs_files:
        raise SystemExit("找不到 dump：%s（先设 XBGTASIX_DUMP_SHADERS 跑一次游戏）" % dumps)

    rs = {}
    for f in rs_files:
        try:
            rs[f.name] = parse_dxbc(f.read_bytes())["RTS0"]
        except (ValueError, KeyError) as e:
            print("跳过 %s：%s" % (f.name, e), file=sys.stderr)

    pairs, unmatched = [], []
    for f in cs_files:
        try:
            rts0 = parse_dxbc(f.read_bytes())["RTS0"]
        except (ValueError, KeyError) as e:
            unmatched.append((f.name, "parse:%s" % e))
            continue
        want = strip_host_flags(rts0)
        # 先比剥过的，再比原样（rs 侧可能本来就是剥过的，也可能相反）
        hit = next((n for n, blob in rs.items() if blob == want), None) or \
              next((n for n, blob in rs.items() if blob == rts0), None)
        if hit:
            pairs.append((f.name, hit))
        else:
            unmatched.append((f.name, "no-rs-match(len=%d)" % len(rts0)))

    for name, why in unmatched:
        print("未匹配 %s：%s" % (name, why), file=sys.stderr)
    used = {rs_name for _, rs_name in pairs}
    print("映射：%d 个 shader → %d 个不同根签名（%d 个 rs 未被引用）"
          % (len(pairs), len(used), len(rs) - len(used)))
    return pairs, len(rs)


def ensure_exe(rebuild):
    # ★ exe 必须和两个 dll 放在同一个目录：wine 的 LoadLibraryA("d3d12core.dll") 走
    #   "exe 所在目录 → 系统目录" 的顺序，exe 在别处就会加载 wine builtin 那个空壳，
    #   报 "unimplemented function d3d12core.dll.D3D12GetInterface"（踩过，很迷惑）。
    exe = OUTDIR / "bin" / "pso_matrix.exe"
    src = ROOT / "tools" / "pso_matrix" / "pso_matrix.cpp"
    if exe.exists() and not rebuild and exe.stat().st_mtime > src.stat().st_mtime:
        return exe
    exe.parent.mkdir(parents=True, exist_ok=True)
    if not MINGW.exists():
        raise SystemExit("找不到 mingw：%s" % MINGW)
    # -static：这个 exe 不链任何导入库（全走虚表），整静态最省事；
    # 少了它会要求 libwinpthread-1.dll —— wine 里跑直接 loader_init 失败（踩过）。
    cmd = ["wine", str(MINGW), "-std=c++17", "-O2", "-static",
           "-o", wpath(exe), wpath(src)]
    print("编 pso_matrix.exe …")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not exe.exists():
        sys.stderr.write(r.stdout[-3000:] + r.stderr[-3000:])
        raise SystemExit("编译失败")
    return exe


def stage_dlls(d3d12):
    """把 d3d12.dll 和 d3d12core.dll 放到同一个目录：vkd3d 按"自己所在目录"找 core。

    build 布局里两者分别在 libs/d3d12 和 libs/d3d12core，不搬到一起就找不到
    （`\\d3d12core.dll` + `LOAD_WITH_ALTERED_SEARCH_PATH` 只搜 d3d12.dll 自己那一层）。
    """
    stage = OUTDIR / "bin"
    stage.mkdir(parents=True, exist_ok=True)
    core = d3d12.parent.parent / "d3d12core" / "d3d12core.dll"
    for src in (d3d12, core):
        dst = stage / src.name
        if not dst.exists() or dst.stat().st_size != src.stat().st_size:
            dst.write_bytes(src.read_bytes())
    return stage / "d3d12.dll"


def run_env(icd):
    env = dict(os.environ)
    env["VK_LOADER_LAYERS_DISABLE"] = "VK_LAYER_VKGUARD_compute"
    env["DISABLE_LSFGVK"] = "1"
    # 必须同时覆盖 d3d12core：d3d12.dll 只是转发层，实现全在 core 里；
    # 不覆盖时 wine 会加载自己那个 builtin 空壳（报 unimplemented D3D12GetInterface）。
    env["WINEDLLOVERRIDES"] = "d3d12core,d3d12=n,b"
    env["WINEDEBUG"] = "-all"          # 关掉 wine 的全部 fixme/ern，日志只留 vkd3d 的
    env["VKD3D_DEBUG"] = "err"
    env["VKD3D_SHADER_DEBUG"] = "err"
    if icd:
        env["VK_ICD_FILENAMES"] = ICDS[icd]
    else:
        env.pop("VK_ICD_FILENAMES", None)
    return env


def extract_reason(stderr_text):
    """从 vkd3d stderr 里捞一条最有信息量的原因。"""
    keep = []
    for line in stderr_text.splitlines():
        if "err:" not in line and "warning:" not in line:
            continue
        if "vkguard" in line:
            continue
        body = line.split(":", 4)[-1].strip() if line.count(":") >= 4 else line.strip()
        body = re.sub(r"^vkd3d-proton:", "", body).strip()
        for pat, fmt in REASONS:
            m = pat.search(body)
            if m:
                return (fmt % m.group(1)).strip()[:110]
        if "err:" in line:
            keep.append(body[:110])
    return keep[-1] if keep else ""


def run_one(exe, stage_d3d12, cs, rs, icd, tmp_pairs):
    """cs / rs 是完整路径；单条 shader 一个进程。"""
    pair_path = tmp_pairs / ("%s.tsv" % cs.stem)
    pair_path.write_text("%s\t%s\n" % (wpath(cs), wpath(rs)),
                         encoding="utf-8")
    cmd = ["wine", str(exe), "--d3d12", wpath(stage_d3d12), "--pairs", wpath(pair_path)]
    # vkd3d 会把编好的 PSO 写进 cwd 的 vkd3d-proton.cache。**这会把矩阵污染掉**：
    # 上一次（比如诊断模式下）编过的管线会被下次直接命中，于是"没实现也能 ok"。
    # 所以每条 shader 都跑在自己的空目录里，缓存永远是冷的。
    run_cwd = tmp_pairs / ("cwd_%s" % cs.stem)
    run_cwd.mkdir(exist_ok=True)
    for stale in run_cwd.glob("vkd3d-proton.cache"):
        stale.unlink()

    t0 = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, env=run_env(icd),
                            timeout=PER_SHADER_TIMEOUT, cwd=str(run_cwd))
        rc, out, err = r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        rc, out, err = 124, "", "timeout"
    secs = time.time() - t0

    # stderr 全存下来：失败分类、以后换读法都不用重跑（表是回归依据，原始日志也得留）
    (tmp_pairs / ("%s.err" % cs.stem)).write_text(err, encoding="utf-8", errors="replace")

    row = None
    for line in out.splitlines():
        if line.startswith("cs_") and line.count("\t") >= 8:
            row = line.split("\t")
    if row is None:
        # 没出结果行：要么崩了，要么设备都没建出来
        reason = extract_reason(err)
        if "unimplemented function d3d12core" in err:
            # 环境问题，不是 shader 问题：把它单独标出来，别污染表
            verdict, reason = "env-fail", "加载到 wine builtin d3d12core（WINEDLLOVERRIDES 没生效）"
        elif rc == 124:
            verdict = "timeout"
        elif rc < 0 or rc > 1:
            verdict = "crash"
        else:
            verdict = "no-device"
        return ["%s" % cs.name, rs.name, "", "", "", "", "", "", verdict, reason, "%.2f" % secs]
    while len(row) < 9:
        row.append("")
    verdict = row[8] if row[8] in ("ok", "fail") else row[8]
    reason = "" if verdict == "ok" else extract_reason(err)
    return row[:9] + [reason, "%.2f" % secs]


HEADER = ["cs", "rs", "shader_len", "rs_len", "rs_hr", "pso_hr", "device", "retried",
          "verdict", "reason", "secs"]


def read_rows(path):
    rows = {}
    if not path.exists():
        return rows
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("cs\t") or line.startswith("#"):
            continue
        f = line.split("\t")
        if len(f) >= 9:
            rows[f[0]] = f
    return rows


def summarize(path):
    rows = read_rows(path)
    if not rows:
        print("  ⚠ 没有结果行")
        return
    ok = [k for k, v in rows.items() if v[8] == "ok"]
    bad = {k: v for k, v in rows.items() if v[8] != "ok"}
    print("  ok=%d  非 ok=%d" % (len(ok), len(bad)))
    groups = {}
    for k, v in bad.items():
        key = (v[8], v[9] or v[5] or "-")
        groups.setdefault(key, []).append(k)
    for (verdict, reason), names in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        idx = ", ".join(n.split("_")[1] for n in sorted(names))
        print("  %-9s %-52s ×%d  [cs %s]" % (verdict, reason[:52], len(names), idx))
    slow = sorted(rows.items(), key=lambda kv: -float(kv[1][10] or 0))[:3]
    print("  最慢：%s" % ", ".join("%s %.1fs" % (k.split("_")[1], float(v[10] or 0))
                                 for k, v in slow))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dumps", default=str(DUMPS))
    ap.add_argument("--d3d12", default=str(VKD3D_D3D12))
    ap.add_argument("--icd", choices=list(ICDS) + ["all"], default="all")
    ap.add_argument("--rebuild", action="store_true")
    ap.add_argument("--diff", action="store_true", help="与上次结果对比，只打印变化的行")
    ap.add_argument("--limit", type=int, default=0, help="最多跑 N 个（分块跑，别一整轮跑完）")
    ap.add_argument("--start", type=int, default=0, help="从第 N 个开始（分块跑）")
    ap.add_argument("--resume", action="store_true",
                    help="跳过 result_<icd>.tsv 里已有确定结论的，只补没跑的（可反复重入）")
    args = ap.parse_args()

    dumps = Path(args.dumps)
    if not dumps.exists():
        raise SystemExit("找不到 dump 目录：%s" % dumps)

    pairs, rs_total = build_mapping(dumps)
    if not pairs:
        raise SystemExit("没有可用的 (cs, rs) 对")
    if args.start or args.limit:
        pairs = pairs[args.start:(args.start + args.limit) if args.limit else None]

    OUTDIR.mkdir(parents=True, exist_ok=True)
    tmp_pairs = OUTDIR / "tmp"
    tmp_pairs.mkdir(exist_ok=True)

    exe = ensure_exe(args.rebuild)
    d3d12 = Path(args.d3d12)
    if not d3d12.exists():
        raise SystemExit("找不到 d3d12.dll：%s" % d3d12)
    staged = stage_dlls(d3d12)
    print("d3d12 = %s（core 已搬到同目录）" % staged)

    icd_names = list(ICDS) if args.icd == "all" else [args.icd]
    for icd in icd_names:
        out_tsv = OUTDIR / ("result_%s.tsv" % icd)
        before = read_rows(out_tsv) if args.diff else {}
        print("\n=== %s ===" % (icd or "全部"))
        rows, t0 = [], time.time()
        if args.resume and out_tsv.exists():
            rows = list(read_rows(out_tsv).values())
        seen = {r[0] for r in rows}
        # 写穿：每跑完一条就落盘一次（长跑被 SIGKILL 也不会白跑）
        with open(out_tsv, "w", encoding="utf-8") as f:
            f.write("\t".join(HEADER) + "\n")
            for r in rows:
                f.write("\t".join(r) + "\n")
            f.flush()
            for cs, rs in pairs:
                if cs in seen:
                    continue
                row = run_one(exe, staged, dumps / cs, dumps / rs, icd, tmp_pairs)
                rows.append(row)
                f.write("\t".join(row) + "\n")
                f.flush()
                done = len(rows)
                if done % 5 == 0:
                    print("  …%d/%d（%.0fs）" % (done, len(pairs), time.time() - t0), flush=True)
        print("  → %s" % out_tsv.relative_to(ROOT))
        summarize(out_tsv)
        if args.diff and before:
            after = read_rows(out_tsv)
            changed = [k for k in sorted(set(before) | set(after))
                       if (before.get(k) or [""] * 9)[8] != (after.get(k) or [""] * 9)[8]]
            print("  与上次相比变化 %d 行%s" % (len(changed),
                                          ("：" + ", ".join(changed[:10])) if changed else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
