#!/usr/bin/env bash
# 构建 xbgtasix（MinGW64 / git-bash）。CMake 在本机会卡在编译器探测，直接用 g++。
set -euo pipefail
cd "$(dirname "$0")"

export PATH="/d/mingw64/bin:$PATH"
CXX=${CXX:-g++}
OUT=build
# 注意：MinGW 的 g++ 不认 MSYS 风格 /d/... 路径，必须用 D:/... 或相对路径
gdk_inc_default="$(cd .. && pwd)/gdk/include_clean"
if command -v cygpath >/dev/null 2>&1; then
  gdk_inc_default=$(cygpath -m "$gdk_inc_default")
fi
GDK_INC=${GDK_INC:-$gdk_inc_default}
mkdir -p "$OUT"

FLAGS=(-std=c++20 -O2 -D_WIN32_WINNT=0x0A00 -DNOMINMAX -DWIN32_LEAN_AND_MEAN
       -Iinclude -I"$GDK_INC" -Wno-attributes -Wall -Wextra
       -static-libgcc -static-libstdc++)

# 首次构建（新克隆）时自动从 GDK 导入库生成导出桩
GEN=src/generated/xgameruntime_exports.inc
GDK_LIB_DIR="$(dirname "$GDK_INC")/lib"
if [ ! -f "$GEN" ] && [ -f "$GDK_LIB_DIR/xgameruntime.lib" ]; then
  echo "== 生成导出桩（src/generated 不存在）"
  EXCL=$(grep -v '^;' src/xgameruntime/exports.txt | grep . | paste -sd, -)
  python tools/gen_stubs.py "$(cygpath -m "$GDK_LIB_DIR/xgameruntime.lib")" \
      --dll xgameruntime --out src/generated --exclude "$EXCL"
fi

build_shim() {
  local name=$1; shift
  echo "== $name.dll"
  "$CXX" "${FLAGS[@]}" -shared "src/$name/$name.cpp" src/trace.cpp "$@" \
      -o "$OUT/$name.dll" -Wl,--out-implib,"$OUT/lib$name.dll.a"
}

# xgameruntime = 手写实现 + 从 GDK 导入库生成的 482 个"抛异常"桩
echo "== xgameruntime.dll (手写 + 生成桩)"
{
  echo "LIBRARY xgameruntime"
  echo "EXPORTS"
  cat src/xgameruntime/exports.txt
  [ -f src/generated/xgameruntime_exports.inc ] && cat src/generated/xgameruntime_exports.inc
} > "$OUT/xgameruntime.def"

"$CXX" "${FLAGS[@]}" -shared \
    src/xgameruntime/xgameruntime.cpp src/generated/xgameruntime_stubs.cpp src/trace.cpp \
    "$OUT/xgameruntime.def" \
    -o "$OUT/xgameruntime.dll" -Wl,--out-implib,"$OUT/libxgameruntime.dll.a"

build_shim xmem

# d3d12_xs = 4 个手写（带日志）+ 其余全部转发到系统 d3d12.dll
echo "== d3d12_xs.dll (手写 4 + 转发)"
{
  echo "LIBRARY d3d12_xs"
  echo "EXPORTS"
  grep -vE '^;|^LIBRARY|^EXPORTS' src/d3d12_xs/forward.def | sed 's/^[[:space:]]*//'
  cat src/d3d12_xs/exports.txt
} > "$OUT/d3d12_xs.def"

"$CXX" "${FLAGS[@]}" -shared \
    src/d3d12_xs/d3d12_xs.cpp src/trace.cpp "$OUT/d3d12_xs.def" \
    -o "$OUT/d3d12_xs.dll" -Wl,--out-implib,"$OUT/libd3d12_xs.dll.a"

echo "== shaders (dxc)"
DXC=${DXC:-/c/VulkanSDK/1.4.363.1/Bin/dxc.exe}
if [ -x "$DXC" ]; then
  for pair in "t1_triangle/triangle:VSMain:vs_6_0:PSMain:ps_6_0:triangle" \
              "t2_textured/quad:VSMain:vs_6_0:PSMain:ps_6_0:quad" \
              "t4_bindless/bindless:VSMain:vs_6_0:PSMain:ps_6_0:bindless" \
              "t5_mesh/mesh:MSMain:ms_6_5:PSMain:ps_6_0:mesh" \
              "t8_model/model:VSMain:vs_6_0:PSMain:ps_6_0:model"; do
    IFS=: read -r src vsname vsprof psname psprof out <<< "$pair"
    HLSL=$(cygpath -m "$PWD/tests/$src.hlsl")
    # 输出扩展名跟 profile 走：ms_6_5 -> .ms.cso，vs_6_0 -> .vs.cso
    vsext=${vsprof%%_*}
    "$DXC" -T "$vsprof" -E "$vsname" -O3 -Fo "$(cygpath -m "$PWD/$OUT/$out.$vsext.cso")" "$HLSL"
    "$DXC" -T "$psprof" -E "$psname" -O3 -Fo "$(cygpath -m "$PWD/$OUT/$out.ps.cso")" "$HLSL"
  done
  # t8 诊断用变体：绕开 MVP，直接看顶点数据
  HLSL8=$(cygpath -m "$PWD/tests/t8_model/model.hlsl")
  "$DXC" -T vs_6_0 -E VSMain -D DBG_VB -O3 -Fo "$(cygpath -m "$PWD/$OUT/model_dbg.vs.cso")" "$HLSL8"
  "$DXC" -T ps_6_0 -E PSMain -D DBG_VB -O3 -Fo "$(cygpath -m "$PWD/$OUT/model_dbg.ps.cso")" "$HLSL8"

  "$DXC" -T vs_6_0 -E VSMain -D DBG_HARDMVP -O3 -Fo "$(cygpath -m "$PWD/$OUT/model_hm.vs.cso")" "$HLSL8"
  "$DXC" -T ps_6_0 -E PSMain -D DBG_HARDMVP -O3 -Fo "$(cygpath -m "$PWD/$OUT/model_hm.ps.cso")" "$HLSL8"

  # t6：光追库不需要 -E，导出库里所有 entry point
  "$DXC" -T lib_6_3 -O3 -Fo "$(cygpath -m "$PWD/$OUT/rt.lib.cso")" \
      "$(cygpath -m "$PWD/tests/t6_rt/rt.hlsl")"
else
  echo "  (找不到 dxc，跳过；测试程序会因缺 .cso 退出)"
fi

echo "== t0_hello.exe"
"$CXX" "${FLAGS[@]}" tests/t0_hello/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t0_hello.exe"

echo "== t1_triangle.exe"
"$CXX" "${FLAGS[@]}" tests/t1_triangle/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t1_triangle.exe"

echo "== t2_textured.exe"
"$CXX" "${FLAGS[@]}" tests/t2_textured/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t2_textured.exe"

echo "== t4_bindless.exe"
"$CXX" "${FLAGS[@]}" tests/t4_bindless/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t4_bindless.exe"

echo "== t5_mesh.exe"
"$CXX" "${FLAGS[@]}" tests/t5_mesh/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t5_mesh.exe"

echo "== t6_rt.exe"
"$CXX" "${FLAGS[@]}" tests/t6_rt/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t6_rt.exe"

echo "== t8_model.exe"
"$CXX" "${FLAGS[@]}" tests/t8_model/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 -lole32 -lwindowscodecs \
    -o "$OUT/t8_model.exe"

echo "== t9_async.exe"
"$CXX" "${FLAGS[@]}" tests/t9_async/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t9_async.exe"

echo "== t10_integration.exe"
"$CXX" "${FLAGS[@]}" tests/t10_integration/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t10_integration.exe"

echo "== t11_storage.exe"
"$CXX" "${FLAGS[@]}" tests/t11_storage/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t11_storage.exe"

echo "== t12_sheet.exe"
"$CXX" "${FLAGS[@]}" tests/t12_sheet/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 -lole32 -lwindowscodecs \
    -o "$OUT/t12_sheet.exe"

echo "== t3_xmem.exe"
"$CXX" "${FLAGS[@]}" tests/t3_xmem/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t3_xmem.exe"

echo
echo "done: $OUT/"
ls -1 "$OUT"/*.dll "$OUT"/*.exe "$OUT"/*.cso
