#!/usr/bin/env bash
# ci_build.sh —— 在 CI（或任何干净克隆）里构建并测试
#
# 分两层：
#   1) **不依赖 GDK 的部分**：xmem.dll + t3_xmem（93 条断言）。任何 Windows 环境都能跑。
#   2) **需要 GDK 的部分**：三个 shim + 全套测试（步数不写死，见下面的 SUITE_STEPS）。
#      找不到 GDK 就跳过，并在结论里说清楚。
#
# 为什么这样分：微软的 GDK 头/库不能入库，所以公开 CI 上默认只有第 1 层。
# 但第 1 层也是真代码真断言——能抓到 xmem 那类内存/对齐 bug，以及"克隆后编不过"这类问题。
#
# 用法：
#   ./tools/ci_build.sh              # 自动判断有没有 GDK
#   GDK_INC=<路径> ./tools/ci_build.sh
#   STRICT=1 ./tools/ci_build.sh     # 强制要求 GDK 存在（缺了就算失败；CI 用这个）
set -uo pipefail
cd "$(dirname "$0")/.."

# 全套测试的步数从 tools/run_suite.sh 里数出来（run_one + run_py 的调用行），
# 不写死数字：以前这里和 ci.yml 都写"全套 32 项"，实际早就 39 步了。
SUITE_STEPS=$(grep -cE '^[[:space:]]*run_(one|py)[[:space:]]' tools/run_suite.sh 2>/dev/null || true)
SUITE_STEPS=${SUITE_STEPS:-0}
# t3_xmem 的断言条数下限：实际 93 条，少一条都说明断言被删或没跑到
T3_MIN_CHECKS=93

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

# 关键：残留的旧 DLL / implib 会让下一次链接"成功"地连到**上一次**的 xmem.dll，
# 于是本次链接其实失败也照样报 PASS。构建前先清掉。
rm -f build/xmem.dll build/libxmem.dll.a
if g++ "${CXXFLAGS[@]}" -static -shared src/xmem/xmem.cpp src/trace.cpp \
        -o build/xmem.dll -Wl,--out-implib,build/libxmem.dll.a 2>build/ci_xmem_err.txt; then
  echo "xmem.dll 构建 OK（$(stat -c %s build/xmem.dll 2>/dev/null || wc -c < build/xmem.dll) 字节）"
else
  bad "xmem.dll 构建失败"; head -20 build/ci_xmem_err.txt
fi

# 没有本次生成的 implib 就是失败（以前这里是 `if [ -f ... ]`，缺了就默默跳过）
if [ ! -f build/libxmem.dll.a ]; then
  bad "本次没有生成 build/libxmem.dll.a（xmem.dll 链接失败，旧库已清掉）"
else
  if g++ "${CXXFLAGS[@]}" tests/t3_xmem/main.cpp src/trace.cpp \
          -Lbuild -lxmem -o build/t3_xmem.exe 2>build/ci_t3_err.txt; then
    if (cd build && timeout 120 ./t3_xmem.exe > ci_t3.log 2>&1); then
      CHECK=$(grep -oE '[0-9]+ checks, [0-9]+ failures' build/ci_t3.log | tail -1)
      # 只看退出码不够：实测过"退出码 0 但断言只跑了 40 条"这种假成功。
      read -r CHECKS _ FAILS _ <<< "${CHECK:-}"
      if [ -z "${CHECK:-}" ]; then
        bad "t3_xmem 没打印断言统计（无法确认 $T3_MIN_CHECKS 条断言都跑了）"
      elif [ "${FAILS:-1}" -ne 0 ]; then
        bad "t3_xmem 有 ${FAILS} 条断言失败（${CHECK}）"
      elif [ "${CHECKS:-0}" -lt "$T3_MIN_CHECKS" ]; then
        bad "t3_xmem 只跑了 ${CHECKS} 条断言，少于下限 $T3_MIN_CHECKS"
      else
        echo "t3_xmem: PASS（${CHECKS} checks, ${FAILS} failures）"
      fi
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
      echo "全套测试通过（$SUITE_STEPS 步）"
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
    bad "STRICT=1 要求 GDK，但不存在（$GDK_INC 缺 XTaskQueue.h 或 lib/）"
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
    echo "PASS：完整构建 + 全套测试（$SUITE_STEPS 步）+ 工具自检 + 文档链接"
  else
    echo "PASS（部分覆盖）：无 GDK，只跑了 xmem 子集 + 工具自检 + 文档链接"
    echo "注意：第 2 层（三个 shim + $SUITE_STEPS 步测试）没跑；CI 上用 STRICT=1 可以把它变成硬要求。"
  fi
  exit 0
fi
echo "FAIL：$fail 项"
exit 1
