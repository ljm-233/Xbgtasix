#!/usr/bin/env python3
"""回答"这个包到底有多少是加密的"：按块分类统计，而不是凭感觉。

每 4 KB 一块，判成五类：
  zero       全零（对齐填充）
  zlib       以 0x78 开头且能成功解压 -> 压缩（可读）
  text       可打印字符 >85%（XML/文本，可读）
  structured 有一定结构但非文本（模型/音频等二进制，可读但不能直接看懂）
  high-entropy 熵 > 7.9 且不可解压 -> 很可能是加密

用法：
    python encryption_survey.py <archive.rpf> [--limit-mb N] [--block 4096]
"""
import argparse
import collections
import math
import os
import sys
import zlib

BLOCK = 4096


def entropy(buf):
    cnt = collections.Counter(buf)
    n = len(buf)
    return -sum((c / n) * math.log2(c / n) for c in cnt.values())


def classify(buf):
    if not buf:
        return "zero"
    if not any(buf):
        return "zero"
    printable = sum(1 for b in buf if 9 <= b <= 13 or 32 <= b < 127) / len(buf)
    if buf[0] == 0x78 and buf[1] in (0x01, 0x5E, 0x9C, 0xDA):
        try:
            if len(zlib.decompressobj().decompress(buf)) > BLOCK // 4:
                return "zlib"
        except zlib.error:
            pass
    if printable > 0.85:
        return "text"
    e = entropy(buf)
    if e > 7.9:
        return "high-entropy"
    return "structured"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rpf")
    ap.add_argument("--limit-mb", type=int, default=64,
                    help="只读前 N MB（默认 64；0 表示整文件，大归档慎用）")
    ap.add_argument("--block", type=int, default=BLOCK)
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    size = os.path.getsize(a.rpf)
    with open(a.rpf, "rb") as f:
        data = f.read(a.limit_mb * 1024 * 1024) if a.limit_mb else f.read()
    blocks = [data[i:i + a.block] for i in range(0, len(data) - a.block + 1, a.block)]
    counts = collections.Counter(classify(b) for b in blocks)
    total = sum(counts.values())
    if total == 0:
        print("文件太小（小于一个块），无法统计")
        return 1

    label = {"zero": "全零填充", "zlib": "zlib 压缩（可解）", "text": "文本（可读）",
             "structured": "二进制结构（可读但需解析）", "high-entropy": "高熵（疑似加密）"}
    print("== %s  (%.1f MB%s)" % (os.path.basename(a.rpf), size / 1e6,
                                  "，只读前 %d MB" % a.limit_mb if a.limit_mb else ""))
    print("   按 %d KB 块统计，共 %d 块：" % (a.block // 1024, total))
    for k, n in counts.most_common():
        print("     %-28s %7d 块  %5.1f%%  (%.1f MB)" % (
            label[k], n, 100.0 * n / total, n * a.block / 1e6))

    enc = counts["high-entropy"]
    print("\n   结论：疑似加密的高熵块占 %.1f%% —— %s" % (
        100.0 * enc / total,
        "不是'大部分被加密'" if enc < total * 0.6 else "确实大部分是高熵"))


if __name__ == "__main__":
    main()
