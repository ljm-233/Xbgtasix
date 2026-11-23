#!/usr/bin/env bash
# launch.sh —— 把目标程序放进"shim 环境"里跑起来，并对失败做分类
#
# 用法：
#   ./tools/launch.sh <exe 路径> [--staging 目录] [--timeout 秒] [--stop]
#
# 做的事：
#   1) 建一个 staging 目录，把 shim（xgameruntime/xmem/d3d12_xs）与目标 exe 放进去
#   2) 设好 XBGTASIX_* 环境变量（默认 log 模式，不会因为一个未实现 API 就退出）
#   3) 用超时跑起来，抓退出码 + 日志
#   4) 按退出码/日志关键字**分类失败原因**，并打印下一步该看什么
#
# 为什么不直接双击：双击拿不到退出码，也没有 staging，缺 DLL 时只会弹一个看不懂的框。
set -uo pipefail
cd "$(dirname "$0")/.."

TARGET=${1:-}
shift 2>/dev/null || true
STAGING="build/launch"
TIMEOUT=120
MODE="log"
while [ $# -gt 0 ]; do
  case "$1" in
    --staging) STAGING=$2; shift 2 ;;
    --timeout) TIMEOUT=$2; shift 2 ;;
    --stop) MODE="stop"; shift ;;
    *) echo "未知参数: $1"; exit 2 ;;
  esac
done

if [ -z "$TARGET" ] || [ ! -f "$TARGET" ]; then
  echo "用法: ./tools/launch.sh <exe> [--staging dir] [--timeout 秒] [--stop]"
  exit 2
fi

PKG=${XBGTASIX_PKG:-"D:/GTA6/GTA6.Xbox.v1.0.2.1-CYBERLEEK"}
CFG=${XBGTASIX_CONFIG:-"$PKG/MicrosoftGame.Config"}
BUILD=build

# 1) staging
rm -rf "$STAGING"
mkdir -p "$STAGING"
# cygpath -m 对相对路径原样返回：先转成绝对路径，日志与环境变量才不会指到相对位置
STAGING=$(cygpath -m "$(cd "$(dirname "$STAGING")" && pwd)/$(basename "$STAGING")")
for d in xgameruntime.dll xmem.dll d3d12_xs.dll; do
  if [ -f "$BUILD/$d" ]; then cp "$BUILD/$d" "$STAGING/"; else echo "缺少 shim: $BUILD/$d（先跑 ./build.sh）"; exit 3; fi
done
cp "$TARGET" "$STAGING/"
BASE=$(basename "$TARGET")
echo "== staging: $STAGING（shim 3 个 + $BASE）"

# 2) 环境
LOG="$STAGING/xbgtasix.log"
rm -f "$LOG"
export XBGTASIX_UNIMPL="$MODE"
export XBGTASIX_LOG="$(cygpath -m "$LOG")"
export XBGTASIX_CONFIG="$(cygpath -m "$CFG")"
export XBGTASIX_PACKAGE_ROOT="$(cygpath -m "$PKG")"
export XBGTASIX_PLS_ROOT="$(cygpath -m "$STAGING/pls")"
mkdir -p "$STAGING/pls"

echo "== 运行: $BASE（模式 XBGTASIX_UNIMPL=$MODE，超时 ${TIMEOUT}s）"
START=$(date +%s)
( cd "$STAGING" && timeout "$TIMEOUT" "./$BASE" ) 2>&1 | tail -40
RC=$?
ELAPSED=$(( $(date +%s) - START ))
echo "== 退出码: $RC（耗时 ${ELAPSED}s）"

# 3) 分类
classify() {
  case "$1" in
    0)   echo "PASS        程序正常退出";;
    70)  echo "UNIMPL      撞到未实现的 API（stop 模式）。用 --stop 之外的默认 log 模式跑可继续";;
    124) echo "TIMEOUT     超时。可能是启动卡在某个等不到的回调上";;
    193) echo "BAD_EXE     映像格式无效（不是 x64 PE，或文件损坏）";;
    *)   echo "CRASH       $1";;
  esac
}
W=$(printf '%d' "$RC" 2>/dev/null || echo 0)
if [ "$W" -gt 2147483648 ] 2>/dev/null; then W=$((W - 4294967296)); fi
HEX=$(printf '0x%08X' "$RC" 2>/dev/null || echo "?")
echo "== 分类: $(classify "$W")  原始码=$HEX"

if [ -f "$LOG" ]; then
  UNIMPL=$(grep -c "UNIMPLEMENTED" "$LOG" 2>/dev/null || echo 0)
  echo "== 日志: $(wc -l < "$LOG") 行，其中 UNIMPLEMENTED $UNIMPL 条"
  if [ "$UNIMPL" -gt 0 ]; then
    echo "-- 前 10 个未实现 API（以及调用栈首地址）--"
    grep -A1 "UNIMPLEMENTED" "$LOG" | head -20
  fi
  echo "-- 日志末尾 --"
  tail -8 "$LOG"
else
  echo "== 没有日志（说明还没进到我们的 DLL，或 exe 在加载阶段就失败）"
fi

echo
echo "== 下一步建议"
case "$W" in
  0)   echo "  能跑通：把超时调长、关掉 log 模式复跑，观察是否有功能缺失";;
  70)  echo "  看上面 UNIMPLEMENTED 列表：这些就是必须补的桩";;
  124) echo "  用 --timeout 更长 + 检查最后一条日志停在哪里（多半是等异步完成）";;
  3221225781) echo "  缺依赖 DLL（0xC0000135）：把 package 里对应 DLL 拷进 staging";;
  3221225785) echo "  缺导出（0xC0000139）：某个 DLL 需要的符号我们没有——用 tools/analyze_exe.py 对表";;
  3221225477) echo "  访问冲突（0xC0000005）：看日志最后一条，通常是未实现接口返回了空指针后被解引用";;
  *)   echo "  用 tools/analyze_exe.py 核对导入面，再看日志尾部";;
esac
exit 0
