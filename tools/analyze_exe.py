#!/usr/bin/env python3
"""exe 到手后第一件事：把它需要的东西列清楚。

解析 PE 的导入表，按模块分组，重点回答三个问题：
  1. 它导入了哪些 GDK 模块（xgameruntime / xmem / d3d12_xs / XAudio2_9 …）
  2. 这些导入里，我们现有的 shim 已经覆盖了哪些、还缺哪些
  3. 缺的按"阻塞优先级"排序（进程启动前解析失败 = 致命）

用法：
    python analyze_exe.py <game.exe> [--shim-dir ../../Xbgtasix/build]
"""
import argparse
import os
import struct
import sys

# 主机专有 / 需要 shim 顶上的模块（其余走 Windows 原生）
SHIMMED = ("xgameruntime.dll", "xmem.dll", "d3d12_xs.dll",
           "xgameplatform.dll", "d3d12_x.dll")
# 这些在 PC 上本来就存在
NATIVE_PREFIX = ("api-ms-win", "vcruntime", "msvcp", "ucrtbase", "concrt", "vccorlib",
                 "kernel32", "kernelbase", "user32", "advapi32", "ws2_32", "winhttp",
                 "d3d12", "dxgi", "xinput", "xaudio2", "bcrypt", "crypt32", "ole32",
                 "oleaut32", "shlwapi", "xcurl", "libhttpclient", "bink", "amd_fidelityfx",
                 "xboxbvh")


def is_native(name):
    low = name.lower()
    return any(low.startswith(p) for p in NATIVE_PREFIX)


def parse_imports(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"MZ":
        raise SystemExit("%s 不是 PE" % path)
    e = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e:e + 4] != b"PE\0\0":
        raise SystemExit("%s 没有 PE 签名" % path)
    coff = e + 4
    nsec = struct.unpack_from("<H", data, coff + 2)[0]
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

    mods = {}
    if len(dirs) > 1 and dirs[1][0]:
        off = rva2off(dirs[1][0])
        while off:
            oft, _ts, _fwd, name_rva, ft = struct.unpack_from("<IIIII", data, off)
            if not name_rva:
                break
            mod = cstr(name_rva) or "?"
            thunk_rva = oft or ft
            funcs = []
            t = rva2off(thunk_rva) if thunk_rva else None
            step, fmt = (8, "<Q") if is64 else (4, "<I")
            while t:
                val = struct.unpack_from(fmt, data, t)[0]
                if not val:
                    break
                if val >> (63 if is64 else 31):
                    funcs.append("ordinal#%d" % (val & 0xFFFF))
                else:
                    funcs.append(cstr((val & 0x7FFFFFFF) + 2) or "?")
                t += step
            mods[mod] = funcs
            off += 20
    return mods, is64


def shim_exports(shim_dir):
    """从已构建的 shim DLL 里读导出名（objdump 太脆，这里用简单 PE 导出解析）"""
    out = {}
    for name in ("xgameruntime.dll", "xmem.dll", "d3d12_xs.dll"):
        p = os.path.join(shim_dir, name)
        if not os.path.isfile(p):
            continue
        with open(p, "rb") as f:
            data = f.read()
        e = struct.unpack_from("<I", data, 0x3C)[0]
        coff = e + 4
        nsec = struct.unpack_from("<H", data, coff + 2)[0]
        opt_size = struct.unpack_from("<H", data, coff + 16)[0]
        opt = coff + 20
        dd = opt + (112 if struct.unpack_from("<H", data, opt)[0] == 0x20B else 96)
        exp_rva = struct.unpack_from("<I", data, dd)[0]
        sec = opt + opt_size
        secs = []
        for i in range(nsec):
            o = sec + 40 * i
            vs, va, rs, ra = struct.unpack_from("<IIII", data, o + 8)
            secs.append((va, max(vs, rs), ra))

        def r2o(rva):
            for va, sz, ra in secs:
                if va <= rva < va + sz:
                    return ra + (rva - va)
            return None

        if not exp_rva:
            continue
        off = r2o(exp_rva)
        if off is None:
            continue
        (_c, _t, _maj, _min, _nm, _base, nfunc, nnames,
         _af, _an, _ao) = struct.unpack_from("<IIHHIIIIIII", data, off)
        names_off = r2o(_an)
        if not names_off or nnames > 100000:
            continue
        got = []
        for i in range(nnames):
            nr = struct.unpack_from("<I", data, names_off + 4 * i)[0]
            no = r2o(nr)
            if no is None:
                continue
            end = data.find(b"\0", no)
            got.append(data[no:end].decode("latin1", "replace"))
        out[name] = set(got)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--shim-dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build"))
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    mods, is64 = parse_imports(a.exe)
    total = sum(len(v) for v in mods.values())
    print("== %s" % os.path.basename(a.exe))
    print("   架构: %s   导入模块: %d   导入函数: %d" % ("x64" if is64 else "x86", len(mods), total))

    shim = shim_exports(os.path.abspath(a.shim_dir))
    print("\n   主机专有模块（需要 shim 顶上）：")
    need, have, missing = [], [], []
    silent_native = []
    for mod, funcs in sorted(mods.items()):
        low = mod.lower()
        if low in SHIMMED:
            provided = shim.get(low, set())
            miss = [f for f in funcs if f not in provided]
            print("     %-22s %4d 个导入，已覆盖 %d，缺 %d" % (mod, len(funcs), len(funcs) - len(miss), len(miss)))
            need.append((mod, funcs))
            have.extend(f for f in funcs if f in provided)
            missing.extend((mod, f) for f in miss)
        elif is_native(mod):
            silent_native.append(mod)
        else:
            print("     %-22s %4d 个导入（未知模块，按原生处理，需确认）" % (mod, len(funcs)))

    if silent_native:
        print("     （另有 %d 个 Windows 原生模块，已跳过：%s%s）" % (
            len(silent_native), ", ".join(silent_native[:4]),
            " …" if len(silent_native) > 4 else ""))

    print("\n   合计: 需要 shim 提供 %d 个符号，已覆盖 %d，**缺 %d**" % (
        sum(len(f) for _, f in need), len(have), len(missing)))
    if missing:
        print("\n   缺失清单（前 40 个）：")
        for mod, f in missing[:40]:
            print("     %s!%s" % (mod, f))
        if len(missing) > 40:
            print("     …… 还有 %d 个" % (len(missing) - 40))
        print("\n   下一步：这些名字用 tools/gen_stubs.py 从 GDK 导入库补桩；"
              "能查到语义的先实现，剩下的靠 XBGTASIX_UNIMPL=log 跑一遍按调用顺序排。")
    else:
        print("\n   所有主机专有导入都已被现有 shim 覆盖，可以直接试着启动。")


if __name__ == "__main__":
    main()
