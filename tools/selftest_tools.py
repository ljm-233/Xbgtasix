#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Python 工具自检：不依赖任何游戏文件，用合成样本验证工具链本身。

CI 的门槛用这个 —— 因为完整构建需要微软 GDK 头文件（不能入库），
而 Python 工具可以完全独立验证。

用法：
    python tools/selftest_tools.py
"""
import json
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PY = sys.executable

fail = 0


def ok(cond, what, detail=""):
    global fail
    if cond:
        print("  PASS  %s %s" % (what, detail))
    else:
        print("  FAIL  %s %s" % (what, detail))
        fail += 1


def make_fake_rpf(path, payloads):
    """造一个 RPF9 外壳：64 字节头 + 目录区 + 4KB 对齐 + zlib 载荷。

    不是真的能装的 RPF9（目录表本来就是加密的），只用来验证"工具能否在
    长这样的一坨字节里找到并解出载荷"。
    """
    header = bytearray(64)
    struct.pack_into("<I", header, 0, 0x52504639)      # magic "9FPR"
    data_off = 0x1000
    struct.pack_into("<I", header, 4, data_off)         # data_offset
    struct.pack_into("<I", header, 0x0C, 64)            # names_len
    struct.pack_into("<I", header, 0x24, len(payloads))  # entry_count

    blob = bytearray(header)
    while len(blob) < data_off:
        blob.append(0)
    for raw in payloads:
        blob += zlib.compress(raw, 6)
        while len(blob) % 4096:
            blob.append(0)
    with open(path, "wb") as f:
        f.write(blob)


def run(args, expect_rc=0):
    # 子进程输出统一按 UTF-8 解码，避免中文在 cp936 控制台被拆成乱码后断言失败
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    p = subprocess.run([PY] + args, cwd=ROOT, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", env=env)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    print("== 合成样本")
    xml1 = b'<?xml version="1.0"?><CDataFileMgr><id>selftest_one</id></CDataFileMgr>'
    xml2 = (b'\xef\xbb\xbf<?xml version="1.0"?><Root><Item><filename>'
            b'$(common)/movies/selftest.bk2</filename></Item></Root>')
    binblob = bytes(range(256)) * 8
    # 不用系统 TEMP（某些受限环境不可写），放到仓库的 build/ 下
    base = os.path.join(ROOT, "build")
    os.makedirs(base, exist_ok=True)
    # 注意：不用 tempfile.mkdtemp —— 它在某些受限环境里创建出来的目录不可写
    # （实测 WinError 5），改成自己 makedirs
    tmp = os.path.join(base, "selftest_tmp")
    os.makedirs(tmp, exist_ok=True)
    try:
            fake = os.path.join(tmp, "fake.rpf")
            make_fake_rpf(fake, [xml1, xml2, binblob])
            ok(os.path.getsize(fake) > 0x2000, "合成 RPF9", "%.1f KB" % (os.path.getsize(fake) / 1024))

            print("\n== rpf9_probe")
            rc, out = run(["tools/packaging/rpf9_probe.py", fake, "--limit-mb", "1"])
            ok(rc == 0, "退出码 0")
            ok("0x52504639" in out, "识别 magic")
            ok("data_offset=" in out or "f1=" in out, "打印头字段")

            print("\n== rpf_carve")
            dump = os.path.join(tmp, "dump")
            rc, out = run(["tools/packaging/rpf_carve.py", fake, "--dump-dir", dump])
            ok(rc == 0, "退出码 0")
            ok("XML" in out or "文本" in out, "识别出文本载荷", out.strip().splitlines()[-3]
               if out.strip() else "")
            files = os.listdir(dump) if os.path.isdir(dump) else []
            ok(len(files) >= 2, "导出载荷文件", "%d 个" % len(files))
            if files:
                # 按内容判断，不猜文件名（分类标签可能不准，内容不会骗人）
                xmls = []
                for fn in files:
                    d = open(os.path.join(dump, fn), "rb").read()
                    body = d.lstrip(b"\xef\xbb\xbf").lstrip()
                    if body.startswith(b"<?xml") or body.startswith(b"<"):
                        xmls.append(fn)
                ok(len(xmls) >= 1, "导出物里确有 XML", "共 %d 个，其中 XML %d 个" % (len(files), len(xmls)))

            print("\n== shader_stats（合成索引）")
            idx = os.path.join(tmp, "shaders_index.csv")
            with open(idx, "w", encoding="utf-8") as f:
                f.write("index,stage,pso_hash,code_off,code_size,insns,bindings,has_rt,has_ngg,numthreads\n")
                f.write('0,CS,0x1,0x0,384,31,4,0,0,8x8x1\n')
                f.write('1,PS,0x2,0x200,512,44,14,0,0,\n')
                f.write('2,NGG,0x3,0x400,576,59,98,0,1,\n')
                f.write('3,RT,0x4,0x600,128,12,6,1,0,\n')
            rc, out = run(["tools/packaging/shader_stats.py", idx])
            ok(rc == 0, "退出码 0")
            ok("4" in out and "NGG" in out, "统计出阶段分布")
            ok("98" in out, "识别最大绑定槽 98")

            print("\n== match_texture（无匹配也要正常退出）")
            texdir = os.path.join(tmp, "tex")
            os.makedirs(texdir)
            open(os.path.join(texdir, "unrelated_thing.png"), "wb").write(b"\x89PNG")
            rc, out = run(["tools/match_texture.py", "pantscargo_mr1_002.obj", texdir])
            ok(rc == 0, "退出码 0（无匹配不是错误）")
            ok("没有名字相关" in out, "明确报告无匹配")

            print("\n== analyze_exe（用合成 PE 头验证不崩）")
            pe = bytearray(0x400)
            pe[0:2] = b"MZ"
            struct.pack_into("<I", pe, 0x3C, 0x40)
            pe[0x40:0x44] = b"PE\0\0"
            struct.pack_into("<HH", pe, 0x44, 0x8664, 1)     # machine, sections
            struct.pack_into("<H", pe, 0x54, 0xF0)          # optional header size
            struct.pack_into("<H", pe, 0x58, 0x20B)         # PE32+
            pe_path = os.path.join(tmp, "fake.exe")
            open(pe_path, "wb").write(pe)
            rc, out = run(["tools/analyze_exe.py", pe_path])
            ok(rc == 0, "退出码 0")
            ok("架构: x64" in out, "识别架构")
            ok("导入模块: 0" in out, "无导入也能报结果")

    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    print("\n== 大小写别名（Linux 必需；本机 NTFS 看不到真行为，用注入验证逻辑）")
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "fg", os.path.join(ROOT, "tools", "fetch_gdk.py"))
        fg = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fg)

        d2 = os.path.join(base, "casemock")
        os.makedirs(d2, exist_ok=True)
        open(os.path.join(d2, "grdk.h"), "w").write("// lower\n")
        # 真实的 GDK 头用的是尖括号形式
        open(os.path.join(d2, "XGameRuntime.h"), "w").write('#include <GRDK.h>\n')

        created = []
        real_symlink = os.symlink
        os.symlink = lambda src, dst, **k: created.append((src, os.path.basename(dst)))
        try:
            n = fg.case_alias_fix(d2)
        finally:
            os.symlink = real_symlink
        ok(n == 1 and created and created[0][1] == "GRDK.h",
           "识别 <GRDK.h> 并想创建 GRDK.h 别名", "created=%s" % created)
    except Exception as e:
        ok(False, "别名逻辑验证", str(e)[:60])

    print("\n== 结果: %s" % ("PASS" if fail == 0 else "FAIL（%d 项）" % fail))
    return 0 if fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
