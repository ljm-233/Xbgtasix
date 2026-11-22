#!/usr/bin/env python3
"""从 .lib / 符号表生成"全量导出但全部抛异常"的 shim 桩。

用途：拿到 Microsoft GDK 的导入库（xgameruntime.lib 等）后，一条命令生成
      .def + C 源，覆盖该库的全部导出名。之后按 xbgtasix.log 的命中顺序填实现。

用法：
    python gen_stubs.py <file.lib> --dll xgameruntime --out out/
    python gen_stubs.py --symbols symbols.txt --dll xmem --out out/

说明：
  - .lib 用 COFF archive 的第二个链接器成员（符号索引）解析，不依赖 dumpbin。
  - 自动剥掉 __imp_ 前缀，跳过 __imp_/ __ 开头的内部符号与 C++ 装饰名里的 ? 前缀
    （装饰名会原样保留为导出名，通过 .def 别名映射到合法 C 标识符）。
  - 生成的桩函数返回 void：故意 ABI 不完整 —— 它们只负责"炸出来并记录调用名"。
"""
import argparse
import os
import re
import struct
import sys

# 合法导出名：C 标识符或 C++ 装饰名（?Foo@@...），不含控制字符/空格/等号
SANE = re.compile(r"^[A-Za-z_?$][A-Za-z0-9_?$@]*$")
# mingw/CRT 的伪符号，不应该出现在导出表里
JUNK_PREFIX = ("_guard_", "__guard", "___guard", "__imp_", "___ImageBase",
               # 非 API 符号：CRT/编译器内部件、私有实现前缀、GRDK 构建标记
               "Rtl", "PFX", "_XBLD_", "__CxxFrameHandler", "_CxxThrowException",
               "HRESULT_FROM_WIN32", "_except_handler", "__security")


def read_coff_archive(path):
    """返回 COFF archive 里的全部符号名。"""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"!<arch>\n"):
        raise SystemExit("%s 不是 COFF archive" % path)
    pos = 8
    members = []
    while pos + 60 <= len(data):
        header = data[pos:pos + 60]
        name = header[0:16].decode("latin1").rstrip()
        size_field = header[48:58].decode("latin1").strip()
        try:
            size = int(size_field)
        except ValueError:
            break
        body = data[pos + 60:pos + 60 + size]
        members.append((name, body))
        pos += 60 + size
        if pos % 2:
            pos += 1

    # 第一个链接器成员：count, offsets[count], 之后是 null 结尾的字符串池。
    # 注意：MS 的 COFF 把 count/offsets 写成**大端**，GNU 是小端 —— 两种都试，取
    # 能解析出更多"像符号名"的那一个（否则会从字符串池中间开始读，得到乱码）。
    best = []
    for name, body in members:
        if name not in ("/", "/ "):
            continue
        if len(body) < 4:
            continue
        for fmt in (">I", "<I"):
            count = struct.unpack_from(fmt, body, 0)[0]
            off = 4 + 4 * count
            if count == 0 or off > len(body):
                continue
            blob = body[off:]
            syms = [s for s in blob.split(b"\0") if s]
            good = [s for s in syms if SANE.match(s.decode("latin1", "replace"))]
            if len(good) > len(best):
                best = good
    if best:
        return [s.decode("latin1", "replace") for s in best]
    raise SystemExit("在 %s 里找不到可解析的链接器符号表" % path)


def clean(symbols):
    """去重、剥 __imp_、丢掉明显的内部符号。"""
    seen = []
    have = set()
    for sym in symbols:
        if sym.startswith("?"):
            continue  # C++ 装饰名（模板/内联实例），不是真实导出
        if sym.startswith("__imp_"):
            sym = sym[len("__imp_"):]
        elif sym.startswith("__") or sym.startswith("."):
            continue
        if sym.startswith(JUNK_PREFIX) or not sym or sym in have:
            continue
        have.add(sym)
        seen.append(sym)
    return seen


def ident(name, index):
    return "xbgtasix_stub_%04d" % index


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lib", nargs="?")
    ap.add_argument("--symbols", help="纯文本符号表，每行一个")
    ap.add_argument("--dll", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--exclude", default="", help="逗号分隔：已有手写实现的导出名，不生成桩")
    args = ap.parse_args()

    if args.symbols:
        with open(args.symbols, "r", encoding="utf-8") as f:
            raw = [line.strip() for line in f if line.strip()]
    elif args.lib:
        raw = read_coff_archive(args.lib)
    else:
        raise SystemExit("需要 <file.lib> 或 --symbols")

    syms = clean(raw)
    if not syms:
        raise SystemExit("符号为空")
    excluded = {s.strip() for s in args.exclude.split(",") if s.strip()}
    if excluded:
        before = len(syms)
        syms = [s for s in syms if s not in excluded]
        print("排除 %d 个已实现导出，剩 %d" % (before - len(syms), len(syms)))

    os.makedirs(args.out, exist_ok=True)
    base = os.path.join(args.out, args.dll)

    with open(base + ".def", "w", encoding="utf-8") as f:
        f.write("LIBRARY %s\nEXPORTS\n" % args.dll)
        for i, sym in enumerate(syms):
            f.write("  %s=%s\n" % (sym, ident(sym, i)))

    # 供与其他手写导出合并用的片段（只含 名称=内部符号 行）
    with open(base + "_exports.inc", "w", encoding="utf-8") as f:
        for i, sym in enumerate(syms):
            f.write("%s=%s\n" % (sym, ident(sym, i)))

    with open(base + "_stubs.cpp", "w", encoding="utf-8") as f:
        f.write('// 自动生成 —— 不要手改。重新运行 gen_stubs.py 覆盖。\n')
        f.write('#include <cstdint>\n\n#include "xbgtasix/trace.h"\n\n')
        f.write('extern "C" {\n')
        for i, sym in enumerate(syms):
            label = sym.replace('"', '\\"')
            # 返回类型必须给一个"安全默认值"，不能是 void：
            # 调用方会读 EAX/RAX，void 桩给的是栈垃圾 -> 不崩溃但行为随机。
            # 返回 0 的含义按真实类型看是：HRESULT=S_OK / bool=false / 指针=NULL，
            # 这三种都不会让调用方拿着野指针去解引用。
            f.write('uintptr_t %s(void) { XB_UNIMPL("%s"); return 0; }\n'
                    % (ident(sym, i), label))
        f.write('}  // extern "C"\n')

    print("%s: %d 个导出 -> %s.def / %s_stubs.cpp" % (args.dll, len(syms), base, base))


if __name__ == "__main__":
    main()
