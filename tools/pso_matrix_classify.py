#!/usr/bin/env python3
"""pso_matrix_classify —— 把矩阵的失败按**原因**分档，而不是只报 ok/fail。

为什么要分档：ok/fail 只说明"能不能编"，决定不了下一步做什么。三档之间的
工作量差一个数量级：

  undef-symbol    DXIL 里**只有声明、没有定义**的主机外部函数（`xbox.*`）。
                  修法是"补一个等价实现"，工作量 = 符号个数 × 每个符号的语义。
  unhandled-instr dxil-spirv 的 LLVM bitcode 读取器遇到它没实现的记录类型
                  （dxil-spirv `bc/module.cpp` 里 `LOGE("Unhandled instruction!")` 的
                  default 分支）。修法是"换版本或补解析"，与 shader 语义无关。
  timeout / crash 驱动卡住或崩；与编译语义无关，是稳定性问题。

读的是 `build/pso_matrix/tmp/*.err`（每条 shader 的完整 stderr），所以**换分类规则
不用重跑矩阵**。

用法：
    python3 tools/pso_matrix_classify.py                 # 默认读 intel 那张表
    python3 tools/pso_matrix_classify.py --icd nvidia
    python3 tools/pso_matrix_classify.py --icd all
"""

import argparse
import collections
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUTDIR = ROOT / "build" / "pso_matrix"

RE_UNDEF = re.compile(r"Function '([^']+)' is declared but has no definition")
RE_ERR_LINE = re.compile(r"err:.*?dxil-spirv:\s*(.+)$")


def read_rows(path):
    rows = {}
    if not path.exists():
        return rows
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("cs\t"):
            continue
        f = line.split("\t")
        if len(f) >= 9:
            rows[f[0]] = f
    return rows


def classify(cs_name, row):
    """返回 (bucket, detail)。detail 是符号名 / 指令 / 原因短语。"""
    stem = cs_name[:-len(".bin")] if cs_name.endswith(".bin") else cs_name
    err_path = OUTDIR / "tmp" / ("%s.err" % stem)
    text = err_path.read_text(encoding="utf-8", errors="replace") if err_path.exists() else ""
    verdict = row[8] if len(row) > 8 else ""

    if verdict == "ok":
        return "ok", ""
    if verdict == "timeout":
        return "timeout", "60 s 内没返回（驱动卡住，不是编译失败）"
    if verdict == "env-fail":
        return "env-fail", "环境问题：加载到 wine builtin d3d12core"

    # 未定义外部函数：可能一次报多个，取第一个并把全部名字收集起来
    names = RE_UNDEF.findall(text)
    if names:
        return "undef-symbol", ",".join(sorted(set(names)))

    if "Unhandled instruction" in text:
        return "unhandled-instr", "dxil-spirv bitcode 读取器未实现该记录类型"

    # 其它 vkd3d 报错：抓最后一条 dxil-spirv 的话
    hits = RE_ERR_LINE.findall(text)
    if hits:
        return "other", hits[-1].strip()[:110]
    if verdict == "fail":
        return "other", "vkd3d 没给出原因（hr=%s）" % (row[5] if len(row) > 5 else "?")
    return verdict or "unknown", ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--icd", choices=["intel", "nvidia", "all"], default="intel")
    args = ap.parse_args()
    icds = ["intel", "nvidia"] if args.icd == "all" else [args.icd]

    rc = 0
    for icd in icds:
        result = OUTDIR / ("result_%s.tsv" % icd)
        rows = read_rows(result)
        if not rows:
            print("%s：没有结果（%s 不存在）" % (icd, result.name))
            rc = 1
            continue

        classified = []
        for cs in sorted(rows):
            bucket, detail = classify(cs, rows[cs])
            classified.append((cs, rows[cs], bucket, detail))

        out = OUTDIR / ("classify_%s.tsv" % icd)
        with open(out, "w", encoding="utf-8") as f:
            f.write("cs\tbucket\tdetail\trs_hr\tpso_hr\tverdict\tsecs\n")
            for cs, row, bucket, detail in classified:
                f.write("\t".join([cs, bucket, detail, row[4] if len(row) > 4 else "",
                                   row[5] if len(row) > 5 else "",
                                   row[8] if len(row) > 8 else "",
                                   row[10] if len(row) > 10 else ""]) + "\n")

        print("\n=== %s（%d 条）===" % (icd, len(classified)))
        by_bucket = collections.defaultdict(list)
        for cs, row, bucket, detail in classified:
            by_bucket[bucket].append((cs, detail))

        for bucket in ("ok", "undef-symbol", "unhandled-instr", "timeout", "env-fail", "other"):
            items = by_bucket.get(bucket)
            if not items:
                continue
            print("\n[%s] %d 条" % (bucket, len(items)))
            if bucket == "ok":
                print("   " + ", ".join(cs.split("_")[1] for cs, _ in items))
                continue
            # 按 detail 聚合
            groups = collections.defaultdict(list)
            for cs, detail in items:
                groups[detail].append(cs.split("_")[1])
            for detail, names in sorted(groups.items(), key=lambda kv: -len(kv[1])):
                print("   %-62s ×%-2d [cs %s]" % (detail[:62], len(names), ", ".join(sorted(names))))
        print("\n→ %s" % out.relative_to(ROOT))
    return rc


if __name__ == "__main__":
    sys.exit(main())
