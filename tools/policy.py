#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""静态合规闸：只有"我们写的、小的、文本的"文件能进仓库。

为什么要有代码闸门，而不是在 CONTRIBUTING 里写一句"别提交游戏文件"：
本项目要处理**别人拥有的数据**（200+ GB 主机游戏包、主机 DLL、解出来的 GDK 头/库）。
"不小心把派生数据提交上去"是必然会发生的事故，而事故一旦进了历史，删一个 commit
是不够的——必须 `git filter-repo --path ... --invert-paths` + force-push + 所有人重新
clone。所以规则必须变成两处闸门：进 git 的（本文件）和落盘的（tools/repo_guard.py）。

判定函数是**纯函数** `check(path, content) -> list[str]`，返回违规原因列表（空 = 合规），
五道检查：

  1. 扩展名/文件名白名单（只允许文本源码与文本配置，见 ALLOWED_EXTENSIONS/ALLOWED_NAMES）
  2. 大小 <= 512 KiB
  3. 首字节不匹配任何禁用魔数（PE / 着色器容器 / DDS / RIFF / LLVM bitcode / PDB /
     ar / zip / 7z / cab / MSI）——**魔数优先于扩展名**：一个叫 notes.txt 的 PE 也会被抓
  4. 内容里没有 NUL 字节（即不是二进制）
  5. 没有超过 512 字符的连续 hex/base64 候选段（抓"把数据塞进字符串常量"这条路）

为什么纯函数：单测不需要建仓库、不需要 git、不需要文件系统，CI 里毫秒级跑完。

用法：
    python tools/policy.py --staged        # 只查 git 索引（pre-commit 钩子用）
    python tools/policy.py --check-repo    # 扫全仓（CI / 仓库自测用），跳过 .git/ build/ .dsh/
    python tools/policy.py --selftest      # 自测（不依赖 git 状态）
    python tools/policy.py --install-hooks # 把 core.hooksPath 指到 .githooks
    python tools/policy.py FILE...         # 直接查工作区里的指定文件

判定语义的边界（都写进了 --selftest，改实现前先看用例）：
  * 正好 512 KiB / 正好 512 字符的编码段 = 合规；多 1 字节/1 字符 = 违规。
  * 魔数只在**偏移 0** 判：文本里正常提到 "MZ" 不会被误伤。
  * `--staged` 读的是 `git show :<path>`（索引里的 blob），**不读工作区**——否则
    "先改回干净版、提交脏版"就能绕过闸门。
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

# ---------------------------------------------------------------- 策略参数
# 改这里的任何一个数字/条目，都要同步 docs/asset-policy.md（那是给人看的同一份政策）。

MAX_FILE_SIZE = 512 * 1024      # 512 KiB
MAX_ENCODED_RUN = 512           # 连续 hex/base64 候选字符上限

# 只允许**文本**源码与文本配置。这里按本仓库实际用到的集合 + 宿主文档要求的最小集合定，
# 不预埋用不上的扩展名：每多一个扩展名，就是多一条"可以夹带数据"的通道。
# 需要新格式时：先加用例（SELF_TESTS），再加这里，最后同步 docs/asset-policy.md。
ALLOWED_EXTENSIONS = frozenset({
    # C / C++ 源码与头（src/、tests/、include/；.inc 是本仓库的文本包含件）
    "c", "h", "hpp", "cpp", "inc",
    # 着色器：HLSL 是文本；DXBC/DXIL 是二进制，由魔数拦
    "hlsl",
    # 构建描述（CMakeLists.txt 走 txt）
    "cmake", "def",
    # 工具链脚本（build.sh、tools/*.py、sign_local.ps1）
    "py", "sh", "ps1",
    # 文本数据与配置
    "md", "txt", "json", "yml", "yaml",
})

# 无扩展名的白名单文件名（小写比较）。pre-commit 是钩子本身，.xbgtasix-root 是根标记。
ALLOWED_NAMES = frozenset({
    ".gitignore", ".gitattributes", ".editorconfig", ".xbgtasix-root",
    ".xbgtasix-ignore", "license", "copying", "notice", "pre-commit",
})

# 必须永不入库的格式的**首字节**签名。抄自一份成熟清单，覆盖：
# 可执行、着色器容器、纹理、音频/视频容器、LLVM bitcode、调试符号、库归档、压缩包、安装包。
FORBIDDEN_MAGIC = (
    (b"MZ", "PE executable"),
    (b"DXBC", "shader container"),
    (b"DDS ", "DDS texture"),
    (b"RIFF", "RIFF (audio/video)"),
    (b"BC\xc0\xde", "LLVM bitcode"),
    (b"Microsoft C/C++ MSF", "PDB"),
    (b"!<arch>", "library archive"),
    (b"PK\x03\x04", "zip"),
    (b"7z\xbc\xaf", "7-zip"),
    (b"MSCF", "cabinet"),
    (b"\xd0\xcf\x11\xe0", "MSI/OLE"),
)

# 例外表：**条目数就是技术债计数器**。目标是 0 条。
# 每条都必须带理由字符串，并且路径必须出现在 docs/asset-policy.md 里
# （--selftest 会机械校验这一点，光写代码不写文档会红）。
# 例外表：**目标永远是空的**。每加一条都要在这里写理由，并同步 docs/asset-policy.md。
# 曾经的唯一一条（docs/images/xboxui.png，t38 自绘窗口效果图）已消除：
# 效果图改为由 t38 在测试时生成到 build/suite/xboxui.bmp，不入库。
EXCEPTIONS = {}

# --check-repo 跳过的目录：git 内部、构建产物、DSH 会话产物。
SKIP_DIRS = frozenset({".git", "build", ".dsh", "__pycache__"})

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ---------------------------------------------------------------- 纯函数
def normalize_path(path):
    """统一成 posix 相对路径（仓库内），把 Windows 反斜杠也吃掉。"""
    p = str(path).replace("\\", "/")
    while p.startswith("./"):
        p = p[2:]
    return p.lstrip("/")


def extension_of(name):
    """扩展名（小写，不含点）。点开头的文件名（.gitignore）返回空串。"""
    base = normalize_path(name).rsplit("/", 1)[-1]
    stem, _, ext = base.rpartition(".")
    if not stem:                      # 没有点，或 ".gitignore" 这种点开头的名字
        return ""
    return ext.lower()


def longest_encoded_run(content):
    """最长连续"可能是 hex/base64 的字符段"长度。

    hex = [0-9a-fA-F]，base64 = [A-Za-z0-9+/=]；统一按 base64 字符集算。
    这是"把数据塞进字符串常量"的最便宜探测：正常源码里不会有 512 个连续
    字母数字混排的字符（一个中文注释、一行代码、一个 GUID 都远小于它）。
    """
    best = run = 0
    for b in content:
        if 48 <= b <= 57 or 65 <= b <= 90 or 97 <= b <= 122 or b in (0x2B, 0x2F, 0x3D):
            run += 1
            if run > best:
                best = run
        else:
            run = 0
    return best


def check(path, content):
    """纯函数：path 是仓库内相对路径，content 是 bytes（str 按 UTF-8 编码）。

    返回违规原因列表，空列表 = 合规。原因字符串以 ASCII 码开头，方便机器判定：
    [magic] [ext] [size] [nul] [blob]；[io] 只在 CLI 读文件失败时出现。
    """
    if isinstance(content, str):
        content = content.encode("utf-8", "replace")
    elif isinstance(content, (bytearray, memoryview)):
        content = bytes(content)

    rel = normalize_path(path)

    # 例外表命中：直接放行。理由在 EXCEPTIONS 里，且必须写进 docs/asset-policy.md。
    # 比较用小写：Windows 上同一文件可能以不同大小写出现在索引里。
    if rel.lower() in EXCEPTIONS:
        return []

    reasons = []
    base = rel.rsplit("/", 1)[-1]
    ext = extension_of(base)

    # 3) 魔数优先于扩展名：先看首字节，一个叫 notes.txt 的 PE 也要抓
    for magic, label in FORBIDDEN_MAGIC:
        if content.startswith(magic):
            reasons.append("[magic] 首字节是 %r（%s）：二进制/派生数据不入库"
                           % (magic, label))
            break

    # 1) 扩展名 / 文件名白名单
    if base.lower() not in ALLOWED_NAMES and ext not in ALLOWED_EXTENSIONS:
        reasons.append("[ext] 不在白名单（文件名 %r，扩展名 %r）：仓库只收文本源码与文本配置"
                       % (base, ext))

    # 2) 大小
    if len(content) > MAX_FILE_SIZE:
        reasons.append("[size] %d 字节 > 上限 %d 字节（512 KiB）"
                       % (len(content), MAX_FILE_SIZE))

    # 4) NUL 字节
    if b"\x00" in content:
        reasons.append("[nul] 内容含 NUL 字节：不是文本文件")

    # 5) 超长 hex/base64 段
    longest = longest_encoded_run(content)
    if longest > MAX_ENCODED_RUN:
        reasons.append("[blob] 有 %d 字符连续 hex/base64 候选段 > %d：疑似把数据塞进字符串常量"
                       % (longest, MAX_ENCODED_RUN))

    return reasons


# ---------------------------------------------------------------- 自测
def _text_of_size(n):
    """造一段长度正好 n 的合法文本（避免触发编码段检查）。"""
    unit = b"# a short line of text\n"
    return (unit * (n // len(unit) + 2))[:n]


_PNG_LIKE = b"\x89PNG\r\n\x1a\n" + b"\x00" * 16 + b"IHDR\x00\x00\x05\x00"
_HEX_1000 = b"0123456789abcdef" * 63          # 1008 字符
_HEX_512 = b"0123456789abcdef" * 32           # 正好 512 字符

# (用例名, 路径, 内容, 期望合规?, 期望命中的原因码或 None)
SELF_TESTS = [
    # ---- 接受 ----
    ("接受：常规源码", "src/x.cpp", b"int main() { return 0; }\n", True, None),
    ("接受：大写扩展名", "SRC/X.CPP", b"// ok\n", True, None),
    ("接受：反斜杠路径", "src\\x.cpp", b"// ok\n", True, None),
    ("接受：sh / py / ps1", "tools/a.py", b"print(1)\n", True, None),
    ("接受：hlsl / def / inc", "src/d3d12_xs/x.hlsl", b"float4 main() : SV_Target { return 0; }\n", True, None),
    ("接受：.gitignore", ".gitignore", b"build/\n*.dll\n", True, None),
    ("接受：.gitattributes", ".gitattributes", b"* text=auto\n", True, None),
    ("接受：LICENSE（无扩展名）", "LICENSE", b"GPL-3.0-or-later\n", True, None),
    ("接受：pre-commit 钩子（无扩展名）", ".githooks/pre-commit", b"#!/bin/sh\nexec x\n", True, None),
    ("接受：.xbgtasix-root 标记（空文件）", ".xbgtasix-root", b"", True, None),
    ("接受：CMakeLists.txt", "CMakeLists.txt", b"project(xbgtasix CXX)\n", True, None),
    ("接受：正好 512 KiB", "src/edge_512k.cpp", _text_of_size(MAX_FILE_SIZE), True, None),
    ("接受：正好 512 字符的 hex 段", "src/edge512.cpp", b'x = "' + _HEX_512 + b'"\n', True, None),
    ("接受：正文提到 MZ（魔数只在偏移 0 判）", "docs/notes.md",
     b"note: MZ is the PE magic, but not at offset 0\n", True, None),
    # 例外表已清空（效果图改为 t38 现场生成）：原来那两张 png 现在必须被拒。
    ("拒绝：docs/images/xboxui.png 已不入库（例外表为空）", "docs/images/xboxui.png",
     _PNG_LIKE, False, "[ext]"),
    ("拒绝：大小写变体同样被拒（Windows）", "Docs/Images/XboxUI.png", _PNG_LIKE, False, "[ext]"),

    # ---- 拒绝：扩展名 ----
    ("拒绝：.dds 扩展名", "textures/body.dds", b"not really a texture\n", False, "[ext]"),
    ("拒绝：.exe 扩展名", "bin/helper.exe", b"hello\n", False, "[ext]"),
    ("拒绝：.png 不在例外表", "docs/images/other.png", _PNG_LIKE, False, "[ext]"),
    ("拒绝：无扩展名且不在名单", "tools/mysteryblob", b"hello\n", False, "[ext]"),
    ("拒绝：伪装成 .gitignore 的陌生名字", "x.gitignore", b"build/\n", False, "[ext]"),

    # ---- 拒绝：魔数（魔数优先于扩展名：扩展名合法也会被抓）----
    ("拒绝：notes.txt 但内容是 PE", "notes.txt", b"MZ\x90\x00" + b"\x00" * 64, False, "[magic]"),
    ("拒绝：README.md 但内容是 DXBC", "README.md", b"DXBC" + b"\x01\x00\x00\x00", False, "[magic]"),
    ("拒绝：x.cpp 但内容是 zip", "src/x.cpp", b"PK\x03\x04\x14\x00\x00\x00", False, "[magic]"),
    ("拒绝：.md 但内容是 DDS 纹理头", "docs/a.md", b"DDS " + b"\x7c\x00\x00\x00", False, "[magic]"),
    ("拒绝：.txt 但内容是 MSI/OLE", "a.txt", b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", False, "[magic]"),
    ("拒绝：.txt 但内容是 LLVM bitcode", "b.txt", b"BC\xc0\xde\x35\x14\x00\x00", False, "[magic]"),
    ("拒绝：.txt 但内容是 PDB", "c.txt", b"Microsoft C/C++ MSF 7.00\r\n", False, "[magic]"),
    ("拒绝：.txt 但内容是 ar 归档", "d.txt", b"!<arch>\n/               ", False, "[magic]"),
    ("拒绝：.txt 但内容是 7z", "e.txt", b"7z\xbc\xaf\x27\x1c", False, "[magic]"),
    ("拒绝：.txt 但内容是 cab", "f.txt", b"MSCF\x00\x00\x00\x00", False, "[magic]"),

    # ---- 拒绝：大小 / NUL / 编码段 ----
    ("拒绝：超过 512 KiB（多 1 字节）", "src/big.cpp", _text_of_size(MAX_FILE_SIZE + 1), False, "[size]"),
    ("拒绝：内容含 NUL", "src/nul.cpp", b"int a;\x00int b;\n", False, "[nul]"),
    ("拒绝：1000 字符 hex 段", "tools/blob.py", b'key = "' + _HEX_1000 + b'"\n', False, "[blob]"),
    ("拒绝：513 字符 hex 段（边界外 1）", "tools/blob513.py", b'key = "' + b"0" * 513 + b'"\n', False, "[blob]"),
    ("拒绝：600 字符 base64 段", "tools/b64.py", b'blob = "' + b"A" * 600 + b'+/="\n', False, "[blob]"),
]


def run_selftest():
    """跑 SELF_TESTS + 结构性检查。返回 0 = 全绿。"""
    fails = 0
    seen_codes = set()

    def fail(name, detail):
        nonlocal fails
        fails += 1
        print("  FAIL  %s" % name)
        for line in detail.splitlines():
            print("          %s" % line)

    for case in SELF_TESTS:
        name, path, content, want_ok = case[0], case[1], case[2], case[3]
        needle = case[4] if len(case) > 4 else None
        try:
            reasons = check(path, content)
        except Exception as e:                      # noqa: BLE001 - 自测要把异常变成红色
            fail(name, "%s: %s" % (type(e).__name__, e))
            continue
        ok = not reasons
        if ok != want_ok:
            fail(name, "期望 %s，实际 %s\n%s" % ("合规" if want_ok else "违规",
                                                "合规" if ok else "违规",
                                                "\n".join("  " + r for r in reasons)))
            continue
        if needle is not None:
            hits = {r.split("]")[0] + "]" for r in reasons}
            seen_codes |= hits
            if needle not in hits:
                fail(name, "期望命中 %s，实际原因: %s" % (needle, reasons))
                continue
        print("  PASS  %s" % name)

    # 结构性检查 1：五道检查都被覆盖到（防止以后有人删掉某道检查还全绿）
    need = {"[ext]", "[size]", "[magic]", "[nul]", "[blob]"}
    missing = need - seen_codes
    if missing:
        fail("五道检查覆盖", "以下检查从未在用例里命中: %s" % sorted(missing))
    else:
        print("  PASS  五道检查都被用例命中（ext/size/magic/nul/blob）")

    # 结构性检查 2：例外表每条都带理由，且路径正常化后仍是干净的相对路径
    bad_exc = [k for k, v in EXCEPTIONS.items()
               if len(str(v).strip()) < 20 or normalize_path(k) != k or k.startswith("/")]
    if bad_exc:
        fail("例外表格式（路径 + 理由 >= 20 字）", "不合格条目: %s" % bad_exc)
    else:
        print("  PASS  例外表 %d 条，每条都带理由" % len(EXCEPTIONS))

    # 结构性检查 3：例外必须写进 docs/asset-policy.md（光写代码不写文档 = 红）
    doc = os.path.join(ROOT, "docs", "asset-policy.md")
    if EXCEPTIONS:
        try:
            text = open(doc, encoding="utf-8", errors="replace").read()
        except OSError as e:
            fail("例外写进文档", "读不到 %s: %s" % (doc, e))
        else:
            undoc = [k for k in EXCEPTIONS if k not in text]
            if undoc:
                fail("例外写进文档", "docs/asset-policy.md 里没有: %s" % undoc)
            else:
                print("  PASS  例外表 %d 条都已写进 docs/asset-policy.md" % len(EXCEPTIONS))
    else:
        print("  PASS  例外表为空（目标状态）")

    print("\n自测: %d 个用例，失败 %d 个 -> %s"
          % (len(SELF_TESTS), fails, "PASS" if fails == 0 else "FAIL"))
    return 0 if fails == 0 else 1


# ---------------------------------------------------------------- CLI 辅助
def _setup_console():
    """生成脚本会打中文；Windows 控制台/CI 管道默认不是 UTF-8，不设会 UnicodeEncodeError。"""
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:                                  # noqa: BLE001 - 老 Python/被重定向
            pass


def _git(args, cwd, data=None):
    try:
        return subprocess.run(["git"] + list(args), cwd=cwd, input=data,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except FileNotFoundError:
        raise RuntimeError("找不到 git 可执行文件")


def _err(proc):
    text = os.fsdecode(proc.stderr or b"").strip()
    return text.splitlines()[-1] if text else "(没有 stderr 输出)"


def _print_result(bad, checked, header, notes=()):
    print(header)
    for note in notes:
        print("  " + note)
    for rel, reasons in bad:
        print("  违规  %s" % rel)
        for reason in reasons:
            print("        - %s" % reason)
    print("检查 %d 个文件，违规 %d 个 -> %s"
          % (checked, len(bad), "PASS" if not bad else "FAIL"))
    return 0 if not bad else 1


def _walk_files(root):
    """仓库里"本可能进 git"的所有文件（posix 相对路径）。目录按 SKIP_DIRS 剪枝。"""
    out = []
    for base, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for fn in sorted(files):
            full = os.path.join(base, fn)
            if os.path.islink(full) and not os.path.exists(full):
                continue                                   # 断链，不是内容
            out.append(os.path.relpath(full, root).replace(os.sep, "/"))
    out.sort()
    return out


def _gitignored(root, rels):
    """用 `git check-ignore --stdin -z` 批量问哪些文件被忽略（不入库就不算违规）。

    一次进程调用处理全部路径；没有 git / 不是仓库 / 出错都退化成"不过滤"。
    """
    if not rels:
        return set()
    try:
        top = _git(["rev-parse", "--show-toplevel"], root)
        if top.returncode != 0:
            return set()
        toplevel = os.path.realpath(os.fsdecode(top.stdout.strip()))
        names = [os.path.relpath(os.path.join(root, r.replace("/", os.sep)), toplevel)
                 .replace(os.sep, "/") for r in rels]
        data = b"".join(os.fsencode(n) + b"\0" for n in names)
        proc = _git(["check-ignore", "--stdin", "-z"], toplevel, data)
        if proc.returncode not in (0, 1):                  # 1 = 一个都没命中，正常
            return set()
        hit = {os.fsdecode(x) for x in proc.stdout.split(b"\0") if x}
        return {rel for name, rel in zip(names, rels) if name in hit}
    except RuntimeError:
        return set()


def cmd_staged(root=ROOT):
    """只查 git 索引。**不读工作区**：否则"先改回干净版、提交脏版"就绕过了。"""
    proc = _git(["diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"], root)
    if proc.returncode != 0:
        print("错误：git diff --cached 失败（不在 git 仓库里，或索引损坏）：%s"
              % _err(proc))
        return 2
    rels = [os.fsdecode(x) for x in proc.stdout.split(b"\0") if x]

    bad = []
    for rel in rels:
        blob = _git(["show", ":" + rel], root)
        if blob.returncode != 0:
            bad.append((rel, ["[io] git show :%s 失败：%s" % (rel, _err(blob))]))
            continue
        reasons = check(rel, blob.stdout)
        if reasons:
            bad.append((rel, reasons))

    return _print_result(
        bad, len(rels),
        "== 合规闸（--staged）：逐个读 git 索引里的 blob，不读工作区")


def cmd_check_repo(root=ROOT):
    """一次性扫全仓。CI / 装机自测都跑这条：克隆下来没装钩子也会被拦。"""
    rels = _walk_files(root)
    ignored = _gitignored(root, rels)
    kept = [r for r in rels if r not in ignored]
    exc_hits = sorted(r for r in kept if normalize_path(r).lower() in EXCEPTIONS)

    bad = []
    for rel in kept:
        full = os.path.join(root, rel.replace("/", os.sep))
        try:
            with open(full, "rb") as f:
                content = f.read()
        except OSError as e:
            bad.append((rel, ["[io] 读失败：%s" % e]))
            continue
        reasons = check(rel, content)
        if reasons:
            bad.append((rel, reasons))

    notes = ["跳过目录：%s" % " ".join(d + "/" for d in sorted(SKIP_DIRS)),
             ".gitignore 排除 %d 个本地文件（不入库就不算违规）" % (len(rels) - len(kept))]
    if exc_hits:
        notes.append("例外命中 %d 条（理由见 docs/asset-policy.md）：%s"
                     % (len(exc_hits), " ".join(exc_hits)))
    else:
        notes.append("例外命中 0 条")

    return _print_result(bad, len(kept), "== 合规闸（--check-repo）：全仓扫描", notes)


def cmd_files(paths):
    """直接查指定文件（调试 / 手工排查用），不做 gitignore 过滤。"""
    bad = []
    for given in paths:
        full = given if os.path.isabs(given) else os.path.join(os.getcwd(), given)
        try:
            with open(full, "rb") as f:
                content = f.read()
        except OSError as e:
            print("错误：读不到 %s：%s" % (given, e))
            return 2
        rel = os.path.relpath(full, ROOT).replace(os.sep, "/")
        reasons = check(rel, content)
        if reasons:
            bad.append((rel, reasons))
    return _print_result(bad, len(paths), "== 合规闸（指定文件）")


def cmd_install_hooks(root=ROOT):
    """把 core.hooksPath 指到本仓库的 .githooks（钩子本身只有一行 exec）。"""
    marker = os.path.join(root, ".xbgtasix-root")
    hook = os.path.join(root, ".githooks", "pre-commit")
    if not os.path.isfile(marker):
        print("错误：%s 不存在（这是仓库根标记，缺了运行时闸也认不出仓库）" % marker)
        return 2
    if not os.path.isfile(hook):
        print("错误：%s 不存在" % hook)
        return 2

    prev = _git(["config", "--get", "core.hooksPath"], root)
    old = os.fsdecode(prev.stdout).strip() if prev.returncode == 0 else ""

    proc = _git(["config", "core.hooksPath", ".githooks"], root)
    if proc.returncode != 0:
        print("错误：git config core.hooksPath 失败：%s" % _err(proc))
        return 2
    try:
        os.chmod(hook, 0o755)                              # POSIX 需要可执行位
    except OSError:
        pass

    cur = _git(["config", "--get", "core.hooksPath"], root)
    print("已安装 pre-commit 钩子：core.hooksPath = %s"
          % os.fsdecode(cur.stdout).strip())
    print("  钩子文件: %s" % hook)
    print("  钩子内容只有一行 exec，调用本文件，保证钩子/CI/自测走同一份代码")
    if old and old != ".githooks":
        print("  注意：原来的 core.hooksPath 是 %r，已覆盖；还原用：git config core.hooksPath %s"
              % (old, old))
    print("  卸载：git config --unset core.hooksPath")
    return 0


# ---------------------------------------------------------------- CLI
def main(argv=None):
    _setup_console()
    ap = argparse.ArgumentParser(
        prog="policy.py",
        description="仓库静态合规闸：只有小的、文本的、我们写的文件能进 git。")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--staged", action="store_true",
                   help="只查 git 索引（pre-commit 钩子用；不读工作区）")
    g.add_argument("--check-repo", action="store_true",
                   help="扫全仓（跳过 .git/ build/ .dsh/），CI/仓库自测用")
    g.add_argument("--install-hooks", action="store_true",
                   help="把 core.hooksPath 指到 .githooks")
    g.add_argument("--selftest", action="store_true",
                   help="跑内置单测（不读 git 状态）")
    ap.add_argument("files", nargs="*", help="直接检查这些文件（工作区）")
    a = ap.parse_args(argv)

    if a.selftest:
        return run_selftest()
    if a.install_hooks:
        return cmd_install_hooks()
    if a.staged:
        return cmd_staged()
    if a.check_repo:
        return cmd_check_repo()
    if a.files:
        return cmd_files(a.files)

    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
