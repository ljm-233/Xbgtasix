"""Print the bytes at an RVA of a PE image, optionally disassembled.

为什么需要它：`llvm-objdump --start-address` 对那个 114 MB 的标题 exe 不返回任何指令
（RVA 区间不落在它愿意映射的范围里），而我们要看的就是"断言前那几条比较指令在比什么"。
这里按节表自己把 RVA 映射成文件偏移，取出字节；`--disasm` 再把字节喂给 `llvm-mc`。

工作区里那个 `llvm-mc.exe` 是只编了 x86 的构建 —— 对标题自己的 x86-64 代码正好够用
（反汇编 GPU 着色器才需要 AMDGPU target，那是另一件事）。

用法：
    python -I tools/rva_bytes.py <exe> 0x247AF2D              # 看 32 字节
    python -I tools/rva_bytes.py <exe> 0x247AEE0 0x78 --disasm
    LLVM_MC="/c/Program Files/LLVM/bin/llvm-mc.exe" ...       # 显式指定

退出码：0 成功；1 RVA 不在任何节里；2 找不到 llvm-mc（此时仍然打印了字节）。
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys


def rva_to_offset(d, rva):
    e = struct.unpack_from("<I", d, 0x3C)[0]
    if d[e:e + 4] != b"PE\0\0":
        raise SystemExit("不是 PE 映像")
    nsec = struct.unpack_from("<H", d, e + 6)[0]
    optsz = struct.unpack_from("<H", d, e + 20)[0]
    sec = e + 24 + optsz
    for i in range(nsec):
        o = sec + i * 40
        name = d[o:o + 8].rstrip(b"\0").decode("latin-1")
        vsize, vaddr, rawsize, rawoff = struct.unpack_from("<IIII", d, o + 8)
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return name, rawoff + (rva - vaddr)
    return None, None


def find_llvm_mc():
    for cand in (os.environ.get("LLVM_MC"), "llvm-mc"):
        if not cand:
            continue
        found = shutil.which(cand)
        if found:
            return found
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("rva", type=lambda s: int(s, 0))
    ap.add_argument("count", nargs="?", type=lambda s: int(s, 0), default=0x20)
    ap.add_argument("--disasm", action="store_true", help="把字节交给 llvm-mc 反汇编（x86-64）")
    ap.add_argument("--triple", default="x86_64")
    a = ap.parse_args()

    with open(a.exe, "rb") as f:
        d = f.read()
    section, off = rva_to_offset(d, a.rva)
    if off is None:
        print(f"RVA {a.rva:#x} 不在任何节里", file=sys.stderr)
        return 1
    blob = d[off:off + a.count]
    print(f"; section {section}  rva {a.rva:#x} -> file {off:#x}  ({len(blob)} bytes)",
          file=sys.stderr)
    print(" ".join(f"0x{b:02x}" for b in blob))

    if not a.disasm:
        return 0
    mc = find_llvm_mc()
    if not mc:
        print("找不到 llvm-mc（设 LLVM_MC 或放进 PATH）", file=sys.stderr)
        return 2
    text = " ".join(f"0x{b:02x}" for b in blob) + "\n"
    r = subprocess.run([mc, "-disassemble", f"-triple={a.triple}"], input=text.encode(),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    sys.stdout.write(r.stdout.decode("utf-8", "replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
