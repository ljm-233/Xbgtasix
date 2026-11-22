#!/usr/bin/env python3
"""生成"全量转发"的 .def：把目标 DLL 的每个导出原样转发过去。

用途：d3d12_xs.dll 需要与 d3d12.dll 同名的全部导出，否则游戏一导入就解析失败。
转发（forwarder）由 PE 加载器原生解析，零运行时开销、零手写代码。

用法：
    python forward_shim.py <源DLL> --dll d3d12_xs --target d3d12 --out src/d3d12_xs/forward.def \
        --exclude D3D12CreateDevice,D3D12SerializeVersionedRootSignature

跳过的名字：带 @ 的（stdcall 修饰）、以及 --exclude 里手写实现的那几个。
"""
import argparse
import os
import struct
import sys


def pe_exports(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        raise SystemExit("%s 不是 PE 文件" % path)
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise SystemExit("%s 没有 PE 签名" % path)
    coff = e_lfanew + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    export_rva, export_size = struct.unpack_from("<II", data, dd)
    sec = opt + opt_size
    sections = []
    for i in range(nsec):
        off = sec + 40 * i
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((vaddr, max(vsize, rsize), raddr))

    def rva2off(rva):
        for vaddr, size, raddr in sections:
            if vaddr <= rva < vaddr + size:
                return raddr + (rva - vaddr)
        return None

    def cstr(rva):
        off = rva2off(rva)
        if off is None:
            return None
        end = data.find(b"\0", off)
        return data[off:end].decode("latin1", "replace") if end > 0 else None

    if not export_rva:
        raise SystemExit("%s 没有导出表" % path)
    off = rva2off(export_rva)
    (_c, _t, _maj, _min, _name, _base, nfunc, nnames,
     a_funcs, a_names, a_ords) = struct.unpack_from("<IIHHIIIIIII", data, off)
    names = []
    if nnames and nnames < 100000:
        o_names = rva2off(a_names)
        for i in range(nnames):
            n_rva = struct.unpack_from("<I", data, o_names + 4 * i)[0]
            nm = cstr(n_rva)
            if nm:
                names.append(nm)
    return names, nfunc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dll")
    ap.add_argument("--dll-name", required=True, help="生成的 shim 名（LIBRARY 行）")
    ap.add_argument("--target", required=True, help="转发目标模块名，如 d3d12")
    ap.add_argument("--out", required=True)
    ap.add_argument("--exclude", default="")
    args = ap.parse_args()

    names, total = pe_exports(args.dll)
    excluded = {s.strip() for s in args.exclude.split(",") if s.strip()}

    keep = []
    seen = set()
    skipped_mod = 0
    for nm in names:
        if nm in excluded:
            continue
        if "@" in nm:          # stdcall 修饰名，转发无意义
            skipped_mod += 1
            continue
        if nm in seen:
            continue
        seen.add(nm)
        keep.append(nm)

    keep.sort()
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("; 自动生成，勿手改。由 tools/forward_shim.py 从 %s 生成。\n" % os.path.basename(args.dll))
        f.write("; 源导出 %d 个；转发 %d 个；手写实现 %d 个；跳过修饰名 %d 个。\n" % (
            total, len(keep), len(excluded), skipped_mod))
        f.write("LIBRARY %s\nEXPORTS\n" % args.dll_name)
        for nm in keep:
            f.write("  %s=%s.%s\n" % (nm, args.target, nm))

    print("%s: 源 %d 导出 -> 转发 %d（手写 %d，跳过修饰名 %d）" % (
        args.dll_name, total, len(keep), len(excluded), skipped_mod))


if __name__ == "__main__":
    sys.exit(main())
