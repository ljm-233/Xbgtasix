#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""离线 PE 交叉引用分析器 —— 给无符号 exe 里的某个 RVA 找"身份"。

为什么需要它：目标 exe（约 114 MB，x86-64，无 PDB）里某个 int3 断言没有任何
字符串证据，只能反过来问"这个函数碰了哪些导入 API / 引用了哪些字符串"来推断
它属于哪个子系统。

四条主线（全部离线，纯标准库）：
  1) 函数边界：.pdata（DataDirectory[3]）里的 RUNTIME_FUNCTION 数组给出精确的
     (BeginAddress, EndAddress)，不靠启发式"往上找 int3/ret"；
  2) 调用者：在可执行节里扫 E8/E9 rel32，算出调用目标，目标落在目标函数区间内
     的就是"谁调了它"；调用点自身再回查 (1) 的区间表得到所属函数（顺带给出
     call 的返回地址 = 调用点 + 5，方便和运行时栈帧对齐）；
  3) 命名：扫函数区间里的 FF 15 / FF 25（call/jmp [rip+disp32]）与 48 8B 0x
     （mov reg,[rip+disp32]），把 disp32 解析成 IAT 槽 RVA，反查导入表得到
     "模块!函数名"。**另外**：MSVC 会把"取过地址"的导入编译成 .text 末尾的
     `FF 25 [rip+IAT]` thunk，代码里用 `E8` 直接调 thunk —— 只扫 FF 15 会漏掉
     这类导入引用，所以直接调用目标若是 thunk 也会被解析成导入名；
  4) 递归上一层：--depth N 继续反向找调用者，形成调用链；--callers K 限制每层
     最多列出的调用者函数数。

额外的"命名"证据（本工具的主要目的就是命名，故默认开启，可用 --no-strings 关）：
  * 字符串引用：lea reg,[rip+disp] 指向可打印字符串（大小写/路径/命令符等）；
  * 混淆字符串：这个游戏把很多字符串在栈上做"加/减密钥"解码（GTA 常见形态），
    本工具能识别该解码循环并还原密钥与源字节，把 pack 挂载点之类的关键字符串
    直接打出来（启发式，但输出前会校验可打印性，误报率很低）。

纯标准库（mmap + struct + bisect + array + re），不依赖 pefile / capstone。
所有搜索都走 mmap.find / re.finditer（C 层），对 100 MB 的 .text 全量扫描只需
数秒，不逐字节跑 Python 循环。

用法：
    python tools/xref.py <exe> --addr 0x2237178 [--depth 2] [--callers 10]

输出为中文、紧凑分行；末尾"线索"汇总直接给出与各子系统关键词的命中情况。
"""

import argparse
import bisect
import mmap
import os
import re
import struct
import sys
import time
from array import array
from collections import Counter, defaultdict, OrderedDict, namedtuple

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

IMAGE_SCN_MEM_EXECUTE = 0x20000000   # 可执行节标志
MAX_THUNKS = 200000                  # 单个导入描述符最多解析的 thunk 数（防御脏数据）
MAX_IMPORT_NAMES = 24                # 每个函数默认最多列出的导入名
MAX_CALLEE_NAMES = 12                # 每个函数默认最多列出的直接调用目标
MAX_SITES_PER_ENTRY = 6              # 每个调用者函数最多展示多少个调用点
MAX_STRINGS = 6                      # 每个函数最多展示多少条字符串

Section = namedtuple("Section", "name va vsize raw raw_size chars")

# 子系统关键词表：(说明, 关键词元组)。用于最后的"线索"汇总。
# 同时匹配"导入名（模块!函数）"和"字符串内容"，小写比较。
WATCHLIST = [
    ("资源包挂载 / 虚拟文件系统（packroot:/、packcache:/、.rpf）",
     ("packroot", "packcache", "pack:/", ".rpf", "\\rpf", "vfs", "rpf", "mount")),
    ("命令行 / 平台配置（+arg、地图名）",
     ("cmdline", "commandline", "+disable", "+enable", "americas", "-level", "-map")),
    ("Xbox 内存分配器 XMem", ("xmem",)),
    ("资源流式加载 / DirectStorage", ("dstorage",)),
    ("Xbox 运行时 / 服务 (XGameRuntime/Xbl/XStore/XSystem)",
     ("xgameruntime", "xbl", "xstore", "xsystem", "xuser", "xaccess", "xnetworking")),
    ("图形 API / 渲染 (D3D12, FidelityFX)", ("d3d12", "d3d", "fidelityfx")),
    ("文件 / 路径 IO", ("createfile", "getfileattributes", "readfile", "writefile",
                        "mapviewoffile", "createfilemapping", "setfilepointer",
                        "getfilesize", "findfirstfile", "_wfopen", "fopen")),
    ("堆 / 虚拟内存", ("heapalloc", "heapfree", "heaprealloc", "virtualalloc",
                       "virtualfree", "virtualprotect", "virtualquery",
                       "virtualalloc2", "allocateuserphysicalpages")),
    ("注册表 / 配置", ("regopenkey", "regqueryvalue", "reggetvalue", "regcreatekey",
                       "regsetvalue", "regclosekey")),
    ("输入映射", ("custommapping", "gameinput")),
    ("统计 / 存档", ("hero_stat", "completionpercent", "savegame", "profilestat",
                     "stat_", "achievement")),
    ("错误上报 / 遥测", ("alert_error", "support.rockstargames", "telemetry",
                          "werreport", "crashreport", "reportfault")),
    ("C++ 运行库 / std::", ("msvcp", "vcruntime", "concrt", "vccorlib")),
    ("断言 / 调试", ("assert", "dbgprint", "outputdebugstring", "raiseexception",
                     "terminateprocess")),
]

# GTA 系"栈上字符串解码"循环的字节形态（mov dl,[rsp+rax+disp8]; add cl,dl;
# dec cl; mov [rsp+rax+disp8],dl; inc rax; jne）。组 1 = 源偏移，组 3 = 目标偏移。
OBF_LOOP_RE = re.compile(
    rb"\x8a\x54\x04(.)(..)\xfe\xc9\x88\x54\x04(.)\x48\xff\xc0\x75(.)",
    re.DOTALL)


# ---------------------------------------------------------------------------
# 基础读取（任何越界都返回 0 / None，绝不抛异常）
# ---------------------------------------------------------------------------

def u16(buf, off):
    if off < 0 or off + 2 > len(buf):
        return 0
    return struct.unpack_from("<H", buf, off)[0]


def u32(buf, off):
    if off < 0 or off + 4 > len(buf):
        return 0
    return struct.unpack_from("<I", buf, off)[0]


def u64(buf, off):
    if off < 0 or off + 8 > len(buf):
        return 0
    return struct.unpack_from("<Q", buf, off)[0]


def i32(buf, off):
    if off < 0 or off + 4 > len(buf):
        return 0
    return struct.unpack_from("<i", buf, off)[0]


# ---------------------------------------------------------------------------
# PE 镜像
# ---------------------------------------------------------------------------

class PEImage:
    """最小 PE 解析：头、节表、数据目录；文件用 mmap 只读映射。"""

    def __init__(self, path):
        self.path = path
        self.fh = open(path, "rb")
        self.file_size = os.fstat(self.fh.fileno()).st_size
        self.mm = mmap.mmap(self.fh.fileno(), 0, access=mmap.ACCESS_READ)
        d = self.mm

        if d[:2] != b"MZ":
            raise SystemExit("不是 PE 文件：缺少 MZ 头")
        self.pe_off = u32(d, 0x3C)
        if d[self.pe_off:self.pe_off + 4] != b"PE\0\0":
            raise SystemExit("不是 PE 文件：缺少 PE 签名")

        coff = self.pe_off + 4
        self.machine = u16(d, coff)
        self.nsec = u16(d, coff + 2)
        self.opt_size = u16(d, coff + 16)
        opt = coff + 20
        self.opt = opt
        magic = u16(d, opt)
        self.is64 = magic == 0x20B
        if magic not in (0x10B, 0x20B):
            raise SystemExit("未知的可选头 magic：0x%X" % magic)

        self.image_base = u64(d, opt + 24) if self.is64 else u32(d, opt + 28)
        self.entry = u32(d, opt + 16)
        self.size_image = u32(d, opt + 56)
        self.size_headers = u32(d, opt + 60)
        ndirs = u32(d, opt + (108 if self.is64 else 92))
        dir_off = opt + (112 if self.is64 else 96)
        self.dirs = []
        for i in range(min(ndirs, 16)):
            self.dirs.append((u32(d, dir_off + i * 8), u32(d, dir_off + i * 8 + 4)))
        while len(self.dirs) < 16:
            self.dirs.append((0, 0))

        self.sections = []
        base = opt + self.opt_size
        for i in range(self.nsec):
            b = base + i * 40
            if b + 40 > self.file_size:
                break
            name = d[b:b + 8].rstrip(b"\0").decode("latin-1", "replace")
            vsize, va, raw_size, raw = struct.unpack_from("<IIII", d, b + 8)
            chars = u32(d, b + 36)
            self.sections.append(Section(name, va, vsize, raw, raw_size, chars))

        # 按虚拟地址排序，便于二分定位"某 RVA 属于哪个节"
        self.secs_by_va = sorted(self.sections, key=lambda s: s.va)
        self.sec_begins = [s.va for s in self.secs_by_va]

    # -- 地址换算 ----------------------------------------------------------

    def section_of(self, rva):
        """RVA -> 节；不在任何节内返回 None。"""
        if rva < 0:
            return None
        i = bisect.bisect_right(self.sec_begins, rva) - 1
        if i < 0:
            return None
        s = self.secs_by_va[i]
        span = max(s.vsize, s.raw_size)
        if s.va <= rva < s.va + span:
            return s
        return None

    def rva_off(self, rva):
        """RVA -> 文件偏移；落在节的未初始化尾部（BSS）或节外返回 None。"""
        s = self.section_of(rva)
        if s is None:
            return None
        delta = rva - s.va
        if delta >= s.raw_size:
            return None
        off = s.raw + delta
        if off >= self.file_size:
            return None
        return off

    def read(self, rva, n):
        """读 n 字节；越界/跨节/落在 BSS 返回 None。"""
        s = self.section_of(rva)
        if s is None or n <= 0:
            return None
        delta = rva - s.va
        if delta + n > s.raw_size:
            return None
        off = s.raw + delta
        if off + n > self.file_size:
            return None
        return self.mm[off:off + n]

    def read_cstr(self, rva, maxlen=512):
        """读以 NUL 结尾的拉丁字符字符串。"""
        s = self.section_of(rva)
        if s is None:
            return None
        delta = rva - s.va
        if delta >= s.raw_size:
            return None
        off = s.raw + delta
        end = min(off + maxlen, s.raw + s.raw_size, self.file_size)
        if end <= off:
            return None
        raw = self.mm[off:end]
        z = raw.find(b"\0")
        if z < 0:
            z = len(raw)
        try:
            return raw[:z].decode("latin-1")
        except Exception:
            return None


# ---------------------------------------------------------------------------
# 1) .pdata -> 函数区间表
# ---------------------------------------------------------------------------

def load_pdata(pe):
    """解析异常目录里的 RUNTIME_FUNCTION 数组。

    每项 12 字节：BeginAddress, EndAddress, UnwindInfoAddress（均 RVA）。
    返回 (排序去重后的 [(begin, end)], 原始条目数)。
    """
    rva, size = pe.dirs[3]
    if not rva or not size:
        return [], 0
    off = pe.rva_off(rva)
    if off is None:
        return [], 0
    count = size // 12
    usable = min(count * 12, pe.file_size - off)
    if usable <= 0:
        return [], 0
    blob = pe.mm[off:off + usable]
    raw_count = len(blob) // 12

    ents = []
    for begin, end, _unwind in struct.iter_unpack("<III", blob[:raw_count * 12]):
        # BeginAddress == EndAddress 的条目（链式 unwind / 占位）直接跳过
        if begin == 0 or end == 0 or end <= begin:
            continue
        ents.append((begin, end))

    ents.sort()
    uniq = []
    prev = None
    for e in ents:
        if e != prev:
            uniq.append(e)
            prev = e
    return uniq, count


def func_of(table, begins, rva):
    """RVA -> 包含它的函数 (begin, end)；没有 .pdata 覆盖返回 None。"""
    if not table:
        return None
    i = bisect.bisect_right(begins, rva) - 1
    if i < 0:
        return None
    b, e = table[i]
    if b <= rva < e:
        return table[i]
    # 少量回退：兼容 ICF 折叠/嵌套 funclets 造成的重叠条目
    for k in range(i - 1, max(-1, i - 9), -1):
        b2, e2 = table[k]
        if b2 <= rva < e2:
            return table[k]
    return None


# ---------------------------------------------------------------------------
# 2) 导入表 / IAT 槽 -> 模块!函数名
# ---------------------------------------------------------------------------

def _walk_thunks(pe, iat, dll, thunk_rva, ft_rva, ptr):
    """遍历一个导入描述符的 thunk 数组，填充 iat[槽 RVA] = "dll!name"。"""
    if not thunk_rva:
        return
    for i in range(MAX_THUNKS):
        raw = pe.read(thunk_rva + i * ptr, ptr)
        if raw is None:
            break
        val = u64(raw, 0) if ptr == 8 else u32(raw, 0)
        if val == 0:
            break
        if ptr == 8 and (val >> 63):
            name = "%s!#%d" % (dll, val & 0xFFFF)
        elif ptr == 4 and (val >> 31):
            name = "%s!#%d" % (dll, val & 0xFFFF)
        else:
            fname = pe.read_cstr(val + 2)  # 跳过 2 字节 Hint
            if not fname:
                continue
            name = "%s!%s" % (dll, fname)
        if ft_rva:
            iat.setdefault(ft_rva + i * ptr, name)


def load_imports(pe):
    """解析导入目录（以及 delay-load 导入），返回 {IAT 槽 RVA: 模块!函数名}。"""
    iat = {}
    ptr = 8 if pe.is64 else 4

    # 常规导入：IMAGE_IMPORT_DESCRIPTOR 每项 20 字节
    rva, size = pe.dirs[1]
    if rva and size:
        pos = 0
        while pos + 20 <= size:
            b = pe.read(rva + pos, 20)
            if b is None:
                break
            oft, _ts, _fc, name_rva, ft = struct.unpack("<IIIII", b)
            if oft == 0 and name_rva == 0 and ft == 0:
                break
            dll = pe.read_cstr(name_rva) if name_rva else None
            if dll:
                _walk_thunks(pe, iat, dll, oft or ft, ft, ptr)
            pos += 20

    # delay-load 导入：每项 32 字节；Attributes bit0 = 1 时字段是 RVA，否则是 VA
    drva, dsize = pe.dirs[13]
    if drva and dsize:
        pos = 0
        while pos + 32 <= dsize:
            b = pe.read(drva + pos, 32)
            if b is None:
                break
            attrs, name_rva, _hmod, iat_rva, int_rva, _biat, _uiat, _ts = struct.unpack(
                "<IIIIIIII", b)
            if attrs == 0 and name_rva == 0 and iat_rva == 0:
                break
            conv = ((lambda v: v) if (attrs & 1)
                    else (lambda v: (v - pe.image_base) & 0xFFFFFFFF))
            dll = pe.read_cstr(conv(name_rva)) if name_rva else None
            if dll:
                _walk_thunks(pe, iat, dll, conv(int_rva), conv(iat_rva), ptr)
            pos += 32
    return iat


def thunk_import(pe, iat, rva, cache):
    """若 rva 是 `FF 25 disp32`（jmp [rip+IAT]）thunk，返回它指向的导入名。

    MSVC 会给"被取过地址"的导入在 .text 尾部生成这种 thunk，游戏代码用 E8
    直接调 thunk；只扫 FF 15 会漏掉这些导入引用。
    """
    if rva in cache:
        return cache[rva]
    name = None
    off = pe.rva_off(rva)
    if off is not None and pe.mm[off:off + 2] == b"\xff\x25":
        slot = (rva + 6 + i32(pe.mm, off + 2)) & 0xFFFFFFFF
        name = iat.get(slot)
    cache[rva] = name
    return name


# ---------------------------------------------------------------------------
# 3) 扫描辅助
# ---------------------------------------------------------------------------

def _range_bytes(pe, begin, end):
    """把 [begin, end) 限制在同一节的可读范围内，返回 (文件偏移, 长度)。"""
    s = pe.section_of(begin)
    if s is None:
        return None, 0
    if begin < s.va or begin >= s.va + s.raw_size:
        return None, 0
    if end > s.va + s.raw_size:
        end = s.va + s.raw_size
    if end <= begin:
        return None, 0
    off = pe.rva_off(begin)
    if off is None:
        return None, 0
    n = end - begin
    if off + n > pe.file_size:
        n = pe.file_size - off
    return off, n


def scan_range(pe, begin, end, iat):
    """扫描一个函数区间。

    返回 (imports, direct)：
      imports = [(site_rva, 形态, 槽 RVA, 模块!函数名或 None)]
      direct  = [(site_rva, "call"/"jmp", 目标 RVA)]
    """
    imports = []
    direct = []
    off, n = _range_bytes(pe, begin, end)
    if off is None:
        return imports, direct
    mm = pe.mm
    lo, hi = off, off + n

    # -- 间接引用：call/jmp [rip+disp32]（IAT 入口通常是 6 字节指令）--
    for pat, kind in ((b"\xff\x15", "call [rip+d]"), (b"\xff\x25", "jmp [rip+d]")):
        pos = lo
        while True:
            p = mm.find(pat, pos, hi)
            if p < 0:
                break
            if p + 6 <= hi:
                site = begin + (p - lo)
                slot = (site + 6 + i32(mm, p + 2)) & 0xFFFFFFFF
                imports.append((site, kind, slot, iat.get(slot)))
            pos = p + 1

    # -- 间接取地址（可选线索）：mov rax/rcx/rdx/rbx/rsi/rdi, [rip+disp32] --
    for modrm in (0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D):
        pat = bytes((0x48, 0x8B, modrm))
        pos = lo
        while True:
            p = mm.find(pat, pos, hi)
            if p < 0:
                break
            if p + 7 <= hi:
                site = begin + (p - lo)
                slot = (site + 7 + i32(mm, p + 3)) & 0xFFFFFFFF
                name = iat.get(slot)
                if name:
                    imports.append((site, "mov reg,[rip+d]", slot, name))
            pos = p + 1

    # -- 直接调用 / 直接跳转：E8/E9 rel32 --
    for pat, kind in ((b"\xe8", "call"), (b"\xe9", "jmp")):
        pos = lo
        while True:
            p = mm.find(pat, pos, hi)
            if p < 0:
                break
            if p + 5 <= hi:
                site = begin + (p - lo)
                t = site + 5 + i32(mm, p + 1)
                if 0 < t < pe.size_image:
                    direct.append((site, kind, t))
            pos = p + 1

    return imports, direct


def collect_imports(pe, iat, imports, direct, thunk_cache):
    """把"函数内 IAT 引用"与"直接调用 IAT thunk"合并成统一的导入引用列表。"""
    out = list(imports)
    for site, kind, t in direct:
        name = thunk_import(pe, iat, t, thunk_cache)
        if name:
            out.append((site, kind + "→thunk", t, name))
    return out


def build_call_index(pe):
    """一次扫完所有可执行节，建立 (调用点 RVA, 目标 RVA, 类型) 三个平行数组。

    类型：0 = E8 call，1 = E9 jmp。目标不做严格校验（落点合法性由调用者查询时
    的区间表保证），只丢掉明显越界的值。
    """
    sites = array("I")
    targets = array("I")
    kinds = array("B")
    scanned = 0
    mm = pe.mm

    for s in pe.sections:
        if not (s.chars & IMAGE_SCN_MEM_EXECUTE):
            continue
        if s.raw >= pe.file_size:
            continue
        start = s.raw
        end = min(s.raw + s.raw_size, pe.file_size)
        if end <= start:
            continue
        scanned += end - start

        for pat, kind in ((b"\xe8", 0), (b"\xe9", 1)):
            pos = start
            while True:
                p = mm.find(pat, pos, end)
                if p < 0:
                    break
                if p + 5 <= end:
                    site = s.va + (p - s.raw)
                    t = site + 5 + i32(mm, p + 1)
                    if 0 < t < pe.size_image:
                        sites.append(site)
                        targets.append(t)
                        kinds.append(kind)
                pos = p + 1
    return sites, targets, kinds, scanned


def find_callers(sites, targets, kinds, funcs):
    """在已建好的调用索引里，找出目标落在 funcs 各区间内的调用点。

    返回 {(begin, end): [(调用点 RVA, 类型, 目标 RVA), ...]}。
    内部先排序，避免调用方传入的函数表乱序导致二分失效。
    """
    res = defaultdict(list)
    fs = sorted(set(funcs))
    if not fs:
        return res
    begins = [f[0] for f in fs]
    ends = [f[1] for f in fs]
    lo, hi = begins[0], ends[-1]
    for i in range(len(targets)):
        t = targets[i]
        if t < lo or t > hi:
            continue
        j = bisect.bisect_right(begins, t) - 1
        if j >= 0 and t < ends[j]:
            res[fs[j]].append((sites[i], kinds[i], t))
    return res


# ---------------------------------------------------------------------------
# 4) 字符串证据
# ---------------------------------------------------------------------------

def find_plain_strings(pe, begin, end, limit=MAX_STRINGS):
    """lea reg,[rip+disp32] 指向的可打印字符串（普通字符串常量）。"""
    off, n = _range_bytes(pe, begin, end)
    if off is None or n < 7:
        return []
    mm = pe.mm
    limit_end = off + n
    out = []
    seen = set()
    for pref in (0x48, 0x4C, 0x49, 0x4D):
        for modrm in (0x05, 0x0D, 0x15, 0x1D, 0x25, 0x2D, 0x35, 0x3D):
            pat = bytes((pref, 0x8D, modrm))
            pos = off
            while True:
                p = mm.find(pat, pos, limit_end)
                if p < 0:
                    break
                if p + 7 <= limit_end:
                    site = begin + (p - off)
                    slot = (site + 7 + i32(mm, p + 3)) & 0xFFFFFFFF
                    s = pe.read_cstr(slot, 120)
                    if s and 4 <= len(s) <= 120 and all(32 <= ord(c) < 127 for c in s):
                        if s not in seen:
                            seen.add(s)
                            out.append((site, s))
                pos = p + 1
    out.sort()
    return out[:limit]


def find_obf_strings(pe, begin, end, limit=MAX_STRINGS):
    """还原"栈上加/减密钥解码"的混淆字符串（GTA 系常见）。

    识别循环形态：mov dl,[rsp+rax+sdisp]; add cl,dl; dec cl;
                  mov [rsp+rax+ddisp],dl; inc rax; jne
    再往前找 `mov rax,-count` 与 `mov cl,key`，并用窗口内的 store 指令还原源
    字节（movabs+store / mov imm→[rsp+d] / vmovups+store / lea rdi+movb）。
    解出的字节必须 NUL 终止且全可打印才算数，因此误报率很低。
    """
    off, n = _range_bytes(pe, begin, end)
    if off is None or n < 48:
        return []
    mm = pe.mm
    limit_end = off + n
    out = []
    seen = set()

    for m in OBF_LOOP_RE.finditer(mm, off, limit_end):
        loop_at = m.start()
        sdisp = m.group(1)[0]
        ddisp = m.group(3)[0]
        win_lo = max(off, loop_at - 384)
        win = mm[win_lo:loop_at]
        if len(win) < 12:
            continue

        # 1) 循环次数：mov rax, -count   （48 c7 c0 imm32）
        count = None
        for i in range(len(win) - 7):
            if win[i:i + 3] == b"\x48\xc7\xc0":
                v = struct.unpack_from("<i", win, i + 3)[0]
                if -256 < v < 0:
                    count = -v
        if not count or count > 256:
            continue

        # 2) 密钥：循环前最近的 mov cl, imm8 （b1 imm8）
        key = None
        tail = win[max(0, len(win) - 40):]
        kb = tail.rfind(b"\xb1")
        if kb >= 0 and kb + 1 < len(tail):
            key = tail[kb + 1]
        if key is None:
            continue

        # 3) 还原源字节：窗口内所有写入 [rsp+disp8] 的 store
        stores = {}
        cur64 = None
        cur16 = None
        rdi_disp = None
        i = 0
        while i < len(win):
            if win[i:i + 2] == b"\x48\xb8" and i + 10 <= len(win):
                cur64 = win[i + 2:i + 10]
                i += 10
                continue
            if win[i:i + 4] == b"\x48\x89\x44\x24" and i + 5 <= len(win):
                if cur64:
                    stores[win[i + 4]] = cur64
                i += 5
                continue
            if win[i:i + 3] == b"\xc7\x44\x24" and i + 8 <= len(win):
                stores[win[i + 3]] = win[i + 4:i + 8]
                i += 8
                continue
            if win[i:i + 4] == b"\x66\xc7\x44\x24" and i + 7 <= len(win):
                stores[win[i + 4]] = win[i + 5:i + 7]
                i += 7
                continue
            if win[i:i + 3] == b"\xc6\x44\x24" and i + 5 <= len(win):
                stores[win[i + 3]] = win[i + 4:i + 5]
                i += 5
                continue
            if win[i:i + 3] == b"\x40\x88\x7c\x24" and i + 5 <= len(win):
                stores[win[i + 4]] = b"\x00"   # mov dil,[rsp+d]，dil 通常为 0
                i += 5
                continue
            if win[i:i + 4] == b"\x48\x8d\x7c\x24" and i + 5 <= len(win):
                rdi_disp = win[i + 4]
                i += 5
                continue
            if win[i:i + 3] == b"\xc6\x47\xff" and i + 4 <= len(win) and rdi_disp:
                stores[(rdi_disp - 1) & 0xFF] = win[i + 3:i + 4]
                i += 4
                continue
            if win[i:i + 4] == b"\xc5\xf8\x10\x05" and i + 8 <= len(win):
                disp = struct.unpack_from("<i", win, i + 4)[0]
                # rip 相对是按 RVA 算的；这里先换算指令 RVA，再落到文件偏移
                instr_rva = begin + (win_lo + i - off)
                src_rva = (instr_rva + 8 + disp) & 0xFFFFFFFF
                src_off = pe.rva_off(src_rva)
                if src_off is not None and src_off + 16 <= pe.file_size:
                    cur16 = mm[src_off:src_off + 16]
                i += 8
                continue
            if win[i:i + 5] in (b"\xc5\xf8\x29\x44\x24", b"\xc5\xf8\x11\x44\x24") \
                    and i + 6 <= len(win):
                if cur16:
                    stores[win[i + 5]] = cur16
                i += 6
                continue
            i += 1

        # 4) 取源字节并解码（rax 从 -count 走到 -1，源区间 = sdisp-count .. sdisp-1）
        #    先把"多字节 store"摊平成 disp -> 单字节
        flat = {}
        for d, chunk in stores.items():
            for j, b in enumerate(chunk):
                flat[(d + j) & 0xFF] = b
        buf = bytearray()
        ok = True
        for k in range(count):
            b = flat.get((sdisp - count + k) & 0xFF)
            if b is None:
                ok = False
                break
            buf.append(b)
        if not ok:
            continue

        dec = bytes((b + key - k) & 0xFF for k, b in enumerate(buf))
        z = dec.find(b"\x00")
        if z < 4:
            continue
        s = dec[:z]
        if not all(0x20 <= c < 0x7F for c in s):
            continue
        try:
            text = s.decode("ascii")
        except Exception:
            continue
        if text in seen:
            continue
        seen.add(text)
        out.append((begin + (loop_at - off), text))
        if len(out) >= limit:
            break
    return out


# ---------------------------------------------------------------------------
# 5) 报告
# ---------------------------------------------------------------------------

def analyze_func(pe, table, begins, func, iat, cache, thunk_cache):
    """带缓存的 scan_range + thunk 解析：同一函数被多层引用时只扫一次。"""
    key = func[0]
    if key not in cache:
        imports, direct = scan_range(pe, func[0], func[1], iat)
        cache[key] = (collect_imports(pe, iat, imports, direct, thunk_cache), direct)
    return cache[key]


def report_imports(imports, indent, max_names):
    """打印一个函数的导入引用统计；返回 [(模块!函数名, 首个引用点)]。"""
    named = [x for x in imports if x[3]]
    if not named:
        print("%s该函数引用导入：0 个（既无 IAT 间接调用，也无导入 thunk 调用）" % indent)
        return []
    first = OrderedDict()
    for site, _kind, _slot, name in sorted(named, key=lambda x: x[0]):
        first.setdefault(name, site)
    mods = Counter(name.split("!", 1)[0] for name in first)
    mod_str = " / ".join("%s %d 个" % (m, c)
                         for m, c in sorted(mods.items(), key=lambda kv: (-kv[1], kv[0])))
    print("%s该函数引用的导入：%s" % (indent, mod_str))
    names = sorted(first)
    show = names[:max_names]
    tail = "" if len(names) <= max_names else " …还有 %d 个" % (len(names) - max_names)
    print("%s  具体（去重 %d 个，引用点 %d 处）：%s%s"
          % (indent, len(names), len(named), ", ".join(show), tail))
    return [(n, first[n]) for n in names]


def report_direct(pe, iat, direct, indent, max_items, thunk_cache):
    """打印函数内的直接调用（E8/E9）目标统计，thunk 目标直接标成导入名。"""
    if not direct:
        print("%s未发现直接调用（E8/E9）" % indent)
        return
    cnt = Counter(t for _site, _kind, t in direct)
    thunked = sum(c for t, c in cnt.items() if thunk_import(pe, iat, t, thunk_cache))
    items = sorted(cnt.items(), key=lambda kv: (-kv[1], kv[0]))
    show = items[:max_items]
    tail = "" if len(items) <= max_items else " …还有 %d 个" % (len(items) - max_items)
    print("%s直接调用（E8/E9）%d 处 → 去重目标 %d 个（其中 %d 个是导入 thunk）："
          % (indent, len(direct), len(items), thunked))
    parts = []
    for t, c in show:
        name = thunk_import(pe, iat, t, thunk_cache)
        if name:
            parts.append("0x%X→%s(×%d)" % (t, name, c))
        else:
            parts.append("0x%X(×%d)" % (t, c))
    print("%s  %s%s" % (indent, ", ".join(parts), tail))


def callee_fingerprint(pe, table, begins, func, iat, cache, thunk_cache, limit=10):
    """把目标函数"直接调用的函数"再扫一层，汇总它们用到的导入（命名用）。

    返回 [(模块!函数名, 来自哪个被调函数起始 RVA)]。
    """
    _imports, direct = analyze_func(pe, table, begins, func, iat, cache, thunk_cache)
    seen = set()
    out = []
    for _site, _kind, t in direct:
        name = thunk_import(pe, iat, t, thunk_cache)
        if name:
            if (name, t) not in seen:
                seen.add((name, t))
                out.append((name, t))
            continue
        cf = func_of(table, begins, t)
        if cf is None:
            continue
        cimports, _cdirect = analyze_func(pe, table, begins, cf, iat, cache, thunk_cache)
        for _s, _k, _sl, n in cimports:
            if n and n not in seen:
                seen.add(n)
                out.append((n, cf[0]))
        if len(out) >= limit:
            break
    return out[:limit]


def print_strings(plain, obf, indent, max_items=MAX_STRINGS):
    if plain:
        print("%s字符串（lea rip+disp）：%s"
              % (indent, ", ".join("'%s'@0x%X" % (s, a) for a, s in plain[:max_items])))
    if obf:
        print("%s混淆字符串（启发式还原）：%s"
              % (indent, ", ".join("'%s'@0x%X" % (s, a) for a, s in obf[:max_items])))


def classify(evidence):
    """按关键词表分类证据；evidence = [(文本, 引用点, 函数起始 RVA, 类别)]。"""
    out = []
    for title, kws in WATCHLIST:
        hit = []
        seen = set()
        for text, site, fbegin, kind in evidence:
            low = text.lower()
            if text in seen:
                continue
            if any(k in low for k in kws):
                seen.add(text)
                hit.append((text, site, fbegin, kind))
        if hit:
            out.append((title, hit))
    return out


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv=None):
    # Windows 下管道/重定向时默认编码是 GBK，这里强制 UTF-8，避免中文输出乱码
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    ap = argparse.ArgumentParser(
        prog="xref.py",
        description="离线 PE 交叉引用分析器：用 .pdata 函数边界 + 调用扫描 + IAT/字符串解析，"
                    "给无符号 exe 里的某个 RVA 推断所属子系统。",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="示例：python tools/xref.py game.exe --addr 0x2237178 --depth 3 --callers 10")
    ap.add_argument("exe", help="目标 PE 文件（exe/dll）")
    ap.add_argument("--addr", required=True,
                    help="目标 RVA，如 0x2237178（也接受绝对 VA，会自动减去 ImageBase）")
    ap.add_argument("--depth", type=int, default=2, help="向上追溯调用者的层数（默认 2）")
    ap.add_argument("--callers", type=int, default=10, help="每层最多列出的调用者函数数（默认 10）")
    ap.add_argument("--max-imports", type=int, default=MAX_IMPORT_NAMES,
                    help="每个函数最多列出的导入名数量（默认 %d）" % MAX_IMPORT_NAMES)
    ap.add_argument("--no-strings", action="store_true",
                    help="关闭字符串/混淆字符串证据（只做 pdata + 调用 + IAT 分析）")
    ap.add_argument("--max-strings", type=int, default=MAX_STRINGS,
                    help="每个函数最多展示的字符串条数（默认 %d）" % MAX_STRINGS)
    args = ap.parse_args(argv)

    try:
        addr = int(args.addr, 0)
    except ValueError:
        raise SystemExit("--addr 不是合法整数：%s" % args.addr)
    if addr < 0:
        raise SystemExit("--addr 必须是非负 RVA")

    t0 = time.time()
    want_strings = not args.no_strings

    # ---- 加载 ----
    pe = PEImage(args.exe)
    if addr >= pe.image_base and addr - pe.image_base < pe.size_image:
        rva = addr - pe.image_base
        print("# 提示：--addr 0x%X 看起来是绝对 VA，已换算为 RVA 0x%X" % (addr, rva))
        addr = rva

    table, raw_count = load_pdata(pe)
    begins = [f[0] for f in table]
    iat = load_imports(pe)

    print("== 目标 RVA 0x%X" % addr)
    print("   PE: %s，镜像基址 0x%X，节 %d 个，文件 %.1f MB"
          % ("x86-64" if pe.is64 else "x86", pe.image_base, len(pe.sections),
             pe.file_size / (1024.0 * 1024.0)))
    print("   .pdata: 原始条目 %d 条 → 有效去重后 %d 个函数区间；导入表 %d 个 IAT 槽"
          % (raw_count, len(table), len(iat)))

    func = func_of(table, begins, addr)
    if func is None:
        print("   所属函数：.pdata 未覆盖 0x%X（可能是 leaf 函数或被剥离了 unwind 信息）" % addr)
        print("   → 无法确定函数边界，后续调用者/导入分析跳过。")
        print("# 耗时 %.1f s" % (time.time() - t0))
        return 2
    print("   所属函数 0x%X..0x%X（长度 0x%X）" % (func[0], func[1], func[1] - func[0]))

    cache = {}          # 函数起始 -> (imports, direct)
    thunk_cache = {}    # 目标 RVA -> 导入名或 None
    evidence = []       # (文本, 站点, 函数起始, "导入"/"字符串")

    def describe(f, indent, is_target):
        imports, direct = analyze_func(pe, table, begins, f, iat, cache, thunk_cache)
        print("%s函数 0x%X..0x%X（长度 0x%X）" % (indent, f[0], f[1], f[1] - f[0]))
        ev = report_imports(imports, indent + "  ", args.max_imports)
        for name, site in ev:
            evidence.append((name, site, f[0], "导入"))
        report_direct(pe, iat, direct, indent + "  ", MAX_CALLEE_NAMES, thunk_cache)
        if want_strings:
            plain = find_plain_strings(pe, f[0], f[1], args.max_strings)
            obf = find_obf_strings(pe, f[0], f[1], args.max_strings)
            print_strings(plain, obf, indent + "  ", args.max_strings)
            for site, s in plain + obf:
                evidence.append((s, site, f[0], "字符串"))
        if is_target:
            fp = callee_fingerprint(pe, table, begins, f, iat, cache, thunk_cache)
            if fp:
                print("%s  被调函数 1 层导入指纹：%s"
                      % (indent, ", ".join("%s(来自 0x%X)" % (n, b) for n, b in fp)))
                for n, b in fp:
                    evidence.append((n, b, f[0], "导入"))

    print("\n-- 目标函数自身 --")
    describe(func, "   ", True)

    # ---- 调用索引（一次扫描，所有层共用）----
    sites, targets, kinds, scanned = build_call_index(pe)

    # ---- 逐层反向追溯 ----
    visited = {func[0]}          # 已出现过的函数起点（防环）
    listed = 0
    cur = [func]                 # 当前层"被调用者"的函数区间
    for layer in range(1, args.depth + 1):
        hits = find_callers(sites, targets, kinds, cur)
        if not hits:
            print("\n== 调用者（层 %d）：未找到直接 E8/E9 调用"
                  "（可能只被间接调用：虚表/函数指针/导出）" % layer)
            break

        # 汇总成"调用者函数 -> 调用点列表"
        groups = OrderedDict()
        for callee, sites_hit in hits.items():
            for site, kind, tgt in sites_hit:
                owner = func_of(table, begins, site)
                if owner is None:
                    continue
                groups.setdefault(owner, []).append((site, kind, callee, tgt))
        if not groups:
            print("\n== 调用者（层 %d）：有调用点但都无 .pdata 覆盖，无法继续" % layer)
            break

        # 排序：先列"调用目标正好是函数入口"的（可信），再列"目标落在函数内部"的
        # （E8 往往是立即数里的假 call，标出来避免误导）
        def group_key(item):
            _owner, lst = item
            has_entry = any(t == c[0] for _s, _k, c, t in lst)
            return (0 if has_entry else 1, min(s for s, _k, _c, _t in lst))

        ordered = sorted(groups.items(), key=group_key)
        n_sites = sum(len(v) for _k, v in ordered)
        n_entry = sum(1 for _k, v in ordered for _s, _k2, c, t in v if t == c[0])
        listed += len(ordered)
        print("\n== 调用者（层 %d）：%d 个函数 / %d 个调用点（%d 个命中函数入口，%d 个落在函数内部）%s"
              % (layer, len(ordered), n_sites, n_entry, n_sites - n_entry,
                 "（仅列前 %d 个函数）" % args.callers if len(ordered) > args.callers else ""))

        shown = ordered[:args.callers]
        for idx, (owner, lst) in enumerate(shown, 1):
            lst = sorted(lst, key=lambda x: (x[3] != x[2][0], x[0]))
            tag = "（递归：就是层 0 的目标函数）" if owner[0] == func[0] else \
                  ("（已在上层出现）" if owner[0] in visited else "")
            parts = []
            for s, k, c, t in lst[:MAX_SITES_PER_ENTRY]:
                head = "0x%X(call, ret 0x%X)" % (s, s + 5) if k == 0 else "0x%X(jmp)" % s
                if t != c[0]:
                    head += "→目标+0x%X(非入口，疑似 E8 误报)" % (t - c[0])
                parts.append(head)
            if len(lst) > MAX_SITES_PER_ENTRY:
                parts.append("…共 %d 处" % len(lst))
            print("   [%d] 函数 0x%X..0x%X（长度 0x%X）← 调用点 %s%s"
                  % (idx, owner[0], owner[1], owner[1] - owner[0],
                     ", ".join(parts), tag))
            describe(owner, "       ", False)

        # 下一层：以本层列出的调用者函数为新的"被调用者"
        nxt = []
        for owner, _lst in shown:
            if owner[0] in visited:
                continue
            visited.add(owner[0])
            nxt.append(owner)
        if not nxt:
            print("\n   （本层函数都已在上层出现过，调用链在此收敛）")
            break
        cur = nxt

    # ---- 线索汇总 ----
    print("\n== 线索（目标函数 + 已列出调用者的导入/字符串命中）")
    cats = classify(evidence)
    if not cats:
        print("   未命中任何子系统关键词：这些函数只用了通用 CRT/内存例程，"
              "需要往更上层（--depth 更大）继续找。")
    else:
        for title, hit in cats:
            print("   %s：%d 个" % (title, len(hit)))
            for text, site, fbegin, kind in hit[:6]:
                shown = text if kind == "字符串" else text
                print("      %s @0x%X（函数 0x%X）" % (repr(shown) if kind == "字符串"
                                                       else shown, site, fbegin))
            if len(hit) > 6:
                print("      …还有 %d 个" % (len(hit) - 6))

    # ---- 性能数据 ----
    print("\n# 扫描可执行节 %.1f MB，E8/E9 候选 %d 个，导入槽 %d 个，上层函数 %d 个，耗时 %.1f s"
          % (scanned / (1024.0 * 1024.0), len(sites), len(iat), listed,
             time.time() - t0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
