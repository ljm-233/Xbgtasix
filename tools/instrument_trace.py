#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""给 src/xgameruntime/*.cpp 里的每个导出 C 函数插入统一入口追踪。

规则（机械、可核对）：
  * 只处理 exports*.txt 里列出的 268 个导出名，且只处理**函数定义**
    （形参列表后面跟着 `{`），声明/调用一律不碰；
  * 在函数体 `{` 之后插入一行 `XB_TRACE_ENTRY("Name", a, b, c, d);`；
  * 参数名从形参列表取；无名参数 / 引用类型 / 函数指针一律传 0ULL；
  * 形参不足 4 个补 0ULL，超过 4 个只取前 4 个；
  * 跳过 logf / unimpl / 名字含 trace 的函数（避免递归）；
  * 每个文件在 `#include "xbgtasix/trace.h"` 后补一行 trace_entry.h 的 include；
  * 幂等：已经插过的函数（`{` 后第一行就是 XB_TRACE_ENTRY）不会重复插。

用法：
    python tools/instrument_trace.py           # 只报告，不写文件
    python tools/instrument_trace.py --apply   # 真正写入
"""

from __future__ import annotations

import argparse
import collections
import glob
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = ROOT / "src" / "xgameruntime"
INCLUDE_LINE = '#include "trace_entry.h"   // 统一入口追踪（XB_TRACE_ENTRY）'
INCLUDE_ANCHOR = '#include "xbgtasix/trace.h"'
SKIP_SUBSTRINGS = ("trace",)          # 名字里含 trace 的不插（递归风险）
SKIP_NAMES = {"logf", "unimpl", "trace_entry_log", "trace_entry_enabled"}
MAX_ARGS = 4


def load_exports() -> set[str]:
    names: set[str] = set()
    for f in sorted(glob.glob(str(SRC_DIR / "exports*.txt"))):
        with open(f, encoding="utf-8") as fh:
            for line in fh:
                s = line.strip()
                if not s or s.startswith(";"):
                    continue
                names.add(s.split()[0])
    return names


def brace_depths(text: str) -> list[int]:
    """每个字符位置处的花括号深度。"""
    depths = [0] * len(text)
    d = 0
    for i, ch in enumerate(text):
        depths[i] = d
        if ch == "{":
            d += 1
        elif ch == "}":
            d -= 1
    return depths


def split_params(sig: str) -> list[str]:
    parts: list[str] = []
    depth = 0
    cur = ""
    for ch in sig:
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return [p.strip() for p in parts]


TYPE_WORDS = {
    "void", "const", "volatile", "signed", "unsigned", "char", "short", "int",
    "long", "float", "double", "bool", "struct", "enum", "union", "class",
    "size_t", "std", "wchar_t",
}


def param_arg(decl: str) -> str | None:
    """从一个形参声明里取可安全求值的实参表达式；不取则返回 None（补 0ULL）。"""
    decl = decl.strip()
    if not decl or decl == "void":
        return None
    # 引用（含 &&）与函数指针：插桩时统一传 0ULL，避免解引用/语法噪声
    if "&" in decl or "(*" in decl or "[" in decl:
        return None
    m = re.search(r"([A-Za-z_]\w*)\s*$", decl)
    if not m:
        return None          # 无名参数
    name = m.group(1)
    if name in TYPE_WORDS:
        return None          # 只剩类型、没有名字
    return name


def find_definitions(text: str, exports: set[str]):
    """返回 [(name, name_start, name_end, open_paren, close_paren, brace_pos)]。"""
    name_re = re.compile(
        r"\b(" + "|".join(sorted(map(re.escape, exports), key=len, reverse=True)) + r")\s*\("
    )
    depths = brace_depths(text)
    defs = []
    for m in name_re.finditer(text):
        name = m.group(1)
        p = m.end() - 1                      # '(' 的位置
        # 找配对的 ')'
        i, depth = p, 0
        while i < len(text):
            c = text[i]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        if i >= len(text):
            continue
        close = i
        # 形参列表后面只允许 noexcept / const / override 之类，然后必须是 '{'
        mo = re.match(
            r"\s*(?:noexcept\s*(?:\([^)]*\))?\s*)?(?:const\s*)?(?:override\s*)?\{",
            text[close + 1:close + 200],
        )
        if not mo:
            continue                         # 声明或调用，不是定义
        if depths[m.start()] > 1:
            continue                         # 在别的函数体里：不可能是定义
        brace = close + 1 + mo.end() - 1
        # 名字前面必须是返回类型 + WINAPI/STDAPI 这类，排除 "返回 XFoo(...)" 之类
        line_start = text.rfind("\n", 0, m.start()) + 1
        prefix = text[line_start:m.start()].strip()
        if not prefix:
            continue
        defs.append((name, m.start(), m.end(), p, close, brace))
    return defs


def trace_call_text(name: str, params: list[str]) -> str:
    args = []
    for decl in params[:MAX_ARGS]:
        args.append(param_arg(decl) or "0ULL")
    while len(args) < MAX_ARGS:
        args.append("0ULL")
    return f'    XB_TRACE_ENTRY("{name}", {", ".join(args)});'


def add_include(text: str) -> tuple[str, bool]:
    if INCLUDE_LINE in text:
        return text, False
    anchor = text.find(INCLUDE_ANCHOR)
    if anchor < 0:
        raise RuntimeError("找不到 trace.h 的 include 锚点")
    line_end = text.find("\n", anchor)
    if line_end < 0:
        raise RuntimeError("trace.h include 行没有换行")
    return text[:line_end + 1] + INCLUDE_LINE + "\n" + text[line_end + 1:], True


def process_file(path: Path, exports: set[str], apply: bool):
    text = path.read_text(encoding="utf-8")
    if "\r\n" in text:
        raise RuntimeError(f"{path} 里有 CRLF，脚本不处理（本仓这些文件是 LF）")
    defs = find_definitions(text, exports)
    seen: collections.Counter[str] = collections.Counter(d[0] for d in defs)
    dup = [n for n, c in seen.items() if c > 1]
    if dup:
        raise RuntimeError(f"{path}: 同名定义出现多次：{dup}")

    plan = []
    for name, _ns, _ne, p, close, brace in defs:
        if name in SKIP_NAMES or any(s in name.lower() for s in SKIP_SUBSTRINGS):
            plan.append((name, brace, None))
            continue
        body_head = text[brace + 1:brace + 120]
        if body_head.lstrip().startswith("XB_TRACE_ENTRY("):
            plan.append((name, brace, "already"))
            continue
        params = split_params(text[p + 1:close])
        line = trace_call_text(name, params)
        plan.append((name, brace, line))

    insert = [(brace, line) for _n, brace, line in plan if line and line not in ("already",)]
    skipped = [_n for _n, _b, line in plan if line is None]
    already = sum(1 for _n, _b, line in plan if line == "already")

    new_text = text
    if insert:
        # 从后往前改，保持前面位置有效
        for brace, line in sorted(insert, reverse=True):
            line_start = new_text.rfind("\n", 0, brace) + 1
            line_end = new_text.find("\n", brace)
            rest = new_text[brace + 1:line_end].strip()
            if rest == "":
                new_text = new_text[:brace + 1] + "\n" + line + new_text[brace + 1:]
            elif rest == "}":
                # 空函数体 `{}`：展开成三行，保持 4 空格缩进
                old = new_text[brace:line_end]
                assert old.strip() == "{}", (path, old)
                new_text = new_text[:brace] + "{\n" + line + "\n}" + new_text[line_end:]
            else:
                raise RuntimeError(f"{path}: 不认识的函数体写法：{rest[:30]!r}")
        new_text, include_added = add_include(new_text)
    else:
        include_added = False

    changed = new_text != text
    if apply and changed:
        # newline="" 禁用换行翻译：本仓 .gitattributes 是 *.cpp/*.h eol=lf，
        # 直接用默认文本模式写会把整份文件变成 CRLF（行尾混用）。
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(new_text)
    return {
        "file": str(path.relative_to(ROOT)).replace("\\", "/"),
        "defs": len(defs),
        "names": [d[0] for d in defs],
        "inserted": len(insert),
        "skipped": skipped,
        "already": already,
        "include_added": include_added,
        "changed": changed,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true", help="真正写文件（默认只报告）")
    args = ap.parse_args()

    exports = load_exports()
    files = sorted(p for p in SRC_DIR.glob("*.cpp") if p.name != "trace_entry.cpp")
    print(f"# 导出名 {len(exports)} 个；扫描 {len(files)} 个 .cpp")
    print(f"# 模式：{'APPLY（写文件）' if args.apply else 'DRY-RUN（不写文件）'}")
    total_defs = total_ins = 0
    covered: set[str] = set()
    for path in files:
        info = process_file(path, exports, args.apply)
        total_defs += info["defs"]
        total_ins += info["inserted"]
        covered |= set(info["names"])
        print(
            f"{info['file']}: 定义 {info['defs']}，插入 {info['inserted']}"
            f"{'（已插过 ' + str(info['already']) + '）' if info['already'] else ''}"
            f"{'（跳过 ' + str(len(info['skipped'])) + '）' if info['skipped'] else ''}"
            f"{'（补 include）' if info['include_added'] else ''}"
        )
    print(f"\n合计：函数定义 {total_defs}，插入 {total_ins}")
    missing = exports - covered
    extra = covered - exports
    if missing or extra:
        print(f"错误：导出名与定义对不上 missing={sorted(missing)} extra={sorted(extra)}")
        return 1
    print(f"核对：{len(exports)} 个导出名全部找到唯一函数定义")
    return 0


if __name__ == "__main__":
    sys.exit(main())
