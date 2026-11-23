#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""把一次运行日志压成可读摘要：谁被调了、崩在哪、断言前的最后几步是什么。

用法：
    python tools/summarize_run.py build/first_run.log [--tail 15]

为什么要它：真机 exe 的日志一次几千行，`tail` 只能看到最后一条 FATAL，
而定位一个断言真正需要的是"FATAL 之前那几个调用"。这个脚本就是干这个的。
"""
import argparse
import re
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--tail", type=int, default=15, help="断言前打印多少个调用")
    args = ap.parse_args()

    try:
        with open(args.log, encoding="utf-8", errors="replace") as f:
            lines = [l.rstrip("\n") for l in f]
    except OSError as e:
        print(f"打不开日志: {e}")
        return 2

    if not lines:
        print("日志是空的（可能进程根本没起来，或 XBGTASIX_LOG 指错）")
        return 1

    # 1) 各子系统调用计数
    counters = {}
    for l in lines:
        m = re.match(r"\[(xmem|d3d12_xs|xgameruntime|console-hw|xcrdapi|sysui|trace)\]", l)
        if m:
            counters[m.group(1)] = counters.get(m.group(1), 0) + 1

    print("== 日志概况")
    print(f"  总行数 {len(lines)}")
    for k, v in sorted(counters.items(), key=lambda kv: -kv[1]):
        print(f"  [{k}] {v} 行")

    # 2) 未实现 API
    unimpl = sorted({l.split("UNIMPLEMENTED:", 1)[1].strip() for l in lines if "UNIMPLEMENTED:" in l})
    print(f"\n== 未实现 API：{len(unimpl)} 个")
    for name in unimpl[:20]:
        print(f"  - {name}")
    if len(unimpl) > 20:
        print(f"  ...（共 {len(unimpl)} 个）")

    # 3) 第一次致命异常 + 它的调用栈
    fi = next((i for i, l in enumerate(lines) if "FATAL 异常" in l), None)
    if fi is None:
        print("\n== 没有记录到致命异常（可能正常退出，或异常钩子没开：XBGTASIX_VEH=1）")
        return 0

    print(f"\n== 第一次致命异常（第 {fi + 1} 行）")
    print("  " + lines[fi].split("]", 1)[-1].strip())
    # 紧随其后的栈帧
    frames = []
    for l in lines[fi + 1: fi + 40]:
        if re.search(r"#\d+\s+\S+\+0x[0-9A-Fa-f]+", l):
            frames.append(l.split("]", 1)[-1].strip())
        elif frames:
            break
    if frames:
        print("  调用栈：")
        for fr in frames[:12]:
            print("    " + fr)

    # 4) 断言前最后 N 个调用（去掉栈帧行本身）
    head = lines[:fi]
    calls = [l for l in head if re.match(r"\[(trace|xmem|d3d12_xs|console-hw|xcrdapi|sysui)\]", l)]
    n = max(0, args.tail)
    print(f"\n== 断言前最后 {n} 个调用")
    for l in calls[-n:] if n else []:
        print("  " + l[:150])
    return 0


if __name__ == "__main__":
    sys.exit(main())
