#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""dxil_scan.py —— 把 dump 里的每一份 DXIL 拆出来、反汇编、按特征统计

为什么需要它
------------
`docs/transient-block-wall.md` 与 `docs/rts0-analysis.md` 已经排除了两条：格式（是标准
DXIL SM6.0）、根签名（RTS0 无系统性差异）。剩下的嫌疑落在 **bitcode 本体**：
"NVIDIA 编这份 DXIL 就崩" 这类问题上，只能看程序块里的东西，而 IIS 之外没人能直接读。

本脚本不依赖 Windows / DXC：
  1. DXBC 容器 → `DXIL` 段
  2. `DXIL` 段 → `XDAT` 条目表 → 类型 `0x07` 的 payload（里面是 `DXIL` 子头 + 裸 bitcode）
  3. bitcode → 交给本机 `llvm-dis` 反汇编（Arch 的 llvm 包自带；DXC 出的是 LLVM 3.7 的
     位码，新 llvm-dis 能读旧位码，实测 53/53 能解）
  4. 统计：target triple、Handle 类型、用到的 dx.op、原子操作及其类型、raw/typed 缓冲访问

用法
----
    python3 tools/dxil_scan.py build/dumps            # 汇总
    python3 tools/dxil_scan.py build/dumps --dump cs_01   # 打印某一份的 .ll 片段
    python3 tools/dxil_scan.py build/dumps --ll-dir /tmp/ll  # 把 .ll 全部落盘

退出码 0 = 正常。
"""

import os
import re
import struct
import subprocess
import sys
import tempfile


def read(path):
    with open(path, "rb") as f:
        return f.read()


def dxbc_chunk(blob, fourcc):
    if len(blob) < 32 or blob[:4] != b"DXBC":
        return None
    n = struct.unpack_from("<I", blob, 28)[0]
    for o in struct.unpack_from("<%dI" % n, blob, 32):
        if blob[o:o + 4] == fourcc:
            size = struct.unpack_from("<I", blob, o + 4)[0]
            return blob[o + 8:o + 8 + size]
    return None


def xdat_entry(blob, want):
    """在（DXIL 段体这样的）blob 里找一个 XDAT 条目，返回 payload 字节。"""
    i = blob.find(b"XDAT")
    if i < 0:
        return None
    _ver, tab = struct.unpack_from("<II", blob, i + 4)
    first = struct.unpack_from("<I", blob, tab + 4)[0]
    n = (first - tab) // 12
    for k in range(n):
        t, off, size = struct.unpack_from("<III", blob, tab + 12 * k)
        if t == want:
            return blob[off:off + size]
    return None


def extract_bitcode(path):
    """cs_*.bin → (bitcode, 元信息)。GTA6 这批是 DXIL 段 → XDAT[0x07] → 子头(24B) + bitcode。"""
    blob = read(path)
    dxil = dxbc_chunk(blob, b"DXIL")
    if dxil is None:
        return None, {"err": "没有 DXIL 段"}
    payload = xdat_entry(dxil, 0x07)
    if payload is None:
        return None, {"err": "XDAT 里没有 0x07 条目"}
    if len(payload) < 24:
        return None, {"err": "0x07 条目太短"}
    prog, mid = struct.unpack_from("<II", payload, 0)
    tag = payload[8:12]
    sub_ver, sub_unk, bc_size = struct.unpack_from("<III", payload, 12)
    bc = payload[24:24 + bc_size]
    meta = {"prog": prog, "mid": mid, "tag": tag, "sub_ver": sub_ver,
            "sub_unk": sub_unk, "bc_size": bc_size, "payload": len(payload)}
    if bc[:4] != b"BC\xc0\xde":
        meta["err"] = "bitcode magic 不对：%s" % bc[:4].hex()
        return None, meta
    return bc, meta


DXOP = re.compile(r"@dx\.op\.([A-Za-z0-9_.]+)\(")
HANDLE = re.compile(r"%dx\.types\.Handle = type \{([^}]*)\}")
TRIPLE = re.compile(r'target triple = "([^"]+)"')
ATOMIC = re.compile(r"@dx\.op\.(atomic[A-Za-z]*)\.")


def disassemble(bc, llvm_dis="llvm-dis"):
    with tempfile.NamedTemporaryFile(suffix=".bc", delete=False) as f:
        f.write(bc)
        bc_path = f.name
    out = bc_path + ".ll"
    try:
        r = subprocess.run([llvm_dis, bc_path, "-o", out],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return None, (r.stderr.strip().splitlines() or ["llvm-dis 失败"])[-1]
        with open(out, "r", encoding="utf-8", errors="replace") as g:
            return g.read(), None
    finally:
        for p in (bc_path, out):
            try:
                os.unlink(p)
            except OSError:
                pass


def analyse(ll_text):
    info = {}
    m = TRIPLE.search(ll_text)
    info["triple"] = m.group(1) if m else "?"
    m = HANDLE.search(ll_text)
    info["handle"] = (m.group(1).strip() if m else "?")
    ops = {}
    for name in DXOP.findall(ll_text):
        ops[name] = ops.get(name, 0) + 1
    info["ops"] = ops
    info["atomic"] = sorted({m for m in ATOMIC.findall(ll_text)})
    # 原子操作的**返回值/操作数类型**：ll 里形如 `call i32 @dx.op.atomicBinOp.i32(...)`，
    # 也有 `call float @dx.op.atomicBinOp.f32(...)` —— 后者正是"对浮点做整数原子"的味道
    info["atomic_types"] = sorted(set(re.findall(
        r"call\s+([A-Za-z0-9.]+)\s+@dx\.op\.atomic[A-Za-z]*\.", ll_text)))
    info["f32_atomic"] = bool(re.search(r"call\s+float\s+@dx\.op\.atomic", ll_text))
    info["typed_uav"] = sorted(set(re.findall(r"@dx\.op\.(textureLoad|textureStore|"
                                              r"textureGather|bufferLoad|bufferStore)", ll_text)))
    info["raw"] = sorted(set(re.findall(r"@dx\.op\.(rawBufferLoad|rawBufferStore)", ll_text)))
    info["lines"] = ll_text.count("\n")
    return info


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 0
    d = argv[1]
    want_dump = None
    ll_dir = None
    for i, a in enumerate(argv):
        if a == "--dump" and i + 1 < len(argv):
            want_dump = argv[i + 1]
        if a == "--ll-dir" and i + 1 < len(argv):
            ll_dir = argv[i + 1]
    if ll_dir:
        os.makedirs(ll_dir, exist_ok=True)

    files = sorted(f for f in os.listdir(d)
                   if f.startswith("cs_") and f.endswith(".bin"))
    if not files:
        print("没有 cs_*.bin：%s" % d)
        return 1

    rows = []
    for f in files:
        bc, meta = extract_bitcode(os.path.join(d, f))
        if bc is None:
            rows.append((f, meta, None, meta.get("err")))
            continue
        if want_dump and want_dump in f:
            print("== %s 元信息 %s" % (f, meta))
        ll, err = disassemble(bc)
        if ll is None:
            rows.append((f, meta, None, err))
            continue
        if ll_dir:
            with open(os.path.join(ll_dir, f + ".ll"), "w", encoding="utf-8") as g:
                g.write(ll)
        if want_dump and want_dump in f:
            print(ll[:2000])
        rows.append((f, meta, analyse(ll), None))

    ok = [r for r in rows if r[2]]
    print("反汇编成功 %d / %d" % (len(ok), len(rows)))
    if ok:
        trip = sorted({r[2]["triple"] for r in ok})
        hand = sorted({r[2]["handle"] for r in ok})
        print("target triple: %s" % ", ".join(trip))
        print("dx.types.Handle 定义: %s" % " | ".join("{ %s }" % h for h in hand))
        f32 = [r[0] for r in ok if r[2]["f32_atomic"]]
        print("对 float 做原子（call float @dx.op.atomic*）: %s"
              % (", ".join(f32) if f32 else "无"))
        at = sorted({t for r in ok for t in r[2]["atomic_types"]})
        print("原子操作返回值类型: %s" % (", ".join(at) if at else "无原子"))
        atomic_users = [r[0] for r in ok if r[2]["atomic"]]
        print("用了原子的 shader: %d 个%s" % (len(atomic_users),
              ("（" + ", ".join(atomic_users[:6]) + ("…" if len(atomic_users) > 6 else "") + "）")
              if atomic_users else ""))
        raw_users = [r[0] for r in ok if r[2]["raw"]]
        typed_users = [r[0] for r in ok if r[2]["typed_uav"]]
        print("用 raw（untyped）缓冲访问: %d/%d；用 typed texture/buffer 访问: %d/%d"
              % (len(raw_users), len(ok), len(typed_users), len(ok)))
        allops = {}
        for r in ok:
            for k, v in r[2]["ops"].items():
                allops[k] = allops.get(k, 0) + v
        print("dx.op 出现频次（前 12）:")
        for k, v in sorted(allops.items(), key=lambda kv: -kv[1])[:12]:
            print("   %-28s %d" % (k, v))
    bad = [r for r in rows if not r[2]]
    if bad:
        print("失败 %d 个:" % len(bad))
        for r in bad:
            print("   %-24s %s" % (r[0], r[3]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
