#!/usr/bin/env bash
# run_wine.sh —— 在 Linux 上（经 wine/Proton）跑 xbgtasix。
#
# 为什么需要这个脚本：直接 `wine build/xbgtasix.exe run ...` 在本机会在
# d3d12_xs.dll 的**导入**阶段就死，报 0xC0000135（MISSING_DLL）：
#   wine 自带的 builtin dxgi.dll 静态依赖 wined3d.dll -> libvkd3d-1.dll，
#   而 libvkd3d-*.dll 只躺在 Proton 的 lib/vkd3d/ 与 lib/vkd3d-proton/ 下，
#   不在 wine 默认的 PE 搜索路径里。
# Proton 启动游戏时靠它自己的脚本补这几条路径，绕开 Proton 直接调 wine 就没有了。
# 这里把路径补齐，并把 dxgi/d3d12 让给 dxvk / vkd3d-proton（Proton 的正常组合），
# 这样 d3d12_xs.dll 才加载得起来。
#
# 用法：
#   ./tools/run_wine.sh                       # 用默认包与 build/ 里的 shim
#   ./tools/run_wine.sh --dll-dir build/wine-bridge --timeout 60
#   PKG=/path/to/pkg ./tools/run_wine.sh
#
# 环境变量（都可覆盖）：
#   PROTON_DIR    Proton runner 目录（默认 DW-Proton Latest）
#   WINEPREFIX    wine 前缀（默认 ~/Games/gta6-x11）
#   PKG           游戏包目录（win 路径可省略，脚本会转成 Z:\...）
set -uo pipefail
cd "$(dirname "$0")/.."

HERE=$(pwd)
PROTON_DIR=${PROTON_DIR:-"/home/link/.local/share/proton/runners/DW-Proton Latest/files"}
WINEPREFIX=${WINEPREFIX:-"$HOME/Games/gta6-x11"}
PKG=${PKG:-"$(cd .. && pwd)/GTA6.Xbox.v1.0.2.1-CYBERLEEK"}

WINE="$PROTON_DIR/bin/wine"
[ -x "$WINE" ] || { echo "找不到 wine：$WINE（用 PROTON_DIR= 指定）"; exit 1; }

# win 路径：wine 要 Z:\... 形式
to_win() { printf 'Z:%s' "$(printf '%s' "$1" | tr '/' '\\')"; }

args=()
dll_dir=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dll-dir) dll_dir=$2; shift 2 ;;
    --timeout) args+=("$1" "$2"); shift 2 ;;
    *) args+=("$1"); shift ;;
  esac
done

export WINEPREFIX
export PATH="$PROTON_DIR/bin:/usr/bin:/bin"
export DISPLAY=${DISPLAY:-:0}
# Proton 把 dxvk / vkd3d-proton 单独放，默认搜索路径里没有
export WINEDLLPATH="$PROTON_DIR/lib/wine/dxvk/x86_64-windows:$PROTON_DIR/lib/wine/vkd3d-proton/x86_64-windows:$PROTON_DIR/lib/wine/x86_64-windows"
export WINEDLLOVERRIDES="dxgi=n;d3d11=n;d3d10core=n;d3d12=n;d3d12core=n"
export VKGUARD_DISABLE=${VKGUARD_DISABLE:-1}
export XBGTASIX_VEH=${XBGTASIX_VEH:-1}
export WINEDEBUG=${WINEDEBUG:--all}

run=( "$WINE" "$HERE/build/xbgtasix.exe" run "$(to_win "$PKG")" )
[ -n "$dll_dir" ] && run+=( --dll-dir "$(to_win "$HERE/$dll_dir")" )
run+=( "${args[@]}" )

echo "== wine      $WINE"
echo "== prefix    $WINEPREFIX"
echo "== package   $PKG"
echo "== 命令      ${run[*]}"
exec "${run[@]}"
