#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""定位本机 Microsoft GDK 的头文件与导入库，并可一键生成导出桩。

GDK 装到哪没有固定路径（安装器版本不同、还有 NuGet 分发），所以这里扫全部已知根目录，
按文件名匹配，最后可选调用 gen_stubs.py。

用法：
    python tools/find_gdk.py             # 只报告
    python tools/find_gdk.py --gen       # 找到 .lib 后自动生成桩到 src/generated
"""
import argparse
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

WANT_HEADERS = ["xgameruntime.h", "xmem.h", "xgameruntimeinit.h", "xsystem.h", "xuser.h",
                "xstore.h", "xtaskqueue.h", "xgameerr.h"]
WANT_LIBS = ["xgameruntime.lib", "xmem.lib", "xgameruntimeinit.lib"]

# 生成桩时只对这些库名出桩（其余先不管）
GEN_TARGETS = {"xgameruntime.lib": "xgameruntime", "xmem.lib": "xmem"}


def candidate_roots():
    roots = []
    for var in ("GameDKLatest", "GameDK", "GRDK", "GXDK"):
        v = os.environ.get(var)
        if v:
            roots.append(v)
    for base in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")):
        if not base:
            continue
        roots += [os.path.join(base, "Microsoft GDK"),
                  os.path.join(base, "Microsoft", "GDK")]
    profile = os.path.expanduser("~")
    roots.append(os.path.join(profile, ".nuget", "packages"))
    roots.append(r"C:\Program Files (x86)\Microsoft GDK")
    return [r for r in dict.fromkeys(roots) if r and os.path.isdir(r)]


def walk_find(roots, want):
    hits = {}
    for root in roots:
        for name in want:
            for path in glob.iglob(os.path.join(root, "**", name), recursive=True):
                hits.setdefault(name, []).append(path)
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", action="store_true")
    args = ap.parse_args()

    try:  # 终端编码不一致时别让中文输出变成乱码/报错
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    roots = candidate_roots()
    if not roots:
        print("没有找到任何 GDK 安装根目录。")
        print("先安装：winget install Microsoft.Gaming.GDK")
        return 1

    print("搜索根目录：")
    for r in roots:
        print("  " + r)

    headers = walk_find(roots, WANT_HEADERS)
    libs = walk_find(roots, WANT_LIBS)

    print("\n头文件：")
    if not headers:
        print("  （未找到）—— 可能只装了 PC 包，或安装在非标准路径")
    for name in WANT_HEADERS:
        for path in headers.get(name, [])[:4]:
            print("  %-22s %s" % (name, path))

    print("\n导入库：")
    if not libs:
        print("  （未找到）")
    for name in WANT_LIBS:
        for path in libs.get(name, [])[:4]:
            print("  %-22s %s" % (name, path))

    if args.gen:
        out = os.path.join(HERE, "..", "src", "generated")
        for name, dll in GEN_TARGETS.items():
            paths = libs.get(name)
            if not paths:
                continue
            lib = paths[0]
            print("\n生成桩：%s -> %s" % (lib, dll))
            subprocess.run([sys.executable, os.path.join(HERE, "gen_stubs.py"),
                            lib, "--dll", dll, "--out", out], check=False)

    print("\n提示：XMem 文档属于 NDA 范围，但默认实现源码随 GDK 一起安装，")
    print("     所以装上之后 xmem.h 通常能在上面列出的路径里直接找到。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
