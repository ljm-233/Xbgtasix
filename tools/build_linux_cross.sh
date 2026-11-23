#!/usr/bin/env bash
# build_linux_cross.sh —— 在 Linux（或任何有 mingw-w64 的机器）上交叉编译三个 shim
#
# 为什么这样就能"支持 Arch Linux"：产物是 **Windows PE DLL**，
# Arch 上通过 Wine/Proton 加载。不需要把代码移植成 ELF，
# 也就不需要处理 SysV ↔ Microsoft x64 的调用约定差异（那是 Wine 的活）。
#
# 用法：
#   ./tools/build_linux_cross.sh                 # 产物在 build/linux/
#   CXX=x86_64-w64-mingw32-g++ ./tools/build_linux_cross.sh
#   GDK_INC=/opt/gdk/include_clean ./tools/build_linux_cross.sh
#
# 前置：
#   Arch:    sudo pacman -S mingw-w64-gcc python
#   Debian:  sudo apt install g++-mingw-w64-x86-64 python3
#
# 不做什么：不编 shader（需要 dxc）、不编测试 exe（测试本身也是 PE，
# 但只有 shim 是"必须交叉构建"的部分；测试可在 Wine 下直接跑 Windows 版）。
set -uo pipefail
cd "$(dirname "$0")/.."

CXX=${CXX:-x86_64-w64-mingw32-g++}
OUT=${OUT:-build/linux}
GDK_INC=${GDK_INC:-"../gdk/include_clean"}

command -v "$CXX" >/dev/null || {
  echo "找不到交叉编译器: $CXX"
  echo "  Arch:   sudo pacman -S mingw-w64-gcc"
  echo "  Debian: sudo apt install g++-mingw-w64-x86-64"
  exit 1
}
[ -f "$GDK_INC/XTaskQueue.h" ] || {
  echo "找不到 GDK 头文件: $GDK_INC"
  echo "  先跑: python tools/fetch_gdk.py --out ../gdk"
  exit 1
}

mkdir -p "$OUT"
FLAGS=(-std=c++20 -O2 -D_WIN32_WINNT=0x0A00 -DNOMINMAX -DWIN32_LEAN_AND_MEAN
       -Iinclude -I"$GDK_INC" -static)
fail=0

echo "== 编译 xmem.dll"
"$CXX" "${FLAGS[@]}" -shared src/xmem/xmem.cpp src/trace.cpp \
    -o "$OUT/xmem.dll" -Wl,--out-implib,"$OUT/libxmem.dll.a" || fail=1

echo "== 编译 d3d12_xs.dll"
"$CXX" "${FLAGS[@]}" -shared src/d3d12_xs/d3d12_xs.cpp src/trace.cpp \
    src/d3d12_xs/forward.def -ldxgi -ldxguid \
    -o "$OUT/d3d12_xs.dll" -Wl,--out-implib,"$OUT/libd3d12_xs.dll.a" || fail=1

echo "== 编译 xgameruntime.dll（手写 + 生成桩 + GUID 数据）"
GDK_LIB=$(dirname "$GDK_INC")/lib/xgameruntime.lib
if [ ! -f "$GDK_LIB" ]; then
  echo "  缺少 $GDK_LIB（fetch_gdk.py 会一起解出来）"
  fail=1
else
  if [ ! -f src/generated/xgameruntime_exports.inc ]; then
    EXCL=$(cat src/xgameruntime/exports.txt src/xgameruntime/exports_*.txt 2>/dev/null \
           | grep -v '^;' | grep . | paste -sd, -)
    PYTHONIOENCODING=utf-8 python tools/gen_stubs.py "$GDK_LIB" --dll xgameruntime \
        --out src/generated --exclude "$EXCL" || fail=1
  fi
  PYTHONIOENCODING=utf-8 python tools/gen_guids.py "$GDK_LIB" \
      src/generated/xgameruntime_guids.cpp src/generated/xgameruntime_guids.inc || fail=1
  {
    echo "LIBRARY xgameruntime"
    echo "EXPORTS"
    cat src/xgameruntime/exports.txt
    cat src/xgameruntime/exports_*.txt 2>/dev/null || true
    cat src/generated/xgameruntime_exports.inc
    cat src/generated/xgameruntime_guids.inc 2>/dev/null || true
  } > "$OUT/xgameruntime.def"
  "$CXX" "${FLAGS[@]}" -shared \
      src/xgameruntime/*.cpp src/generated/xgameruntime_stubs.cpp \
      src/generated/xgameruntime_guids.cpp src/trace.cpp "$OUT/xgameruntime.def" \
      -lole32 -loleaut32 -loleacc -lsapi \
      -o "$OUT/xgameruntime.dll" -Wl,--out-implib,"$OUT/libxgameruntime.dll.a" || fail=1
fi

echo
echo "== 产物"
ls -la "$OUT"/*.dll 2>/dev/null | awk '{printf "  %.2f MB  %s\n", $5/1048576, $9}'
echo
echo "== 运行时依赖（应当只有 KERNEL32 / ole32 / SHELL32 / dxgi 这类 Wine 都有的）"
for d in "$OUT"/*.dll; do
  [ -f "$d" ] || continue
  printf "  %-18s " "$(basename "$d")"
  "${CXX%g++}objdump" -p "$d" 2>/dev/null | sed -n '/DLL Name/p' \
      | sed 's/.*DLL Name: //' | grep -v "api-ms-win" | tr '\n' ' '
  echo
done

if [ "$fail" -ne 0 ]; then
  echo "有步骤失败"; exit 1
fi
echo
echo "OK：三个 shim 交叉编译完成 -> $OUT"
echo "Arch 上使用：把这三个 DLL 放到 exe 同目录，用 Wine/Proton 启动（见 docs/arch-linux.md）"
