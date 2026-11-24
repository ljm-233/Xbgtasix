#!/usr/bin/env python3
"""host_probe.py —— 在 Linux 这一侧回答 “`xbgtasix doctor` 在 Wine 下该报什么”。

为什么单独一个脚本：`xbgtasix.exe` 是 Windows PE，跑起来要 Wine；而“宿主是不是 Wine、
真实显卡是哪张”这两个问题的答案在 **Linux 这一侧**才拿得到（/sys、lspci、wine 版本）。
doctor 里那两段 C++ 只有在有 mingw 的机器上才编得出来，这个脚本不需要编译就能验。

用法：
  python3 tools/host_probe.py            # 宿主 + 真实 GPU
  python3 tools/host_probe.py --doctor   # 顺便跑一次 wine 下的 xbgtasix doctor，并对比适配器
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

VENDORS = {0x10DE: "NVIDIA", 0x8086: "Intel", 0x1002: "AMD", 0x13B5: "ARM"}


def read_hex(path: str) -> int | None:
    try:
        with open(path) as f:
            return int(f.read().strip(), 16)
    except (OSError, ValueError):
        return None


def pci_names() -> dict[tuple[int, int], str]:
    """从 /usr/share/hwdata 或 lspci 拿 “厂商 设备号 -> 名字”。拿不到就空表。"""
    out: dict[tuple[int, int], str] = {}
    for path in ("/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids"):
        if not os.path.exists(path):
            continue
        vend = None
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                for line in f:
                    if line.startswith("#") or not line.strip():
                        continue
                    if not line.startswith("\t"):
                        vend = int(line.split()[0], 16) if re.match(r"^[0-9a-fA-F]{4} ", line) else None
                        continue
                    m = re.match(r"^\t([0-9a-fA-F]{4})  (.+)$", line)
                    if m and vend is not None:
                        out[(vend, int(m.group(1), 16))] = m.group(2).strip()
        except OSError:
            continue
        if out:
            break
    return out


def real_gpus() -> list[dict]:
    names = pci_names()
    gpus = []
    for card in sorted(glob.glob("/sys/class/drm/card[0-9]*")):
        dev = os.path.join(card, "device")
        vendor = read_hex(os.path.join(dev, "vendor"))
        if vendor is None:
            continue
        device = read_hex(os.path.join(dev, "device")) or 0
        sub_v = read_hex(os.path.join(dev, "subsystem_vendor")) or 0
        sub_d = read_hex(os.path.join(dev, "subsystem_device")) or 0
        boot = read_hex(os.path.join(dev, "boot_vga")) or 0
        gpus.append({
            "card": os.path.basename(card),
            "vendor": vendor,
            "device": device,
            "sub": (sub_v, sub_d),
            "boot_vga": bool(boot),
            "name": names.get((vendor, device), ""),
        })
    # 同一颗芯片会出现多个 cardN（不同连接器/不同驱动实例），按 vendor:device 去重
    seen, uniq = set(), []
    for g in gpus:
        key = (g["vendor"], g["device"], g["sub"])
        if key in seen:
            continue
        seen.add(key)
        uniq.append(g)
    return uniq


def wine_version(files_dir: str) -> str:
    p = os.path.join(files_dir, "bin", "wine")
    if not os.path.exists(p):
        return ""
    try:
        r = subprocess.run([p, "--version"], capture_output=True, text=True, timeout=20)
        return (r.stdout or r.stderr).strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def doctor_gpu_lines(files_dir: str, prefix: str) -> list[str]:
    """跑一次 wine 下的 `xbgtasix doctor`，抓它报的 GPU 行（DXGI 视角）。"""
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = os.path.join(here, "build", "xbgtasix.exe")
    if not os.path.exists(exe):
        return ["（build/xbgtasix.exe 不存在，先 build.sh）"]
    env = dict(os.environ)
    env.update({
        "WINEPREFIX": prefix,
        "PATH": os.path.join(files_dir, "bin") + ":/usr/bin:/bin",
        "WINEDEBUG": "-all",
        "DISPLAY": env.get("DISPLAY", ":0"),
        "WINEDLLPATH": ":".join([
            os.path.join(files_dir, "lib/wine/dxvk/x86_64-windows"),
            os.path.join(files_dir, "lib/wine/vkd3d-proton/x86_64-windows"),
            os.path.join(files_dir, "lib/wine/x86_64-windows"),
        ]),
        "WINEDLLOVERRIDES": "dxgi=n;d3d11=n;d3d10core=n;d3d12=n;d3d12core=n",
    })
    try:
        r = subprocess.run([os.path.join(files_dir, "bin", "wine"), exe, "doctor"],
                           capture_output=True, text=True, timeout=180, env=env)
    except (OSError, subprocess.SubprocessError) as e:
        return [f"（doctor 跑不起来：{e}）"]
    out = (r.stdout or "") + (r.stderr or "")
    return [ln for ln in out.splitlines() if "GPU" in ln or "适配器" in ln or "dxgi" in ln]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--files", default="/home/link/.local/share/proton/runners/DW-Proton Latest/files",
                    help="Proton runner 的 files/ 目录")
    ap.add_argument("--prefix", default=os.path.expanduser("~/Games/gta6-x11"))
    ap.add_argument("--doctor", action="store_true", help="顺便跑 wine 下的 doctor 做对比")
    a = ap.parse_args()

    in_wine = bool(os.environ.get("WINEPREFIX") or os.environ.get("WINELOADERNOEXEC"))
    print(f"宿主          {'wine（在 Windows PE 里）' if in_wine else 'linux（Wine 之外）'}")
    wv = wine_version(a.files)
    print(f"wine          {wv or '找不到 ' + a.files + '/bin/wine'}")
    print(f"WINEPREFIX    {os.environ.get('WINEPREFIX') or a.prefix}")

    gpus = real_gpus()
    if not gpus:
        print("真实 GPU      读不到 /sys/class/drm")
    for g in gpus:
        vname = VENDORS.get(g["vendor"], "其它")
        print(f"真实 GPU      {g['card']}: {vname} {g['vendor']:04X}:{g['device']:04X}"
              f" (sub {g['sub'][0]:04X}:{g['sub'][1]:04X})"
              f"{'  [boot_vga]' if g['boot_vga'] else ''}"
              f"{'  ' + g['name'] if g['name'] else ''}")

    if a.doctor:
        print("--- wine 下 doctor 报的（DXGI/vkd3d 视角，可能是假卡）")
        for ln in doctor_gpu_lines(a.files, a.prefix):
            print(ln)
    return 0


if __name__ == "__main__":
    sys.exit(main())
