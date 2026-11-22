#!/usr/bin/env python3
"""按名字给模型找最可能的贴图（OBJ 不带材质信息时的兜底办法）。

思路：把模型名和贴图名都切成 token，去掉"尺寸/版型"这类噪声词
（mr1/fr1/loose/regular/slim/long/mid/full/dr/ft/hl/mw…），
剩下的人物/服装类 token 重合度越高越可能是同一套。

用法：
    python match_texture.py <模型名或.obj> <贴图目录> [--top 5]
"""
import argparse
import os
import re
import sys

# 版型/尺寸/性别/后缀噪声，不参与匹配
NOISE = {
    "mr1", "mr2", "mr3", "fr1", "fr2", "fr3", "mr", "fr",
    "loose", "regular", "slim", "disheveled", "o", "hl", "mw", "lw",
    "long", "mid", "short", "full", "dr", "ft", "dd", "up", "lod", "lod0",
    "char", "bipedbounds", "bounds", "archetype", "male", "female",
}


def tokens(name):
    name = os.path.splitext(os.path.basename(name))[0].lower()
    parts = re.split(r"[_#.\-]", name)
    return {p for p in parts if p and p not in NOISE and not p.isdigit()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("texdir")
    ap.add_argument("--top", type=int, default=5)
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    mt = tokens(a.model)
    if not mt:
        sys.exit("模型名里没有有效 token")
    print("模型 token:", " ".join(sorted(mt)))

    scored = []
    for fn in os.listdir(a.texdir):
        if not fn.lower().endswith((".png", ".dds", ".jpg")):
            continue
        tt = tokens(fn)
        common = mt & tt
        if not common:
            continue
        # 共同 token 数优先，其次 Jaccard
        score = (len(common), len(common) / len(mt | tt))
        scored.append((score, fn, common))

    scored.sort(key=lambda x: (-x[0][0], -x[0][1]))
    if not scored:
        print("没有名字相关的贴图（该目录里 %d 个文件都不含模型 token）" %
              len([f for f in os.listdir(a.texdir) if f.lower().endswith(".png")]))
        return
    print("\n候选（共同 token 数 / Jaccard）：")
    for (n, j), fn, common in scored[:a.top]:
        print("  %-52s %d / %.2f   共同: %s" % (fn, n, j, " ".join(sorted(common))))


if __name__ == "__main__":
    main()
