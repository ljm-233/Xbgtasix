#!/usr/bin/env python3
"""用包自带的 layout XML 校验"这份泄露包到底缺什么"。

layout_*.xml 是 makepkg 的清单：每个 <FileGroup Include="..."> 都是包内必须存在的文件。
把它和实际目录对比，就能精确列出缺失项 —— 比"扫目录找 exe"强得多。

用法：
    python verify_package.py <包目录>
"""
import argparse
import os
import sys
import xml.etree.ElementTree as ET


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    root_dir = a.dir
    layouts = [f for f in os.listdir(root_dir) if f.startswith("layout_") and f.endswith(".xml")]
    if not layouts:
        sys.exit("目录里没有 layout_*.xml")
    path = os.path.join(root_dir, layouts[0])
    tree = ET.parse(path)

    entries = []   # (include, source_path, chunk)
    for chunk in tree.getroot().iter("Chunk"):
        cid = chunk.get("Id", "?")
        for fg in chunk.iter("FileGroup"):
            inc = fg.get("Include")
            if inc:
                entries.append((inc, fg.get("SourcePath", ""), cid))

    print("== %s" % os.path.basename(path))
    print("   清单条目: %d（来自 %d 个 Chunk）" % (len(entries), len(set(c for _, _, c in entries))))

    # 泄露包是按"源目录结构"摊开的（例如 packs/story000/story000.rpf），
    # 而清单里写的是打包后的根目录路径。所以按 basename 建索引再查，
    # 否则会把"在子目录里但确实存在"的文件误报为缺失。
    index = {}
    for base, _dirs, files in os.walk(root_dir):
        for f in files:
            index.setdefault(f, os.path.join(base, f))

    present, missing = [], []
    for inc, src, cid in entries:
        p = os.path.join(root_dir, inc)
        if os.path.isfile(p):
            present.append((inc, os.path.getsize(p)))
        elif inc in index:
            present.append((inc, os.path.getsize(index[inc])))
        else:
            missing.append((inc, src, cid))

    total_size = sum(s for _, s in present)
    print("   实际存在: %d 个，共 %.2f GB" % (len(present), total_size / 1e9))
    print("   **缺失: %d 个**" % len(missing))
    for inc, src, cid in missing:
        print("     Chunk %-5s %-40s （清单说来自 %s）" % (cid, inc, src or "?"))

    # 反向检查：目录里有但清单没列的（多数是打包中间产物/缓存）
    listed = {inc for inc, _, _ in entries}
    extra = [f for f in os.listdir(root_dir)
             if os.path.isfile(os.path.join(root_dir, f)) and f not in listed
             and not f.startswith("layout_") and not f.startswith("Update.")
             and f not in ("appxmanifes.xml", "Update.alignmentchunk")]
    if extra:
        print("   清单外文件: %s%s" % (", ".join(extra[:6]), " …" if len(extra) > 6 else ""))

    # 与 MicrosoftGame.Config 声明的可执行文件对账
    cfg = os.path.join(root_dir, "MicrosoftGame.Config")
    if os.path.isfile(cfg):
        import re
        txt = open(cfg, encoding="utf-8-sig", errors="replace").read()
        exes = re.findall(r'Executable Name="([^"]+)"', txt)
        for e in exes:
            st = "存在" if os.path.isfile(os.path.join(root_dir, e)) else "**不存在**"
            print("   MicrosoftGame.Config 声明的可执行文件: %-32s %s" % (e, st))


if __name__ == "__main__":
    main()
