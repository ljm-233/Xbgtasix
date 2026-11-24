# t47_pso —— 主机 PSO 槽（55/60/61/62/66/67）参数 dump 桩验收

这一轮的交付是**诊断桩**，不是 PSO 创建实现：本仓库 `build/**.log`（202 个）里
没有任何 slot=60/61/62 的实测调用，真机在 `game+0x247AF2D`（XboxBvhBuilder 构造后的
自身断言）就停了，还没走到 PSO 创建。槽号身份只来自参考实现（omonkeyz/xbox-emulator
`device_wrap.rs` 末尾 `vtable()` 的注释），**签名未实测**，因此默认一律返回 E_NOTIMPL。

## 编译（不跑 build.sh）

```bash
cd /d/GTA6/Xbgtasix
/d/mingw64/bin/g++ -std=c++20 -O2 -D_WIN32_WINNT=0x0A00 -DNOMINMAX -DWIN32_LEAN_AND_MEAN \
  -Wno-attributes -Wall -Wextra -static-libgcc -static-libstdc++ \
  -Isrc -Iinclude -I"D:/GTA6/gdk/include_clean" \
  tests/t47_pso/main.cpp -ld3d12 -ldxgi -ldxguid -o build/t47_pso.exe
```

测试直接 `#include "d3d12_xs/device_abi.h"`（所以需要 `-Isrc`），并把
`xbgtasix::logf` 换成本地捕获器 —— 不启动游戏、不使用游戏数据、也不依赖
`build/d3d12_xs.dll` 是最新构建。

## 运行

```bash
./build/t47_pso.exe           # 只打印汇总：<N> checks, <M> failures
./build/t47_pso.exe --dump    # 额外打印捕获到的桩日志样例（slot 60 / slot 62）
```

退出码：0 = 全部通过；1 = 有失败；2 = 一条 check 都没跑（无法测试）。
没有可用 D3D12 设备时，槽 10/11 的"桌面转发"那一组会打印 SKIP，不算失败。

## 接进正式构建 / 套件（需要构建负责人改，本轮禁改 build.sh）

`build.sh`（放在 t40 之后即可）：

```bash
echo "== t47_pso.exe"
"$CXX" "${FLAGS[@]}" "${LDFLAGS_TESTS[@]}" -Isrc tests/t47_pso/main.cpp \
    -ld3d12 -ldxgi -ldxguid -o "$OUT/t47_pso.exe"
```

`tools/run_suite.sh`（放在 `run_one t41 ...` 附近）：

```bash
run_one t47 t47_pso.exe
```
