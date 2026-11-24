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
# 盘符：Wine 只认 $WINEPREFIX/dosdevices 里的符号链接。
# 实测（docs/wine-host.md 8.8）：shim 里的 DefineDosDeviceW(DDD_RAW_TARGET_PATH) 在 Wine 下
# **返回成功但不生效** —— G:\appdata0.rpf 依旧 ERROR_FILE_NOT_FOUND(2)，而同一个文件用
# D:\GTA6\... 全路径能开。三种写法（raw DOS / raw unix / 非 raw）都试过，都不行。
# 于是这一步放到宿主侧做：启动**之前**把 g:/t: 建出来，新起的进程就能看见。
# 注意：这不是 drives.cpp 的活。drives.cpp 在进程里跑，那时候 Wine 的盘符表已经建好了，
# 它再调 DefineDosDeviceW 也加不进来（实测三种写法都"成功但不生效"）。drives.cpp 现在只负责
# "映射完自检 + 失败时大声报 404"。删掉下面这两行，立刻回到"文件明明在、GetLastError=2"。
#   g:  Xbox 的持久存储/内容盘（游戏在这里找 appdata0.rpf 等包内文件）
#   t:  临时盘（崩溃转储、缓存）
# ---------------------------------------------------------------------------
setup_drive() {
  local letter=$1 target=$2
  mkdir -p "$target"
  ln -sfn "$target" "$WINEPREFIX/dosdevices/$letter:"
  echo "== 盘符      $letter: -> $target"
}
setup_drive g "$PKG"
setup_drive t "$WINEPREFIX/drive_c/users/steamuser/AppData/Local/Xbgtasix/T"

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
