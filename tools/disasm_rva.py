#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""按 RVA 反汇编 PE，绕开 objdump 对高 RVA 的静默失败。

## 为什么需要它

排查时最容易犯的错是**把 RVA 当成 VA 用**：模块基址 0x140000000 + RVA 0x5F3D60
= 0x1405F3D60（不是 0x145F3D60 —— 少一位就落到镜像外面，objdump 会静默地什么都不打印，
看起来像"objdump 坏了"）。本仓库真的这么错过一次。

所以本工具**只收 RVA**，自己换算 VA（`base + rva`）并按"RVA -> 节 -> 文件偏移"
切出字节，再用 `objdump -D -b binary -m i386:x86-64 --adjust-vma=<VA>` 反汇编。
这样传参永远不会出现"RVA/VA 混用"的错误，也顺便绕开了直接对 PE 反汇编时
按节边界裁切的一些麻烦。

## 用法

    python tools/disasm_rva.py <exe> <rva> [长度]
    python tools/disasm_rva.py game.exe 0x247AF2D 0x80
    python tools/disasm_rva.py game.exe 0x247AF2D 0x80 --stop-at-int3

默认镜像基址 0x140000000（可用 `--base` 覆盖），长度默认 0x80 字节。
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile


def sections(data: bytes):
    """返回 [(name, rva, vsize, file_off, raw_size, characteristics)]。"""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    sec = opt + (240 if magic == 0x20B else 224)
    out = []
    for k in range(nsec):
        o = sec + k * 40
        name = data[o:o + 8].rstrip(b"\0").decode("latin1", "replace")
        vsize, rva, raw, off = struct.unpack_from("<IIII", data, o + 8)
        chars = struct.unpack_from("<I", data, o + 36)[0]
        out.append((name, rva, vsize, off, raw, chars))
    return out


def rva_to_offset(secs, rva: int):
    for name, s_rva, _vsize, off, raw, _chars in secs:
        if s_rva <= rva < s_rva + raw:
            return name, off + (rva - s_rva)
    return None, None


def main() -> int:
    ap = argparse.ArgumentParser(description="按 RVA 反汇编 PE（绕开 objdump 高 RVA 静默失败）")
    ap.add_argument("exe")
    ap.add_argument("rva", help="起始 RVA，可写 0x...")
    ap.add_argument("length", nargs="?", default="0x80", help="字节数，默认 0x80")
    ap.add_argument("--base", default="0x140000000", help="镜像基址，默认 0x140000000")
    ap.add_argument("--objdump", default="objdump", help="objdump 路径")
    args = ap.parse_args()

    rva = int(args.rva, 0)
    length = int(args.length, 0)
    base = int(args.base, 0)

    with open(args.exe, "rb") as f:
        data = f.read()
    secs = sections(data)
    name, off = rva_to_offset(secs, rva)
    if off is None:
        print(f"RVA 0x{rva:X} 不在任何节里（节表 {len(secs)} 项）", file=sys.stderr)
        return 1

    blob = data[off:off + length]
    if not blob:
        print("区间为空", file=sys.stderr)
        return 1

    tmp = os.path.join(tempfile.gettempdir(), f"disasm_{rva:X}_{length:X}.bin")
    with open(tmp, "wb") as f:
        f.write(blob)
    cmd = [args.objdump, "-D", "-b", "binary", "-m", "i386:x86-64",
           f"--adjust-vma=0x{base + rva:X}", tmp]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, check=False).stdout
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass

    lines = [ln for ln in out.splitlines()
             if ln.strip() and not ln.startswith(tmp) and "file format" not in ln
             and not ln.strip().endswith(":")]
    print(f"== {os.path.basename(args.exe)} 段 {name} RVA 0x{rva:X}（VA 0x{base + rva:X}）"
          f" 共 {length} 字节")
    for ln in lines:
        print(ln)
    return 0


if __name__ == "__main__":
    sys.exit(main())
