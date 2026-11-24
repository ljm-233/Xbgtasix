#!/usr/bin/env python3
"""add_exports.py —— 给一个 PE DLL **追加**导出名（原有导出一个都不动）。

为什么需要它：标题的引导代码按名字 GetProcAddress 主机 GDK 运行时的几个内部入口点
（`InitializeApiImpl` / `InitializeApiImplEx` / `InitializeApiImplEx2` /
`UninitializeApiImpl`）。我们的 shim 不导出它们，引导就返回 `0x89240101` 并 int3。
真 Windows 上补这四条只要改 `src/generated/xgameruntime.def` 再 build.sh；
本机（Arch）没有 mingw-w64，编不了，所以先用"改 PE 导出表"把行为验出来。

做了什么：
  1. 文件末尾追加新节 `.edata2`：每个新名字一个 4 字节函数体（`33 C0 C3` = xor eax,eax; ret
     —— 返回 0/S_OK），加重建的导出目录 + 地址表 + 名字指针表 + 序号表 + 新名字字符串。
     旧名字的字符串按原 RVA 复用，不复制。
  2. 补一个节头（PE 头区尾部通常有空位，不搬文件）。
  3. 改 NumberOfSections / SizeOfImage / 数据目录[0]（导出表）。
名字表按字节序排序：加载器对导出名做二分查找，乱序会查不到。

用法：
  python3 tools/add_exports.py build/xgameruntime.dll -o out/xgameruntime.dll \\
      --add InitializeApiImpl --add InitializeApiImplEx --add InitializeApiImplEx2 \\
      --add UninitializeApiImpl
"""
from __future__ import annotations

import argparse
import struct
import sys

import pefile

CODE = b"\x33\xC0\xC3\x90"      # xor eax,eax; ret; nop —— 返回 0（S_OK）


def align(v: int, a: int) -> int:
    return (v + a - 1) // a * a


def rva2off(pe, rva: int):
    for s in pe.sections:
        start = s.VirtualAddress
        size = max(s.Misc_VirtualSize, s.SizeOfRawData)
        if start <= rva < start + size:
            return s.PointerToRawData + (rva - start)
    return None


def read_cstr(pe, rva: int) -> bytes:
    off = rva2off(pe, rva)
    end = bytes(pe.__data__).index(b"\x00", off)
    return bytes(pe.__data__)[off:end + 1]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dll")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--add", action="append", default=[], required=True)
    args = ap.parse_args()

    pe = pefile.PE(args.dll)
    data = bytearray(pe.__data__)
    hdr_off = pe.DOS_HEADER.e_lfanew
    fh = hdr_off + 4
    oh = fh + 20
    nsec = pe.FILE_HEADER.NumberOfSections
    sec_tab = oh + pe.FILE_HEADER.SizeOfOptionalHeader
    if sec_tab + (nsec + 1) * 40 > pe.OPTIONAL_HEADER.SizeOfHeaders:
        print(f"错误：PE 头区放不下第 {nsec+1} 个节头（头区 {pe.OPTIONAL_HEADER.SizeOfHeaders}）",
              file=sys.stderr)
        return 1

    exp = pe.DIRECTORY_ENTRY_EXPORT
    es = exp.struct
    nfunc = es.NumberOfFunctions
    nname = es.NumberOfNames
    base = es.Base

    names_off = rva2off(pe, es.AddressOfNames)
    ords_off = rva2off(pe, es.AddressOfNameOrdinals)
    eat_off = rva2off(pe, es.AddressOfFunctions)

    name_rva = [struct.unpack_from("<I", data, names_off + 4 * i)[0] for i in range(nname)]
    ords = [struct.unpack_from("<H", data, ords_off + 2 * i)[0] for i in range(nname)]
    eat = [struct.unpack_from("<I", data, eat_off + 4 * i)[0] for i in range(nfunc)]

    old = []                                    # (名字, 地址表下标, 名字 RVA)
    for i in range(nname):
        nm = read_cstr(pe, name_rva[i])[:-1].decode("ascii")
        old.append((nm, ords[i], name_rva[i]))
    have = {n for n, _, _ in old}

    new_names = [n for n in args.add if n not in have]
    if not new_names:
        print("错误：要加的名字全都已经存在", file=sys.stderr)
        return 1
    for n in new_names:
        if any(c in n for c in " \t"):
            print(f"错误：名字里有空白：{n!r}", file=sys.stderr)
            return 1

    # ------------------------------------------------------------ 新节内容
    va = align(pe.OPTIONAL_HEADER.SizeOfImage, pe.OPTIONAL_HEADER.SectionAlignment)
    off = align(len(data), pe.OPTIONAL_HEADER.FileAlignment)

    body = bytearray()
    func_rvas = []
    for _ in new_names:
        func_rvas.append(va + len(body))
        body += CODE
    while len(body) % 16:
        body += b"\x00"

    dir_off = len(body)
    eat2_off = dir_off + 40
    nm2_off = eat2_off + 4 * (nfunc + len(new_names))
    ord2_off = nm2_off + 4 * (nname + len(new_names))
    str_off = ord2_off + 2 * (nname + len(new_names))

    strings = bytearray()
    new_name_rva = {}
    for n in new_names:
        new_name_rva[n] = va + str_off + len(strings)
        strings += n.encode("ascii") + b"\x00"
    modname_rva = va + str_off + len(strings)
    strings += read_cstr(pe, es.Name)

    merged = [(nm, idx, rva) for nm, idx, rva in old]
    for i, n in enumerate(new_names):
        merged.append((n, nfunc + i, new_name_rva[n]))
    merged.sort(key=lambda t: t[0].encode("ascii"))

    eat2 = eat + func_rvas
    blk = bytearray(40)
    struct.pack_into("<IIHHIIIIIII", blk, 0, 0, es.TimeDateStamp, 0, 0,
                     modname_rva, base,
                     nfunc + len(new_names), nname + len(new_names),
                     va + eat2_off, va + nm2_off, va + ord2_off)
    body += blk
    assert len(body) == eat2_off
    body += struct.pack("<%dI" % len(eat2), *eat2)
    assert len(body) == nm2_off
    body += struct.pack("<%dI" % len(merged), *[t[2] for t in merged])
    assert len(body) == ord2_off
    body += struct.pack("<%dH" % len(merged), *[t[1] for t in merged])
    assert len(body) == str_off
    body += strings

    vsize = len(body)
    rawsize = align(vsize, pe.OPTIONAL_HEADER.FileAlignment)
    body += b"\x00" * (rawsize - vsize)

    # ------------------------------------------------------------ 写回
    if len(data) < off:
        data += b"\x00" * (off - len(data))
    data += body

    sh = bytearray(40)
    sh[0:8] = b".edata2\x00"
    struct.pack_into("<IIIIIIHHI", sh, 8, vsize, va, rawsize, off, 0, 0, 0, 0, 0x60000040)
    data[sec_tab + nsec * 40: sec_tab + (nsec + 1) * 40] = sh

    struct.pack_into("<H", data, fh + 2, nsec + 1)                     # NumberOfSections
    struct.pack_into("<I", data, oh + 56,
                     va + align(vsize, pe.OPTIONAL_HEADER.SectionAlignment))  # SizeOfImage
    struct.pack_into("<I", data, oh + 64, 0)                           # CheckSum
    struct.pack_into("<II", data, oh + 112, va + dir_off, str_off)     # 数据目录[0]

    open(args.out, "wb").write(data)
    print(f"写出 {args.out}: 导出 {nfunc} -> {nfunc + len(new_names)}"
          f"（.edata2 @RVA {va:#x}，{rawsize} 字节）")
    for n, r in zip(new_names, func_rvas):
        print(f"  + {n} -> {r:#x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
