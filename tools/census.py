#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""导入解析阶梯的静态 census —— 模式 C（设计说明见 docs/import-ladder.md）。

对目标 exe 的每一个 `dll!symbol` 导入，按**阶梯顺序**判定"该由谁提供"：

    1 Hle    该 dll 是我们自己实现的（OUR_DLLS 名单，写死）且这个符号在我们的导出清单里
    2 Host   HostPolicy.allows(dll) 通过（白名单、默认拒绝、每条带理由）
    3 Title  目标二进制在 exe 同目录自带的同名 DLL（第 2 级不负责时才轮到它）
    4 Stub   以上都不负责 → 掉进生成的 stub（调用时打印 dll!name）

三条纪律（和 docs/import-ladder.md 一致，改这里必须同步改文档与 tests/t45_census）：

  * **纯静态**：本脚本只读目标 PE 的导入表、shim 的导出表，以及磁盘上"文件名存在性"。
    它**不加载、不绑定、不运行目标**，所以能在任何系统上对任何 exe 做分诊。
  * **默认拒绝**：HostPolicy 里没有的模块一律不透传。给白名单加一条必须同时写理由。
  * **我们是第一级**：dll 名在我们的名单里时，**永远不从 Host 拿**（即使 PC 上恰好也有
    同名 DLL，例如 GDK for PC 的 xgameruntime.dll —— 那份的语义是 PC 服务，不是主机语义）。
    这是 HostPolicy 之外的"注册表否决权"，见 classify() 里的 registry_veto。

为什么需要它：`xbgtasix check` 只回答"缺哪些符号"，不回答"每个符号该由谁提供"。
census 是**第一把排查工具**（"为什么它在这个环境崩"）也是**移植工作量的度量**：
`--no-host` 打印的缺口清单就是"一个都不透传时，我们真正要实现的 API 面"。

用法：
    python tools/census.py <exe>
    python tools/census.py <exe> --csv out.csv
    python tools/census.py <exe> --only stub          # 只看掉进 stub 的
    python tools/census.py <exe> --only missing       # 只看"我们的模块缺符号"
    python tools/census.py <exe> --no-host            # 一个都不透传 → 缺口清单
    python tools/census.py <exe> --policy             # 打印白名单全表（含理由）
    python tools/census.py --self-test                # 纯逻辑自测（不需要 exe）

退出码：0 = 报告成功；2 = 用法/解析错误（census 本身不对缺口判 FAIL —— 它是只读度量，
要卡 CI 缺口请用 `--only missing | grep .` 或 check）。
"""
import argparse
import os
import re
import struct
import sys

# --------------------------------------------------------------------------
# 我们自己实现的模块（第 1 级 Hle 的唯一名单）
#
# "写死"是刻意的：这份名单是**我们承诺接管的模块名**，不能从构建产物的目录里猜。
# 目录里多一个 dll 不等于我们负责它；目录里少一个 dll 也不等于它不归我们（可能是
# 这次构建没构建出来 —— 那种情况要报"导出清单缺失"，而不是静默降级到 Host/Title）。
#
# purpose 会打印在 --policy/报告头部，方便对着文档核对。
# --------------------------------------------------------------------------
OUR_DLLS = {
    "xgameruntime.dll": "Xbox GDK 运行时的主 API 面（461 导出）：task queue / async / user / storage / package",
    "xmem.dll": "主机内存分配器：物理页、虚拟地址映射、工作集统计（主机语义，必须我们实现）",
    "d3d12_xs.dll": "Xbox D3D12 变体（device ABI / 描述符堆布局）；转发到 PC d3d12 的部分见 forward.def",
    "AcpHal.dll": "音频/计算 HAL：APU 堆、shape context、APU 虚拟地址映射",
    "xapu.dll": "APU 命令通道：connect / enqueue / dequeue 结果",
    "chad.dll": "DSP/CHAD 客户端：XDsp* 连接、激活、命令提交",
    "xg_xs.dll": "Xbox 图形辅助：swizzle 建议、texture computer 创建",
    "XCRDAPI.dll": "DirectStorage 的 Xbox 侧导出面（DStorageGetFactory）",
}

# 构建目录（默认 build/）里按小写名找我们的 DLL；找不到时退回源码清单。
# 每条： (相对仓库根的路径, 解析器名)
MANIFEST_SOURCES = {
    "xgameruntime.dll": [
        ("src/generated/xgameruntime_exports.inc", "eqlist"),
        ("src/generated/xgameruntime.def", "deflist"),
        ("src/generated/xgameruntime_guids.inc", "names"),
        ("src/xgameruntime/exports.txt", "names"),
        ("src/xgameruntime/exports_input.txt", "names"),
        ("src/xgameruntime/exports_pkg.txt", "names"),
        ("src/xgameruntime/exports_pkg2.txt", "names"),
        ("src/xgameruntime/exports_save.txt", "names"),
        ("src/xgameruntime/exports_speech.txt", "names"),
        ("src/xgameruntime/exports_stream.txt", "names"),
        ("src/xgameruntime/exports_sys.txt", "names"),
        ("src/xgameruntime/exports_ui.txt", "names"),
    ],
    "xmem.dll": [
        ("src/xmem/exports_more.txt", "names"),
        ("src/xmem/xmem.cpp", "cpp"),
        ("src/host_stall/host_stall.cpp", "cpp"),
    ],
    "d3d12_xs.dll": [
        ("src/d3d12_xs/exports.txt", "names"),
        ("src/d3d12_xs/forward.def", "deflist"),
        ("src/d3d12_xs/d3d12_xs.cpp", "cpp"),
    ],
    "AcpHal.dll": [("src/console_hw/acphal.cpp", "cpp")],
    "xapu.dll": [("src/console_hw/xapu.cpp", "cpp")],
    "chad.dll": [("src/console_hw/chad.cpp", "cpp")],
    "xg_xs.dll": [("src/console_hw/xg_xs.cpp", "cpp")],
    "XCRDAPI.dll": [("src/console_hw/xcrdapi.cpp", "cpp")],
}

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# --------------------------------------------------------------------------
# PE 解析（纯标准库、独立实现：不 import tools/analyze_pe.py，那个是给人看的报告工具，
# 这里要的是稳定的数据结构。思路一致：节表 + 数据目录 + 导入表 + 导出表）
# --------------------------------------------------------------------------
class PEError(Exception):
    pass


def _u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def _u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


class PE:
    """最小只读 PE 视图：节表、数据目录、RVA→文件偏移、导入表、导出表。"""

    IMPORT_DIR = 1
    EXPORT_DIR = 0

    def __init__(self, path):
        self.path = path
        try:
            with open(path, "rb") as f:
                self.d = f.read()
        except OSError as e:
            raise PEError("打不开 %s：%s" % (path, e))
        self.size = len(self.d)
        d = self.d
        if d[:2] != b"MZ" or len(d) < 0x40:
            raise PEError("不是 PE（缺 MZ）：%s" % path)
        pe = _u32(d, 0x3C)
        if d[pe:pe + 4] != b"PE\0\0":
            raise PEError("不是 PE（缺 PE 签名）：%s" % path)
        coff = pe + 4
        self.machine = _u16(d, coff)
        nsec = _u16(d, coff + 2)
        opt_size = _u16(d, coff + 16)
        opt = coff + 20
        magic = _u16(d, opt)
        if magic not in (0x20B, 0x10B):
            raise PEError("未知的可选头 magic 0x%X：%s" % (magic, path))
        self.is64 = magic == 0x20B
        ndirs = _u32(d, opt + (108 if self.is64 else 92))
        dir_off = opt + (112 if self.is64 else 96)
        self.dirs = []
        for i in range(min(ndirs, 16)):
            self.dirs.append((_u32(d, dir_off + 8 * i), _u32(d, dir_off + 8 * i + 4)))
        self.sections = []
        sec = opt + opt_size
        for i in range(nsec):
            o = sec + 40 * i
            name = d[o:o + 8].rstrip(b"\0").decode("latin-1", "replace")
            vsize, va, rsize, raw = struct.unpack_from("<IIII", d, o + 8)
            self.sections.append((name, va, max(vsize, rsize), raw, rsize))

    def rva_to_off(self, rva):
        for _name, va, size, raw, rsize in self.sections:
            if va <= rva < va + size:
                # 只落在有文件数据的区间里（.bss 之类不可读）
                delta = rva - va
                if delta >= rsize and rsize:
                    return None
                return raw + delta
        return None

    def cstr(self, rva):
        off = self.rva_to_off(rva)
        if off is None or off >= self.size:
            return None
        end = self.d.find(b"\0", off)
        if end < 0:
            return None
        return self.d[off:end].decode("latin-1", "replace")

    # -- 导入表 ---------------------------------------------------------
    def imports(self):
        """返回 [(module_name, [symbol...]), ...]，保持 PE 里的原始顺序。

        符号形式：名字，或按序号导入写成 '#<n>'（这种无法按名字核对，见 classify）。
        """
        out = []
        if len(self.dirs) <= self.IMPORT_DIR:
            return out
        rva, size = self.dirs[self.IMPORT_DIR]
        if not rva:
            return out
        off = self.rva_to_off(rva)
        if off is None:
            return out
        step, fmt, high = (8, "<Q", 1 << 63) if self.is64 else (4, "<I", 1 << 31)
        n = 0
        while True:
            o = off + n * 20
            if o + 20 > self.size:
                break
            oft, _ts, _fwd, name_rva, ft = struct.unpack_from("<IIIII", self.d, o)
            if not name_rva:
                break
            mod = self.cstr(name_rva)
            if not mod:
                break
            funcs = []
            thunk_rva = oft or ft
            t = self.rva_to_off(thunk_rva) if thunk_rva else None
            guard = 0
            while t is not None and t + step <= self.size and guard < 1 << 20:
                guard += 1
                val = struct.unpack_from(fmt, self.d, t)[0]
                if not val:
                    break
                if val & high:
                    funcs.append("#%d" % (val & 0xFFFF))
                else:
                    nm = self.cstr((val & (high - 1)) + 2)
                    funcs.append(nm if nm else "?")
                t += step
            out.append((mod, funcs))
            n += 1
            if n > 4096:
                break
        return out

    # -- 导出表 ---------------------------------------------------------
    def exports(self):
        """返回 (按名字导出的集合, 按序号导出的序号集合)。转发导出也算导出（名字在表里）。"""
        names, ords = set(), set()
        if len(self.dirs) <= self.EXPORT_DIR:
            return names, ords
        rva, _size = self.dirs[self.EXPORT_DIR]
        if not rva:
            return names, ords
        off = self.rva_to_off(rva)
        if off is None:
            return names, ords
        try:
            (_c, _t, _maj, _min, _nm, base, nfunc, nnames,
             _af, an, _ao) = struct.unpack_from("<IIHHIIIIIII", self.d, off)
        except struct.error:
            return names, ords
        if nfunc > 200000 or nnames > 200000:
            return names, ords
        no = self.rva_to_off(an) if an else None
        for i in range(nnames):
            if no is None:
                break
            nr = _u32(self.d, no + 4 * i)
            nm = self.cstr(nr)
            if nm:
                names.add(nm)
        for i in range(nfunc):
            ords.add(base + i)
        return names, ords


def read_module_species(path):
    """给 shim 文件做个粗分类，用于报告里区分 'PE' / '损坏'。"""
    try:
        pe = PE(path)
    except PEError as e:
        return None, str(e)
    return pe, ""


# --------------------------------------------------------------------------
# HostPolicy：默认拒绝的白名单，通配只支持尾部 '*'
#
# 每一条 = (pattern, reason, risk)。reason 会原样打印在 --policy 里 —— 没有理由的条目
# 不允许进表（改表必须连带改 docs/import-ladder.md 的同名小节）。
#
# risk 的含义：
#   none   两边语义/二进制契约一致，透传是"显然正确"的选择
#   low    函数契约一致，但环境差异（编解码器可用集、证书链、代理）可能让行为不同
#   （medium/high 的条目不该出现在白名单里 —— 那种应该由 Hle 或 stub 负责）
# --------------------------------------------------------------------------
POLICY_RULES = [
    ("api-ms-win-*",
     "API Set 契约：名字本身就是'转发到系统实现'的稳定别名（Win10 起由 apisetschema 解析）。"
     "主机与 PC 在 file/thread/synch/heap 这些原语上都直接落到各自的 Nt*，函数契约、参数、"
     "返回码一致；这类导入没有'主机专有语义'，自己实现只会引入偏差", "none"),
    ("ext-ms-win-*",
     "ext API set（rtcore/ntuser 的窗口消息面）：同样是'转发别名'，指向 user32/ntdll。"
     "主机上的窗口消息语义与 PC 一致（GWLP/WM_* 数值共用），透传即可", "none"),
    ("kernel32.dll",
     "基础 Win32 面。真正'主机语义不同'的部分（内存属性、线程调度、时间源）在 xmem/"
     "xgameruntime 里由我们覆盖，轮不到 kernel32；剩下的是 CreateFile/HeapAlloc 这类契约", "none"),
    ("kernelbase.dll",
     "kernel32 的实现层，契约与 kernel32 相同；标题直接导入它时（新版工具链会这样优化）"
     "没有额外语义", "none"),
    ("ucrtbase.dll",
     "Universal CRT：微软承诺的二进制稳定 ABI（VC redist 就是它）。printf/malloc/"
     "locale 的行为由 CRT 版本决定，主机与 PC 用的是同一套 UCRT 源码", "none"),
    ("msvcp140.dll",
     "MSVC C++ 标准库：二进制兼容是 MSVC 的官方承诺（v140 ABI 跨小版本稳定）。"
     "标题自带的 Xbox 版 msvcp140.dll 反而依赖主机 CRT 内部，PC 上加载不了 —— 所以这里必须"
     "透传宿主那份（注意：透传走 System32-only，exe 同目录那份不会被采纳，见 3.3）", "low"),
    ("msvcp140_1.dll", "msvcp140 的拆分件（C++17 新符号），与 msvcp140.dll 同一 ABI 承诺", "none"),
    ("msvcp140_2.dll", "msvcp140 的拆分件（并行/特殊数学），同上", "none"),
    ("msvcp140_atomic_wait.dll", "msvcp140 的拆分件（atomic wait/notify），同上", "none"),
    ("msvcp140_codecvt_ids.dll", "msvcp140 的拆分件（codecvt id 常量），同上", "none"),
    ("vcruntime140.dll",
     "MSVC C 运行时（异常展开、CRT 初始化）：同一份 v140 契约；主机版的 SEH 展开表与 PC 版"
     "布局一致，透传不需要翻译", "none"),
    ("vcruntime140_1.dll", "vcruntime140 的拆分件（x64 C++ EH 展开），同上", "none"),
    ("d3d12.dll",
     "PC 的 D3D12。主机侧标题导入的是 d3d12_xs.dll（那份归我们，Hle 优先），而直接导入 "
     "d3d12.dll 的符号是跨平台共有面；device/command list 的 ABI 已由 tests/t41 验过一致", "low"),
    ("dxgi.dll",
     "适配器枚举、交换链、output 描述：PC 与主机共用同一套 DXGI 契约。主机专有的"
     "present 路径走 d3d12_xs 的 PresentX，在这里不会被调用到", "low"),
    ("bcrypt.dll",
     "BCrypt 加密原语（哈希/AES/RNG）：算法与参数契约由 CNG 定义，两边实现同源；"
     "主机上多出的策略（密钥隔离）不在标题会调的面里", "none"),
    ("advapi32.dll",
     "注册表/令牌/服务控制：标题读 registry 拿系统配置，PC 与主机都是同一套 Win32 注册表"
     "语义（主机是只读子集，读取路径一致）", "low"),
    ("ws2_32.dll",
     "Winsock 2：TCP/UDP socket 契约与 BSD 兼容层一致；主机与 PC 走同一个内核网络栈，"
     "差异只在可达地址（主机只连 Xbox 服务），不是 API 语义", "low"),
    ("winhttp.dll",
     "WinHTTP 客户端：HTTP/1.1、TLS、代理配置的 API 契约两边一致；主机版同样有 WinHTTP。"
     "差异在证书链与代理策略（环境问题，不是函数语义）", "low"),
    ("mfplat.dll",
     "Media Foundation 平台层：Xbox 系统同样自带 MFPlat（GDK 的视频播放就走它），"
     "attribute/媒体类型/拓扑的 API 契约一致", "low"),
    ("mfreadwrite.dll",
     "MF 的读写层（SourceReader/SinkWriter）：同上，契约一致；差异只在可用编解码器集合", "low"),
]


class HostPolicy:
    """白名单 + 默认拒绝。通配只支持**尾部** '*'（和设计文档一致，不要发明中缀/正则）。

    - `HostPolicy.default()`：POSIX 式的"只放行语义两边一致的"
    - `HostPolicy.none()`：一个都不透传 —— 用来测量真实 API 面（缺口清单）
    """

    def __init__(self, rules=(), name="custom"):
        self.name = name
        self.rules = [(p.lower(), why, risk) for (p, why, risk) in rules]

    @classmethod
    def default(cls):
        return cls(POLICY_RULES, name="default")

    @classmethod
    def none(cls):
        return cls((), name="none")

    def allows(self, dll):
        low = dll.lower()
        for pat, _why, _risk in self.rules:
            if pat.endswith("*"):
                if low.startswith(pat[:-1]):
                    return True
            elif low == pat:
                return True
        return False

    def match(self, dll):
        """返回命中的 (pattern, reason, risk)；不命中返回 None。"""
        low = dll.lower()
        for pat, why, risk in self.rules:
            if pat.endswith("*"):
                if low.startswith(pat[:-1]):
                    return pat, why, risk
            elif low == pat:
                return pat, why, risk
        return None


# --------------------------------------------------------------------------
# 阶梯判定（纯函数：只吃"事实"，不碰磁盘/不加载 —— 便于 --self-test）
# --------------------------------------------------------------------------
class Facts:
    """分类需要知道的全部事实。构造它要读磁盘（PE/文件名），classify 只用不读。"""

    def __init__(self, our_exports, title_files, policy, no_title_ours=False):
        # our_exports: {小写 dll 名: set(导出符号名)}
        self.our_exports = our_exports
        self.title_files = title_files          # {小写文件名: 磁盘上的真实文件名}
        self.policy = policy
        self.no_title_ours = no_title_ours


class Row:
    __slots__ = ("dll", "symbol", "module_prov", "prov", "gap", "shadowed", "note", "why")

    def __init__(self, dll, symbol, module_prov, prov, gap=False, shadowed=False,
                 note="", why=""):
        self.dll = dll
        self.symbol = symbol
        self.module_prov = module_prov
        self.prov = prov
        self.gap = gap
        self.shadowed = shadowed
        self.note = note
        self.why = why


HLE, HOST, TITLE, STUB = "Hle", "Host", "Title", "Stub"
PROVENANCES = (HLE, HOST, TITLE, STUB)


def module_provenance(dll, facts):
    """整个模块该由谁提供（不看具体符号）。用于"68 个模块的归属分布"。"""
    low = dll.lower()
    if low in facts.our_exports:
        return HLE, "ours"
    hit = facts.policy.match(dll)
    if hit:
        return HOST, hit[0]
    if low in facts.title_files:
        return TITLE, facts.title_files[low]
    return STUB, ""


def classify(dll, symbol, facts):
    """对一个导入符号跑阶梯。**只分类、不绑定、不运行目标**。

    顺序即语义：
      1 Hle    我们是这个模块的 owner，且导出清单里有这个符号
      2 Host   HostPolicy 允许该模块（我们是 owner 时**不走这条** —— registry veto）
      3 Title  exe 同目录有同名 DLL（我们是 owner 且 no_title_ours 时不走）
      4 Stub

    返回值里的 gap=True 表示"这个模块归我们，但我们没导出这个符号" —— 这是**实现缺口**，
    不是"别人提供"。它最终落在 Host/Title/Stub 里的哪一个，取决于阶梯后两级；但修法只有
    一个：补我们的导出（`--only missing` 就是列这个）。
    """
    low = dll.lower()
    ours = low in facts.our_exports
    gap = False
    note = ""
    why = ""

    if symbol.startswith("#"):
        note = "按序号导入，无法按名字核对"

    # ---- 1 Hle
    if ours:
        if not symbol.startswith("#") and symbol in facts.our_exports[low]:
            return Row(dll, symbol, HLE, HLE, why="our export list")
        gap = True
        why = "我们的模块缺这个导出"
    # ---- 2 Host（registry veto：我们的模块永远不从宿主拿）
    elif facts.policy.allows(dll):
        hit = facts.policy.match(dll)
        shadow = low in facts.title_files
        if shadow:
            note = (note + "；" if note else "") + (
                "exe 同目录也有同名 DLL（%s）—— 按阶梯 Host 优先，且 Host 只从 System32 加载"
                % facts.title_files[low])
        return Row(dll, symbol, HOST, HOST, shadowed=shadow, note=note,
                   why="HostPolicy: %s" % (hit[0] if hit else "?"))
    # ---- 3 Title
    if low in facts.title_files and not (ours and facts.no_title_ours):
        note = ("；".join(x for x in [note,
                ("会顶掉我们的实现 —— P0 缺口" if ours else "")] if x))
        return Row(dll, symbol, module_provenance(dll, facts)[0], TITLE, gap=gap,
                   note=note, why="exe 同目录 %s" % facts.title_files[low])
    # ---- 4 Stub
    if not note:
        note = "无 HLE、未透传、exe 同目录也没有 → 生成 stub"
    return Row(dll, symbol, module_provenance(dll, facts)[0], STUB, gap=gap,
               note=note, why="ladder exhausted")


# --------------------------------------------------------------------------
# 事实收集
# --------------------------------------------------------------------------
def collect_our_exports(shim_dirs, verbose=True):
    """返回 ({小写 dll: set(符号)}, {小写 dll: 来源说明})。

    优先用构建出来的 DLL 的**导出表**（那就是加载器看到的东西，权威）；文件不在时退回
    源码清单（exports*.txt / .def / .inc / cpp 里的 dllexport），这样在没有构建产物的机器上
    也能给出有意义的 census（只是要标出来源，别让人误以为等同于产物）。
    """
    exports, sources = {}, {}
    found_dirs = [d for d in shim_dirs if d and os.path.isdir(d)]
    for dll in OUR_DLLS:
        low = dll.lower()
        got = None
        for d in found_dirs:
            for cand in sorted(os.listdir(d)):
                if cand.lower() != low:
                    continue
                p = os.path.join(d, cand)
                try:
                    pe = PE(p)
                    names, _ords = pe.exports()
                except PEError as e:
                    sources[low] = "损坏：%s（%s）" % (p, e)
                    names = set()
                if names:
                    got = names
                    sources[low] = "PE 导出表 %s（%d 个）" % (p, len(names))
                    break
                else:
                    sources[low] = "PE 无导出：%s" % p
            if got is not None:
                break
        if got is None:
            got = set()
            used = []
            for rel, kind in MANIFEST_SOURCES.get(dll, []):
                p = os.path.join(REPO_ROOT, rel)
                if not os.path.isfile(p):
                    continue
                n = _parse_manifest(p, kind)
                if n:
                    used.append("%s(%d)" % (rel, len(n)))
                    got |= n
            if used:
                sources[low] = "源码清单 " + " + ".join(used) + "（%d 个）" % len(got)
            else:
                sources[low] = "!! 找不到导出清单（既没有构建产物也没有源码清单）"
        exports[low] = got
        if verbose and not got:
            print("   ! 警告：%s 没有可用导出清单 —— 它的导入会被误报成缺口" % dll,
                  file=sys.stderr)
    return exports, sources


def _parse_manifest(path, kind):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return set()
    out = set()
    if kind == "names":
        for line in text.splitlines():
            line = line.split(";", 1)[0].split("#", 1)[0].strip()
            if not line:
                continue
            # exports.txt 是"一行一个名字"；容错：取第一个 token（去掉可能的签名）
            tok = re.split(r"[\s(]", line, maxsplit=1)[0].strip()
            if tok and re.fullmatch(r"[A-Za-z_?$@][\w?$@.]*", tok):
                out.add(tok)
    elif kind == "eqlist":
        for line in text.splitlines():
            line = line.split(";", 1)[0].strip()
            if "=" in line:
                out.add(line.split("=", 1)[0].strip())
    elif kind == "deflist":
        in_exports = False
        for line in text.splitlines():
            s = line.split(";", 1)[0].strip()
            if not s:
                continue
            if s.upper().startswith("EXPORTS"):
                in_exports = True
                continue
            if not in_exports or s.upper().startswith(("LIBRARY", "NAME", "DESCRIPTION")):
                continue
            out.add(s.split("=", 1)[0].strip())
    elif kind == "cpp":
        for m in re.finditer(r"dllexport\b[^;{}]*?\b([A-Za-z_]\w*)\s*\(", text):
            out.add(m.group(1))
    out.discard("")
    return out


def collect_title_files(exe_dir):
    files = {}
    try:
        for name in os.listdir(exe_dir):
            if name.lower().endswith(".dll"):
                files.setdefault(name.lower(), name)
    except OSError:
        pass
    return files


# --------------------------------------------------------------------------
# 报告
# --------------------------------------------------------------------------
def human_size(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return "%.1f %s" % (n, unit) if unit != "B" else "%d B" % n
        n /= 1024.0
    return "?"


def print_policy(policy):
    print("HostPolicy = %s（%d 条；默认拒绝；通配只支持尾部 '*'）"
          % (policy.name, len(policy.rules)))
    if not policy.rules:
        print("  （空表：一个模块都不透传 —— 这是 --no-host 的测量模式）")
        return
    for pat, why, risk in policy.rules:
        print("  %-26s risk=%-4s %s" % (pat, risk, why))


def build_report(args, facts, rows, sources, exe_path):
    lines = []

    def out(s=""):
        lines.append(s)

    exe_name = os.path.basename(exe_path)
    try:
        size = os.path.getsize(exe_path)
    except OSError:
        size = 0
    out("== census of %s" % exe_name)
    out("   path       %s  (%s)" % (os.path.abspath(exe_path), human_size(size)))
    out("   exe dir    %s" % (os.path.dirname(os.path.abspath(exe_path)) or "."))
    out("   policy     %s（%d 条规则%s）"
        % (facts.policy.name, len(facts.policy.rules),
           "；--no-host：一个都不透传" if facts.policy.name == "none" else ""))
    out("   title DLLs exe 同目录 %d 个 DLL（第 3 级 Title 的候选）"
        % len(facts.title_files))
    out("")

    out("-- 我们的导出清单来源（第 1 级 Hle 的唯一依据）")
    for dll in OUR_DLLS:
        low = dll.lower()
        n = len(facts.our_exports.get(low, ()))
        flag = "" if n else "   <== 空！"
        out("   %-18s %4d 个   %s%s" % (dll, n, sources.get(low, "?"), flag))
    out("")

    # 计数
    sym_counts = {p: 0 for p in PROVENANCES}
    mod_counts = {p: 0 for p in PROVENANCES}
    mods = sorted({r.dll for r in rows}, key=str.lower)
    mod_prov = {}
    for m in mods:
        prov, _why = module_provenance(m, facts)
        mod_prov[m] = prov
        mod_counts[prov] += 1
    for r in rows:
        sym_counts[r.prov] += 1
    total_sym = len(rows)
    gaps = [r for r in rows if r.gap]
    shadows = [r for r in rows if r.shadowed]

    out("-- 归属分布")
    out("   模块 %d 个：  " % len(mods)
        + "  ".join("%s %d" % (p, mod_counts[p]) for p in PROVENANCES))
    out("   符号 %d 个：  " % total_sym
        + "  ".join("%s %d (%.1f%%)"
                    % (p, sym_counts[p], 100.0 * sym_counts[p] / max(total_sym, 1))
                    for p in PROVENANCES))
    out("")
    out("   缺口（我们的模块缺符号）    %d 个符号 / %d 个模块   <- 补我们的导出，不是换来源"
        % (len(gaps), len({r.dll for r in gaps})))
    out("   遮蔽（Host 与 exe 同目录同名）%d 个符号 / %d 个模块   <- 阶梯让 Host 赢；"
        "透传只认 System32" % (len(shadows), len({r.dll for r in shadows})))
    if facts.policy.name == "none":
        miss = [r for r in rows if r.prov == STUB]
        out("   --no-host 缺口清单：%d 个符号 / %d 个模块必须由 Hle 或 stub 负责"
            % (len(miss), len({r.dll for r in miss})))
    out("")

    if not args.no_modules:
        out("-- 模块明细（%d 个，按名字排序；括号里是符号级分布）" % len(mods))
        per = {}
        for r in rows:
            per.setdefault(r.dll, {p: 0 for p in PROVENANCES})
            per[r.dll][r.prov] += 1
        for m in mods:
            c = per.get(m, {p: 0 for p in PROVENANCES})
            detail = " ".join("%s=%d" % (p, c[p]) for p in PROVENANCES if c[p])
            out("   %-9s %-40s %4d  %s"
                % (mod_prov[m], m, sum(c.values()), detail))
        out("")

    # 明细表（可过滤）
    shown = rows
    if args.only:
        if args.only == "missing":
            shown = [r for r in rows if r.gap]
        elif args.only == "stub":
            shown = [r for r in rows if r.prov == STUB]
        elif args.only == "shadow":
            shown = [r for r in rows if r.shadowed]
        elif args.only in PROVENANCES:
            low = args.only.lower()
            shown = [r for r in rows if r.prov.lower() == low]
        else:
            raise SystemExit("--only 只接受 missing / stub / shadow / hle / host / title")
        if not shown:
            out("-- 明细：过滤 '%s' 之后 0 行" % args.only)
            return lines

    hdr = "%-4s %-9s %-44s %-46s %-5s %s" % ("#", "PROV", "DLL", "SYMBOL", "GAP", "依据/备注")
    out("-- 明细（共 %d 行%s）" % (len(shown),
                                 "" if not args.only else "，过滤 '%s' 后" % args.only))
    out(hdr)
    out("-" * len(hdr))
    limit = len(shown) if args.limit <= 0 else min(args.limit, len(shown))
    for i, r in enumerate(shown[:limit], 1):
        sym = r.symbol if len(r.symbol) <= 44 else r.symbol[:41] + "..."
        note = r.why
        if r.note:
            note = (note + "；" if note else "") + r.note
        out("%-4d %-9s %-44s %-46s %-5s %s"
            % (i, r.prov, r.dll, sym, "GAP" if r.gap else "", note))
    if limit < len(shown):
        out("...（还有 %d 行没打印；--limit 0 打印全部，或 --csv 落盘）"
            % (len(shown) - limit))
    return lines


def write_csv(path, rows):
    import csv
    with open(path, "w", encoding="utf-8-sig", newline="") as f:
        w = csv.writer(f)
        w.writerow(["dll", "symbol", "provenance", "module_provenance",
                    "gap", "shadowed", "reason", "note"])
        for r in rows:
            w.writerow([r.dll, r.symbol, r.prov, r.module_prov,
                        "1" if r.gap else "", "1" if r.shadowed else "",
                        r.why, r.note])


# --------------------------------------------------------------------------
# 自测：不需要 exe，验证阶梯与 HostPolicy 的语义（CI 里也能跑）
# --------------------------------------------------------------------------
def self_test():
    checks = 0
    fails = 0

    def ok(cond, what):
        nonlocal checks, fails
        checks += 1
        if not cond:
            fails += 1
            print("[FAIL] %s" % what)
        else:
            print("[ok]   %s" % what)

    pol = HostPolicy.default()
    ok(pol.allows("api-ms-win-core-file-l1-1-0.dll"), "白名单：api-ms-win-* 尾部通配命中")
    ok(pol.allows("API-MS-WIN-CORE-FILE-L1-1-0.DLL"), "白名单：大小写不敏感")
    ok(pol.allows("kernel32.dll"), "白名单：精确名命中")
    ok(pol.allows("kernelbase.dll"), "白名单：kernelbase 精确名命中")
    ok(not pol.allows("kernel32_extra.dll"), "精确名条目不做前缀匹配（kernel32.dll 不含通配）")
    ok(not pol.allows("xgameruntime.dll"), "默认拒绝：我们的模块不在白名单里")
    ok(not pol.allows("Bink2Scarlett.dll"), "默认拒绝：标题自带库不会透传")
    ok(not pol.allows("GameInput.dll"), "默认拒绝：GameInput 没有理由 → 不透传")
    ok(not HostPolicy.none().allows("kernel32.dll"), "none()：一个都不允许")

    # 通配只支持尾部 '*'：中缀 '*' 当字面量
    p2 = HostPolicy([("api-*-core", "test", "none")], name="t")
    ok(not p2.allows("api-ms-win-core"), "通配只支持尾部：中缀 '*' 不匹配")
    ok(p2.allows("api-*-core"), "通配只支持尾部：中缀 '*' 按字面量匹配")

    facts = Facts(
        our_exports={"xgameruntime.dll": {"XTaskQueueCreate"},
                     "acphal.dll": set()},   # 空集合：我们接管了模块名但这次构建没有导出
        title_files={"bink2scarlett.dll": "Bink2Scarlett.dll",
                     "acphal.dll": "AcpHal.dll"},
        policy=HostPolicy.default(),
    )
    r = classify("xgameruntime.dll", "XTaskQueueCreate", facts)
    ok(r.prov == HLE and not r.gap, "阶梯 1：我们的导出 → Hle")
    r = classify("XGAMERUNTIME.DLL", "XTaskQueueCreate", facts)
    ok(r.prov == HLE, "阶梯 1：dll 名大小写不敏感")
    r = classify("api-ms-win-core-file-l1-1-0.dll", "CreateFileW", facts)
    ok(r.prov == HOST, "阶梯 2：白名单模块 → Host")
    r = classify("Bink2Scarlett.dll", "BinkOpen", facts)
    ok(r.prov == TITLE, "阶梯 3：exe 同目录同名 DLL → Title")
    r = classify("GameInput.dll", "GameInputCreate", facts)
    ok(r.prov == STUB, "阶梯 4：都不负责 → Stub")
    # registry veto：我们的模块即使没导出，也不许从 Host 拿
    r = classify("AcpHal.dll", "ApuCreateHeap", facts)
    ok(r.prov == TITLE and r.gap, "registry veto：我们的模块缺符号 → 不走 Host；Title 带 GAP")
    # --no-title-ours：我们的模块缺符号时干脆落 stub（规划视角）
    f2 = Facts(facts.our_exports, facts.title_files, facts.policy, no_title_ours=True)
    r = classify("AcpHal.dll", "ApuCreateHeap", f2)
    ok(r.prov == STUB and r.gap, "--no-title-ours：我们的模块缺符号 → Stub + GAP")
    # 遮蔽提示
    f3 = Facts({}, {}, HostPolicy.default())
    f3.our_exports = {"d3d12_xs.dll": set()}
    f3.title_files = {"msvcp140.dll": "msvcp140.dll"}
    r = classify("msvcp140.dll", "?_Xlength_error@std@@YAXPEBD@Z", f3)
    ok(r.prov == HOST and r.shadowed, "遮蔽：Host 与 exe 同目录同名 → Host 赢并标记")
    # 模块级归属
    ok(module_provenance("xgameruntime.dll", facts)[0] == HLE, "模块归属：ours → Hle")
    ok(module_provenance("Bink2Scarlett.dll", facts)[0] == TITLE, "模块归属：目录里有 → Title")
    ok(module_provenance("GameInput.dll", facts)[0] == STUB, "模块归属：没人负责 → Stub")
    ok(module_provenance("api-ms-win-core-synch-l1-1-0.dll", facts)[0] == HOST,
       "模块归属：白名单 → Host")

    print("%d checks, %d failures" % (checks, fails))
    return 0 if fails == 0 else 1


# --------------------------------------------------------------------------
def main(argv=None):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    ap = argparse.ArgumentParser(
        description="导入解析阶梯的静态 census（模式 C）：每个 dll!symbol 该由谁提供")
    ap.add_argument("exe", nargs="?", help="目标 exe")
    ap.add_argument("--csv", metavar="OUT.CSV", help="把明细表写成 CSV（utf-8-sig）")
    ap.add_argument("--only", metavar="KIND",
                    help="过滤明细：missing（我们的缺口）/ stub / shadow / hle / host / title")
    ap.add_argument("--no-host", action="store_true",
                    help="HostPolicy::none()：一个都不透传，输出真实 API 面的缺口清单")
    ap.add_argument("--policy", action="store_true", help="打印 HostPolicy 全表（含理由）")
    ap.add_argument("--shim-dir", metavar="DIR", action="append", default=[],
                    help="我们的 DLL 所在目录（可多次；默认 build/，再退回源码清单）")
    ap.add_argument("--limit", type=int, default=40,
                    help="明细表打印多少行（默认 40；0 = 全部）")
    ap.add_argument("--no-modules", action="store_true", help="不打印 68 个模块的明细")
    ap.add_argument("--no-title-ours", action="store_true",
                    help="我们的模块缺符号时不映射到 Title（规划视角，直接算 Stub）")
    ap.add_argument("--self-test", action="store_true", help="只跑纯逻辑自测")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    policy = HostPolicy.none() if args.no_host else HostPolicy.default()
    if args.policy:
        print_policy(policy)
        print()
        if not args.exe:      # 只查白名单表时不需要 exe
            return 0
    if not args.exe:
        ap.print_usage()
        return 2

    shim_dirs = list(args.shim_dir) or [os.path.join(REPO_ROOT, "build")]
    our_exports, sources = collect_our_exports(shim_dirs)
    # 兜底：构建目录里没有的，再从 exe 同目录试一次（那是最可能放我们 DLL 的地方）
    exe_abs = os.path.abspath(args.exe)
    exe_dir = os.path.dirname(exe_abs)
    missing_pe = [d for d in OUR_DLLS if not our_exports.get(d.lower())]
    if missing_pe and os.path.isdir(exe_dir):
        extra, extra_src = collect_our_exports([exe_dir], verbose=False)
        for d in missing_pe:
            low = d.lower()
            if extra.get(low):
                our_exports[low] = extra[low]
                sources[low] = extra_src[low] + "（exe 同目录兜底）"

    try:
        pe = PE(exe_abs)
        imports = pe.imports()
    except PEError as e:
        print("错误：%s" % e, file=sys.stderr)
        return 2
    if not imports:
        print("错误：%s 没有导入表（不像是可执行模块）" % exe_abs, file=sys.stderr)
        return 2

    facts = Facts(our_exports, collect_title_files(exe_dir), policy,
                  no_title_ours=args.no_title_ours)
    rows = []
    for dll, syms in imports:
        for s in syms:
            rows.append(classify(dll, s, facts))

    lines = build_report(args, facts, rows, sources, exe_abs)
    print("\n".join(lines))

    if args.csv:
        write_csv(args.csv, rows)
        print("\nCSV 写出：%s（%d 行，utf-8-sig）" % (args.csv, len(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
