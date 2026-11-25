# vkd3d-patch：`xbox.*` 主机内建的 dxil-spirv 补丁

`vkd3d-src/` 是对账基准（只读），改动在 `../vkd3d-patch/`（vkd3d-src 的拷贝）。
这里只放**能带走的补丁**：新增两个文件 + 三处接入 + meson 一行。

```
dxil-spirv-xbox-intrinsics.txt     对 meson.build 与 dxil_converter.cpp 的 diff
opcodes_xbox_intrinsics.cpp/.hpp     新增文件，放到 subprojects/dxil-spirv/opcodes/
```

## 补丁在做什么

`xbox.*` 是主机专有的着色器外部函数（只有声明、没有定义），任何 DXIL→SPIR-V 翻译器
都会停在 `Function 'xbox.X' is declared but has no definition`。补丁：

1. `is_xbox_intrinsic()` 认定名字前缀；
2. `gather_function_dependencies` / `build_callee_functions` **跳过**它们（否则被当 leaf 硬失败）；
3. `emit_instruction` 在调用点**就地翻译**（`emit_xbox_intrinsic`），不生成 `OpFunctionCall`。

已实现 `xbox.BFI`（按位选择）与 `xbox.Med3_F32`（三数中值），其余符号打印签名后明确失败。
清单与三族分工见 [docs/xbox-intrinsics.md](../../docs/xbox-intrinsics.md)。

## 应用与重建

见 docs/xbox-intrinsics.md 第 6 节（wine 包装 mingw + meson cross file 全流程）。

## 诊断开关

`XBGTASIX_XBOX_DUMMY=1`：未实现的符号返回该类型的零常量，用来把这个 shader 里
**还引用了哪些**符号一次列全。**只用于枚举，绝不能用于验证正确性**。
