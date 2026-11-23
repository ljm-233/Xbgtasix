#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""检查 Markdown 里的相对链接是否都指向真实存在的文件。

CI 用：文档一多，最容易出现"改了文件名但链接没改"。
只用标准库，跑得比任何 linter 都快。

用法：
    python tools/check_docs.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINK = re.compile(r"\[[^\]]*\]\(([^)]+)\)")


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    bad = 0
    checked = 0
    for base, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in (".git", "build", "__pycache__")]
        for fn in files:
            if not fn.endswith(".md"):
                continue
            path = os.path.join(base, fn)
            text = open(path, encoding="utf-8", errors="replace").read()
            for target in LINK.findall(text):
                if target.startswith(("http://", "https://", "#", "mailto:")):
                    continue
                target = target.split("#", 1)[0].strip()
                if not target:
                    continue
                checked += 1
                full = os.path.normpath(os.path.join(base, target))
                if not os.path.exists(full):
                    print("  断裂  %s -> %s" % (os.path.relpath(path, ROOT), target))
                    bad += 1

    print("检查了 %d 个相对链接，断裂 %d 个 -> %s" % (checked, bad, "PASS" if bad == 0 else "FAIL"))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
