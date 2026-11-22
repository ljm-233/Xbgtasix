#!/usr/bin/env python3
"""对 PercyStar 导出的 shaders_index.csv 做统计，回答"移植到 PC 需要哪些 GPU 能力"。

CSV 列：index,stage,pso_hash,code_off,code_size,insns,bindings,has_rt,has_ngg,numthreads

用法：
    python shader_stats.py <shaders_index.csv>
"""
import collections
import csv
import sys


def pct(v, n):
    return 100.0 * v / n if n else 0.0


def quantiles(values, qs=(0.5, 0.9, 0.99)):
    if not values:
        return {}
    s = sorted(values)
    out = {}
    for q in qs:
        idx = min(len(s) - 1, int(q * (len(s) - 1) + 0.5))
        out[q] = s[idx]
    return out


def main(path):
    rows = []
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            try:
                rows.append({
                    "stage": r["stage"].strip(),
                    "code_off": int(r["code_off"], 16),
                    "code_size": int(r["code_size"]),
                    "insns": int(r["insns"]),
                    "bindings": int(r["bindings"]),
                    "has_rt": int(r["has_rt"]),
                    "has_ngg": int(r["has_ngg"]),
                    "numthreads": r["numthreads"].strip(),
                })
            except (KeyError, ValueError):
                continue
    n = len(rows)
    if not n:
        sys.exit("没有可解析的行")

    print("着色器总数: %d" % n)
    total_bytes = sum(r["code_size"] for r in rows)
    total_insns = sum(r["insns"] for r in rows)
    print("字节码合计: %.1f MB   指令合计: %d" % (total_bytes / 1e6, total_insns))

    print("\n== 阶段分布 ==")
    by_stage = collections.Counter(r["stage"] for r in rows)
    by_stage_bytes = collections.Counter()
    for r in rows:
        by_stage_bytes[r["stage"]] += r["code_size"]
    print("%-10s %8s %8s %10s %10s" % ("stage", "数量", "占比", "字节数", "字节占比"))
    for st, c in by_stage.most_common():
        print("%-10s %8d %7.1f%% %10.1f MB %9.1f%%" % (
            st, c, pct(c, n), by_stage_bytes[st] / 1e6, pct(by_stage_bytes[st], total_bytes)))

    print("\n== 主机特性 ==")
    ngg = sum(r["has_ngg"] for r in rows)
    rt = sum(r["has_rt"] for r in rows)
    print("mesh shader (NGG): %d (%.1f%%)" % (ngg, pct(ngg, n)))
    print("硬件光追 (BVH64):  %d (%.1f%%)" % (rt, pct(rt, n)))
    for st in by_stage:
        sub = [r for r in rows if r["stage"] == st]
        print("  %-10s NGG %4d/%d   RT %4d/%d" % (
            st, sum(r["has_ngg"] for r in sub), len(sub), sum(r["has_rt"] for r in sub), len(sub)))

    print("\n== 绑定数量（描述符槽） ==")
    b = collections.Counter(r["bindings"] for r in rows)
    for k in sorted(b):
        print("  bindings=%-3d %6d (%.1f%%)" % (k, b[k], pct(b[k], n)))
    qb = quantiles([r["bindings"] for r in rows])
    print("  中位 %d / p90 %d / p99 %d" % (qb[0.5], qb[0.9], qb[0.99]))

    print("\n== 指令数分布 ==")
    q = quantiles([r["insns"] for r in rows])
    ins = [r["insns"] for r in rows]
    print("  min %d / 中位 %d / p90 %d / p99 %d / max %d" % (min(ins), q[0.5], q[0.9], q[0.99], max(ins)))

    print("\n== Compute 线程组（top 8） ==")
    cs = collections.Counter(r["numthreads"] for r in rows if r["stage"] == "CS")
    for k, c in cs.most_common(8):
        print("  %-12s %5d (%.1f%% of CS)" % (k or "(空)", c, pct(c, sum(cs.values()))))

    print("\n== 代码区间连续性 ==")
    offs = sorted(r["code_off"] for r in rows)
    gaps = sum(1 for a, b in zip(offs, offs[1:]) if b - a > r["code_size"])
    print("  最小 offset 0x%x / 最大 0x%x；相邻区间不连续的跳变 %d 处" % (offs[0], offs[-1], gaps))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    main(sys.argv[1])
