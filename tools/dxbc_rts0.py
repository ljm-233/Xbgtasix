#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""dxbc_rts0.py —— 不依赖 Windows 的 DXBC 容器 / RTS0 根签名 / STAT / PSV0 解析器

为什么需要它
------------
`docs/transient-block-wall.md` 第 28 节给出的判断是「不是格式转换问题，是主机 ABI
对齐问题」：shader 里 RTS0 块声明的是主机 root signature。这个判断需要一个**可复现的
字节级证据**，而现成的工具（dxc / renderdoc / docs.microsoft.com 的 d3d12.dll）都要
Windows。这个脚本只用 Python 标准库，直接读落盘的 dump：

  build/dumps/cs_NN_lenM.bin     槽 62 CreateComputePipelineState 的字节码（DXBC 容器）
  build/dumps/rs_NN_lenM.bin     槽 16 CreateRootSignature 的序列化根签名（RTS0 裸 blob）

两者格式同源（序列化根签名 = "RTS0" 魔数 + 版本 + 偏移 + 描述结构），因此可以
**逐字节对比**「shader 自己声明的根签名」与「我们交给桌面驱动的根签名」。

用法
----
    python3 tools/dxbc_rts0.py list   build/dumps            # 每个文件的 RTS0 摘要
    python3 tools/dxbc_rts0.py detail build/dumps/cs_01_len5136.bin
    python3 tools/dxbc_rts0.py cmp    build/dumps            # shader RTS0 vs rs_*.bin 配对
    python3 tools/dxbc_rts0.py stat   build/dumps
    python3 tools/dxbc_rts0.py pairs  /tmp/vm/shaders        # console.dxil vs host.dxil

退出码 0 = 正常；1 = 有文件解析失败。
"""

import os
import struct
import sys

# ---------------------------------------------------------------- DXBC 容器

DXBC_MAGIC = b"DXBC"


class Chunk:
    def __init__(self, fourcc, offset, size, data):
        self.fourcc = fourcc
        self.offset = offset
        self.size = size
        self.data = data

    def __repr__(self):
        return "<Chunk %r off=0x%X size=%d>" % (self.fourcc, self.offset, self.size)


class DxbcError(Exception):
    pass


def parse_dxbc(blob):
    """解析 DXBC 容器。返回 dict(magic_ok, digest, unknown, total, chunks)。"""
    if len(blob) < 32 or blob[:4] != DXBC_MAGIC:
        raise DxbcError("不是 DXBC 容器（magic=%r）" % blob[:4])
    digest = blob[4:20]
    unknown, total = struct.unpack_from("<II", blob, 20)
    count = struct.unpack_from("<I", blob, 28)[0]
    if count > 64:
        raise DxbcError("chunk 数离谱：%d" % count)
    offs = list(struct.unpack_from("<%dI" % count, blob, 32))
    chunks = []
    for off in offs:
        if off + 8 > len(blob):
            raise DxbcError("chunk 偏移越界：0x%X" % off)
        fourcc = blob[off:off + 4].decode("ascii", "replace")
        size = struct.unpack_from("<I", blob, off + 4)[0]
        data = blob[off + 8:off + 8 + size]
        chunks.append(Chunk(fourcc, off, size, data))
    return {
        "digest": digest,
        "unknown": unknown,
        "total": total,
        "declared_len_ok": total == len(blob),
        "chunks": chunks,
    }


def chunk_map(info):
    return {c.fourcc: c for c in info["chunks"]}


# ------------------------------------------------------- 序列化根签名 RTS0

RTS0_MAGIC = b"RTS0"

# D3D12_ROOT_PARAMETER_TYPE
PARAM_TYPES = {0: "DescriptorTable", 1: "32bitConst", 2: "CBV", 3: "SRV", 4: "UAV"}
# D3D12_SHADER_VISIBILITY
VISIBILITY = {0: "ALL", 1: "VERTEX", 2: "HULL", 3: "DOMAIN", 4: "GEOMETRY",
              5: "PIXEL", 6: "AMPLIFICATION", 7: "MESH"}
# D3D12_ROOT_SIGNATURE_FLAGS
RS_FLAGS = {0x1: "ALLOW_INPUT_ASSEMBLER", 0x2: "DENY_VERTEX_SHADER_ROOT_ACCESS",
            0x4: "DENY_HULL", 0x8: "DENY_DOMAIN", 0x10: "DENY_GEOMETRY",
            0x20: "DENY_PIXEL", 0x40: "ALLOW_STREAM_OUTPUT"}


class Rts0:
    """序列化根签名（RTS0 段体）的结构化解读。

    段体布局（对 17 个 rs_*.bin + 53 个 shader 内嵌副本逐个验证过，
    校验式：24 + 24*n == 段体总长，无一例外）：

        +0  u32 版本（1 = RTS1.0，2 = RTS1.1）
        +4  u32 根参数个数 n
        +8  u32 参数表偏移（恒为 24）
        +12 u32 静态采样器个数
        +16 u32 段体总长
        +20 u32 Flags            <-- 主机位 0x03000000 就在这里
        +24 参数表：每项 12 字节 [类型, 辅助/flags, 尾表偏移]
        参数表之后紧接"尾表"：每项 12 字节 [shaderRegister, registerSpace, 额外]
        其中"额外"对 32bitConst 是 Num32BitValues，其余为 0

    注：本仓 root_sig.h 里写的是"1.1 每项 24 字节"，实测是 **12 + 12**
    （记录与尾表分成两段连续存放，尾表偏移字段指过去），24 是两者之和。
    按 24 掰开会把尾表当成半个参数，类型会读出 5/6 这种非法值——本脚本早期版本
    就踩过这个坑，见 docs/rts0-analysis.md。
    """

    def __init__(self, data):
        self.raw = data
        self.error = None
        self.version = self.num_params = self.num_static = self.total = None
        self.flags = None
        self.params = []
        try:
            self._parse()
        except (struct.error, IndexError) as exc:
            self.error = "字段越界：%s" % exc

    def _parse(self):
        d = self.raw
        if len(d) < 24:
            raise struct.error("段体只有 %d 字节，不足 24 字节头" % len(d))
        (self.version, self.num_params, table, self.num_static,
         self.total, self.flags) = struct.unpack_from("<IIIIII", d, 0)
        if self.num_params > 64:
            raise struct.error("参数个数离谱：%d" % self.num_params)
        if 24 + 24 * self.num_params != self.total:
            self.error = ("自洽式不成立：24+24*%d=%d != 段体总长 %d"
                          % (self.num_params, 24 + 24 * self.num_params, self.total))
        for i in range(self.num_params):
            t, aux, tail = struct.unpack_from("<III", d, table + 12 * i)
            if tail + 12 > len(d):
                raise struct.error("尾表偏移 0x%X 越界" % tail)
            reg, space, extra = struct.unpack_from("<III", d, tail)
            self.params.append({
                "index": i, "type_id": t,
                "type": PARAM_TYPES.get(t, "非法type%d" % t),
                "aux": aux, "tail_offset": tail,
                "reg": reg, "space": space, "extra": extra,
            })

    def summary(self):
        types = "/".join(p["type"] for p in self.params) or "-"
        return ("v%d n=%d static=%d flags=0x%08X [%s]"
                % (self.version, self.num_params, self.num_static, self.flags, types))

    def lines(self):
        out = ["  RTS0 v%d  参数=%d  静态采样器=%d  段体总长=%d  flags=0x%08X"
               % (self.version, self.num_params, self.num_static,
                  self.total, self.flags)]
        if self.flags:
            known = [name for bit, name in RS_FLAGS.items() if self.flags & bit]
            if known:
                out.append("    标准位: " + " | ".join(known))
            host = self.flags & ~0x7F
            if host:
                out.append("    ★ 非标准（主机）位: 0x%08X" % host)
        for p in self.params:
            if p["type_id"] == 1:
                detail = "寄存器 r%d space=%d, Num32BitValues=%d" % (
                    p["reg"], p["space"], p["extra"])
            elif p["type_id"] == 0:
                detail = "DescriptorTable, NumRanges=%d, 范围偏移=0x%X" % (
                    p["reg"], p["extra"])
            else:
                detail = "根描述符 r%d space=%d, flags=0x%X" % (
                    p["reg"], p["space"], p["extra"])
            out.append("    参数[%d] %-16s vis=%-5s %s"
                       % (p["index"], p["type"],
                          VISIBILITY.get(p["aux"], "?"), detail))
        return out


# ------------------------------------------------------------ STAT / PSV0

STAT_FIELDS_V1 = [
    "NumTexInstrs", "NumTexBins", "NumTexArrayInstrs", "NumArrayBins",
]


def parse_stat(blob):
    if len(blob) < 4:
        return {}
    n = struct.unpack_from("<I", blob, 0)[0]
    if n * 4 + 4 > len(blob) or n > 64:
        return {"raw_count": n}
    vals = struct.unpack_from("<%dI" % n, blob, 4)
    return {"count": n, "values": list(vals),
            "instructions": vals[0] if vals else None,
            "temp_registers": vals[1] if n > 1 else None}


def psv0_strings(blob):
    """PSV0 里有一张资源名/元素名的 32 字节字符串表，直接捞可打印串。"""
    out = []
    cur = []
    for b in blob:
        if 0x20 <= b < 0x7F:
            cur.append(chr(b))
        else:
            if len(cur) >= 3:
                out.append("".join(cur))
            cur = []
    if len(cur) >= 3:
        out.append("".join(cur))
    seen = []
    for s in out:
        if s not in seen:
            seen.append(s)
    return seen


# ------------------------------------------------------------------ 主流程

def load(path):
    with open(path, "rb") as f:
        return f.read()


def cmd_detail(path):
    blob = load(path)
    print("文件: %s  (%d 字节)" % (path, len(blob)))
    info = parse_dxbc(blob)
    print("  digest=%s  unknown=0x%08X  total=%d(声明%s)  chunks=%d"
          % (info["digest"][:8].hex(), info["unknown"], info["total"],
             "一致" if info["declared_len_ok"] else "★不一致", len(info["chunks"])))
    for c in info["chunks"]:
        print("  chunk %-4s off=0x%05X size=%-6d" % (c.fourcc, c.offset, c.size))
    cm = chunk_map(info)
    if "RTS0" in cm:
        print("  原始 RTS0 字节:")
        d = cm["RTS0"].data
        for i in range(0, len(d), 16):
            row = " ".join("%02x" % b for b in d[i:i + 16])
            asc = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in d[i:i + 16])
            print("    %04X  %-47s  %s" % (i, row, asc))
        rs = Rts0(d)
        print("  解读:")
        for line in rs.lines():
            print(line)
        if rs.error:
            print("  ⚠ %s" % rs.error)
    if "STAT" in cm:
        st = parse_stat(cm["STAT"].data)
        print("  STAT: %s" % st)
    if "PSV0" in cm:
        print("  PSV0 字符串: %s" % psv0_strings(cm["PSV0"].data))


def cs_files(d):
    return sorted(f for f in os.listdir(d) if f.startswith("cs_") and f.endswith(".bin"))


def rs_files(d):
    return sorted(f for f in os.listdir(d) if f.startswith("rs_") and f.endswith(".bin"))


def cmd_list(d):
    print("%-24s %-6s %-4s %s" % ("文件", "长度", "RTS0", "RTS0 摘要"))
    bad = 0
    for f in cs_files(d):
        blob = load(os.path.join(d, f))
        try:
            cm = chunk_map(parse_dxbc(blob))
        except DxbcError as exc:
            print("%-24s %-6d %-4s %s" % (f, len(blob), "-", "★ %s" % exc))
            bad += 1
            continue
        if "RTS0" not in cm:
            print("%-24s %-6d %-4s %s" % (f, len(blob), "无", "-"))
            continue
        rs = Rts0(cm["RTS0"].data)
        print("%-24s %-6d %-4d %s" % (f, len(blob), cm["RTS0"].size, rs.summary()))
    return 1 if bad else 0


def digest_of_rts0(data):
    return data.hex()


def desc_off(blob):
    """序列化根签名第 8 字节 = 描述结构相对偏移。"""
    return struct.unpack_from("<I", blob, 8)[0] if len(blob) >= 12 else 0


def cmd_cmp(d):
    """把每个 shader 自己的 RTS0 与 rs_*.bin 逐字节配对。"""
    rss = []
    for f in rs_files(d):
        raw = load(os.path.join(d, f))
        # rs_*.bin 是完整 DXBC 容器（'DXBC' + 头 + 单个 'RTS0' 段），
        # 要和 shader 里那段**段体**比，先把容器壳去掉。
        if raw[:4] == DXBC_MAGIC:
            cm = chunk_map(parse_dxbc(raw))
            if "RTS0" in cm:
                raw = cm["RTS0"].data
        rss.append((f, raw, Rts0(raw)))
    print("== 游戏真实根签名（rs_*.bin）%d 个" % len(rss))
    for f, raw, rs in rss:
        print("  %-20s len=%-4d %s" % (f, len(raw), rs.summary()))
    print()
    print("== shader 内嵌 RTS0 与 rs_*.bin 的配对")
    unmatched = []
    for f in cs_files(d):
        blob = load(os.path.join(d, f))
        try:
            cm = chunk_map(parse_dxbc(blob))
        except DxbcError:
            continue
        if "RTS0" not in cm:
            unmatched.append((f, "无 RTS0"))
            print("  %-24s (无 RTS0 chunk)" % f)
            continue
        mine = cm["RTS0"].data
        exact = [name for name, raw, _ in rss if raw == mine]
        body = [name for name, raw, _ in rss
                if len(raw) >= 12 and len(mine) >= 12
                and raw[desc_off(raw):] == mine[desc_off(mine):]]
        if exact:
            print("  %-24s 逐字节等于 %s" % (f, ",".join(exact)))
        elif body:
            # 找出到底哪几个字节不一样（实战里差的就是 +20 的 flags 双字）
            ref = [raw for name, raw, _ in rss if name == body[0]][0]
            diff = [i for i in range(min(len(ref), len(mine))) if ref[i] != mine[i]]
            where = ("差字节 " + ",".join("+0x%X" % i for i in diff[:8])
                     if diff else "长度不同")
            print("  %-24s 与 %s 只差 flags 等头部字节（%s）"
                  % (f, ",".join(body), where))
        else:
            unmatched.append((f, "无匹配"))
            print("  %-24s ★ 与 17 个 rs_*.bin 全都不匹配（len=%d）" % (f, len(mine)))
    print()
    print("未匹配：%d / %d" % (len(unmatched), len(cs_files(d))))
    for f, why in unmatched:
        print("  - %-24s %s" % (f, why))
    return 0


def cmd_stat(d):
    """按 RTS0 的「指纹」分组，看有没有系统性的两类。"""
    groups = {}
    for f in cs_files(d):
        blob = load(os.path.join(d, f))
        try:
            cm = chunk_map(parse_dxbc(blob))
        except DxbcError:
            continue
        key = "无RTS0"
        if "RTS0" in cm:
            rs = Rts0(cm["RTS0"].data)
            if rs.error:
                key = "RTS0异常"
            else:
                ptypes = tuple(sorted({p["type"] for p in rs.params}))
                key = "flags=0x%08X params=%d types=%s static=%d" % (
                    rs.flags, rs.num_params, ",".join(ptypes), rs.num_static)
        groups.setdefault(key, []).append(f)
    for k in sorted(groups):
        print("[%d] %s" % (len(groups[k]), k))
        for f in groups[k]:
            print("      %s" % f)
    return 0


def part_tags(blob):
    """非 DXBC 的裸 Xbox DXIL blob：按已知 fourcc 抓子块位置。"""
    tags = []
    for tag in (b"DXBC", b"DXIL", b"XDAT", b"XBCH", b"SFI0", b"ISG1", b"OSG1",
                b"PSV0", b"RTS0", b"STAT", b"ILDN", b"HASH", b"PRIV"):
        start = 0
        while True:
            at = blob.find(tag, start)
            if at < 0:
                break
            tags.append((tag.decode(), at))
            start = at + 1
    tags.sort(key=lambda x: x[1])
    return tags


def cmd_pairs(d):
    """console.dxil 与 host.dxil 成对比较（victory 的落盘，只读）。"""
    files = os.listdir(d)
    keys = sorted({f.rsplit(".", 2)[0] for f in files if f.endswith(".dxil")})
    has_rts0 = {"console": 0, "host": 0}
    total = 0
    for k in keys:
        con = os.path.join(d, k + ".console.dxil")
        host = os.path.join(d, k + ".host.dxil")
        if not (os.path.exists(con) and os.path.exists(host)):
            continue
        total += 1
        print("=== %s" % k)
        for tag, path in (("console", con), ("host", host)):
            blob = load(path)
            if blob[:4] == DXBC_MAGIC:
                info = parse_dxbc(blob)
                names = [c.fourcc for c in info["chunks"]]
                print("  %-8s len=%-7d DXBC 容器 chunks=%s"
                      % (tag, len(blob), ",".join(names)))
                cm = chunk_map(info)
                if "RTS0" in cm:
                    has_rts0[tag] += 1
                    print("           RTS0 len=%d %s"
                          % (cm["RTS0"].size, Rts0(cm["RTS0"].data).summary()))
                else:
                    print("           RTS0 无")
            else:
                tags = part_tags(blob)
                print("  %-8s len=%-7d 裸 Xbox blob 子块=%s"
                      % (tag, len(blob), " ".join("%s@%d" % t for t in tags[:8])))
                if blob.find(b"RTS0") >= 0:
                    has_rts0[tag] += 1
                    print("           ★ 含 RTS0")
                else:
                    print("           RTS0 无")
    print()
    print("成对文件 %d 组；含 RTS0 的：console %d / host %d"
          % (total, has_rts0["console"], has_rts0["host"]))
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 0
    cmd = argv[1]
    arg = argv[2] if len(argv) > 2 else "build/dumps"
    if cmd == "detail":
        cmd_detail(arg)
        return 0
    if cmd == "list":
        return cmd_list(arg)
    if cmd == "cmp":
        return cmd_cmp(arg)
    if cmd == "stat":
        return cmd_stat(arg)
    if cmd == "pairs":
        return cmd_pairs(arg)
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
