#!/usr/bin/env python3
"""从 xgameruntime.lib 里扫出 _GUID_xxx，生成真正的 GUID 数据定义，并补进 .def。

名字本身就是值：_GUID_014d1cc3_bcfe_41ff_b2f0_e1ef07155828
  -> {0x014d1cc3, 0xbcfe, 0x41ff, {0xb2,0xf0,0xe1,0xef,0x07,0x15,0x58,0x28}}

为什么必须这么做：这些是**数据**导出（COM IID）。当成函数桩导出时，调用方
取到的是代码地址，IsEqualGUID / QueryInterface 会静默全部失效。

用法：python tools/gen_guids.py <xgameruntime.lib> <out_guids.cpp> <out.def>
"""
import io
import sys

try:   # Windows 控制台默认是 CP1252/CP936，打中文会抛 UnicodeEncodeError
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass
import re
import sys


def main():
    lib, cpp, incfile = sys.argv[1], sys.argv[2], sys.argv[3]
    data = open(lib, "rb").read()
    names = sorted({m.decode() for m in re.findall(rb"_GUID_[0-9a-fA-F_]{36}", data)})

    lines = ["// 自动生成：_GUID_xxx 是数据导出（COM IID），名字即 GUID 值",
             "#include <guiddef.h>", "", 'extern "C" {']
    ok = []
    for g in names:
        parts = g[len("_GUID_"):].split("_")
        if len(parts) != 5:
            continue
        d1, d2, d3, b1, b2 = parts
        if not (len(d1) == 8 and len(d2) == 4 and len(d3) == 4
                and len(b1) == 4 and len(b2) == 12):
            continue
        bytes1 = ",".join("0x%s" % b1[i:i + 2] for i in range(0, 4, 2))
        bytes2 = ",".join("0x%s" % b2[i:i + 2] for i in range(0, 12, 2))
        lines.append("extern const GUID %s = {0x%s,0x%s,0x%s,{%s,%s}};"
                     % (g, d1, d2, d3, bytes1, bytes2))
        ok.append(g)
    lines.append('}  // extern "C"')
    io.open(cpp, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")

    # 导出名片段（build.sh 会把它拼进 .def；数据符号不需要别名）
    io.open(incfile, "w", encoding="utf-8", newline="\n").write(
        "".join("  %s DATA\n" % g for g in ok))
    print("%d 个 GUID 数据定义 -> %s / %s" % (len(ok), cpp, incfile))


if __name__ == "__main__":
    main()
