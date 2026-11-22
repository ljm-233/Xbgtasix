#!/usr/bin/env python3
"""RPF9 结构探针：把"逆向目录"拆成几个可复现、可证伪的观测。

不做猜测，只出证据：
  1. 头部 4 个字段的值（magic = 0x52504639 确认为 "RPF9" 的小端写法）
  2. 分窗口熵图 —— 高熵=压缩或加密，低熵=明文/零填充
  3. 零填充区间 —— 章节对齐边界（决定载荷从哪开始）
  4. 已知魔数扫描 —— 哪些载荷是明文（可雕刻）
  5. zlib/deflate 探测 —— 排除"只是压缩"的可能

用法：
    python rpf9_probe.py <a.rpf> [--window 512] [--limit-mb 8]
"""
import argparse
import collections
import math
import struct
import sys
import zlib

MAGIC = 0x52504639  # "RPF9"，小端写作 39 46 50 52

SIGS = {
    b"DDS ": "DDS 纹理", b"KTX ": "KTX 纹理", b"\x89PNG": "PNG", b"OggS": "Ogg",
    b"BIK": "Bink", b"BK2": "Bink2", b"RIFF": "RIFF", b"ftyp": "MP4",
    b"DXBC": "DXBC 着色器", b"DXIL": "DXIL 着色器",
}


def entropy(block):
    if not block:
        return 0.0
    cnt = collections.Counter(block)
    n = len(block)
    return -sum((c / n) * math.log2(c / n) for c in cnt.values())


def zero_runs(data, min_len=1024):
    runs, start = [], None
    for i, b in enumerate(data):
        if b == 0:
            if start is None:
                start = i
        elif start is not None:
            if i - start >= min_len:
                runs.append((start, i))
            start = None
    if start is not None and len(data) - start >= min_len:
        runs.append((start, len(data)))
    return runs


def probe(path, window=512, limit_mb=8):
    with open(path, "rb") as f:
        data = f.read(limit_mb * 1024 * 1024)
    magic, f1, f2, f3 = struct.unpack_from("<IIII", data, 0)
    print("== %s  (读了 %.1f MB)" % (path, len(data) / 1e6))
    print("   magic = 0x%08x %s" % (magic, "OK (RPF9)" if magic == MAGIC else "!! 不是 RPF9"))
    print("   字段:  f1=%-10d f2=%-10d f3=%-10d" % (f1, f2, f3))
    print("   差值:  f1-f2=%-8d f1-f3=%-8d f2-f3=%-8d" % (f1 - f2, f1 - f3, f2 - f3))

    print("\n   分窗口熵（%d B/窗）:" % window)
    row = []
    for off in range(0, min(len(data), window * 24), window):
        row.append("%d:%.1f" % (off, entropy(data[off:off + window])))
    print("     " + " ".join(row))

    runs = zero_runs(data, 1024)
    if runs:
        total_zero = sum(b - a for a, b in runs)
        biggest = max(runs, key=lambda r: r[1] - r[0])
        print("\n   零填充: %d 段，共 %.1f KB（占读入的 %.1f%%），最大一段 0x%x..0x%x (%.1f KB）" % (
            len(runs), total_zero / 1024, 100.0 * total_zero / max(1, len(data)),
            biggest[0], biggest[1], (biggest[1] - biggest[0]) / 1024))
        print("     前 3 段: %s%s" % (
            ", ".join("0x%x..0x%x" % (a, b) for a, b in runs[:3]),
            " …" if len(runs) > 3 else ""))
    else:
        print("\n   零填充: 无")

    print("\n   已知魔数:")
    for sig, label in SIGS.items():
        c = data.count(sig)
        if c:
            print("     %-12s %d" % (label, c))
    if not any(data.count(s) for s in SIGS):
        print("     （上限内未命中任何已知载荷魔数）")

    hits = []
    for i in range(16, min(len(data) - 64, 65536)):
        if data[i] == 0x78 and data[i + 1] in (0x01, 0x9C, 0xDA):
            try:
                out = zlib.decompressobj().decompress(data[i:i + 131072])
                if len(out) > 512:
                    hits.append((i, len(out)))
            except Exception:
                pass
    print("\n   zlib/deflate 可解流:", hits[:5] if hits else "无（所以不是'只是压缩'）")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rpf")
    ap.add_argument("--window", type=int, default=512)
    ap.add_argument("--limit-mb", type=int, default=8)
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    probe(a.rpf, a.window, a.limit_mb)


if __name__ == "__main__":
    sys.exit(main())
