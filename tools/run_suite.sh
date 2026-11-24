#!/usr/bin/env bash
# 一条命令跑全套测试，并给出机器可读的汇总。
#
# 返回码约定（和测试本身一致）：
#   0 = PASS    2 = SKIP（本机不支持该能力，例如没有光追）    其它 = FAIL
#
# 判定门槛（2026-11 加严）：
#   * 只有"退出码 0 且输出里有验证证据（见 has_evidence）"才记 PASS；
#     退出码 0 但一条断言都没输出 -> 记 FAIL（防"跑过了 = 验过了"的假绿）。
#   * SKIP 不是通过：汇总时会额外警告"有 N 项未验证"。
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
PKG=${XBGTASIX_PKG:-"D:/GTA6/GTA6.Xbox.v1.0.2.1-CYBERLEEK"}
MODEL=${XBGTASIX_MODEL:-"D:/GTA6/out_models2/pantscargo_mr1_002_disheveled_mw_long_full_dr.obj"}
TEX=${XBGTASIX_TEXTURE:-"D:/GTA6/out_tex_all/blank_midgrey.png"}
TTF=${XBGTASIX_TTF:-"D:/GTA6/out_ttpf"}
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
  exit 3
fi

want() {
  [ -z "$ONLY" ] && return 0
  case ",$ONLY," in *",$1,"*) return 0 ;; esac
  return 1
}

pass=0; skip=0; fail=0; total=0
report="$BIN/suite/report.txt"
: > "$report"

# 判定门槛：退出码 0 **不再等于 PASS**。
# 以前只看 exit code，于是"一条断言都没有"的测试（t29 就是长期这样绿的）
# 打印"（无断言输出）"仍然记 PASS。现在要求输出里必须有"验证过的证据"：
#   1) 测试自己 ok() 打的 PASS 字样
#   2) t16（python 静态核对）的 "加载成功 N / 失败 0"
#   3) t0 的 "smoke done with N failure(s)"（N 必须是 0）
#   4) 老测试的 "N checks, 0 failures" / "failures=0"
has_evidence() {
  grep -qE 'PASS|加载成功 [0-9]+ / 失败 0|smoke done with 0 failure|checks, 0 failures|failures=0' <<< "$1"
}

run_one() {
  local id=$1; shift
  want "$id" || return 0
  total=$((total + 1))
  local t0=$(date +%s%N)
  local out rc
  skip_if_missing() {   # $1=路径 $2=说明；缺了就记 SKIP 并返回 1
    [ -e "$1" ] && return 0
    skip=$((skip+1))
    printf "%-4s %-22s %-5s %6sms  %s\n" "$id" "-" SKIP 0 "$2"
    return 1
  }
  case "$1" in
    t8_model.exe)   skip_if_missing "$MODEL" "缺少模型样本（XBGTASIX_MODEL）" || return 0 ;;
    t10_integration.exe|t11_storage.exe|t19_save.exe|t18_package.exe|t30_fakegame.exe)
                    skip_if_missing "$CFG" "缺少 MicrosoftGame.Config（XBGTASIX_CONFIG）" || return 0 ;;
    t12_sheet.exe)  skip_if_missing "$TTF" "缺少贴图样本目录（XBGTASIX_TTF）" || return 0 ;;
  esac
  out=$(cd "$BIN" && timeout 300 "./$@" 2>&1); rc=$?
  local ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  local key
  key=$(echo "$out" | grep -oE 'PASS[^"]{0,40}|[0-9]+ checks, [0-9]+ failures|failures=0 -> PASS|failures=0|smoke done with 0 failure[^"]{0,10}|非背景像素 [0-9.]+%|SKIP[^"]{0,30}' | tail -1)
  local verdict
  if [ $rc -eq 0 ]; then
    if has_evidence "$out"; then
      verdict="PASS"; pass=$((pass + 1));
    else
      # 退出码 0 但没有断言输出 = 假绿（"跑过"不等于"验过"）
      verdict="FAIL"; fail=$((fail + 1)); key="无断言输出 -> 判 FAIL（假绿）"
    fi
  elif [ $rc -eq 2 ]; then verdict="SKIP"; skip=$((skip + 1));
  else verdict="FAIL"; fail=$((fail + 1)); fi
  # 测试自己报了失败数却返回 0 的情况：按失败数判红，别被退出码骗过
  if [ "$verdict" = "PASS" ]; then
    nfail=$(echo "$out" | grep -oE '[0-9]+ checks, [0-9]+ failures' | tail -1 | grep -oE ', [0-9]+' | tr -d ', ')
    if [ -n "${nfail:-}" ] && [ "$nfail" -gt 0 ]; then
      verdict="FAIL"; fail=$((fail + 1)); pass=$((pass - 1)); key="测试自报 $nfail 项失败（rc=0 也不能算过）"
    fi
  fi
  printf "%-4s %-22s %-5s %6sms  %s\n" "$id" "$1" "$verdict" "$ms" "${key:-（无断言输出）}"
  echo -e "$id\t$1\t$verdict\t$rc\t${ms}ms\t${key}" >> "$report"
}

# 已知失败的 WIP 测试：**照样运行并如实打印真实结果**，只是不计入 main 的绿/红门槛。
# 用途：别人正在改的子系统（例如设备 ABI 代理）带着会崩的回归测试，不想让 main 长期变红，
# 但又不能假装它通过 —— 所以单独统计，并在汇总里点名"N 项已知失败"。
known=0
run_known_fail() {
  local id=$1 reason=$2; shift 2
  want "$id" || return 0
  total=$((total + 1))
  local t0=$(date +%s%N) out rc
  out=$(cd "$BIN" && timeout 300 "./$@" 2>&1); rc=$?
  local ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  local key
  key=$(echo "$out" | grep -oE 'PASS[^"]{0,40}|FAIL[^"]{0,40}|failures=[0-9]+' | tail -1)
  local verdict="KNOW"
  if [ $rc -eq 0 ] && has_evidence "$out"; then
    verdict="PASS"; pass=$((pass + 1));   # 修好了就自动转正
  else
    known=$((known + 1)); key="${key:-rc=$rc}（$reason）"
  fi
  printf "%-4s %-22s %-5s %6sms  %s\n" "$id" "$1" "$verdict" "$ms" "$key"
  echo -e "$id\t$1\t$verdict\t$rc\t${ms}ms\t${key}" >> "$report"
}

# Python 步子：没有样本数据时按 SKIP 处理
run_py() {
  local id=$1 script=$2; shift 2
  want "$id" || return 0
  total=$((total + 1))
  local t0=$(date +%s%N) out rc
  out=$(cd "$PWD" && timeout 300 python "$script" "$@" 2>&1); rc=$?
  local ms=$(( ($(date +%s%N) - t0) / 1000000 ))
  local key verdict
  key=$(echo "$out" | grep -oE 'PASS（无缺失）|加载成功 [0-9]+ / 失败 [0-9]+|自测: [0-9]+ 个用例，失败 [0-9]+ 个 -> (PASS|FAIL)|检查 [0-9]+ 个文件，违规 [0-9]+ 个 -> (PASS|FAIL)|FAIL[^"]{0,30}' | tail -1)
  if [ $rc -eq 0 ]; then
    if has_evidence "$out"; then
      verdict="PASS"; pass=$((pass + 1));
    else
      verdict="FAIL"; fail=$((fail + 1)); key="无断言输出 -> 判 FAIL（假绿）"
    fi
  elif [ $rc -eq 2 ]; then verdict="SKIP"; skip=$((skip + 1));
  else verdict="FAIL"; fail=$((fail + 1)); fi
  printf "%-4s %-22s %-5s %6sms  %s\n" "$id" "$script" "$verdict" "$ms" "${key:-（无断言输出）}"
  echo -e "$id\t$script\t$verdict\t$rc\t${ms}ms\t${key}" >> "$report"
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
run_one t12 t12_sheet.exe "$TTF" suite/sheet.png 128 10
run_one t13 t13_caps.exe
run_one t14 t14_vram.exe
run_one t17 t17_async2.exe
run_one t18 t18_package.exe "$CFG"
run_one t19 t19_save.exe "$CFG"
run_one t20 t20_taskqueue2.exe
run_one t21 t21_user.exe
# 合规护栏（模式 B）：静态白名单 + 运行时落点闸门
run_py p1 tools/policy.py --selftest
run_py p2 tools/policy.py --check-repo
run_py p3 tools/repo_guard.py --selftest

if [ -d "$PKG" ]; then
  run_one t22 t22_extras.exe "$CFG" "$PKG"
else
  want t22 && { total=$((total + 1)); skip=$((skip + 1)); }
  echo "t22  t22_extras.exe         SKIP   缺少游戏包目录（XBGTASIX_PKG）"
fi
run_one t23 t23_pkg2.exe
run_one t24 t24_save2.exe
run_one t25 t25_sys.exe
run_one t26 t26_stream.exe
run_one t27 t27_composite.exe
run_one t28 t28_speech.exe
run_one t29 t29_perf.exe "$CFG"
run_one t30 t30_fakegame.exe "$CFG" "$PKG"
run_one t31 t31_swapchain.exe
run_one t32 t32_window.exe
run_one t33 t33_memory.exe
run_one t34 t34_residency.exe
run_one t35 t35_gamepad.exe
run_one t36 t36_ui_host.exe
run_one t37 t37_system_ui.exe
run_one t38 t38_xboxui.exe --shot suite/xboxui.bmp
run_one t39 t39_host_tool.exe
run_one t40 t40_taskqueue3.exe
run_one t41 t41_d3d12_device_abi.exe
run_one t46 t46_rootsig.exe
run_known_fail t47 "WIP: 槽 62 已从取证桩升级为真实实现，t47 的桩契约断言待更新" t47_pso.exe
run_one t48 t48_present.exe
run_one t49 t49_pso_warp.exe
run_one t44 t44_diff.exe
run_one t45 t45_census.exe   # 曾是 known-fail（代理转发器 bug），已修好转正
run_one t42 t42_launcher_backup.exe
run_known_fail t43 "WIP: host-stall 检测，测试自报 2 项失败（GPT 在进行中）" t43_host_stall.exe
run_one t15 t15_budget.exe

# 合规护栏（模式 B）：静态白名单 + 运行时落点闸门
run_py p1 tools/policy.py --selftest
run_py p2 tools/policy.py --check-repo
run_py p3 tools/repo_guard.py --selftest

if [ -d "$PKG" ]; then
  run_py t16 tools/check_real_dlls.py "$PKG"
else
  want t16 && { total=$((total + 1)); skip=$((skip + 1)); }
  echo "t16  真实 DLL 加载            SKIP   缺少游戏包目录（XBGTASIX_PKG 可指定）"
fi

echo
if [ $total -eq 0 ]; then
  echo "== 汇总: 没有跑任何测试（--only 没匹配到？）" >&2
  exit 1
fi
echo "== 汇总: $pass PASS / $skip SKIP / $fail FAIL / $known 已知失败（共 $total）"
echo "   报告: $report"
if [ $known -gt 0 ]; then
  # 已知失败不是通过：明细就在上面 KNOW 行里，这里点名以免被扫过去
  echo "!! 注意: 有 $known 项已知失败（WIP，正在改；不计入本次红绿门槛，见上面 KNOW 行）" >&2
fi
if [ $skip -gt 0 ]; then
  # SKIP 不是通过：它意味着这套测试里有 N 项"这次没有验证"。
  # 以前没有这句警告，SKIP 混在绿油油的报表里没人注意。
  echo "!! 警告: 有 $skip 项未验证（SKIP 只能说明本机不支持，不代表通过；见上面 SKIP 行）" >&2
fi
[ $fail -eq 0 ] || exit 1
exit 0
