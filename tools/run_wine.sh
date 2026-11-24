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

# ---------------------------------------------------------------------------
# 盘符 vs 路径重写
# ---------------------------------------------------------------------------
# 默认走**路径重写**：shim（xmem.dll 的 IAT 钩子）在文件 API 转发之前把 "G:/x"、"G:\x"
# 换成真实目录，不需要任何宿主盘符机制。这一层在 windows 和 wine 上走同一条代码路径。
# 开启方式：不设任何变量（默认开）；显式打开 XBGTASIX_PATH_REWRITE=1。
#
# 回退到"造盘符"模式：XBGTASIX_PATH_REWRITE=0（等价于 XBGTASIX_NO_PATH_REWRITE=1）。
# 那个模式下 Wine 只认 $WINEPREFIX/dosdevices 里的符号链接 —— 实测（docs/wine-host.md 8.8）：
# 进程里 DefineDosDeviceW(DDD_RAW_TARGET_PATH) 三种写法在 Wine 下**返回成功但不生效**，
# G:\appdata0.rpf 依旧 ERROR_FILE_NOT_FOUND(2)，而同一个文件用全路径能开。所以必须
# 启动**之前**把 g:/t: 建出来。删掉这两行，立刻回到"文件明明在、GetLastError=2"。
# ---------------------------------------------------------------------------
setup_drive() {
  local letter=$1 target=$2
  mkdir -p "$target"
  ln -sfn "$target" "$WINEPREFIX/dosdevices/$letter:"
  echo "== 盘符      $letter: -> $target"
}

# 重写模式默认开；只认 0/1，别的值当默认
REWRITE=${XBGTASIX_PATH_REWRITE:-1}
if [ "$REWRITE" = "0" ]; then
  export XBGTASIX_PATH_REWRITE=0
  export XBGTASIX_NO_PATH_REWRITE=1
  echo "== 模式      盘符映射（shim 不重写路径）"
  setup_drive g "$PKG"
  setup_drive t "$WINEPREFIX/drive_c/users/steamuser/AppData/Local/Xbgtasix/T"
else
  export XBGTASIX_PATH_REWRITE=1
  unset XBGTASIX_NO_PATH_REWRITE
  echo "== 模式      路径重写（不建盘符；G: -> $PKG，T: -> LOCALAPPDATA\\Xbgtasix\\T）"
  if [ -e "$WINEPREFIX/dosdevices/g:" ] || [ -e "$WINEPREFIX/dosdevices/t:" ]; then
    echo "             注意：dosdevices 里仍有 g:/t: 符号链接（重写模式下用不到，留着不影响结论）"
  fi
fi

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
# Wine 的 gameinput.dll 在 GameInputCreate 开头就 `getenv("SteamGameId")`，拿不到直接返回
# E_NOTIMPL（0x80004001，实测反汇编 0x1800014f9 -> 0x180001709）。标题在 GDK 引导尾段必须建
# 一个 IGameInput 单例，失败就 int3（game+0x5925915）。给了这个变量它才会继续往下走。
# 这是宿主差异，不是游戏的怪癖 —— 真 Windows 上装了 GameInput redist 就有时不需要它。
export SteamGameId=${SteamGameId:-1771300}

run=( "$WINE" "$HERE/build/xbgtasix.exe" run "$(to_win "$PKG")" )
[ -n "$dll_dir" ] && run+=( --dll-dir "$(to_win "$HERE/$dll_dir")" )
run+=( "${args[@]}" )

echo "== wine      $WINE"
echo "== prefix    $WINEPREFIX"
echo "== package   $PKG"
echo "== 命令      ${run[*]}"
exec "${run[@]}"
