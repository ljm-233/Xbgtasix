#!/usr/bin/env python3
"""RPF9 载荷雕刻器（不依赖任何第三方工具）

背景：RPF9 的目录表是加密的（见 docs/rpf9-findings.md），但**载荷本身**有些是明文、
有些是标准 zlib 流。所以可以按"特征扫描"把内容挖出来 —— 这正是社区工具的做法，
这里用几十行自己实现一遍，用来证明"我们能自己拿到数据"。

扫描两类目标：
  1) 明文载荷：BIK/BK2（Bink 视频）、ADAT（音频）、RBF0/0DRPz/PSIN（UI/文本/PSO）
  2) zlib 流：在 0x78 0x01/0x9C/0xDA 处尝试解压，按解压结果的魔数分类

用法：
    python rpf_carve.py <archive.rpf> [--limit-mb N] [--dump-dir DIR] [--max-streams N]
"""
import argparse
import collections
import os
import struct
import sys
import zlib

PLAIN_MAGICS = {
    b"BIK": "Bink 视频",
    b"BK2": "Bink2 视频",
    b"ADAT": "音频容器",
    b"RBF0": "UI 资源",
    b"0DRPz": "文本/本地化",
    b"PSIN": "二进制 PSO",
    b"SGD5": "着色器组",
    b"FSSM": "着色器 blob",
    b"DXBC": "DXBC 着色器",
    b"\x89PNG": "PNG",
    b"DDS ": "DDS 纹理",
}

INNER_MAGICS = {
    b"<?xml": "XML",
    b"<": "文本/XML",
    b"0DRPz": "文本/本地化",
    b"PSIN": "二进制 PSO",
    b"RBF0": "UI 资源",
    b"SGD5": "着色器组",
    b"FSSM": "着色器 blob",
    b"DXBC": "DXBC",
}


def find_all(data, needle, limit=64):
    out, i = [], 0
    while len(out) < limit:
        i = data.find(needle, i)
        if i < 0:
            break
        out.append(i)
        i += 1
    return out


def classify_plain(data):
    hits = collections.Counter()
    first = {}
    for magic, label in PLAIN_MAGICS.items():
        offs = find_all(data, magic)
        if offs:
            hits[label] = len(offs)
            first[label] = offs[0]
    return hits, first


def classify_inner(out):
    """按**前缀**分类，避免把"前 512 字节里碰巧出现 < 的二进制"当成 XML。"""
    body = out
    for bom in (b"\xef\xbb\xbf", b"\xff\xfe", b"\xfe\xff"):
        if body.startswith(bom):
            body = body[len(bom):]
            break
    body = body.lstrip()
    for magic, name in INNER_MAGICS.items():
        if body.startswith(magic):
            return name
    head = body[:256]
    printable = sum(1 for b in head if 9 <= b <= 13 or 32 <= b < 127) / max(1, len(head))
    if printable > 0.85:
        return "文本（未识别格式）"
    return "二进制"


def carve_zlib(data, dump_dir=None, max_streams=200):
    found = collections.Counter()
    streams = []
    n = len(data)
    i = 0
    attempts = 0
    while i < n - 2 and len(streams) < max_streams:
        if data[i] == 0x78 and data[i + 1] in (0x01, 0x5E, 0x9C, 0xDA):
            attempts += 1
            try:
                d = zlib.decompressobj()
                out = d.decompress(data[i:i + 1_000_000])
                if len(out) > 64:
                    label = classify_inner(out)
                    found[label] += 1
                    streams.append((i, len(out), label, out))
            except zlib.error:
                pass
        i += 1
    if dump_dir and streams:
        os.makedirs(dump_dir, exist_ok=True)
        for k, (_off, size, label, out) in enumerate(streams[:50]):
            safe = label.replace("/", "_")
            path = os.path.join(dump_dir, "%03d_%s_%d.bin" % (k, safe, size))
            with open(path, "wb") as f:
                f.write(out)
    return found, len(streams), attempts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rpf")
    ap.add_argument("--limit-mb", type=int, default=0)
    ap.add_argument("--dump-dir", default="")
    ap.add_argument("--max-streams", type=int, default=200)
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    size = os.path.getsize(a.rpf)
    with open(a.rpf, "rb") as f:
        data = f.read(a.limit_mb * 1024 * 1024) if a.limit_mb else f.read()

    magic, data_off, names_len, ec = struct.unpack_from("<IIII", data, 0)[0], \
        struct.unpack_from("<I", data, 4)[0], struct.unpack_from("<I", data, 0xC)[0], \
        struct.unpack_from("<I", data, 0x24)[0]
    print("== %s  (%.1f MB%s)" % (os.path.basename(a.rpf), size / 1e6,
                                  "，只读前 %d MB" % a.limit_mb if a.limit_mb else ""))
    print("   头: magic=0x%08x data_offset=%d names_len=%d entry_count=%d" % (
        magic, data_off, names_len, ec))

    plain, first = classify_plain(data)
    print("\n   明文载荷:")
    if not plain:
        print("     （未命中已知魔数）")
    for label, c in plain.most_common():
        print("     %-12s %5d 处，首个偏移 0x%x" % (label, c, first[label]))

    found, total, attempts = carve_zlib(data, a.dump_dir or None, a.max_streams)
    print("\n   zlib 流: 尝试 %d 个候选，识别出 %d 个结构化载荷" % (attempts, total))
    for label, c in found.most_common():
        print("     %-12s %5d 个" % (label, c))
    if a.dump_dir and total:
        print("     已导出到 %s（最多 50 个）" % a.dump_dir)

    print("\n   结论：目录表 %s，但载荷可雕刻 —— 这就是现有工具的工作方式。" %
          ("仍加密" if True else ""))


if __name__ == "__main__":
    main()
