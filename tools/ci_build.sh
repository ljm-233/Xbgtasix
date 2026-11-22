#!/usr/bin/env bash
# ci_build.sh —— 在 CI（或任何干净克隆）里构建并测试
#
# 分两层：
#   1) **不依赖 GDK 的部分**：xmem.dll + t3_xmem（93 条断言）。任何 Windows 环境都能跑。
#   2) **需要 GDK 的部分**：三个 shim + 全套 32 项。找不到 GDK 就跳过，并在结论里说清楚。
#
# 为什么这样分：微软的 GDK 头/库不能入库，所以公开 CI 上默认只有第 1 层。
# 但第 1 层也是真代码真断言——能抓到 xmem 那类内存/对齐 bug，以及"克隆后编不过"这类问题。
#
# 用法：
#   ./tools/ci_build.sh              # 自动判断有没有 GDK
#   GDK_INC=<路径> ./tools/ci_build.sh
#   STRICT=1 ./tools/ci_build.sh     # 强制要求 GDK 存在（缺了就算失败）
set -uo pipefail
cd "$(dirname "$0")/.."

fail=0
say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
bad() { printf 'FAIL: %s\n' "$*" >&2; fail=$((fail + 1)); }

say "0) 环境"
command -v g++ >/dev/null || { echo "缺少 g++"; exit 1; }
echo "g++: $(g++ --version | head -1)"
echo "python: $(python --version 2>&1)"

# ---------------------------------------------------------------- 第 1 层
say "1) 无 GDK 依赖部分：xmem.dll + t3_xmem（93 条断言）"
mkdir -p build
CXXFLAGS=(-std=c++20 -O2 -D_WIN32_WINNT=0x0A00 -DNOMINMAX -DWIN32_LEAN_AND_MEAN -Iinclude)

if g++ "${CXXFLAGS[@]}" -static -shared src/xmem/xmem.cpp src/trace.cpp \
        -o build/xmem.dll -Wl,--out-implib,build/libxmem.dll.a 2>build/ci_xmem_err.txt; then
  echo "xmem.dll 构建 OK（$(stat -c %s build/xmem.dll 2>/dev/null || wc -c < build/xmem.dll) 字节）"
else
  bad "xmem.dll 构建失败"; head -20 build/ci_xmem_err.txt
fi

if [ -f build/libxmem.dll.a ]; then
  if g++ "${CXXFLAGS[@]}" tests/t3_xmem/main.cpp src/trace.cpp \
          -Lbuild -lxmem -o build/t3_xmem.exe 2>build/ci_t3_err.txt; then
    if (cd build && timeout 120 ./t3_xmem.exe > ci_t3.log 2>&1); then
      CHECK=$(grep -oE '[0-9]+ checks, [0-9]+ failures' build/ci_t3.log | tail -1)
      echo "t3_xmem: PASS（${CHECK:-无断言输出}）"
    else
      bad "t3_xmem 运行失败"; tail -20 build/ci_t3.log
    fi
  else
    bad "t3_xmem 构建失败"; head -20 build/ci_t3_err.txt
  fi
fi

# ---------------------------------------------------------------- 第 2 层
say "2) 依赖 GDK 的部分"
GDK_INC=${GDK_INC:-"$(cd .. 2>/dev/null && pwd)/gdk/include_clean"}
HAVE_GDK=0
if [ -f "$GDK_INC/XTaskQueue.h" ] && [ -d "$(dirname "$GDK_INC")/lib" ]; then
  HAVE_GDK=1
fi

if [ "$HAVE_GDK" -eq 1 ]; then
  echo "找到 GDK: $GDK_INC"
  if ./build.sh; then
    echo "三个 shim 构建 OK"
    if ./tools/run_suite.sh; then
      echo "全套测试通过"
    else
      bad "全套测试有 FAIL"
    fi
  else
    bad "build.sh 失败"
  fi
else
  echo "未找到 GDK（$GDK_INC）"
  echo "  微软的 GDK 头/库不入库，公开 CI 上默认跳过第 2 层。"
  echo "  要覆盖它：把 GDK 解到仓库旁 ../gdk/，或设 GDK_INC 指向 include_clean。"
  if [ "${STRICT:-0}" = "1" ]; then
    bad "STRICT=1 要求 GDK，但不存在"
  fi
fi

# ---------------------------------------------------------------- 工具链自检
say "3) Python 工具自检"
if python tools/selftest_tools.py > build/ci_selftest.log 2>&1; then
  tail -1 build/ci_selftest.log
else
  bad "selftest_tools.py 失败"; tail -20 build/ci_selftest.log
fi

say "4) 文档链接"
if python tools/check_docs.py > build/ci_docs.log 2>&1; then
  tail -1 build/ci_docs.log
else
  bad "check_docs.py 有断裂"; cat build/ci_docs.log
fi

# ---------------------------------------------------------------- 结论
say "结论"
if [ "$fail" -eq 0 ]; then
  if [ "$HAVE_GDK" -eq 1 ]; then
    echo "PASS：完整构建 + 全套测试 + 工具自检 + 文档链接"
  else
    echo "PASS（部分覆盖）：无 GDK，只跑了 xmem 子集 + 工具自检 + 文档链接"
    echo "生产 CI 上这里应当被标记为'未完全覆盖'，不要当作完整通过"
  fi
  exit 0
fi
echo "FAIL：$fail 项"
exit 1
