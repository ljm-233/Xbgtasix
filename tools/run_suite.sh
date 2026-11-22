#!/usr/bin/env bash
# 一条命令跑全套测试，并给出机器可读的汇总。
#
# 返回码约定（和测试本身一致）：
#   0 = PASS    2 = SKIP（本机不支持该能力，例如没有光追）    其它 = FAIL
#
# 用法：
#   ./tools/run_suite.sh                       # 全部
#   XBGTASIX_ADAPTER=0 ./tools/run_suite.sh    # 强制核显跑
#   ./tools/run_suite.sh --only t5,t6          # 只跑指定几项
set -uo pipefail
cd "$(dirname "$0")/.."

ONLY=""
if [ "${1:-}" = "--only" ]; then ONLY=${2:-}; fi

BIN=build
CFG=${XBGTASIX_CONFIG:-"D:/GTA6/GTA6.Xbox.v1.0.2.1-CYBERLEEK/MicrosoftGame.Config"}
MODEL=${XBGTASIX_MODEL:-"D:/GTA6/out_models2/pantscargo_mr1_002_disheveled_mw_long_full_dr.obj"}
TEX=${XBGTASIX_TEXTURE:-"D:/GTA6/out_tex_all/blank_midgrey.png"}
# Windows 原生调用拿不了 MSYS 的 /d/... 路径，统一转成 D:/... 形式
if command -v cygpath >/dev/null 2>&1; then
  PLS_DEFAULT=$(cygpath -m "$PWD/$BIN/pls")
else
  PLS_DEFAULT="$PWD/$BIN/pls"
fi
export XBGTASIX_PLS_ROOT=${XBGTASIX_PLS_ROOT:-"$PLS_DEFAULT"}
mkdir -p "$BIN/pls" "$BIN/suite"

if [ ! -x "$BIN/t0_hello.exe" ]; then
  echo "先构建：./build.sh"
  exit 2
fi

want() {
  [ -z "$ONLY" ] && return 0
  case ",$ONLY," in *",$1,"*) return 0 ;; esac
  return 1
}

pass=0; skip=0; fail=0; total=0
report="$BIN/suite/report.txt"
: > "$report"

run_one() {
  local id=$1; shift
  want "$id" || return 0
  total=$((total + 1))
  local t0=$(date +%s%N)
  local out rc
  out=$(cd "$BIN" && timeout 300 "./$@" 2>&1); rc=$?
  local ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  local key
  key=$(echo "$out" | grep -oE 'PASS[^"]{0,40}|93 checks, 0 failures|failures=0 -> PASS|smoke done with 0 failure|非背景像素 [0-9.]+%|SKIP[^"]{0,30}' | tail -1)
  local verdict
  if [ $rc -eq 0 ]; then verdict="PASS"; pass=$((pass + 1));
  elif [ $rc -eq 2 ]; then verdict="SKIP"; skip=$((skip + 1));
  else verdict="FAIL"; fail=$((fail + 1)); fi
  printf "%-4s %-22s %-5s %6sms  %s\n" "$id" "$1" "$verdict" "$ms" "${key:-（无断言输出）}"
  echo -e "$id\t$1\t$verdict\t$rc\t${ms}ms\t${key}" >> "$report"
}

echo "== 测试套件（adapter=${XBGTASIX_ADAPTER:-best}）"
run_one t0 t0_hello.exe --smoke
run_one t1 t1_triangle.exe
run_one t2 t2_textured.exe
run_one t3 t3_xmem.exe
run_one t4 t4_bindless.exe
run_one t5 t5_mesh.exe
run_one t6 t6_rt.exe
run_one t8 t8_model.exe "$MODEL" "$TEX" suite/model.png
run_one t9 t9_async.exe
run_one t10 t10_integration.exe "$CFG"
run_one t11 t11_storage.exe "$CFG"
run_one t12 t12_sheet.exe "D:/GTA6/out_ttpf" suite/sheet.png 128 10
run_one t13 t13_caps.exe
run_one t14 t14_vram.exe
run_one t15 t15_budget.exe

echo
echo "== 汇总: $pass PASS / $skip SKIP / $fail FAIL（共 $total）"
echo "   报告: $report"
[ $fail -eq 0 ] || exit 1
exit 0
