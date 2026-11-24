#!/usr/bin/env bash
# 构建 xbgtasix（MinGW64 / git-bash）。CMake 在本机会卡在编译器探测，直接用 g++。
set -euo pipefail
cd "$(dirname "$0")"

export PATH="/d/mingw64/bin:$PATH"
# 生成脚本会打中文；CI 的 Windows 控制台是 CP1252，不设这个会 UnicodeEncodeError
export PYTHONIOENCODING=utf-8
# 优先 python3：有些 Windows 机器只有 python3，而 "python" 可能命中应用商店占位符
PY=$(command -v python3 || command -v python || echo python)
CXX=${CXX:-g++}
# 断言工具链目标是 x86_64-w64-mingw32：落到 32 位或别的 CRT 变体时，
# 构建"成功"也会产出不能用的产物（历史上踩过 libwinpthread 依赖那类问题）
if command -v "$CXX" >/dev/null 2>&1; then
  TRIPLE=$("$CXX" -dumpmachine 2>/dev/null || echo "?")
  case "$TRIPLE" in
    x86_64-w64-mingw32*) : ;;
    *) echo "错误：g++ 目标三元组是 $TRIPLE，需要 x86_64-w64-mingw32*（用 CXX= 指定）" >&2; exit 1 ;;
  esac
  echo "工具链: $TRIPLE ($("$CXX" -dumpversion 2>/dev/null))"
fi
OUT=build
# 注意：MinGW 的 g++ 不认 MSYS 风格 /d/... 路径，必须用 D:/... 或相对路径
gdk_inc_default="$(cd .. && pwd)/gdk/include_clean"
if command -v cygpath >/dev/null 2>&1; then
  gdk_inc_default=$(cygpath -m "$gdk_inc_default")
fi
GDK_INC=${GDK_INC:-$gdk_inc_default}
# 外部传进来的 MSYS 风格路径（GDK_INC=/d/gdk/include_clean）MinGW 的 g++ 认不了，
# 结果就是"缺 XTaskQueue.h"这种莫名其妙的报错。这里统一转成 D:/... 形式。
case "$GDK_INC" in
  /[A-Za-z]/*)
    if command -v cygpath >/dev/null 2>&1; then
      GDK_INC=$(cygpath -m "$GDK_INC")
    fi
    ;;
esac
mkdir -p "$OUT"

# shim 会与别人的 exe/DLL 一起加载，必须自包含：静态链接 MinGW 运行库，
# 否则真实程序加载我们的 DLL 时会因缺 libwinpthread-1.dll / libgcc 而失败（实测踩过）。
LDFLAGS_STATIC=(-static)
# 测试 exe 不能整体 -static：它们要链我们自己的导入库（libxmem.dll.a 等），
# 而 -static 让 ld 只找 .a、不认导入库 -> "cannot find -lxmem"。
# 这里只要静态 C++ 运行库就够，DLL 仍走导入库。
LDFLAGS_TESTS=(-static-libgcc -static-libstdc++)

FLAGS=(-std=c++20 -O2 -D_WIN32_WINNT=0x0A00 -DNOMINMAX -DWIN32_LEAN_AND_MEAN
       -Iinclude -I"$GDK_INC" -Wno-attributes -Wall -Wextra
       -static-libgcc -static-libstdc++)

# ---------------------------------------------------------------- 断言小工具
# .def 里出现同名导出时 ld 不报警、**后写的赢**：生成的桩会静默盖掉手写实现。
# 注意必须剥掉行首空白再比：生成的 .inc 条目是缩进的，而手写的 exports_*.txt 是顶格的，
# 只比顶格那几行正好漏掉"给 exports_sys.txt 加了名字但老桩没重新生成"这种真实故障。
check_def_dups() {
  local def=$1 dups
  dups=$(sed -E 's/^[[:space:]]+//' "$def" \
         | grep -E '^[A-Za-z_]' \
         | grep -vE '^(LIBRARY|EXPORTS)([[:space:]]|$)' \
         | sed 's/[[:space:]].*//' \
         | sort | uniq -d || true)
  if [ -n "$dups" ]; then
    echo "错误：$def 里有重名导出（ld 不报警，后写的那条会覆盖前面的）：" >&2
    printf '%s\n' "$dups" | sed 's/^/  /' >&2
    exit 1
  fi
}

# 数 .def 里真正的导出条目（剥缩进、跳过注释和 LIBRARY/EXPORTS 关键字）
count_def_exports() {
  sed -E 's/^[[:space:]]+//' "$1" \
    | grep -E '^[A-Za-z_]' \
    | grep -vE '^(LIBRARY|EXPORTS)([[:space:]]|$)' \
    | wc -l
}

# 数 PE 里真实导出的名字数：用来和 .def 对账，抓"导出被静默丢弃"
count_pe_exports() {
  objdump -p "$1" 2>/dev/null \
    | sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p' \
    | grep -cE '^[[:space:]]*\[[[:space:]]*[0-9]+\]' || true
}

# dxc 输出一律进日志：既避免 `| head -N` 在 pipefail 下因 EPIPE 把成功构建判成失败，
# 也避免把真正的编译报错截断到 2 行。
dxc_run() {
  if ! "$DXC" "$@" > "$DXC_LOG" 2>&1; then
    echo "错误：dxc 失败（参数：$*），日志末 20 行：" >&2
    tail -20 "$DXC_LOG" >&2 || true
    exit 1
  fi
}

# 首次构建（新克隆）时自动从 GDK 导入库生成导出桩
GEN=src/generated/xgameruntime_exports.inc
GDK_LIB_DIR="$(dirname "$GDK_INC")/lib"
if [ ! -f "$GEN" ] && [ -f "$GDK_LIB_DIR/xgameruntime.lib" ]; then
  echo "== 生成导出桩（src/generated 不存在）"
  EXCL=$(cat src/xgameruntime/exports.txt src/xgameruntime/exports_*.txt 2>/dev/null \
         | grep -v '^;' | grep . | paste -sd, -)
  "$PY" tools/gen_stubs.py "$(cygpath -m "$GDK_LIB_DIR/xgameruntime.lib")" \
      --dll xgameruntime --out src/generated --exclude "$EXCL"
fi

build_shim() {
  local name=$1; shift
  echo "== $name.dll"
  "$CXX" "${FLAGS[@]}" "${LDFLAGS_STATIC[@]}" -shared "src/$name/$name.cpp" src/trace.cpp "$@" \
      -o "$OUT/$name.dll" -Wl,--out-implib,"$OUT/lib$name.dll.a"
}

# xgameruntime = 手写实现 + 从 GDK 导入库生成的 482 个"抛异常"桩
echo "== xgameruntime.dll (手写 + 生成桩)"
# _GUID_xxx 是数据导出，必须生成真 GUID 而不是函数桩
if [ -f "$GDK_LIB_DIR/xgameruntime.lib" ]; then
  "$PY" tools/gen_guids.py "$(cygpath -m "$GDK_LIB_DIR/xgameruntime.lib")" \
      src/generated/xgameruntime_guids.cpp src/generated/xgameruntime_guids.inc
fi

# gen_guids.py 扫不到 GUID 时是"静默成功"（0 条也算写完文件）：50 个 _GUID_* 数据导出
# 会整体消失，DLL 照样编得过，QueryInterface / IsEqualGUID 全线失效且没有报错。
# 所以这里写死下限当断言，不依赖脚本自己报告。
GUID_MIN=50
if [ ! -f src/generated/xgameruntime_guids.inc ]; then
  echo "错误：缺少 src/generated/xgameruntime_guids.inc（.def 会少 50 个 GUID 数据导出）" >&2
  echo "      xgameruntime.lib 不可用？先跑 tools/fetch_gdk.py，或用 GDK_INC 指定 GDK。" >&2
  exit 1
fi
GUID_N=$(grep -cE '^[[:space:]]*_GUID_[0-9a-fA-F_]+[[:space:]]' src/generated/xgameruntime_guids.inc || true)
if [ "${GUID_N:-0}" -lt "$GUID_MIN" ]; then
  echo "错误：xgameruntime_guids.inc 只有 ${GUID_N:-0} 条 GUID，少于下限 $GUID_MIN 条" >&2
  echo "      这会让 _GUID_* 数据导出整体缺失（QueryInterface/IsEqualGUID 全部失效）。" >&2
  exit 1
fi
echo "  GUID 数据导出：$GUID_N 条（下限 $GUID_MIN）"

# exports*.txt 缺失以前被 `2>/dev/null || true` 吞掉：.def 会少 180 个导出，构建却照样成功。
# 现在缺任何一个、或是空文件，都直接失败。
for f in src/xgameruntime/exports*.txt; do
  if [ ! -f "$f" ]; then
    echo "错误：缺少导出清单 $f（.def 会少导出，构建不该继续）" >&2
    exit 1
  fi
  if [ ! -s "$f" ]; then
    echo "错误：导出清单 $f 是空文件（.def 会少导出）" >&2
    exit 1
  fi
done
# 只删掉其中一个时 glob 只会少一项、上面循环照样通过，所以再钉一个文件数下限。
# 目前应有 9 个：exports.txt + exports_{input,pkg,pkg2,save,speech,stream,sys,ui}.txt
EXPORT_TXT_MIN=9
EXPORT_TXT_N=$(ls -1 src/xgameruntime/exports*.txt 2>/dev/null | wc -l)
if [ "$EXPORT_TXT_N" -lt "$EXPORT_TXT_MIN" ]; then
  echo "错误：只找到 $EXPORT_TXT_N 个 src/xgameruntime/exports*.txt，应有 $EXPORT_TXT_MIN 个" >&2
  ls -1 src/xgameruntime/exports*.txt 2>/dev/null | sed 's/^/  /' >&2 || true
  exit 1
fi

{
  echo "LIBRARY xgameruntime"
  echo "EXPORTS"
  cat src/xgameruntime/exports.txt
  # 上面已经逐个断言存在，这里不再 2>/dev/null || true 掩盖失败
  cat src/xgameruntime/exports_*.txt
  if [ -f src/generated/xgameruntime_exports.inc ]; then
    cat src/generated/xgameruntime_exports.inc
  else
    echo "警告：缺少 generated/xgameruntime_exports.inc，.def 会缺导出" >&2
  fi
  if [ -f src/generated/xgameruntime_guids.inc ]; then
    cat src/generated/xgameruntime_guids.inc
  fi
} > "$OUT/xgameruntime.def"

# 生成后立刻查重名：同名导出 ld 不报警、后写的赢 → 桩会静默覆盖真实现
check_def_dups "$OUT/xgameruntime.def"
# 导出条目下限：实测 461 条（ci.yml 的 linux-cross 校验用的也是 461）。
# 写 455 留出余量，只用来抓"整批导出消失"这种量级的故障。
DEF_MIN=455
DEF_N=$(count_def_exports "$OUT/xgameruntime.def")
if [ "$DEF_N" -lt "$DEF_MIN" ]; then
  echo "错误：$OUT/xgameruntime.def 只有 $DEF_N 个导出条目，少于下限 $DEF_MIN" >&2
  echo "      检查 src/xgameruntime/exports*.txt 与 src/generated/xgameruntime_exports.inc。" >&2
  exit 1
fi
echo "  .def 导出条目：$DEF_N 个"

"$CXX" "${FLAGS[@]}" "${LDFLAGS_STATIC[@]}" -shared \
    src/xgameruntime/*.cpp src/generated/xgameruntime_stubs.cpp \
    src/generated/xgameruntime_guids.cpp \
    src/trace.cpp \
    "$OUT/xgameruntime.def" \
    -ld3d12 -ldxgi -ldxguid -lole32 -loleaut32 -loleacc -lsapi \
    -o "$OUT/xgameruntime.dll" -Wl,--out-implib,"$OUT/libxgameruntime.dll.a"

# 对账：.def 声明的导出数必须等于 PE 里真实导出的名字数，否则就是"导出被静默丢弃"
if command -v objdump >/dev/null 2>&1; then
  PE_N=$(count_pe_exports "$OUT/xgameruntime.dll")
  if [ "${PE_N:-0}" -ne "$DEF_N" ]; then
    echo "错误：xgameruntime.dll 实际导出 $PE_N 个，.def 声明 $DEF_N 个（不一致）" >&2
    exit 1
  fi
  echo "  导出对账：.def $DEF_N / PE $PE_N  OK"
else
  echo "  警告：没有 objdump，跳过导出数对账（只保留 .def 下限断言）" >&2
fi

echo "== xmem.dll（含 fstrace：静态导入的 DllMain 能最早接管文件 API / Sleep）"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_STATIC[@]}" -shared \
    src/xmem/xmem.cpp src/fstrace/fstrace.cpp src/fstrace/drives.cpp \
    src/host_stall/host_stall.cpp src/trace.cpp \
    -o "$OUT/xmem.dll" -Wl,--out-implib,"$OUT/libxmem.dll.a"

# 五个主机硬件抽象层 DLL：每个模块一份源文件（各自只导出自己那组符号）。
# 需求来源：目标 exe 的导入表 —— 这些 DLL 在真实主机上来自系统分区，游戏包里没有。
shim_source() {
  case "$1" in
    AcpHal)   echo src/console_hw/acphal.cpp ;;
    xapu)     echo src/console_hw/xapu.cpp ;;
    chad)     echo src/console_hw/chad.cpp ;;
    xg_xs)    echo src/console_hw/xg_xs.cpp ;;
    XCRDAPI)  echo src/console_hw/xcrdapi.cpp ;;
  esac
}
for m in AcpHal xapu chad xg_xs XCRDAPI; do
  SRC=$(shim_source "$m")
  if [ ! -f "$SRC" ]; then
    echo "错误：缺少 $SRC（$m.dll 的源文件）" >&2
    exit 1
  fi
  echo "== $m.dll ($SRC)"
  # XCRDAPI 要自己开文件（IDStorageFactoryX::OpenFile 拿到的就是游戏的 G:\... 路径），
  # 而 xmem 的 IAT 钩子管不到别的 DLL，所以它的路径重写表也编进来（drives.cpp）。
  EXTRA=()
  [ "$m" = "XCRDAPI" ] && EXTRA=(src/fstrace/drives.cpp)
  "$CXX" "${FLAGS[@]}" "${LDFLAGS_STATIC[@]}" -shared \
      "$SRC" src/trace.cpp "${EXTRA[@]}" \
      -o "$OUT/$m.dll" -Wl,--out-implib,"$OUT/lib$m.dll.a" || exit 1
done

# d3d12_xs = 4 个手写（带日志）+ 其余全部转发到系统 d3d12.dll
echo "== d3d12_xs.dll (手写 4 + 转发)"
{
  echo "LIBRARY d3d12_xs"
  echo "EXPORTS"
  grep -vE '^;|^LIBRARY|^EXPORTS' src/d3d12_xs/forward.def | sed 's/^[[:space:]]*//'
  cat src/d3d12_xs/exports.txt
} > "$OUT/d3d12_xs.def"

# 同样查重名：forward.def 是自动生成的，手写 exports.txt 里再加同名就会出现"后写的赢"
check_def_dups "$OUT/d3d12_xs.def"

"$CXX" "${FLAGS[@]}" "${LDFLAGS_STATIC[@]}" -shared \
    src/d3d12_xs/d3d12_xs.cpp src/trace.cpp "$OUT/d3d12_xs.def" \
    -ldxgi -ldxguid \
    -o "$OUT/d3d12_xs.dll" -Wl,--out-implib,"$OUT/libd3d12_xs.dll.a"

D3D_N=$(count_def_exports "$OUT/d3d12_xs.def")
if command -v objdump >/dev/null 2>&1; then
  D3D_PE=$(count_pe_exports "$OUT/d3d12_xs.dll")
  if [ "${D3D_PE:-0}" -ne "$D3D_N" ]; then
    echo "错误：d3d12_xs.dll 实际导出 $D3D_PE 个，.def 声明 $D3D_N 个（不一致）" >&2
    exit 1
  fi
  echo "  d3d12_xs 导出对账：.def $D3D_N / PE $D3D_PE  OK"
fi

echo "== shaders (dxc)"
# 以前用 `[ -x "$DXC" ]` 判断：对裸名字 `dxc`（PATH 里找到的）恒为假，于是"缺 dxc"
# 被当成"跳过着色器"，改了 HLSL 也验的是旧 .cso。统一改用 command -v。
SKIP_SHADERS=${SKIP_SHADERS:-0}
DXC=${DXC:-/c/VulkanSDK/1.4.363.1/Bin/dxc.exe}
HAVE_DXC=0
if command -v "$DXC" >/dev/null 2>&1; then
  HAVE_DXC=1
  DXC=$(command -v "$DXC")   # 裸名字解析成真路径，后面日志里能看清用的是哪个
else
  # 默认路径里写死了 SDK 版本号，choco 装的 Vulkan SDK 版本会变：兜底扫一遍已装的 SDK
  for cand in /c/VulkanSDK/*/Bin/dxc.exe; do
    if [ -f "$cand" ]; then
      HAVE_DXC=1
      DXC=$cand
    fi
  done
fi
DXC_LOG="$OUT/.dxc.log"
if [ "$SKIP_SHADERS" = "1" ]; then
  echo "  警告：SKIP_SHADERS=1，显式跳过着色器编译——$OUT 里的 .cso 可能与 HLSL 不符" >&2
elif [ "$HAVE_DXC" -ne 1 ]; then
  echo "错误：找不到 dxc（试过 \$DXC='$DXC' 和 PATH）" >&2
  echo "      用到旧 .cso 会让测试验的是历史着色器，所以默认直接失败。" >&2
  echo "      确实要先跳过着色器：SKIP_SHADERS=1 ./build.sh" >&2
  exit 1
else
  # 旧 .cso 必须清掉：否则 dxc 中途失败/被跳过时，测试验的是上个月的产物
  rm -f "$OUT"/*.cso
  for pair in "t1_triangle/triangle:VSMain:vs_6_0:PSMain:ps_6_0:triangle" \
              "t2_textured/quad:VSMain:vs_6_0:PSMain:ps_6_0:quad" \
              "t4_bindless/bindless:VSMain:vs_6_0:PSMain:ps_6_0:bindless" \
              "t5_mesh/mesh:MSMain:ms_6_5:PSMain:ps_6_0:mesh" \
              "t8_model/model:VSMain:vs_6_0:PSMain:ps_6_0:model"; do
    IFS=: read -r src vsname vsprof psname psprof out <<< "$pair"
    HLSL=$(cygpath -m "$PWD/tests/$src.hlsl")
    # 输出扩展名跟 profile 走：ms_6_5 -> .ms.cso，vs_6_0 -> .vs.cso
    vsext=${vsprof%%_*}
    dxc_run -T "$vsprof" -E "$vsname" -O3 -Fo "$(cygpath -m "$PWD/$OUT/$out.$vsext.cso")" "$HLSL"
    dxc_run -T "$psprof" -E "$psname" -O3 -Fo "$(cygpath -m "$PWD/$OUT/$out.ps.cso")" "$HLSL"
  done
  # t8 诊断用变体：绕开 MVP，直接看顶点数据
  HLSL8=$(cygpath -m "$PWD/tests/t8_model/model.hlsl")
  dxc_run -T vs_6_0 -E VSMain -D DBG_VB -O3 -Fo "$(cygpath -m "$PWD/$OUT/model_dbg.vs.cso")" "$HLSL8"
  dxc_run -T ps_6_0 -E PSMain -D DBG_VB -O3 -Fo "$(cygpath -m "$PWD/$OUT/model_dbg.ps.cso")" "$HLSL8"

  dxc_run -T vs_6_0 -E VSMain -D DBG_HARDMVP -O3 -Fo "$(cygpath -m "$PWD/$OUT/model_hm.vs.cso")" "$HLSL8"
  dxc_run -T ps_6_0 -E PSMain -D DBG_HARDMVP -O3 -Fo "$(cygpath -m "$PWD/$OUT/model_hm.ps.cso")" "$HLSL8"

  # t6：光追库不需要 -E，导出库里所有 entry point
  dxc_run -T lib_6_3 -O3 -Fo "$(cygpath -m "$PWD/$OUT/rt.lib.cso")" \
      "$(cygpath -m "$PWD/tests/t6_rt/rt.hlsl")"
fi

echo "== t0_hello.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t0_hello/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t0_hello.exe"

echo "== t1_triangle.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t1_triangle/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t1_triangle.exe"

echo "== t2_textured.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t2_textured/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t2_textured.exe"

echo "== t4_bindless.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t4_bindless/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t4_bindless.exe"

echo "== t5_mesh.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t5_mesh/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t5_mesh.exe"

echo "== t6_rt.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t6_rt/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t6_rt.exe"

echo "== t8_model.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t8_model/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 -lole32 -lwindowscodecs \
    -o "$OUT/t8_model.exe"

echo "== t9_async.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t9_async/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t9_async.exe"

echo "== t10_integration.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t10_integration/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t10_integration.exe"

echo "== t11_storage.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t11_storage/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t11_storage.exe"

echo "== t12_sheet.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t12_sheet/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 -lole32 -lwindowscodecs \
    -o "$OUT/t12_sheet.exe"

echo "== t13_caps.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t13_caps/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t13_caps.exe"

echo "== t14_vram.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t14_vram/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t14_vram.exe"

echo "== t15_budget.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t15_budget/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t15_budget.exe"

echo "== t17_async2.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t17_async2/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t17_async2.exe"

echo "== t18_package.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t18_package/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t18_package.exe"

echo "== t19_save.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t19_save/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t19_save.exe"

echo "== t20_taskqueue2.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t20_taskqueue2/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t20_taskqueue2.exe"

echo "== t21_user.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t21_user/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t21_user.exe"

echo "== t22_extras.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t22_extras/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t22_extras.exe"

echo "== t23_pkg2.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t23_pkg2/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t23_pkg2.exe"

echo "== t24_save2.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t24_save2/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t24_save2.exe"

echo "== t25_sys.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t25_sys/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t25_sys.exe"

echo "== t26_stream.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t26_stream/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t26_stream.exe"

echo "== t27_composite.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t27_composite/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t27_composite.exe"

echo "== t28_speech.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t28_speech/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 -lole32 \
    -o "$OUT/t28_speech.exe"

echo "== t29_perf.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t29_perf/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t29_perf.exe"

echo "== t30_fakegame.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t30_fakegame/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t30_fakegame.exe"

echo "== t31_swapchain.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t31_swapchain/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t31_swapchain.exe"

echo "== t32_window.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t32_window/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t32_window.exe"

echo "== t33_memory.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t33_memory/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t33_memory.exe"

echo "== t34_residency.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t34_residency/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t34_residency.exe"

echo "== t35_gamepad.exe"
# 不链接 GameInput.lib：缺这个 DLL 的机器上进程会起不来（0xC0000135）。
# 测试内部用 LoadLibrary + GetProcAddress 运行时解析（XInput 走同一思路）。
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t35_gamepad/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 -lole32 \
    -o "$OUT/t35_gamepad.exe"

echo "== t36_ui_host.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t36_ui_host/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t36_ui_host.exe"

echo "== t37_system_ui.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t37_system_ui/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t37_system_ui.exe"

echo "== t38_xboxui（UI 着色器 + 自绘窗口）"
if [ "$HAVE_DXC" -eq 1 ] && [ "$SKIP_SHADERS" != "1" ]; then
  # ui.hlsl 的路径要转成 D:/... 才交给原生的 dxc；输出进日志，失败时看末尾而不是 `| head -2`
  dxc_run -T vs_6_0 -E VSMain "$(cygpath -m "$PWD/tests/t38_xboxui/ui.hlsl")" \
      -Fo "$(cygpath -m "$PWD/$OUT/ui.vs.cso")"
  dxc_run -T ps_6_0 -E PSMain "$(cygpath -m "$PWD/tests/t38_xboxui/ui.hlsl")" \
      -Fo "$(cygpath -m "$PWD/$OUT/ui.ps.cso")"
  "$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t38_xboxui/main.cpp src/trace.cpp \
      -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
      -ldxgi -ldxguid -ld3d12 -ld3dcompiler -lgdi32 -luser32 \
      -o "$OUT/t38_xboxui.exe"
else
  echo "  警告：没有 dxc 或 SKIP_SHADERS=1，跳过 UI 着色器（t38 运行时会缺 .cso）" >&2
fi

echo "== t40_taskqueue3.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t40_taskqueue3/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t40_taskqueue3.exe"

echo "== t41_d3d12_device_abi.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t41_d3d12_device_abi/main.cpp \
    -L"$OUT" -ld3d12_xs -ldxgi -ldxguid -o "$OUT/t41_d3d12_device_abi.exe"

# 主机侧停顿 -> guest Sleep 的记账（需 xmem.dll 的 DllMain 装 IAT 钩子）
echo "== t48_present.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" -Isrc tests/t48_present/main.cpp src/trace.cpp \
    -ldxguid -ldxgi -ld3d12 -o "$OUT/t48_present.exe"

echo "== t49_pso_warp.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t49_pso_warp/main.cpp \
    -ld3d12 -ldxgi -ldxguid -o "$OUT/t49_pso_warp.exe"

echo "== t47_pso.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" -Isrc tests/t47_pso/main.cpp \
    -L"$OUT" -ld3d12_xs -ldxguid -ldxgi -ld3d12 -o "$OUT/t47_pso.exe"

echo "== t46_rootsig.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t46_rootsig/main.cpp \
    -L"$OUT" -ld3d12_xs -ldxguid -ldxgi -ld3d12 -o "$OUT/t46_rootsig.exe"

echo "== t44_diff.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t44_diff/main.cpp \
    -L"$OUT" -ld3d12_xs -ldxguid -ldxgi -ld3d12 -o "$OUT/t44_diff.exe"

echo "== t45_census.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t45_census/main.cpp \
    -o "$OUT/t45_census.exe"

echo "== t43_host_stall.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t43_host_stall/main.cpp \
    -L"$OUT" -lxmem -o "$OUT/t43_host_stall.exe"

# 黑盒测启动器的备份/还原决策：不链接 shim，只要求 xbgtasix.exe 已经在 $OUT 里
echo "== t42_launcher_backup.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t42_launcher_backup/main.cpp \
    -o "$OUT/t42_launcher_backup.exe"

echo "== t3_xmem.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t3_xmem/main.cpp src/trace.cpp \
    -L"$OUT" -lxgameruntime -lxmem -ld3d12_xs \
    -ldxgi -ldxguid -ld3d12 \
    -o "$OUT/t3_xmem.exe"

echo "== t39 探针（给 xbgtasix.exe 的测试用）"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t39_host_tool/probes.cpp -L"$OUT" -lxgameruntime -o "$OUT/probe_ok.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" -DPROBE_UNIMPL tests/t39_host_tool/probes.cpp \
    -L"$OUT" -lxgameruntime -o "$OUT/probe_unimpl.exe"

echo "== t39_host_tool.exe（启动器的测试）"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" tests/t39_host_tool/main.cpp src/trace.cpp \
    -o "$OUT/t39_host_tool.exe"

echo "== xbgtasix.exe（启动器/诊断工具）"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_STATIC[@]}" src/host/main.cpp src/host/pe.cpp \
    -Id3d12 -ld3d12 -ldxgi -ldxguid -lwinmm \
    -o "$OUT/xbgtasix.exe"

# 重新构建会覆盖 exe，之前的签名随之失效。默认**不**签名：签名要证书/签名机，
# 而且签名失败不该把一次成功的构建判成失败。要签就显式 SIGN=1。
# （首次签名：powershell -ExecutionPolicy Bypass -File tools\sign_local.ps1）
SIGN=${SIGN:-0}
if [ "$SIGN" != "1" ]; then
  echo "== 跳过签名（默认不签；要签：SIGN=1 ./build.sh）"
elif [ ! -f "$OUT/xbgtasix.exe" ]; then
  echo "警告：SIGN=1 但没有 $OUT/xbgtasix.exe，跳过签名" >&2
elif ! command -v powershell >/dev/null 2>&1; then
  echo "警告：SIGN=1 但找不到 powershell，跳过签名" >&2
else
  SUBJ="CN=Xbgtasix Local Dev"
  if powershell -NoProfile -Command "if (Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue | Where-Object { \$_.Subject -eq '$SUBJ' }) { exit 0 } else { exit 1 }" >/dev/null 2>&1; then
    echo "== 重新签名 xbgtasix.exe（构建会覆盖旧签名）"
    SIGN_LOG="$OUT/.sign.log"
    # 输出先进日志再摘：`| grep | head -4` 在 pipefail 下会因 EPIPE 把构建判成失败，
    # 而且签名失败本来就不该中止（产物依然可用）。
    if powershell -NoProfile -ExecutionPolicy Bypass -File tools/sign_local.ps1 > "$SIGN_LOG" 2>&1; then
      grep -E "签名|状态|主体" "$SIGN_LOG" | sed -n '1,4p' || true
    else
      echo "警告：签名失败（不影响构建产物，可事后手动重签），日志末 20 行：" >&2
      tail -20 "$SIGN_LOG" >&2 || true
    fi
  else
    echo "警告：SIGN=1 但本机没有自签名证书，跳过签名" >&2
  fi
fi

echo "== 自包含自检（不许依赖 libwinpthread / libstdc++ / libgcc_s）"
# 测试 exe 以前没加 -static：37/38 个 exe 依赖 libwinpthread-1.dll，
# 拷到没装 MinGW 的机器上直接 0xC0000135。这里把"自包含"变成断言。
if ! command -v objdump >/dev/null 2>&1; then
  echo "  警告：没有 objdump，跳过自包含自检" >&2
else
  SELF_BAD=0
  SELF_N=0
  # glob 可能一个都不匹配：保留字面模式，用 -f 过滤，避免 set -e 误判
  # 只检查"要分发的东西"：三个 DLL + 启动器。测试 exe 只在开发机/CI 里跑，
# 那里本来就有 MinGW（要求它们也自包含反而不可能：它们要链我们的导入库）。
for bin in "$OUT"/xgameruntime.dll "$OUT"/xmem.dll "$OUT"/d3d12_xs.dll "$OUT"/xbgtasix.exe; do
  [ -f "$bin" ] || continue
    [ -f "$bin" ] || continue
    SELF_N=$((SELF_N + 1))
    HITS=$(objdump -p "$bin" 2>/dev/null \
           | grep -oE 'DLL Name: (libwinpthread[^ ]*|libstdc\+\+[^ ]*|libgcc_s[^ ]*)' \
           | sort -u || true)
    if [ -n "$HITS" ]; then
      echo "错误：$(basename "$bin") 依赖非静态运行库：" >&2
      printf '%s\n' "$HITS" | sed 's/^/  /' >&2
      SELF_BAD=$((SELF_BAD + 1))
    fi
  done
  if [ "$SELF_N" -eq 0 ]; then
    echo "  警告：$OUT 下没有 dll/exe，跳过自包含自检" >&2
  elif [ "$SELF_BAD" -ne 0 ]; then
    echo "错误：$SELF_BAD/$SELF_N 个产物依赖 libwinpthread/libstdc++/libgcc_s（缺 DLL 的机器上会 0xC0000135）" >&2
    exit 1
  else
    echo "  $SELF_N 个产物全部自包含"
  fi
fi

echo
echo "done: $OUT/"
# 注意：set -e 下 `ls *.cso` 在没有 shader 产出时会非零退出，把成功构建变成失败
ls -1 "$OUT"/*.dll "$OUT"/*.exe "$OUT"/*.cso 2>/dev/null || true
