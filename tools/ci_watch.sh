#!/usr/bin/env bash
# ci_watch.sh —— 短轮询 CI 状态，默认最多等 240 秒
#
# 目的：不要为了一次 CI 结果把对话卡五分钟以上。
# 用法：
#   ./tools/ci_watch.sh              # 等最新一次，最多 240s
#   ./tools/ci_watch.sh 180          # 自定义上限（秒）
#   ./tools/ci_watch.sh 240 --jobs   # 结束时同时打印三个 job 的结论
#
# 退出码：0 = success，1 = failure/cancelled，2 = 还在跑（超时）
set -uo pipefail
cd "$(dirname "$0")/.."

GH=${GH:-"/c/Program Files/GitHub CLI/gh.exe"}
[ -x "$GH" ] || GH=$(command -v gh) || { echo "找不到 gh"; exit 1; }
REPO=${REPO:-ljm-233/Xbgtasix}
LIMIT=${1:-240}
SHOW_JOBS=${2:-}

start=$(date +%s)
status="unknown"
while :; do
  read -r status < <("$GH" run list -R "$REPO" --limit 1 --json status,conclusion,databaseId \
      --jq '.[0].status + " " + (.[0].conclusion // "-") + " " + (.[0].databaseId|tostring)' 2>/dev/null)
  st=${status%% *}
  rest=${status#* }
  concl=${rest%% *}
  rid=${rest#* }
  elapsed=$(( $(date +%s) - start ))
  printf '  [%3ds] %s / %s\n' "$elapsed" "$st" "$concl"
  if [ "$st" = "completed" ]; then
    if [ "$SHOW_JOBS" = "--jobs" ] && [ -n "$rid" ]; then
      "$GH" run view "$rid" -R "$REPO" --json jobs \
        --jq '.jobs[] | "    \(.conclusion // .status)  \(.name)"'
    fi
    [ "$concl" = "success" ] && exit 0 || exit 1
  fi
  if [ "$elapsed" -ge "$LIMIT" ]; then
    echo "  $LIMIT 秒内没跑完（当前 $st）——先干别的，稍后再看"
    exit 2
  fi
  sleep 15
done
