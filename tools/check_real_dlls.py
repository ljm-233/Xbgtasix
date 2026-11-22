#!/usr/bin/env python3
"""用真实游戏侧 DLL 验证 shim 的导出面（不运行游戏，只做加载与符号核对）。

做两件事：
  1) **静态核对**：解析包内每个 DLL 的导入表，凡是来自 xmem / d3d12_xs / xgameruntime
     的名字，逐个检查我们的 DLL 是否导出 —— 缺一个就是"真跑起来会崩"的点
  2) **动态加载**：把 shim 目录加进 DLL 搜索路径，真的 LoadLibrary 每个包内 DLL，
     让 Windows 加载器亲自做一遍符号解析

用法：
    python tools/check_real_dlls.py <包目录> [--shim build]
"""
import argparse
import os
import struct
import sys

SHIM_MODULES = {"xmem.dll", "d3d12_xs.dll", "xgameruntime.dll"}


def parse_pe(data):
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    e = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e:e + 4] != b"PE\0\0":
        return None
    machine, nsec = struct.unpack_from("<HH", data, e + 4)
    opt_size = struct.unpack_from("<H", data, e + 20)[0]
    opt = e + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    pe32plus = magic == 0x20B
    dd = opt + (112 if pe32plus else 96)
    imp_rva, imp_size = struct.unpack_from("<II", data, dd + 8)
    exp_rva, _ = struct.unpack_from("<II", data, dd + 0)

    sec_off = opt + opt_size
    secs = []
    for i in range(nsec):
        o = sec_off + i * 40
        name = data[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", data, o + 8)
        secs.append((name, vaddr, max(vsize, rsize), raddr))

    def rva2off(rva):
        for _n, va, size, raw in secs:
            if va <= rva < va + size:
                return raw + (rva - va)
        return None

    def cstr(off):
        end = data.find(b"\0", off)
        return data[off:end].decode("ascii", "replace") if off is not None else ""

    imports, exports = {}, []
    if exp_rva:
        eo = rva2off(exp_rva)
        if eo is not None:
            nnames = struct.unpack_from("<I", data, eo + 24)[0]
            names_rva = struct.unpack_from("<I", data, eo + 32)[0]
            no = rva2off(names_rva)
            for i in range(nnames):
                nrva = struct.unpack_from("<I", data, no + i * 4)[0]
                exports.append(cstr(rva2off(nrva)))
    if imp_rva:
        off = rva2off(imp_rva)
        while off:
            desc = data[off:off + 20]
            if len(desc) < 20 or desc == b"\0" * 20:
                break
            oft, _ts, _fc, name_rva, ft = struct.unpack("<IIIII", desc)
            dll = cstr(rva2off(name_rva))
            if not dll:
                break
            names = []
            t = rva2off(oft or ft)
            step = 8 if pe32plus else 4
            while t:
                val = struct.unpack_from("<Q" if pe32plus else "<I", data, t)[0]
                if val == 0:
                    break
                if not (val >> (63 if pe32plus else 31)):
                    names.append(cstr(rva2off((val & 0x7FFFFFFF) + 2)))
                t += step
            imports[dll.lower()] = names
            off += 20
    return {"machine": machine, "imports": imports, "exports": exports}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pkg")
    ap.add_argument("--shim", default=os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "build"))
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    shim = a.shim
    shim_exports = {}
    for m in SHIM_MODULES:
        p = os.path.join(shim, m)
        if not os.path.isfile(p):
            print("缺少 shim: %s" % p)
            return 2
        info = parse_pe(open(p, "rb").read())
        shim_exports[m] = set(info["exports"])
    print("shim 导出数: " + ", ".join("%s=%d" % (k, len(v)) for k, v in shim_exports.items()))

    dlls = sorted(f for f in os.listdir(a.pkg) if f.lower().endswith(".dll"))
    print("包内 DLL: %d 个\n" % len(dlls))

    missing_total = 0
    for fn in dlls:
        info = parse_pe(open(os.path.join(a.pkg, fn), "rb").read())
        if not info:
            continue
        need = 0
        missing = []
        for dll, names in info["imports"].items():
            if dll not in SHIM_MODULES:
                continue
            need += len(names)
            for n in names:
                if n not in shim_exports[dll]:
                    missing.append("%s!%s" % (dll, n))
        tag = "OK  " if not missing else "缺失"
        print("  %s %-42s 从 shim 导入 %3d 个符号%s" % (
            tag, fn, need, "" if not missing else "，缺 %d 个" % len(missing)))
        for m in missing[:6]:
            print("        缺: %s" % m)
        missing_total += len(missing)

    print("\n== 静态核对: %s" % ("PASS（无缺失）" if missing_total == 0 else "FAIL（缺 %d 个）" % missing_total))

    # ---- 动态加载 ----
    import ctypes
    print("\n== 动态加载（让 Windows 加载器解析导入表）")
    ok = fail = 0
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(os.path.abspath(shim))
        os.add_dll_directory(os.path.abspath(a.pkg))
    for fn in dlls:
        try:
            ctypes.WinDLL(os.path.join(a.pkg, fn))
            ok += 1
        except OSError as e:
            fail += 1
            print("  FAIL %-42s %s" % (fn, str(e)[:70]))
    print("  加载成功 %d / 失败 %d" % (ok, fail))
    return 0 if (missing_total == 0 and fail == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
