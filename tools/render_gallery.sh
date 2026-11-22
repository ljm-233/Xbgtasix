#!/usr/bin/env bash
# 批量把导出的 OBJ 渲成 PNG（用 t8_model，纯离线、无桌面也能跑）
#
# 用法：
#   ./tools/render_gallery.sh <obj目录> <输出目录> [数量]
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=${1:?用法: render_gallery.sh <obj目录> <输出目录> [数量]}
OUT=${2:?}
N=${3:-8}
mkdir -p "$OUT"

count=0
for f in "$SRC"/*.obj; do
  [ -f "$f" ] || continue
  count=$((count + 1))
  [ "$count" -gt "$N" ] && break
  name=$(basename "$f" .obj)
  # 只挑有实际几何的（<200 顶点多为 bounds 代理盒）
  verts=$(grep -c '^v ' "$f" || true)
  if [ "${verts:-0}" -lt 200 ]; then
    echo "skip $name（仅 $verts 顶点，多为边界代理）"
    continue
  fi
  printf "%-58s " "$name"
  ( cd build && ./t8_model.exe "$f" "" "$OUT/$(printf '%02d' "$count")_$name.png" >/dev/null 2>&1 ) || true
  ( cd build && grep -oE '非背景像素 [0-9.]+%' xbgtasix.log | tail -1 ) || true
done
echo "done: $OUT"
