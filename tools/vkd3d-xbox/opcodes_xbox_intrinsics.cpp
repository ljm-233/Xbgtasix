/* Copyright (c) 2026 Xbgtasix contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Xbox（GDK / d3d12_xs）专有的着色器外部函数：`xbox.*`
 *
 * 背景
 * ----
 * 主机编译器把一部分操作发成**只有声明、没有定义**的外部函数，名字以 `xbox.` 开头；
 * 主机侧由驱动的着色器后端链接进最终 ISA。桌面侧没有对应物，所以任何 DXIL→SPIR-V
 * 翻译器都会停在 `Function 'xbox.X' is declared but has no definition`。
 *
 * 实测（53 个游戏 compute PSO，Intel Mesa 与 NVIDIA 两台逐条相同）：
 *   9/53 ok、38/53 未定义符号（13 个不同符号）、5/53 bitcode 记录未实现。
 *
 * 调用形状（实测。第一版只做前两个）
 * ----------------------------------
 * 每个 `xbox.*` 的第一个实参都是**常量选择子**，不是函数参数：
 *
 *   xbox.BFI              (i32 sel=0x17, i32 m, i32 a, i32 b)   -> i32       ✅ 已实现
 *   xbox.Med3_F32         (i32 sel=0x0a, f32 x, f32 y, f32 z)   -> f32       ✅ 已实现
 *   xbox.global_atomic_add(i32 sel=0x163, i64 addr, i32 v)      -> i32       ⛔ 待设计
 *   xbox.TypedLoad.i32    (i32 sel=0x8a, {ptr[as0]{i8}} desc,
 *                          i32, i32 sel2=0x41)                  -> {i32 x5}  ⛔ 待设计
 *
 * 选择子在同一个符号上恒定（BFI 恒 0x17、Med3_F32 恒 0x0a；cs_15/18/39/40/46 交叉验证）。
 * 因此实现里对选择子做**硬校验**：值不对就大声失败，绝不"猜一个差不多的语义"——
 * 静默编错的像素比编不过更难查。
 *
 * 写法约定（重要）
 * ----------------
 * 每个符号必须写清"它在 PC（Vulkan）上等价于什么"，而不是只写代码。将来交上游或
 * 别人接手时，那行注释是唯一能解释"为什么这样补是对的"的东西。
 */
#include "opcodes_xbox_intrinsics.hpp"
#include "converter_impl.hpp"
#include "logging.hpp"
#include "spirv_module.hpp"
#include "dxil/dxil_common.hpp"

namespace dxil_spv
{
bool is_xbox_intrinsic(const llvm::Function *func)
{
	if (!func)
		return false;
	const char *name = func->getName().data();
	return name && strncmp(name, "xbox.", 5) == 0;
}

String get_xbox_intrinsic_name(const llvm::Function *func)
{
#ifdef HAVE_LLVMBC
	return func->getName();
#else
	return String(func->getName().str().c_str());
#endif
}

namespace
{
// 诊断开关（**绝不能用来验证正确性**）：XBGTASIX_XBOX_DUMMY=1 时，未实现的符号
// 返回该类型的零常量，让翻译继续走下去，用来把这个 shader 里**还引用了哪些** xbox
// 符号一次性列全。不加这个开关时，dxil-spirv 只报遇到的第一个符号，逐个补很容易
// 误判"补一个就好了"。
bool dummy_mode()
{
	static const bool on = []()
	{
		const char *v = getenv("XBGTASIX_XBOX_DUMMY");
		return v && *v == '1';
	}();
	return on;
}

// 把 llvm 类型压成一小段可读文字，只用于诊断日志。
String describe_type(const llvm::Type *type)
{
	char buf[96];
	if (!type)
		return String("<null>");

	const unsigned id = static_cast<unsigned>(type->getTypeID());
	if (id == static_cast<unsigned>(llvm::Type::TypeID::IntegerTyID))
	{
		snprintf(buf, sizeof(buf), "i%u", type->getIntegerBitWidth());
		return String(buf);
	}
	if (id == static_cast<unsigned>(llvm::Type::TypeID::FloatTyID))
		return String("f32");
	if (id == static_cast<unsigned>(llvm::Type::TypeID::PointerTyID))
	{
		const unsigned as = type->getPointerAddressSpace();
		const String pointee = describe_type(type->getPointerElementType());
		snprintf(buf, sizeof(buf), "ptr[as%u]{%s}", as, pointee.c_str());
		return String(buf);
	}
	if (id == static_cast<unsigned>(llvm::Type::TypeID::StructTyID))
	{
		String r = "{";
		for (unsigned i = 0; i < type->getStructNumElements(); i++)
		{
			if (i)
				r += ", ";
			r += describe_type(type->getStructElementType(i));
		}
		r += "}";
		return r;
	}
	if (id == static_cast<unsigned>(llvm::Type::TypeID::VectorTyID))
	{
		const String elem = describe_type(type->getVectorElementType());
		snprintf(buf, sizeof(buf), "%s x %u", elem.c_str(), type->getVectorNumElements());
		return String(buf);
	}
	snprintf(buf, sizeof(buf), "typeid%u", id);
	return String(buf);
}

// 选择子校验：第一个实参必须是指定常量，否则明确失败（不猜语义）。
bool check_selector(const llvm::CallInst &inst, const char *symbol, uint64_t want)
{
	if (inst.getNumArgOperands() < 1)
		return false;
	const auto *sel = llvm::dyn_cast<llvm::ConstantInt>(inst.getArgOperand(0));
	if (!sel)
	{
		LOGE_INNER("[xbox-intrinsic] '%s' 的选择子不是常量，未实现的形态 -> 失败\n", symbol);
		return false;
	}
	const uint64_t value = sel->getUniqueInteger().getZExtValue();
	if (value != want)
	{
		LOGE_INNER("[xbox-intrinsic] '%s' 的选择子 0x%llx 没验证过（只验过 0x%llx）-> 失败\n",
		           symbol, static_cast<unsigned long long>(value),
		           static_cast<unsigned long long>(want));
		return false;
	}
	return true;
}

// 浮点 min/max 走 GLSL.std.450（本项目整套都用这个，SPIR-V 头里没有 core 的 OpFMin/OpFMax）。
spv::Id emit_fminmax(Converter::Impl &impl, bool is_max, spv::Id a, spv::Id b,
                     const llvm::Instruction &inst)
{
	auto *op = impl.allocate(spv::OpExtInst, &inst);
	op->add_id(impl.glsl_std450_ext);
	op->add_literal(is_max ? GLSLstd450FMax : GLSLstd450FMin);
	op->add_id(a);
	op->add_id(b);
	impl.add(op);
	return op->id;
}

// 二元运算的通用写法：allocate(opcode, &inst) 会自动带上指令自己的结果类型。
spv::Id emit_binary_op(Converter::Impl &impl, spv::Op opcode, spv::Id a, spv::Id b,
                       const llvm::Instruction &inst)
{
	auto *op = impl.allocate(opcode, &inst);
	op->add_id(a);
	op->add_id(b);
	impl.add(op);
	return op->id;
}

// ---------------------------------------------------------------------------
// xbox.BFI(sel=0x17, m, a, b) -> i32
//
// PC 上等价于：**按位选择**（bit field select），dst = (m & a) | (~m & b)。
// 依据：RDNA2 的 `V_BFI_B32` 就是这个语义（mask 在第一个源操作数）。它**不是**
// "把 a 的若干位插进 b、用 (width, offset) 指定"的那个 bfi —— 名字极易误读，
// 已按 ISA 语义核对（victory 的 RDNA2 降级实现同构）。
// SPIR-V：OpNot + OpBitwiseAnd×2 + OpBitwiseOr，逐位等价，无近似。
// ---------------------------------------------------------------------------
bool emit_bfi(Converter::Impl &impl, const llvm::CallInst &inst)
{
	if (!check_selector(inst, "xbox.BFI", 0x17) || inst.getNumArgOperands() != 4)
		return false;

	const spv::Id m = impl.get_id_for_value(inst.getArgOperand(1));
	const spv::Id a = impl.get_id_for_value(inst.getArgOperand(2));
	const spv::Id b = impl.get_id_for_value(inst.getArgOperand(3));
	if (!m || !a || !b)
		return false;

	auto *not_op = impl.allocate(spv::OpNot, &inst);
	not_op->add_id(m);
	impl.add(not_op);

	const spv::Id x = emit_binary_op(impl, spv::OpBitwiseAnd, m, a, inst);
	const spv::Id y = emit_binary_op(impl, spv::OpBitwiseAnd, not_op->id, b, inst);
	const spv::Id r = emit_binary_op(impl, spv::OpBitwiseOr, x, y, inst);

	impl.rewrite_value(&inst, r);
	return true;
}

// ---------------------------------------------------------------------------
// xbox.Med3_F32(sel=0x0a, x, y, z) -> f32
//
// PC 上等价于：三数中值 median(x,y,z) = max(min(x,y), min(max(x,y), z))。
// 依据：RDNA2 的 `V_MED3_F32`（同族的 max3/min3 在同一条线上），即三个输入的一阶统计量。
// 边界（写清楚，别当精确等价）：NaN 传播按 SPIR-V OpFMin/OpFMax 走（返回非 NaN 的那个）；
// AMD 对 NaN 的具体规则未实测，本批 shader 里也没有 NaN 路径的证据。
// ---------------------------------------------------------------------------
bool emit_med3(Converter::Impl &impl, const llvm::CallInst &inst)
{
	if (!check_selector(inst, "xbox.Med3_F32", 0x0a) || inst.getNumArgOperands() != 4)
		return false;

	const spv::Id x = impl.get_id_for_value(inst.getArgOperand(1));
	const spv::Id y = impl.get_id_for_value(inst.getArgOperand(2));
	const spv::Id z = impl.get_id_for_value(inst.getArgOperand(3));
	if (!x || !y || !z)
		return false;

	const spv::Id lo = emit_fminmax(impl, false, x, y, inst);
	const spv::Id hi = emit_fminmax(impl, true, x, y, inst);
	const spv::Id t = emit_fminmax(impl, false, hi, z, inst);
	const spv::Id r = emit_fminmax(impl, true, lo, t, inst);

	impl.rewrite_value(&inst, r);
	return true;
}


// 追一层值的来源（最多 4 跳），用来判断 A 族的 i64 地址是
// "某个描述符调用直接给的" 还是 "算出来的裸地址"。
String describe_origin(const llvm::Value *value, int depth = 0)
{
	char buf[64];
	if (!value)
		return String("<null>");
	if (depth >= 4)
		return String("...");
	if (const auto *call = llvm::dyn_cast<llvm::CallInst>(value))
		return String("call:") + get_xbox_intrinsic_name(call->getCalledFunction());
	if (const auto *arg = llvm::dyn_cast<llvm::Argument>(value))
	{
		snprintf(buf, sizeof(buf), "argument#%u", arg->getArgNo());
		return String(buf);
	}
	if (const auto *bin = llvm::dyn_cast<llvm::BinaryOperator>(value))
	{
		snprintf(buf, sizeof(buf), "binary(op%u)", static_cast<unsigned>(bin->getOpcode()));
		return String(buf) + "(" + describe_origin(bin->getOperand(0), depth + 1) + ", " +
		       describe_origin(bin->getOperand(1), depth + 1) + ")";
	}
	if (const auto *inst = llvm::dyn_cast<llvm::Instruction>(value))
	{
		if (llvm::isa<llvm::CastInst>(inst))
			return String("cast(") + describe_origin(inst->getOperand(0), depth + 1) + ")";
		if (llvm::isa<llvm::GetElementPtrInst>(inst))
			return String("gep(") + describe_origin(inst->getOperand(0), depth + 1) + ")";
		if (llvm::isa<llvm::LoadInst>(inst))
			return String("load(") + describe_origin(inst->getOperand(0), depth + 1) + ")";
		if (llvm::isa<llvm::ExtractValueInst>(inst))
			return String("extractvalue(") + describe_origin(inst->getOperand(0), depth + 1) + ")";
		if (llvm::isa<llvm::PHINode>(inst) && inst->getNumOperands() >= 1)
			return String("phi(") + describe_origin(inst->getOperand(0), depth + 1) + ")";
		return String("inst");
	}
#ifdef HAVE_LLVMBC
	return String("ssa");
#else
	return String("ssa");
#endif
}

// 未实现的符号：打一次签名（含每个实参是常量还是 SSA），然后明确失败。
void log_unimplemented(const llvm::CallInst &inst, const String &name)
{
	static Vector<String> seen;
	for (auto &s : seen)
		if (s == name)
			return;
	seen.push_back(name);

	auto *func = inst.getCalledFunction();
	auto *fn_type = func->getFunctionType();
	String sig = name + "(";
	for (uint32_t i = 0; i < fn_type->getNumParams(); i++)
	{
		if (i)
			sig += ", ";
		sig += describe_type(fn_type->getParamType(i));
	}
	sig += ") -> ";
	sig += describe_type(fn_type->getReturnType());

	LOGE_INNER("[xbox-intrinsic] 未实现：%s\n", sig.c_str());
	for (uint32_t i = 0; i < inst.getNumArgOperands(); i++)
	{
		const llvm::Value *arg = inst.getArgOperand(i);
		const auto *ci = llvm::dyn_cast<llvm::ConstantInt>(arg);
		if (ci)
			LOGE_INNER("[xbox-intrinsic]   arg%u = const 0x%llx\n", i,
			           static_cast<unsigned long long>(ci->getUniqueInteger().getZExtValue()));
		else
		{
			// 追一层来源：A 族的 i64 地址到底是"哪儿算出来的"，
			// 决定它能不能在翻译期还原成一个描述符。
			LOGE_INNER("[xbox-intrinsic]   arg%u = %s (%s)\n", i,
			           describe_origin(arg).c_str(),
			           describe_type(arg->getType()).c_str());
		}
	}
}
} // namespace

bool emit_xbox_intrinsic(Converter::Impl &impl, const llvm::CallInst &inst)
{
	auto *func = inst.getCalledFunction();
	if (!func)
		return false;

	const String name = get_xbox_intrinsic_name(func);

	if (name == "xbox.BFI")
		return emit_bfi(impl, inst);
	if (name == "xbox.Med3_F32")
		return emit_med3(impl, inst);

	log_unimplemented(inst, name);

	if (dummy_mode() && inst.getNumArgOperands() >= 0)
	{
		auto *ret_type = inst.getType();
		if (ret_type && ret_type->getTypeID() != llvm::Type::TypeID::VoidTyID)
		{
			const spv::Id zero = impl.builder().makeNullConstant(impl.get_type_id(ret_type));
			impl.rewrite_value(&inst, zero);
			LOGE_INNER("[xbox-intrinsic] 诊断模式：'%s' 用零常量占位（不代表语义）\n",
			           name.c_str());
			return true;
		}
	}
	return false;
}

} // namespace dxil_spv
