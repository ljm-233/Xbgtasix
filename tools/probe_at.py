#!/usr/bin/env python3
"""probe_at.py —— 在游戏的某个 RVA 上"装眼睛"，把那一刻的寄存器与字符串打出来。

为什么不是硬件断点：本项目的 Wine 环境**不投递 Dr0..Dr3 的断点事件**（实测 Dr2/Dr3 装上、
`Dr7` 也对、连必经的函数入口都零命中）。所以走 shim 里的一次性 int3 探针：
把该 RVA 的首字节改成 `0xCC`，命中时打寄存器 + 把 `rbx`/`rdi` 当 C 字符串读出来，
然后还原字节、把 `Rip` 拨回原指令继续跑（只改内存，不碰游戏文件）。

一次最多两个 RVA：
  第一个命中打 **rbx**，第二个命中打 **rdi**（与执行断点的约定一致）。

用法：
  python3 tools/probe_at.py 56EBA8                 # 一个点，看 rbx
  python3 tools/probe_at.py 56EBA8 56EBBD --rpkg   # 两个点：0x56EBA8 看 rbx，0x56EBBD 看 rdi
  python3 tools/probe_at.py 56EBA8 --timeout 60 --dll-dir build/linux

输出：先跑一轮，然后把日志里的 `探针N` / `执行断点` / `FATAL` 行挑出来打印。
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PKG = "/mnt/winbot-software/GTA6/GTA6.Xbox.v1.0.2.1-CYBERLEEK"
DEFAULT_DLL_DIR = "build/linux"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("rvas", nargs="+", help="十六进制 RVA（不要 0x 也行），最多两个")
    ap.add_argument("--pkg", default=DEFAULT_PKG)
    ap.add_argument("--dll-dir", default=DEFAULT_DLL_DIR)
    ap.add_argument("--timeout", default="60")
    ap.add_argument("--log", default=None, help="游戏日志路径（默认 <pkg>/xbgtasix.log）")
    ap.add_argument("--quiet", action="store_true", help="只打印探针命中，不打印启动横幅")
    a = ap.parse_args()

    if len(a.rvas) > 2:
        print("最多两个 RVA（Dr2/Dr3 的槽位约定）", file=sys.stderr)
        return 2
    env_at = ",".join(r.lower().removeprefix("0x") for r in a.rvas)

    log = a.log or os.path.join(a.pkg, "xbgtasix.log")

    env = dict(os.environ)
    env["XBGTASIX_INT3_AT"] = env_at
    cmd = [os.path.join(HERE, "tools", "run_wine.sh"), "--dll-dir", a.dll_dir, "--timeout", a.timeout]
    if not a.quiet:
        print("== 探针 RVA:", env_at)
    r = subprocess.run(cmd, env=env, cwd=HERE, capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    if not a.quiet:
        for line in out.splitlines():
            if line.startswith("== 盘符") or line.startswith("== 结果") or "退出码" in line \
                    or "耗时" in line:
                print(line)

    if not os.path.exists(log):
        print(f"没有日志：{log}", file=sys.stderr)
        return 1
    # 启动器每轮**重写**这个日志，所以整份读就行（别用偏移量：上一轮更长时会漏掉开头）。
    with open(log, "rb") as f:
        tail = f.read().decode("utf-8", errors="replace")

    hits = [ln for ln in tail.splitlines()
            if ("探针" in ln or "执行断点" in ln or "FATAL" in ln or "已装 int3" in ln)]
    if not hits:
        print("这一轮没命中（也没装探针？看 XBGTASIX_INT3_AT 是否生效）")
        return 1
    for ln in hits[:40]:
        print(ln)
    if len(hits) > 40:
        print(f"… 还有 {len(hits) - 40} 行，见 {log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
