#!/usr/bin/env bash
# exe 到手后的启动试跑：把目标 exe 和我们的 shim 放到同一个目录，跑一次，
# 用 tracer 拿到"第一个未实现的调用"。
#
# 用法：
#   ./tools/run_target.sh <game.exe> [参数...]
#
# 为什么要单独建目录：Windows 加载器按 exe 所在目录解析同名 DLL，
# 混在 build/ 里容易和测试程序互相干扰。
set -euo pipefail
cd "$(dirname "$0")/.."

EXE=${1:?用法: run_target.sh <game.exe> [参数...]}
shift || true
[ -f "$EXE" ] || { echo "找不到 $EXE"; exit 2; }

STAGE=build/target
mkdir -p "$STAGE"
cp -f build/xgameruntime.dll build/xmem.dll build/d3d12_xs.dll "$STAGE"/
BASE=$(basename "$EXE")
cp -f "$EXE" "$STAGE/$BASE"

echo "== 试跑 $BASE（XBGTASIX_UNIMPL=log）"
cd "$STAGE"
set +e
XBGTASIX_UNIMPL=log XBGTASIX_LOG="$PWD/target.log" timeout 180 "./$BASE" "$@"
rc=$?
set -e
echo "== 退出码 $rc"
echo
echo "== 第一个未实现调用"
grep -m1 "UNIMPLEMENTED" target.log || echo "  （没有触发未实现调用：可能更早就失败了）"
echo
echo "== 日志尾部"
tail -12 target.log || true
echo
echo "提示：把 XBGTASIX_UNIMPL 去掉（默认 stop）可以看它停在哪一步；"
echo "      按 target.log 里的调用顺序逐个实现，比一次铺满 500 个桩有效。"
