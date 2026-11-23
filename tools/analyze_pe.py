#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""目标 PE 的静态体检：头信息、节表、保护特性、调试目录（PDB 路径）、导入结构与可疑字符串。

为什么单独写一个：`xbgtasix check` 只对导入面；而"它为什么会崩在某个断言"
往往要从 **PDB 路径/源码文件名/构建配置** 这些信息里推。纯标准库，不依赖 pefile。

用法：
    python tools/analyze_pe.py <exe> [--strings 30]
"""
import argparse
import re
import struct
import sys

MACHINES = {0x8664: "x86-64", 0x14C: "x86", 0xAA64: "ARM64"}
SUBSYSTEMS = {2: "GUI (Windows)", 3: "Console (CUI)"}


def u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def parse(d):
    if d[:2] != b"MZ":
        raise SystemExit("不是 PE：缺 MZ")
    pe = u32(d, 0x3C)
    if d[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("不是 PE：缺 PE 签名")
    coff = pe + 4
    nsec = u16(d, coff + 2)
    opt_size = u16(d, coff + 16)
    chars = u16(d, coff + 18)
    opt = coff + 20
    magic = u16(d, opt)
    is64 = magic == 0x20B
    info = {
        "pe": pe,
        "machine": u16(d, coff),
        "nsec": nsec,
        "chars": chars,
        "magic": magic,
        "is64": is64,
        "opt": opt,
        "opt_size": opt_size,
        "subsystem": u16(d, opt + 68),
        "image_base": struct.unpack_from("<Q", d, opt + 24)[0] if is64 else u32(d, opt + 28),
        "entry": u32(d, opt + 16),
        "dll_chars": u16(d, opt + 70),
        "timestamp": u32(d, coff + 4),
        "ndirs": u32(d, opt + (108 if is64 else 92)),
        "dir_off": opt + (112 if is64 else 96),
    }
    secs = []
    o = opt + opt_size
    for i in range(nsec):
        b = o + i * 40
        name = d[b:b + 8].rstrip(b"\0").decode("latin-1")
        vs, va, rs, ra = struct.unpack_from("<IIII", d, b + 8)
        ch = u32(d, b + 36)
        secs.append({"name": name, "va": va, "vsize": vs, "raw": ra, "rawsize": rs, "chars": ch})
    info["sections"] = secs
    info["dirs"] = []
    for i in range(min(info["ndirs"], 16)):
        rva = u32(d, info["dir_off"] + i * 8)
        size = u32(d, info["dir_off"] + i * 8 + 4)
        info["dirs"].append((rva, size))
    return info


def rva_to_off(info, rva):
    for s in info["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vsize"], s["rawsize"]):
            return s["raw"] + (rva - s["va"])
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--strings", type=int, default=25, help="打印多少条可疑字符串")
    args = ap.parse_args()

    d = open(args.exe, "rb").read()
    info = parse(d)

    print(f"== {args.exe}")
    print(f"  大小          {len(d):,} 字节（{len(d)/1048576:.1f} MB）")
    print(f"  机器          {MACHINES.get(info['machine'], hex(info['machine']))}"
          f"  PE{'32+' if info['is64'] else '32'}")
    print(f"  子系统        {SUBSYSTEMS.get(info['subsystem'], info['subsystem'])}")
    print(f"  时间戳        0x{info['timestamp']:08X}")
    print(f"  入口 RVA      0x{info['entry']:X}")
    print(f"  镜像基址      0x{info['image_base']:X}")
    print(f"  节数          {info['nsec']}")

    # 保护特性：这些位决定"能不能被常规手段调试/注入"
    dc = info["dll_chars"]
    flags = []
    if dc & 0x0040:
        flags.append("DYNAMICBASE(ASLR)")
    if dc & 0x0100:
        flags.append("NX_COMPAT(DEP)")
    if dc & 0x0400:
        flags.append("NO_SEH")
    if dc & 0x4000:
        flags.append("CFG(控制流保护)")
    if dc & 0x0080:
        flags.append("HIGH_ENTROPY_VA")
    print(f"  保护特性      {' / '.join(flags) if flags else '（无）'}")

    print("\n  节表：")
    for s in info["sections"]:
        kind = []
        if s["chars"] & 0x20000000:
            kind.append("X")
        if s["chars"] & 0x40000000:
            kind.append("R")
        if s["chars"] & 0x80000000:
            kind.append("W")
        print(f"    {s['name']:<10} RVA 0x{s['va']:08X}  大小 {s['vsize']:>12,}  "
              f"文件偏移 0x{s['raw']:08X}  {'|'.join(kind)}")

    # 调试目录 -> PDB 路径（最有信息量的一项）
    dbg_rva, dbg_size = info["dirs"][6]
    print(f"\n  调试目录      RVA 0x{dbg_rva:X} 大小 {dbg_size}")
    if dbg_rva and dbg_size:
        base = rva_to_off(info, dbg_rva)
        for i in range(dbg_size // 28):
            o = base + i * 28
            typ, size, _, _, ptr = struct.unpack_from("<IIIII", d, o)
            if typ == 2 and size >= 24:   # CODEVIEW
                cv = rva_to_off(info, ptr)
                if cv is None:
                    continue
                sig = d[cv:cv + 4]
                if sig == b"RSDS":
                    guid = d[cv + 4:cv + 20]
                    age = u32(d, cv + 20)
                    path = d[cv + 24: cv + size - 24].split(b"\0")[0].decode("latin-1", "replace")
                    g = "-".join([guid[:4].hex(), guid[4:6].hex(), guid[6:8].hex(),
                                  guid[8:10].hex(), guid[10:].hex()])
                    print(f"  PDB           {path}")
                    print(f"  PDB GUID/Age  {g} / {age}")
    else:
        print("  PDB           （无调试目录：剥离了符号）")

    # 导入结构统计
    imp_rva, imp_size = info["dirs"][1]
    if imp_rva:
        o = rva_to_off(info, imp_rva)
        n = 0
        while True:
            desc = d[o + n * 20: o + n * 20 + 20]
            if len(desc) < 20 or desc == b"\0" * 20:
                break
            n += 1
        print(f"\n  导入模块数    {n}")
    dly_rva, dly_size = info["dirs"][13]
    print(f"  延迟导入      {'有（RVA 0x%X）' % dly_rva if dly_rva else '无'}")
    tls_rva, _ = info["dirs"][9]
    print(f"  TLS 回调      {'有（RVA 0x%X）' % tls_rva if tls_rva else '无'}")
    exc_rva, exc_size = info["dirs"][3]
    print(f"  异常表(C++ EH) {'有，%d 条' % (exc_size // 12) if exc_rva else '无'}")
    rel_rva, rel_size = info["dirs"][5]
    print(f"  重定位        {'有，%d 条' % (rel_size // 10) if rel_rva else '无（固定基址）'}")
    print(f"  资源目录      {'有' if info['dirs'][2][0] else '无'}")

    # 可疑字符串：源码路径、配置名、命令行参数
    print(f"\n  == 字符串抽样（最多 {args.strings} 条）")
    pats = [rb"[A-Za-z]:[\\/][^\0\n]{6,120}", rb"[\w\-.]+\.(?:cpp|h|hpp|cxx|lib|pdb)", rb"-{1,2}[\w-]{2,30}"]
    seen = set()
    shown = 0
    for p in pats:
        for m in re.finditer(p, d):
            s = m.group(0).decode("latin-1", "replace")
            if s in seen:
                continue
            seen.add(s)
            print(f"    {s[:120]}")
            shown += 1
            if shown >= args.strings:
                break
        if shown >= args.strings:
            break
    if shown == 0:
        print("    （没有命中；字符串可能被压缩或加密）")
    return 0


if __name__ == "__main__":
    # Windows 控制台默认 GBK：字节里混着非文本时直接 print 会抛 UnicodeEncodeError，
    # 让整个分析在最后一步崩掉。统一转成 UTF-8 且替换不可编码字符。
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    sys.exit(main())
