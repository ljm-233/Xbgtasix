#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""handle_kind_census.py —— 统计 DXIL 里每个资源句柄「被标成什么 kind」与「实际怎么用」

为什么需要它
------------
victory 从主机 shader 生成桌面 DXIL 时，`dx.op.annotateHandle` 的第三个参数（kind）
是**猜**出来的（`emit/lower.rs` 里按变量名里有没有 "store" 做字符串匹配）。kind 标错
以后 vkd3d 会按错误的资源类型建 view（buffer 当 texture 就要格式，缺省 R32f），
整数原子就被绑到 float 上，SPIR-V 非法。

本脚本做纯静态交叉统计，两条腿：

  gta6     —— 读 `build/dumps/cs_*.bin`（GTA6 原始 Xbox 模块），按 `dxil_scan.py`
              现有路径拆出 bitcode、`llvm-dis` 反汇编，统计**真值侧**的句柄类别与用法。
              GTA6 这批是 DXIL 1.0 风格：**没有 annotateHandle**，kind 来自
              `dx.op.createHandle` 的 class 字节（SRV/UAV/CBV/Sampler），
              具体是 raw buffer 还是 texture 只能由后续访问指令推出。
  victory  —— 读一个装着 `.ll` 的目录（只读），统计 `annotateHandle` 的 kind 取值分布，
              并把每个 annotate 后面的访问指令族拉出来做交叉表，标出**矛盾**：
              kind 说是 texture，访问指令却是 rawBuffer*/buffer*。

`dxil_scan.py` 只 import，不改。素材全部只读，产物只走 stdout / --json。

用法
----
    python3 tools/handle_kind_census.py gta6    build/dumps  [--json /tmp/x.json]
    python3 tools/handle_kind_census.py victory /tmp/atomscan/ll [--json /tmp/y.json]
    python3 tools/handle_kind_census.py victory /tmp/atomscan/ll --conflicts   # 只打矛盾样本
    python3 tools/handle_kind_census.py slots   /tmp/atomscan/ll [--json /tmp/z.json]

退出码 0 = 正常（统计成功）。
"""

import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxil_scan  # noqa: E402  —— 只调 extract_bitcode / disassemble

# ---------------------------------------------------------------- DXIL 常量

# DXIL 的 DXIL::ResourceKind（DXC DxilConstants.h）。llvm-dis 里 annotateHandle 的
# ResourceProperties 第一个 i32 就是它，低位 12 bit 是 kind，0x1000 是 UAV 标志。
RESOURCE_KIND = {
    0: "Invalid", 1: "Texture1D", 2: "Texture2D", 3: "Texture2DArray", 4: "Texture3D",
    5: "TextureCube", 6: "Texture1DArray", 7: "Texture2DMS", 8: "Texture2DMSArray",
    9: "TextureCubeArray", 10: "TypedBuffer", 11: "RawBuffer", 12: "StructuredBuffer",
    13: "CBuffer", 14: "Sampler", 15: "TBuffer", 16: "RTAccelerationStructure",
    17: "FeedbackTexture2D", 18: "FeedbackTexture2DArray",
}
KIND_UAV_FLAG = 0x1000

# 访问指令族 → 它们只可能作用在哪些 kind 上。这是判据的代码化。
ACCESS_FAMILY = {
    "rawBufferLoad": {"raw"},
    "rawBufferStore": {"raw"},
    "bufferLoad": {"buffer"},
    "bufferStore": {"buffer"},
    "textureLoad": {"texture"},
    "textureStore": {"texture"},
    "textureGather": {"texture"},
    "textureGatherCmp": {"texture"},
    "textureSample": {"texture"},
    "textureSampleBias": {"texture"},
    "textureSampleLevel": {"texture"},
    "textureSampleGrad": {"texture"},
    "textureSampleCmp": {"texture"},
    "textureSampleCmpLevelZero": {"texture"},
    "textureGatherCmpGreen": {"texture"},
    "textureGatherCmpRed": {"texture"},
    "textureGatherCmpBlue": {"texture"},
    "textureGatherCmpAlpha": {"texture"},
    "cbufferLoad": {"cbuffer"},
    "cbufferLoadLegacy": {"cbuffer"},
    "getDimensions": {"any"},          # 任何资源都可能被问尺寸
    "calculateLOD": {"texture"},
    "sampleIndex": {"texture"},
    "checkAccessFullyMapped": {"any"},
    "texture2DMSGetSamplePosition": {"texture"},
    "renderTargetGetSamplePosition": {"texture"},
    "renderTargetGetSampleCount": {"texture"},
    "bufferUpdateCounter": {"buffer", "raw"},
    "atomicBinOp": {"uav"},            # 只约束"是 UAV"，不约束 raw/typed/structured
    "atomicCompareExchange": {"uav"},
}

# 家族 → 允许的 kind（低位 12bit）。uav 表示任何 UAV 型的 buffer/typed 都行。
FAMILY_ALLOWED_KINDS = {
    "raw": {11},
    "buffer": {10, 12},
    "texture": {1, 2, 3, 4, 5, 6, 7, 8, 9, 17, 18},
    "cbuffer": {13, 15},
    "uav": {10, 11, 12, 1, 2, 3, 4, 5, 6, 7, 8, 9},
    "any": set(RESOURCE_KIND),
}

ANNOTATE = re.compile(
    r"^\s*(%[\w.]+)\s*=\s*call\s+%dx\.types\.Handle\s+@dx\.op\.annotateHandle"
    r"\(i32\s+216,\s*%dx\.types\.Handle\s+(%[\w.]+),\s*"
    r"%dx\.types\.ResourceProperties\s+\{\s*i32\s+(-?\d+),\s*i32\s+(-?\d+)\s*\}\)")
CREATE_BIND = re.compile(
    r"^\s*(%[\w.]+)\s*=\s*call\s+%dx\.types\.Handle\s+@dx\.op\.createHandleFromBinding"
    r"\(i32\s+217,\s*%dx\.types\.ResBind\s+\{\s*i32\s+(-?\d+),\s*i32\s+(-?\d+),\s*"
    r"i32\s+(-?\d+),\s*i8\s+(-?\d+)\s*\},\s*i32\s+(-?\d+),\s*i1\s+(\w+)\)")
CREATE_HEAP = re.compile(
    r"^\s*(%[\w.]+)\s*=\s*call\s+%dx\.types\.Handle\s+@dx\.op\.createHandleFromHeap"
    r"\(i32\s+218,\s*i32\s+(.+?),\s*i1\s+(\w+),\s*i1\s+(\w+)\)")
CREATE_OLD = re.compile(
    r"^\s*(%[\w.]+)\s*=\s*call\s+%dx\.types\.Handle\s+@dx\.op\.createHandle"
    r"\(i32\s+57,\s*i8\s+(-?\d+),\s*i32\s+(-?\d+),\s*i32\s+(-?\d+),\s*i1\s+(\w+)\)")
DXOP_CALL = re.compile(r"@dx\.op\.([A-Za-z0-9_]+)\.([A-Za-z0-9]+)\((.*)\)\s*$")
PHI = re.compile(r"^\s*(%[\w.]+)\s*=\s*phi\s+%dx\.types\.Handle\s+(.+)$")


def classify_family(op):
    return ACCESS_FAMILY.get(op)


def kind_name(raw):
    k = raw & 0xFFF
    uav = bool(raw & KIND_UAV_FLAG)
    return "%s%s" % (RESOURCE_KIND.get(k, "Kind%d" % k), "+UAV" if uav else ""), k, uav


def parse_module(text):
    """返回 (handles, annotates, creates, unresolved)。

    handles: value -> dict(defkind='annotate'|'bind'|'heap'|'old'|'phi', ...)
    """
    lines = text.splitlines()
    handles = {}
    annotates = []
    phis = collections.defaultdict(list)
    for ln, line in enumerate(lines, 1):
        m = ANNOTATE.match(line)
        if m:
            dst, src, kind, elem = m.groups()
            handles[dst] = {"def": "annotate", "line": ln, "src": src,
                            "kind": int(kind), "elem": int(elem)}
            annotates.append(dst)
            continue
        m = CREATE_BIND.match(line)
        if m:
            dst, lb, ub, space, cls, idx, nu = m.groups()
            handles[dst] = {"def": "bind", "line": ln, "lower": int(lb), "upper": int(ub),
                            "space": int(space), "class": int(cls), "index": int(idx),
                            "nonuniform": nu == "true"}
            continue
        m = CREATE_HEAP.match(line)
        if m:
            dst, idxexpr, nu, dsc = m.groups()
            handles[dst] = {"def": "heap", "line": ln, "index_expr": idxexpr.strip(),
                            "nonuniform": nu == "true", "descriptor_heap": dsc == "true"}
            continue
        m = CREATE_OLD.match(line)
        if m:
            dst, cls, rng, idx, nu = m.groups()
            handles[dst] = {"def": "createHandle", "line": ln, "class": int(cls),
                            "range": int(rng), "index": int(idx), "nonuniform": nu == "true"}
            continue
        m = PHI.match(line)
        if m:
            dst, incoming = m.groups()
            handles[dst] = {"def": "phi", "line": ln, "incoming": incoming}
            for v in re.findall(r"%[\w.]+", incoming):
                phis[dst].append(v)
            continue
    return lines, handles, annotates, phis


def collect_uses(lines, handles):
    """value -> [(line_no, op, family)]。只看作为 dx.op 调用参数出现的用法。"""
    uses = collections.defaultdict(list)
    for ln, line in enumerate(lines, 1):
        m = DXOP_CALL.search(line)
        if not m:
            continue
        op = m.group(1)
        args = re.findall(r"%[\w.]+", m.group(3))
        fam = classify_family(op)
        if fam is not None:
            fam = tuple(sorted(fam))
        for v in args:
            if v in handles:
                uses[v].append({"line": ln, "op": op, "family": fam})
    return uses


def access_ops_for(value, handles, uses, phis, depth=0):
    """把句柄值（含 phi 合并）之后的访问指令全部拉出来。"""
    out = []
    if depth > 8:
        return out
    for u in uses.get(value, ()):
        if u["family"]:
            out.append(u)
    for src in phis.get(value, ()):
        out += access_ops_for(src, handles, uses, phis, depth + 1)
    return out


def origin_label(h):
    """把一个 createXxx 定义压成"槽"的可读标识。"""
    if not h:
        return "?"
    d = h["def"]
    if d == "bind":
        return "bind(space=%d,lower=%d,upper=%d,class=%d,idx=%d)" % (
            h["space"], h["lower"], h["upper"], h["class"], h["index"])
    if d == "heap":
        return "heap(%s)" % h["index_expr"]
    if d == "createHandle":
        return "createHandle(class=%d,range=%d,idx=%d)" % (
            h["class"], h["range"], h["index"])
    if d == "annotate":
        return "annotate->" + str(h["kind"])
    if d == "phi":
        return "phi"
    return d


# ---------------------------------------------------------------- victory 侧

def scan_victory(dirpath, only_conflicts=False):
    files = sorted(f for f in os.listdir(dirpath) if f.endswith(".ll"))
    kinds = collections.Counter()
    kind_access = collections.defaultdict(collections.Counter)
    per_module_kinds = {}
    conflicts = []
    modules_with_annotate = 0
    total_annotates = 0
    # 同一模块同一槽出现多种 kind 的情况（自相矛盾）
    slot_kinds = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    for f in files:
        path = os.path.join(dirpath, f)
        with open(path, "r", encoding="utf-8", errors="replace") as g:
            text = g.read()
        lines, handles, annotates, phis = parse_module(text)
        if not annotates:
            per_module_kinds[f] = {}
            continue
        modules_with_annotate += 1
        total_annotates += len(annotates)
        uses = collect_uses(lines, handles)
        mk = collections.Counter()
        for a in annotates:
            h = handles[a]
            name, low, uav = kind_name(h["kind"])
            kinds[h["kind"]] += 1
            mk[h["kind"]] += 1
            ops = access_ops_for(a, handles, uses, phis)
            opnames = collections.Counter(o["op"] for o in ops)
            for op in opnames:
                kind_access[h["kind"]][op] += 1
            src = h["src"]
            src_h = handles.get(src)
            # 向前溯源：annotate 的输入本身可能也是 annotate（链式）
            hop = 0
            while src_h and src_h["def"] == "annotate" and hop < 4:
                src = src_h["src"]
                src_h = handles.get(src)
                hop += 1
            slot = origin_label(src_h)
            slot_kinds[f][slot][h["kind"]] += 1
            # 冲突判定
            fams = set()
            for o in ops:
                if o["family"]:
                    fams |= set(o["family"])
            fams -= {"any", "uav"}
            if low in (10, 11, 12):
                kind_fam = "buffer"      # TypedBuffer / RawBuffer / StructuredBuffer
            elif low in (13, 15):
                kind_fam = "cbuffer"
            elif low == 14:
                kind_fam = "sampler"
            elif low in (1, 2, 3, 4, 5, 6, 7, 8, 9, 17, 18):
                kind_fam = "texture"
            else:
                kind_fam = "other"
            # sampler 句柄会作为 textureGather/textureSample 的第二个参数出现，
            # 那不是"被当 texture 用"，别算矛盾。
            if kind_fam == "sampler":
                fams -= {"texture"}
            # 只有"跨族"才算矛盾：buffer 族内部的 raw/structured/typed 差别
            # （rawBufferLoad 作用于 StructuredBuffer 之类）是精度问题，不是错标。
            CROSS = {"buffer": {"texture", "cbuffer"},
                     "texture": {"buffer", "raw", "cbuffer"},
                     "cbuffer": {"buffer", "raw", "texture"},
                     "sampler": {"buffer", "raw", "cbuffer"}}
            bad = sorted(fams & CROSS.get(kind_fam, set()))
            if bad:
                conflicts.append({
                    "module": f, "line": h["line"], "kind": h["kind"], "kind_name": name,
                    "slot": slot, "ops": dict(opnames),
                    "families": sorted(fams), "wrong_families": sorted(bad),
                    "handle_src_line": src_h["line"] if src_h else None,
                    "handle_src": slot,
                    "text": lines[h["line"] - 1].strip()[:200],
                })
        per_module_kinds[f] = {str(k): v for k, v in mk.items()}

    conflict_modules = {c["module"] for c in conflicts}
    # 模块内同槽多 kind
    inconsistent = []
    for f, slots in slot_kinds.items():
        for slot, kc in slots.items():
            nz = {k: v for k, v in kc.items() if v}
            if len({kind_name(k)[1] for k in nz}) > 1:
                inconsistent.append({"module": f, "slot": slot,
                                     "kinds": {str(k): v for k, v in nz.items()},
                                     "names": {str(kind_name(k)[0]): v for k, v in nz.items()}})
    result = {
        "dir": dirpath,
        "modules_total": len(files),
        "modules_with_annotate": modules_with_annotate,
        "annotate_calls": total_annotates,
        "kind_census": {str(k): {"name": kind_name(k)[0], "low": kind_name(k)[1],
                                 "uav": kind_name(k)[2], "count": v}
                        for k, v in sorted(kinds.items(), key=lambda kv: -kv[1])},
        "kind_access": {str(k): dict(v) for k, v in kind_access.items()},
        "conflicts": conflicts,
        "conflict_modules": sorted(conflict_modules),
        "inconsistent_slots": inconsistent,
        "per_module_kinds": per_module_kinds,
    }
    if only_conflicts:
        for c in conflicts:
            print("%-34s L%-5d kind=%-14s slot=%-46s ops=%s"
                  % (c["module"], c["line"], c["kind_name"], c["slot"],
                     ",".join("%s:%d" % kv for kv in sorted(c["ops"].items()))))
        return result
    print("== victory: %d 个 .ll，其中 %d 个有 annotateHandle，共 %d 次调用"
          % (len(files), modules_with_annotate, total_annotates))
    print("-- kind 取值分布（低位 12bit 是 DXIL ResourceKind，0x1000 是 UAV 位）")
    for k, v in sorted(kinds.items(), key=lambda kv: -kv[1]):
        nm, low, uav = kind_name(k)
        print("   %-8d (0x%04x) %-22s %5d 次  %.1f%%"
              % (k, k, nm, v, 100.0 * v / max(1, total_annotates)))
    print("-- 每个 kind 后面的访问指令（前 6）")
    for k in sorted(kind_access, key=lambda k: -kinds[k]):
        for kk, vv in sorted(kind_access[k].items(), key=lambda kv: -kv[1])[:6]:
            print("   %-8d %-22s %-26s %d" % (k, kind_name(k)[0], kk, vv))
    print("-- 矛盾样本（kind 与访问指令族不符）：%d 条，涉及 %d 个模块"
          % (len(conflicts), len(conflict_modules)))
    by_mod = collections.Counter(c["module"] for c in conflicts)
    for m, n in by_mod.most_common(12):
        print("   %-34s %d" % (m, n))
    print("-- 同槽多 kind（模块内部自相矛盾）：%d 处" % len(inconsistent))
    for it in inconsistent[:8]:
        print("   %-34s %-46s %s" % (it["module"], it["slot"], it["names"]))
    return result


# ---------------------------------------------------------------- GTA6 侧

def scan_gta6(dirpath):
    files = sorted(f for f in os.listdir(dirpath) if f.startswith("cs_") and f.endswith(".bin"))
    rows = []
    class_census = collections.Counter()
    class_access = collections.defaultdict(collections.Counter)
    handle_uses = collections.Counter()
    atomics = []
    fail = []
    annotate_seen = 0
    for f in files:
        bc, meta = dxil_scan.extract_bitcode(os.path.join(dirpath, f))
        if bc is None:
            fail.append((f, meta.get("err")))
            continue
        text, err = dxil_scan.disassemble(bc)
        if text is None:
            fail.append((f, err))
            continue
        annotate_seen += text.count("dx.op.annotateHandle")
        lines, handles, annotates, phis = parse_module(text)
        uses = collect_uses(lines, handles)
        per_file = collections.Counter()
        for v, h in handles.items():
            if h["def"] != "createHandle":
                continue
            ops = access_ops_for(v, handles, uses, phis)
            opnames = collections.Counter(o["op"] for o in ops)
            cls = h["class"]
            class_census[cls] += 1
            per_file[cls] += 1
            for op in opnames:
                class_access[cls][op] += 1
                handle_uses[op] += opnames[op]
            for u in ops:
                if u["op"] in ("atomicBinOp", "atomicCompareExchange"):
                    atomics.append({"module": f, "class": cls, "create_line": h["line"],
                                    "op": u["op"], "op_line": u["line"],
                                    "range": h["range"], "index": h["index"],
                                    "all_ops": dict(opnames),
                                    "text": lines[u["line"] - 1].strip()[:200]})
        rows.append({"module": f, "handles": {str(k): v for k, v in per_file.items()},
                     "lines": len(lines)})
    # 有多少 handle 同时被 raw 访问 + 原子访问（最接近我们 bug 的参照）
    both = [a for a in atomics if any(o in a["all_ops"] for o in
                                      ("rawBufferLoad", "rawBufferStore",
                                       "bufferLoad", "bufferStore"))]
    result = {
        "dir": dirpath, "modules": len(files), "disassembled": len(rows),
        "failures": fail,
        "annotate_calls": annotate_seen,
        "create_handle_calls": sum(class_census.values()),
        "class_census": {str(k): v for k, v in sorted(class_census.items())},
        "class_access": {str(k): dict(v) for k, v in class_access.items()},
        "atomics": atomics,
        "atomics_with_raw_or_typed": both,
        "per_module": rows,
    }
    CLASSNAME = {0: "SRV", 1: "UAV", 2: "CBV", 3: "Sampler", 4: "TBuffer?"}
    print("== GTA6: %d 个 cs_*.bin，反汇编成功 %d 个" % (len(files), len(rows)))
    if fail:
        print("   失败 %d：%s" % (len(fail), fail[:4]))
    print("   annotateHandle 调用总数: %d" % annotate_seen)
    print("-- createHandle 的 class 字节分布（%d 次）" % sum(class_census.values()))
    for k, v in sorted(class_census.items()):
        print("   class=%d %-8s %5d" % (k, CLASSNAME.get(k, "?"), v))
    print("-- 每个 class 后面出现的访问指令（全量）")
    for k in sorted(class_access):
        items = sorted(class_access[k].items(), key=lambda kv: -kv[1])
        print("   class=%d %s: %s" % (k, CLASSNAME.get(k, "?"),
                                      ", ".join("%s:%d" % kv for kv in items)))
    print("-- 原子操作: %d 处，其中句柄同时被 raw/typed buffer 访问的 %d 处"
          % (len(atomics), len(both)))
    for a in both[:10]:
        print("   %-22s class=%d %s (line %d) 同一句柄: %s"
              % (a["module"], a["class"], a["op"], a["op_line"],
                 ", ".join("%s:%d" % kv for kv in sorted(a["all_ops"].items()))))
    return result


# ---------------------------------------------------------------- 槽口径（语义归一化）

RE_ASSIGN = re.compile(r"^(%[\w.]+)\s*=\s*(.*)$")
RE_HEAP = re.compile(r"createHandleFromHeap\(i32 218, i32 ([^,]+),")
RE_ANN2 = re.compile(r"annotateHandle\(i32 216, %dx\.types\.Handle (%[\w.]+), "
                     r"%dx\.types\.ResourceProperties \{ i32 (-?\d+), i32 (-?\d+) \}\)")
RE_BIND2 = re.compile(r"createHandleFromBinding\(i32 217, %dx\.types\.ResBind \{ i32 (-?\d+), "
                      r"i32 (-?\d+), i32 (-?\d+), i8 (\d+) \}")
RE_HNDARG = re.compile(r"%dx\.types\.Handle (%[\w.]+)")


def _int(t):
    t = t.strip()
    if t.startswith("%"):
        return None
    try:
        return int(t, 0)
    except ValueError:
        return None


class _Aff:
    """常量 + 变量集合。lossy=True 表示常数运算作用在了变量上（`lshr %x, 5`），
    这时 `%x` 和 `lshr %x,5` 不是同一样东西，不能再互相合并。"""
    __slots__ = ("c", "vars", "lossy")

    def __init__(self, c=0, vars=(), lossy=False):
        self.c = c
        self.vars = tuple(sorted(set(vars)))
        self.lossy = lossy


def _aff_bin(a, b, op):
    if op == "add":
        return _Aff(a.c + b.c, a.vars + b.vars)
    if op == "sub":
        return _Aff(a.c - b.c, a.vars + b.vars)
    if not a.vars and not b.vars:
        return _Aff(a.c * b.c, ())
    if not b.vars:
        return _Aff(a.c * b.c, a.vars)
    if not a.vars:
        return _Aff(a.c * b.c, b.vars)
    return _Aff(0, a.vars + b.vars)


def slot_map(lines):
    """把 createHandleFromHeap 的索引表达式折成 (语义变量, 常量) = descriptor 槽。

    只做加/减/乘/移位/与 和 extractvalue 的展开，够覆盖 victory 的
    `lshr (add %base, C), 5` 形态；变量用 semi() 落到
    `cbuf(<handle>,<offset>).<field>`，这样同一个 CBV 字段被 load 成多个 SSA
    名也不会把同一个槽拆成两个。
    """
    defs = {}
    for ln in lines:
        m = RE_ASSIGN.match(ln.strip())
        if m:
            defs[m.group(1)] = m.group(2)
    sem = {}

    def semi(t, d=0):
        t = t.strip()
        if not t.startswith("%") or d > 12:
            return t
        if t in sem:
            return sem[t]
        r = defs.get(t)
        if r is None:
            sem[t] = t
            return t
        m = re.match(r"extractvalue\s+(\S+)\s*,\s*i32\s+(\d+)", r)
        if m:
            src, fld = m.group(1), m.group(2)
            rr = defs.get(src, "")
            mm = (re.search(r"cbufferLoadLegacy\.i32\(i32 59, %dx\.types\.Handle (%[\w.]+), "
                            r"i32 (\d+)\)", rr)
                  or re.search(r"dx\.op\.cbufferLoad\.i32\(i32 60, %dx\.types\.Handle (%[\w.]+), "
                               r"i32 (\d+), i32 (\d+)\)", rr))
            key = ("cbuf(%s,%s).%s" % (semi(mm.group(1), d + 1), mm.group(2), fld)
                   if mm else "%s.%s" % (src, fld))
            sem[t] = key
            return key
        m = re.match(r"(?:zext|trunc|bitcast)\s+\S+\s+(%?[\w.]+)\s+to\s", r)
        if m:
            v = semi(m.group(1), d + 1)
            sem[t] = v
            return v
        sem[t] = t
        return t

    cache = {}

    def ev(t, d=0):
        if d > 20:
            return _Aff(0, ("deep",))
        t = t.strip()
        n = _int(t)
        if n is not None:
            return _Aff(n, ())
        if t in cache:
            return cache[t]
        r = defs.get(t)
        if r is None:
            a = _Aff(0, (semi(t),))
            cache[t] = a
            return a
        m = re.match(r"(add|sub|mul)\s+i\d+\s+(.+)$", r)
        if m:
            ops = [x.strip() for x in m.group(2).split(",")]
            if len(ops) == 2:
                v = _aff_bin(ev(ops[0], d + 1), ev(ops[1], d + 1), m.group(1))
                cache[t] = v
                return v
        m = re.match(r"(shl|lshr|and)\s+i\d+\s+(.+)$", r)
        if m:
            ops = [x.strip() for x in m.group(2).split(",")]
            if len(ops) == 2:
                k = _int(ops[1])
                a = ev(ops[0], d + 1)
                op = m.group(1)
                if not a.vars:
                    if op == "shl" and k is not None:
                        v = _Aff(a.c << k, (), a.lossy)
                    elif op == "lshr" and k is not None:
                        v = _Aff(a.c >> k, (), a.lossy)
                    elif k is not None:
                        v = _Aff(a.c & k, (), a.lossy)
                    else:
                        v = _Aff(0, (), a.lossy)
                elif op == "lshr" and k is not None and a.c % (1 << k) == 0:
                    # (base + C) >> k 在 C 是 2^k 的倍数时能精确拆开，
                    # 这正是 victory 的 `lshr (add %base, C), 5` 形态。
                    v = _Aff(a.c >> k, a.vars, a.lossy)
                elif op == "and" and k is not None and (a.c & ~k) == 0:
                    v = _Aff(a.c & k, a.vars, a.lossy)
                else:
                    v = _Aff(0, ("~" + x for x in a.vars), True)
                cache[t] = v
                return v
        v = _Aff(0, (semi(t),))
        cache[t] = v
        return v

    root = {}
    for ln in lines:
        s = ln.strip()
        m = RE_ASSIGN.match(s)
        lhs = m.group(1) if m else None
        if not lhs:
            continue
        h = RE_HEAP.search(s)
        if h:
            a = ev(h.group(1))
            root[lhs] = ("+".join(a.vars) or "-", a.c, a.lossy)
            continue
        b = RE_BIND2.search(s)
        if b:
            root[lhs] = ("BIND:class%s:space%s:[%s..%s]" % (b.group(4), b.group(3),
                                                           b.group(1), b.group(2)), 0, False)
            continue
        a2 = RE_ANN2.search(s)
        if a2:
            root[lhs] = root.get(a2.group(1), ("?", 0, False))
    # 变量再归一一次（同一 CBV 字段的不同 SSA 名合并）
    norm = {}
    for v, val in root.items():
        name, off = val[0], val[1]
        lossy = val[2] if len(val) > 2 else False
        if name.startswith("BIND:") or name == "?":
            norm[v] = (name, off, lossy)
        else:
            vs = "+".join(sorted({semi(x.lstrip("~")) if x.startswith("~") else semi(x)
                                  for x in name.split("+")}))
            norm[v] = (vs, off, lossy)
    return norm


def scan_victory_slots(dirpath):
    """槽口径：同一个 descriptor 槽上，annotate 标的 kind 与访问指令族对照。"""
    files = sorted(f for f in os.listdir(dirpath) if f.endswith(".ll"))
    rows = []
    total_slots = 0
    for f in files:
        with open(os.path.join(dirpath, f), "r", encoding="utf-8", errors="replace") as g:
            lines = g.read().splitlines()
        slot = slot_map(lines)
        kinds = collections.defaultdict(collections.Counter)
        ops = collections.defaultdict(collections.Counter)
        evid = collections.defaultdict(lambda: collections.defaultdict(list))
        for ln, line in enumerate(lines, 1):
            s = line.strip()
            a = RE_ANN2.search(s)
            if a:
                h, kind = a.group(1), int(a.group(2))
                sl = slot.get(h)
                if sl:
                    kinds[sl][kind] += 1
                    evid[sl][kind].append(ln)
                continue
            m = re.search(r"@dx\.op\.([A-Za-z0-9_]+)\.", s)
            if not m:
                continue
            hh = RE_HNDARG.search(s)
            if not hh:
                continue
            sl = slot.get(hh.group(1))
            if not sl:
                continue
            ops[sl][m.group(1)] += 1
        for sl in set(kinds) | set(ops):
            name, off = sl[0], sl[1]
            lossy = sl[2] if len(sl) > 2 else False
            if name.startswith("BIND:") or name == "?":
                continue           # 绑定槽另算，槽口径只覆盖 heap 直索引
            total_slots += 1
            kk = {int(k): v for k, v in kinds[sl].items()}
            oo = dict(ops[sl])
            klows = {kind_name(k)[1] for k in kk}
            fams = set()
            for op in oo:
                fam = classify_family(op)
                if fam:
                    fams |= set(fam)
            tex_ops = bool(fams & {"texture"})
            buf_ops = bool(fams & {"raw", "buffer"})
            cb_ops = bool(fams & {"cbuffer"})
            has_atomic = any(op.startswith("atomic") for op in oo)
            bad = []
            if tex_ops and (klows - ({1, 2, 3, 4, 5, 6, 7, 8, 9, 17, 18} | {14})):
                bad.append("texture-ops-but-kind-%s" % sorted(klows))
            if buf_ops and klows & {1, 2, 3, 4, 5, 6, 7, 8, 9, 17, 18}:
                bad.append("buffer-ops-but-texture-kind-%s" % sorted(klows))
            if cb_ops and (klows - {13, 15}):
                bad.append("cbuffer-ops-but-kind-%s" % sorted(klows))
            if has_atomic and klows & {1, 2, 3, 4, 5, 6, 7, 8, 9, 17, 18}:
                bad.append("atomic-on-texture-kind-%s" % sorted(klows))
            if len(klows) > 1:
                bad.append("multi-kind-%s" % sorted(klows))
            if bad:
                rows.append({"module": f, "slot": "%s|%d" % (name, off),
                             "kinds": {str(k): kind_name(k)[0] for k in kk},
                             "ops": oo, "why": bad, "exact": not lossy,
                             "evid": {"%s" % k: sorted(set(evid[sl][k]))
                                      for k in kk}})
    exact = [r for r in rows if r["exact"]]
    mods = sorted({r["module"] for r in exact})
    res = {"dir": dirpath, "modules": len(files), "heap_slots": total_slots,
           "bad_slots": rows, "bad_slots_exact": exact, "bad_modules": mods}
    print("== 槽口径（heap 直索引，语义归一化）")
    print("   模块 %d，heap 槽 %d，异常槽 %d（其中索引可精确解析 %d），异常模块 %d"
          % (len(files), total_slots, len(rows), len(exact), len(mods)))
    for r in exact:
        print("   %-34s %-26s kinds=%-40s ops=%s  <- %s"
              % (r["module"], r["slot"], r["kinds"],
                 ",".join("%s:%d" % kv for kv in sorted(r["ops"].items())),
                 ";".join(r["why"])))
    return res


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 0
    mode, target = argv[1], argv[2]
    out = None
    only_conflicts = "--conflicts" in argv
    for i, a in enumerate(argv):
        if a == "--json" and i + 1 < len(argv):
            out = argv[i + 1]
    if mode == "gta6":
        res = scan_gta6(target)
    elif mode == "victory":
        res = scan_victory(target, only_conflicts)
    elif mode == "slots":
        res = scan_victory_slots(target)
    else:
        print(__doc__)
        return 2
    if out:
        with open(out, "w", encoding="utf-8") as g:
            json.dump(res, g, ensure_ascii=False, indent=1)
        print("JSON -> %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
