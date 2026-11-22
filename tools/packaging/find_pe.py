#!/usr/bin/env python3
"""在任意二进制里找 PE 文件（按 MZ + PE\\0\\0 结构验证），用于确认"包里到底有没有 exe"。

用法：
    python find_pe.py <file-or-dir> [--limit-mb N] [--dir]

输出每个候选 PE 的偏移、机器类型、大小估算与附近的 ASCII 线索。
"""
import argparse
import os
import struct
import sys

MACHINES = {0x14c: "x86", 0x8664: "x64", 0xAA64: "arm64", 0x1c0: "arm"}


def hint_around(data, off, span=4096):
    lo = max(0, off - span)
    hi = min(len(data), off + span)
    blob = data[lo:hi]
    keep = []
    cur = bytearray()
    for b in blob:
        if 32 <= b < 127:
            cur.append(b)
        else:
            if len(cur) >= 8:
                keep.append(cur.decode("latin1"))
            cur.clear()
    if len(cur) >= 8:
        keep.append(cur.decode("latin1"))
    words = [s for s in keep if any(k in s.lower() for k in
                                    ("game_", "scarlett", "gta", ".exe", "xgameruntime", "d3d12", "rpf9"))]
    return words[:4]


def scan(data, base=0, out=None):
    out = out if out is not None else []
    i = 0
    n = len(data)
    while True:
        i = data.find(b"MZ", i)
        if i < 0 or i + 0x40 > n:
            break
        try:
            e_lfanew = struct.unpack_from("<I", data, i + 0x3C)[0]
        except struct.error:
            break
        if 0x40 <= e_lfanew < 0x1000 and i + e_lfanew + 24 <= n:
            if data[i + e_lfanew:i + e_lfanew + 4] == b"PE\0\0":
                machine = struct.unpack_from("<H", data, i + e_lfanew + 4)[0]
                secs = struct.unpack_from("<H", data, i + e_lfanew + 6)[0]
                out.append({
                    "off": base + i,
                    "machine": MACHINES.get(machine, hex(machine)),
                    "sections": secs,
                    "hint": hint_around(data, i),
                })
        i += 2
    return out


def process(path, limit_mb=None):
    size = os.path.getsize(path)
    if limit_mb and size > limit_mb * 1024 * 1024:
        with open(path, "rb") as f:
            data = f.read(limit_mb * 1024 * 1024)
        scope = "前 %d MB" % limit_mb
    else:
        with open(path, "rb") as f:
            data = f.read()
        scope = "全文件 %.1f MB" % (size / 1e6)
    hits = scan(data)
    print("%-42s %-16s PE 候选 %d 个" % (os.path.basename(path), scope, len(hits)))
    for h in hits[:6]:
        print("     +0x%-10x %-6s sections=%-3d %s" % (h["off"], h["machine"], h["sections"], h["hint"]))
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("target")
    ap.add_argument("--limit-mb", type=int, default=0)
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    if os.path.isdir(a.target):
        for name in sorted(os.listdir(a.target)):
            p = os.path.join(a.target, name)
            if os.path.isfile(p) and os.path.getsize(p) > 1024:
                process(p, a.limit_mb or None)
    else:
        process(a.target, a.limit_mb or None)


if __name__ == "__main__":
    main()
