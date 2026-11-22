#!/usr/bin/env python3
"""Dump PE import/export surface of the package DLLs (read-only, no deps).

Usage: python pe_surface.py [folder] [-o out.md]
"""
import glob
import os
import struct
import sys

FOLDER = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "."
OUT = "tools/gdk_api_surface.md"


def parse_pe(data):
    if data[:2] != b"MZ":
        return None
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        return None
    coff = e_lfanew + 4
    machine, nsec = struct.unpack_from("<HH", data, coff)
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    is64 = magic == 0x20B
    dd = opt + (112 if is64 else 96)
    ndirs = struct.unpack_from("<I", data, dd - 4)[0]
    dirs = [struct.unpack_from("<II", data, dd + 8 * i) for i in range(min(ndirs, 16))]
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

    result = {"is64": is64, "machine": machine, "imports": {}, "exports": []}

    # imports
    if len(dirs) > 1 and dirs[1][0]:
        off = rva2off(dirs[1][0])
        if off:
            while True:
                oft, _ts, _fwd, name_rva, ft = struct.unpack_from("<IIIII", data, off)
                if not name_rva:
                    break
                mod = cstr(name_rva)
                thunk_rva = oft or ft
                funcs = []
                t = rva2off(thunk_rva) if thunk_rva else None
                step = 8 if is64 else 4
                fmt = "<Q" if is64 else "<I"
                if t:
                    while True:
                        val = struct.unpack_from(fmt, data, t)[0]
                        if not val:
                            break
                        if val >> (63 if is64 else 31):
                            funcs.append("ordinal#%d" % (val & 0xFFFF))
                        else:
                            n = cstr((val & 0x7FFFFFFF) + 2)
                            funcs.append(n or "?")
                        t += step
                if mod:
                    result["imports"][mod] = funcs
                off += 20

    # exports
    if dirs and dirs[0][0]:
        off = rva2off(dirs[0][0])
        if off:
            (_c, _t, _maj, _min, _nm, _base, nfunc, nnames,
             a_funcs, a_names, a_ords) = struct.unpack_from("<IIHHIIIIIII", data, off)
            o_names = rva2off(a_names)
            o_ords = rva2off(a_ords)
            if o_names and o_ords and nnames < 200000:
                for i in range(nnames):
                    n_rva = struct.unpack_from("<I", data, o_names + 4 * i)[0]
                    nm = cstr(n_rva)
                    if nm:
                        result["exports"].append(nm)
    return result


def main():
    rows = []
    modules = {}
    for path in sorted(glob.glob(os.path.join(FOLDER, "*.dll"))):
        with open(path, "rb") as f:
            data = f.read()
        pe = parse_pe(data)
        if not pe:
            continue
        rows.append((os.path.basename(path), pe))
        for mod, funcs in pe["imports"].items():
            modules.setdefault(mod, {}).setdefault(os.path.basename(path), []).extend(funcs)

    lines = ["# Package DLL surface", "",
             "| DLL | 64-bit | imports | exports |", "|---|---|---|---|"]
    for name, pe in rows:
        lines.append("| %s | %s | %d | %d |" % (
            name, "yes" if pe["is64"] else "no",
            sum(len(v) for v in pe["imports"].values()), len(pe["exports"])))
    lines += ["", "## Imported modules", ""]
    for mod in sorted(modules):
        who = ", ".join(sorted(modules[mod]))
        total = sum(len(v) for v in modules[mod].values())
        lines.append("- **%s** (%d funcs) <- %s" % (mod, total, who))
    lines += ["", "## Exports by DLL", ""]
    for name, pe in rows:
        if not pe["exports"]:
            continue
        lines.append("### %s (%d)" % (name, len(pe["exports"])))
        lines.append("")
        lines.append("```")
        lines.extend(pe["exports"][:400])
        lines.append("```")
        lines.append("")
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    print("dlls parsed: %d" % len(rows))
    print("imported modules: %d" % len(modules))
    for mod in sorted(modules):
        print("  %-40s %5d funcs" % (mod, sum(len(v) for v in modules[mod].values())))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
