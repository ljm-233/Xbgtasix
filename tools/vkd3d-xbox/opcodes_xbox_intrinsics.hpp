/* Copyright (c) 2026 Xbgtasix contributors
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "converter_impl.hpp"

namespace dxil_spv
{
// 名字以 `xbox.` 开头的外部函数 = 主机专有着色器内建函数（只有声明、没有定义）。
bool is_xbox_intrinsic(const llvm::Function *func);

// 取名字（与 dxil_converter.cpp 里的 get_function_name 等价，但那个是 static）。
String get_xbox_intrinsic_name(const llvm::Function *func);

// 把一个 `xbox.*` 调用翻译成 SPIR-V。返回 false = 这个符号还没实现（转换失败）。
bool emit_xbox_intrinsic(Converter::Impl &impl, const llvm::CallInst &inst);
} // namespace dxil_spv
