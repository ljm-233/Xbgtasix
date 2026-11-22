#!/usr/bin/env python3
"""从 NuGet 拉官方 GDK 包，抽出头文件与导入库。

为什么走 NuGet：微软在 nuget.org 上公开发布 `Microsoft.GDK.Windows` / `Microsoft.GDK.Core`，
版本号和本地 GDK 安装一致（本仓库用 2610.0.9661）。这样：
  - 不需要我们转发微软的二进制（LICENSE 问题）
  - CI 可以自取，不需要仓库里放 GDK
  - 版本可锁定，构建可复现

用法：
    python tools/fetch_gdk.py                      # 默认版本 -> build/gdk
    python tools/fetch_gdk.py --version 2610.0.9661 --out ../gdk
    python tools/fetch_gdk.py --check              # 只检查是否已就绪

产物布局（与 build.sh 期望一致）：
    <out>/include_clean/*.h
    <out>/lib/xgameruntime.lib
"""
import argparse
import io
import os
import shutil
import sys
import urllib.request
import zipfile

DEFAULT_VERSION = "2610.0.9661"
PACKAGES = ("microsoft.gdk.windows", "microsoft.gdk.core")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def url_for(pkg, version):
    return ("https://api.nuget.org/v3-flatcontainer/%s/%s/%s.%s.nupkg"
            % (pkg, version, pkg, version))


def ready(out):
    inc = os.path.join(out, "include_clean")
    lib = os.path.join(out, "lib", "xgameruntime.lib")
    if not (os.path.isfile(os.path.join(inc, "XTaskQueue.h")) and os.path.isfile(lib)):
        return False
    # 库里只有 xgameruntime.lib 不够：.def 生成需要它，但 xmem/d3d12_xs 只用头文件
    return True


def fetch(pkg, version, dest):
    path = os.path.join(dest, "%s.%s.nupkg" % (pkg, version))
    if os.path.isfile(path) and os.path.getsize(path) > 1024 * 1024:
        print("  已有缓存: %s" % os.path.basename(path))
        return path
    print("  下载: %s" % url_for(pkg, version))
    # nuget.org 对 uv 或默认 UA 都可能 403，显式给个 UA
    req = urllib.request.Request(url_for(pkg, version), headers={"User-Agent": "xbgtasix-fetch/1.0"})
    with urllib.request.urlopen(req, timeout=300) as r, open(path, "wb") as f:
        shutil.copyfileobj(r, f)
    print("  完成: %.1f MB" % (os.path.getsize(path) / 1e6))
    return path


def extract(nupkg, out):
    inc_out = os.path.join(out, "include_clean")
    lib_out = os.path.join(out, "lib")
    os.makedirs(inc_out, exist_ok=True)
    os.makedirs(lib_out, exist_ok=True)
    n_h = n_l = 0
    with zipfile.ZipFile(nupkg) as z:
        for name in z.namelist():
            low = name.lower()
            if low.endswith(".h") and "/include/" in low:
                with z.open(name) as src, open(os.path.join(inc_out, os.path.basename(name)), "wb") as dst:
                    shutil.copyfileobj(src, dst)
                n_h += 1
            elif low.endswith(".lib") and "/lib/x64/" in low:
                base = os.path.basename(name)
                with z.open(name) as src, open(os.path.join(lib_out, base), "wb") as dst:
                    shutil.copyfileobj(src, dst)
                n_l += 1
    return n_h, n_l


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default=os.environ.get("GDK_VERSION", DEFAULT_VERSION))
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "gdk"))
    ap.add_argument("--check", action="store_true", help="只检查是否已就绪，不下载")
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    out = os.path.abspath(a.out)
    if ready(out):
        print("GDK 已就绪: %s" % out)
        return 0
    if a.check:
        print("GDK 未就绪: %s" % out)
        return 1

    os.makedirs(out, exist_ok=True)
    cache = os.path.join(out, "_pkg")
    os.makedirs(cache, exist_ok=True)
    total_h = total_l = 0
    for pkg in PACKAGES:
        print("== %s %s" % (pkg, a.version))
        try:
            path = fetch(pkg, a.version, cache)
        except Exception as e:
            print("  下载失败: %s" % e)
            if pkg == PACKAGES[0]:
                return 1
            continue
        h, l = extract(path, out)
        total_h += h
        total_l += l
        print("  抽出 %d 个头文件、%d 个导入库" % (h, l))

    ok = ready(out)
    print("\n结果: %d 头 + %d lib -> %s" % (total_h, total_l, out))
    if not ok:
        print("仍然不完整：缺少 XTaskQueue.h 或 lib/xgameruntime.lib")
        return 1
    print("可以直接用：GDK_INC=%s ./build.sh" % os.path.join(out, "include_clean"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
