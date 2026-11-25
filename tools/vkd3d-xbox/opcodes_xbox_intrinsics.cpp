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
#include "dxil/dxil_buffer.hpp"

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
	// 注意：中间结果**不能**用 allocate(op, &inst) —— 那会拿 llvm 值本身的 SPIR-V id
	// 当结果 id，同一条指令发第二条 op 时就会出现重复 Id（spirv-val: "Id N is defined
	// more than once"），驱动直接拒掉整条 pipeline。中间结果一律用显式类型 id。
	auto *op = impl.allocate(spv::OpExtInst, impl.get_type_id(inst.getType()));
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
	// 同 emit_fminmax：中间结果用显式类型 id，别复用 llvm 值的 id。
	auto *op = impl.allocate(opcode, impl.get_type_id(inst.getType()));
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


// ===========================================================================
// 第二批：TypedLoad + D 族（CNDMASK / ALIGNBYTE）+ C 族（wave 6 个）
//
// 实参形态与选择子不是猜的：53 个 shader 的 DXBC 里 DXIL chunk 就是 LLVM
// bitcode，llvm-dis 反汇编后每个 `call ... @xbox.X` 的实参逐字可查
// （DXIL program header：+8 = bitcode 偏移，+12 = bitcode 长度）。下面每条
// 选择子都是全量语料里唯一出现过的那个值。
// ===========================================================================

// 当前 lane 在 wave 内的编号（0 .. waveSize-1）。
static spv::Id emit_current_lane_id(Converter::Impl &impl)
{
	auto &builder = impl.builder();
	spv::Id var = impl.spirv_module.get_builtin_shader_input(spv::BuiltInSubgroupLocalInvocationId);
	auto *load = impl.allocate(spv::OpLoad, builder.makeUintType(32));
	load->add_id(var);
	impl.add(load);
	return load->id;
}

// 显式给出结果类型的二元运算（OpIEqual 之类的结果类型和操作数不同，不能用 emit_binary_op）
static spv::Id emit_typed_op2(Converter::Impl &impl, spv::Op opcode, spv::Id type_id, spv::Id a, spv::Id b)
{
	auto *op = impl.allocate(opcode, type_id);
	op->add_id(a);
	op->add_id(b);
	impl.add(op);
	return op->id;
}

static spv::Id emit_component_extract(Converter::Impl &impl, spv::Id composite, unsigned index, spv::Id type_id)
{
	auto *op = impl.allocate(spv::OpCompositeExtract, type_id);
	op->add_id(composite);
	op->add_literal(index);
	impl.add(op);
	return op->id;
}

// ---------------------------------------------------------------------------
// xbox.TypedLoad.i32(sel=0x8a, {ptr[as0]{i8}} desc, i32 byteOffset, i32 fmt=0x41)
//   -> ResRet.i32 { i32 x4, i32 status }
//
// 句柄来自 `dx.op.createHandle`（真描述符，不是裸地址），所以**不碰 VA**：
// 直接复用 vkd3d/dxil-spirv 已有的 buffer-load 通道（对 RawBuffer SRV 就是
// texel-buffer 的 OpImageFetch，访问掩码由 llvm_composite_meta 决定）。
// 依据：cs_49 里这个句柄是 `createHandle(i8 1, 0, 0)`，它在 !dx.resources 里
// 声明为 kind=11（RawBuffer），与同 shader 里 `dx.op.rawBufferLoad.i64` 用
// 同一张表；TypedLoad 的实参 2 按 RawBuffer 规则就是**字节偏移**。
// fmt=0x41 只见过这一个值：结果按 4 个原始 dword 取（cs_49 拿到后自己做位域
// 拆包：`& 15`、`<< 8 |`），所以这里不做任何格式转换——不是"忽略"，是 shader
// 的用法证明它就该是原始位。
// ---------------------------------------------------------------------------
bool emit_typed_load(Converter::Impl &impl, const llvm::CallInst &inst)
{
	if (!check_selector(inst, "xbox.TypedLoad.i32", 0x8a) || inst.getNumArgOperands() != 4)
		return false;

	const auto *fmt = llvm::dyn_cast<llvm::ConstantInt>(inst.getArgOperand(3));
	if (!fmt || fmt->getUniqueInteger().getZExtValue() != 0x41)
	{
		LOGE_INNER("[xbox-intrinsic] TypedLoad 的 fmt 不是核对过的 0x41 -> 失败（不猜格式）\n");
		return false;
	}

	const auto *handle = llvm::dyn_cast<llvm::CallInst>(inst.getArgOperand(1));
	if (!handle || !handle->getCalledFunction() ||
	    strncmp(handle->getCalledFunction()->getName().data(), "dx.op.createHandle", 18) != 0)
	{
		LOGE_INNER("[xbox-intrinsic] TypedLoad 的句柄不是 dx.op.createHandle -> 失败\n");
		return false;
	}

	return emit_buffer_load_instruction(impl, &inst, false);
}

// ---------------------------------------------------------------------------
// xbox.V_ALIGNBYTE_B32(sel=0x14c, s0, s1, s2) -> i32
//
// PC 上等价于：把 64 位 {s0:s1} 逻辑右移 ((s2 & 3) * 8) 位，取低 32 位。
// 依据：victory 的 RDNA2 降级实现
// `crates/victory-shader/src/emit/lower.rs` 里 "v_alignbit_b32" | "v_alignbyte_b32"
// 分支：alignbyte 的移位量是 `(s2 & 3) << 3`（字节粒度，最大 24），alignbit 才是
// `s2 & 31`；两者共用 `(s1 >> sh) | (s0 << ((32 - sh) & 31))`，并且 sh == 0 时
// 要把高位那一半清零（否则 s0 << 0 会把 s0 漏进结果）。
// ---------------------------------------------------------------------------
bool emit_alignbyte(Converter::Impl &impl, const llvm::CallInst &inst)
{
	if (!check_selector(inst, "xbox.V_ALIGNBYTE_B32", 0x14c) || inst.getNumArgOperands() != 4)
		return false;

	const spv::Id s0 = impl.get_id_for_value(inst.getArgOperand(1));
	const spv::Id s1 = impl.get_id_for_value(inst.getArgOperand(2));
	const spv::Id s2 = impl.get_id_for_value(inst.getArgOperand(3));
	if (!s0 || !s1 || !s2)
		return false;

	auto &builder = impl.builder();
	const spv::Id u32 = builder.makeUintType(32);
	const spv::Id zero = builder.makeUintConstant(0);

	const spv::Id sh = emit_binary_op(impl, spv::OpShiftLeftLogical,
	                                  emit_binary_op(impl, spv::OpBitwiseAnd, s2, builder.makeUintConstant(3), inst),
	                                  builder.makeUintConstant(3), inst);
	const spv::Id lo = emit_binary_op(impl, spv::OpShiftRightLogical, s1, sh, inst);
	const spv::Id nsh = emit_binary_op(impl, spv::OpBitwiseAnd,
	                                   emit_binary_op(impl, spv::OpISub, builder.makeUintConstant(32), sh, inst),
	                                   builder.makeUintConstant(31), inst);
	const spv::Id hi = emit_binary_op(impl, spv::OpShiftLeftLogical, s0, nsh, inst);
	const spv::Id is_zero = emit_typed_op2(impl, spv::OpIEqual, builder.makeBoolType(), sh, zero);

	auto *hi_masked = impl.allocate(spv::OpSelect, u32);
	hi_masked->add_id(is_zero);
	hi_masked->add_id(zero);
	hi_masked->add_id(hi);
	impl.add(hi_masked);

	impl.rewrite_value(&inst, emit_binary_op(impl, spv::OpBitwiseOr, lo, hi_masked->id, inst));
	return true;
}

// ---------------------------------------------------------------------------
// xbox.V_CNDMASK_B32(sel=0xaa, {i32,i32} vcc, {i32,i32} {s0, s1}) -> i32
//
// PC 上等价于：**按 lane 的整 dword 选择**，dst = vcc[lane] ? s1 : s0。
// （不是"按位选择"——按位读法会让 cs_22 的 5 步 wave32 前缀和直接算错，见下。）
//
// 证据链（三条互相独立）：
// 1. cs_29 的实参来源：arg1 = `xbox.Ballot64(25, ...)` 的返回值（64 位 lane 掩码），
//    arg2 = `insertvalue {undef,s..}, %v0, %v1` 拼出来的两个标量 -> arg1 是 VCC，
//    arg2 是 (s0, s1) 这一对候选值。实参角色由此确定，不是从名字猜的。
// 2. cs_22 的用法：掩码恒为 0xAAAAAAAA/0xCCCCCCCC/0xF0F0F0F0/0xFF00FF00/0xFFFF0000
//    （bit i = (i & 2^k) != 0），候选对是 {0, 1}，结果再 `icmp ne 0` 当"该 lane 参不
//    参与这一步扫描"。按 lane 选：lane0 的 bit 是 0 -> 取 s0 = 0 -> 跳过 ✓；
//    按位选：结果是 ~mask（恒非零）-> 所有 lane 都参与 -> 扫描算错 ✗。
// 3. victory 的 RDNA2 降级实现 `"v_cndmask_b32" => select(cond, s1, s0)` 同构。
//
// wave 宽度：掩码是 64 位（两个 dword），lane 编号 >= 32 取高半部分；wave32 下
// 高半部分自然用不到。所以这里把两半拼成 u64 后按 lane & 63 取位，两种宽度都对。
// ---------------------------------------------------------------------------
bool emit_cndmask(Converter::Impl &impl, const llvm::CallInst &inst)
{
	if (!check_selector(inst, "xbox.V_CNDMASK_B32", 0xaa) || inst.getNumArgOperands() != 3)
		return false;

	const spv::Id vcc = impl.get_id_for_value(inst.getArgOperand(1));
	const spv::Id operands = impl.get_id_for_value(inst.getArgOperand(2));
	if (!vcc || !operands)
		return false;

	auto &builder = impl.builder();
	const spv::Id u32 = builder.makeUintType(32);
	const spv::Id u64 = builder.makeUintType(64);

	const spv::Id vcc_lo = emit_component_extract(impl, vcc, 0, u32);
	const spv::Id vcc_hi = emit_component_extract(impl, vcc, 1, u32);
	const spv::Id s0 = emit_component_extract(impl, operands, 0, u32);
	const spv::Id s1 = emit_component_extract(impl, operands, 1, u32);

	auto *lo64 = impl.allocate(spv::OpUConvert, u64);
	lo64->add_id(vcc_lo);
	impl.add(lo64);
	auto *hi64 = impl.allocate(spv::OpUConvert, u64);
	hi64->add_id(vcc_hi);
	impl.add(hi64);
	const spv::Id hi_shifted = emit_typed_op2(impl, spv::OpShiftLeftLogical, u64, hi64->id,
	                                          builder.makeUint64Constant(32));
	const spv::Id mask64 = emit_typed_op2(impl, spv::OpBitwiseOr, u64, lo64->id, hi_shifted);

	const spv::Id lane = emit_current_lane_id(impl);
	const spv::Id lane_masked = emit_typed_op2(impl, spv::OpBitwiseAnd, u32, lane, builder.makeUintConstant(63));
	auto *lane64 = impl.allocate(spv::OpUConvert, u64);
	lane64->add_id(lane_masked);
	impl.add(lane64);

	const spv::Id bit = emit_typed_op2(impl, spv::OpBitwiseAnd, u64,
	                                   emit_typed_op2(impl, spv::OpShiftRightLogical, u64, mask64, lane64->id),
	                                   builder.makeUint64Constant(1));
	const spv::Id cond = emit_typed_op2(impl, spv::OpINotEqual, builder.makeBoolType(), bit,
	                                    builder.makeUint64Constant(0));

	auto *select_op = impl.allocate(spv::OpSelect, u32);
	select_op->add_id(cond);
	select_op->add_id(s1);
	select_op->add_id(s0);
	impl.add(select_op);
	impl.rewrite_value(&inst, select_op->id);
	return true;
}

// ---------------------------------------------------------------------------
// xbox.GetLaneID(sel=0x40) -> i32
// 当前 lane 在 wave 内的编号。SPIR-V：gl_SubgroupLocalInvocationId。
// ---------------------------------------------------------------------------
bool emit_get_lane_id(Converter::Impl &impl, const llvm::CallInst &inst)
{
	if (!check_selector(inst, "xbox.GetLaneID", 0x40) || inst.getNumArgOperands() != 1)
		return false;

	impl.rewrite_value(&inst, emit_current_lane_id(impl));
	return true;
}

// ---------------------------------------------------------------------------
// xbox.ReadLane.i32(sel=0x33, value, lane) -> i32
// 取 lane 号那一路的 value。SPIR-V：OpGroupNonUniformShuffle（subgroup shuffle）。
// 与 DXIL 的 WaveReadLaneAt 是同一件事；这里不做 "常量 lane 转 broadcast" 的
// 优化分支（dxil_waveops 那套是为 pixel shader 的 quad 优化，这里没有收益）。
// ---------------------------------------------------------------------------
bool emit_read_lane(Converter::Impl &impl, const llvm::CallInst &inst)
{
	if (!check_selector(inst, "xbox.ReadLane.i32", 0x33) || inst.getNumArgOperands() != 3)
		return false;

	const spv::Id value = impl.get_id_for_value(inst.getArgOperand(1));
	const spv::Id lane = impl.get_id_for_value(inst.getArgOperand(2));
	if (!value || !lane)
		return false;

	auto &builder = impl.builder();
	auto *op = impl.allocate(spv::OpGroupNonUniformShuffle, &inst);
	op->add_id(builder.makeUintConstant(spv::ScopeSubgroup));
	op->add_id(value);
	op->add_id(lane);
	impl.add(op);
	builder.addCapability(spv::CapabilityGroupNonUniformShuffle);

	impl.rewrite_value(&inst, op->id);
	return true;
}

// ---------------------------------------------------------------------------
// xbox.LaneSwizzle.i32(sel=0x35, value, pattern) -> i32
//
// 语义（推导 + 数值验证，见下）：
//   m = 1 << (ctz(pattern) - 1)                  // pattern = 0b1111 << (ctz)
//   src_lane = (lane & ~(2m - 1)) | (m - 1)      // 每组 2m 个 lane 广播第 m-1 路
//   dst = value(src_lane)
//
// 为什么是这个语义：全量语料里 pattern 只有 5 个值 30/60/120/240/480
// （= 0b1111 << 1..5），而它们恰好只出现在同一张"5 步 wave32 前缀和"网上：
// cs_22..cs_29 里每一步都是
//     s = LaneSwizzle(x, P); c = V_CNDMASK(vcc_{i&2^k}, {0,1}) != 0; x = x + (c ? s : 0)
// 掩码 bit i = (i & 2^k) != 0。要让这个网算对，第 k 步参战的 lane 必须拿到
// "本组已完成的 2^k 长度块和"，而持有它的唯一 lane 就是 (lane & ~(2m-1)) | (m-1)。
// 这一步是可验证的：把该规则代进网络，200 组随机输入**全部**等于包含式前缀和；
// 换成"普通 shfl_up 2^k"则 200 组全部不等。所以不是猜，是唯一满足数据流的解。
//
// 关键简化：**非参战 lane 的返回值不可观测**——每个 LaneSwizzle 的结果都只喂给
// `select(mask, s, 0)`（75 个调用点全部核过），所以这个规则只要在参战 lane 上
// 正确就够，不需要知道主机 ISA 的那条底层指令长什么样。
// ---------------------------------------------------------------------------
bool emit_lane_swizzle(Converter::Impl &impl, const llvm::CallInst &inst)
{
	if (!check_selector(inst, "xbox.LaneSwizzle.i32", 0x35) || inst.getNumArgOperands() != 3)
		return false;

	const auto *pattern = llvm::dyn_cast<llvm::ConstantInt>(inst.getArgOperand(2));
	if (!pattern)
	{
		LOGE_INNER("[xbox-intrinsic] LaneSwizzle 的 pattern 不是常量 -> 失败（不猜语义）\n");
		return false;
	}

	const uint64_t bits = pattern->getUniqueInteger().getZExtValue();
	unsigned ctz = 0;
	while (ctz < 64 && ((bits >> ctz) & 1) == 0)
		ctz++;

	// 只接受 0b1111 << ctz（ctz >= 1）的形状；其它形状一律失败，别推广。
	if (ctz < 1 || ctz > 59 ||
	    bits != ((1ull << (ctz + 4)) - (1ull << ctz)))
	{
		LOGE_INNER("[xbox-intrinsic] LaneSwizzle pattern 0x%llx 不是已验证的 0b1111<<k 形状 -> 失败\n",
		           static_cast<unsigned long long>(bits));
		return false;
	}

	const uint64_t m = 1ull << (ctz - 1);
	const uint64_t group_mask = ~((m << 1) - 1);

	const spv::Id value = impl.get_id_for_value(inst.getArgOperand(1));
	if (!value)
		return false;

	auto &builder = impl.builder();
	const spv::Id u32 = builder.makeUintType(32);
	const spv::Id lane = emit_current_lane_id(impl);
	const spv::Id base = emit_typed_op2(impl, spv::OpBitwiseAnd, u32, lane,
	                                    builder.makeUintConstant(static_cast<uint32_t>(group_mask)));
	const spv::Id src_lane = emit_typed_op2(impl, spv::OpBitwiseOr, u32, base,
	                                        builder.makeUintConstant(static_cast<uint32_t>(m - 1)));

	auto *op = impl.allocate(spv::OpGroupNonUniformShuffle, &inst);
	op->add_id(builder.makeUintConstant(spv::ScopeSubgroup));
	op->add_id(value);
	op->add_id(src_lane);
	impl.add(op);
	builder.addCapability(spv::CapabilityGroupNonUniformShuffle);

	impl.rewrite_value(&inst, op->id);
	return true;
}

// ---------------------------------------------------------------------------
// xbox.MakeUniform.i32 / .i64(sel=0x32, value) -> value
//
// 把一个值 "在 wave 内统一"（主机侧是 scalar 化：取一 lane 的值发给全 wave）。
// SPIR-V：OpGroupNonUniformBroadcastFirst。用 BroadcastFirst 而不是
// Broadcast(lane=0)，是因为它按**活动 lane** 取第一个——与 dxil_waveops 里
// WaveReadLaneFirst 的处理一致（`emit_wave_read_lane_first_instruction`）。
// 边界：如果源值在 wave 内本来就一致（编译器插 MakeUniform 的常见情形），两者
// 完全等价；若不一致，主机语义取 lane0，这里取第一个活动 lane —— 记录为近似，
// 本批语料里没有分歧路径的证据（cs_21/cs_43 的实参都是广播来的标量）。
// ---------------------------------------------------------------------------
bool emit_make_uniform(Converter::Impl &impl, const llvm::CallInst &inst)
{
	if (!check_selector(inst, "xbox.MakeUniform", 0x32) || inst.getNumArgOperands() != 2)
		return false;

	const spv::Id value = impl.get_id_for_value(inst.getArgOperand(1));
	if (!value)
		return false;

	auto &builder = impl.builder();
	auto *op = impl.allocate(spv::OpGroupNonUniformBroadcastFirst, &inst);
	op->add_id(builder.makeUintConstant(spv::ScopeSubgroup));
	op->add_id(value);
	impl.add(op);
	builder.addCapability(spv::CapabilityGroupNonUniformBallot);

	impl.rewrite_value(&inst, op->id);
	return true;
}

// ---------------------------------------------------------------------------
// xbox.Ballot64(sel=0x19, i32 pred) -> {i32, i32}
//
// 64 位 lane 投票：bit i = 第 i 路 pred 非零。SPIR-V 的
// OpGroupNonUniformBallot 返回 uvec4，低两个 dword 就是需要的那 64 位
// （wave32 时高半部分为 0，正好是 "64 位掩码" 的零扩展）。
// 注意实参是 i32 不是 i1：要先 `!= 0` 变成 bool。
// ---------------------------------------------------------------------------
bool emit_ballot64(Converter::Impl &impl, const llvm::CallInst &inst)
{
	if (!check_selector(inst, "xbox.Ballot64", 0x19) || inst.getNumArgOperands() != 2)
		return false;

	const spv::Id pred = impl.get_id_for_value(inst.getArgOperand(1));
	if (!pred)
		return false;

	auto &builder = impl.builder();
	const spv::Id u32 = builder.makeUintType(32);
	const spv::Id cond = emit_typed_op2(impl, spv::OpINotEqual, builder.makeBoolType(), pred,
	                                    builder.makeUintConstant(0));

	auto *ballot = impl.allocate(spv::OpGroupNonUniformBallot, builder.makeVectorType(u32, 4));
	ballot->add_id(builder.makeUintConstant(spv::ScopeSubgroup));
	ballot->add_id(cond);
	impl.add(ballot);
	builder.addCapability(spv::CapabilityGroupNonUniformBallot);

	auto *construct = impl.allocate(spv::OpCompositeConstruct, &inst);
	construct->add_id(emit_component_extract(impl, ballot->id, 0, u32));
	construct->add_id(emit_component_extract(impl, ballot->id, 1, u32));
	impl.add(construct);

	impl.rewrite_value(&inst, construct->id);
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


// ---------------------------------------------------------------------------
// 内存访问族：xbox.global_load_dword{,x2,x3,x4}_with_cache(sel, i64 addr, i32, i32)
//
// PC 上等价于：**从 64 位地址读 N 个 dword**。
// `_with_cache` 是主机侧的缓存策略提示，Vulkan 侧由内存模型 + barrier 承担，
// 对一个普通 load 没有额外语义 —— 但**地址空间是真的**：这个 i64 是裸 GPU 地址，
// 所以必须走 PhysicalStorageBuffer（OpConvertUToPtr + OpLoad），不能当缓冲区偏移。
//
// 前提（重要）：整个模块必须在 PhysicalStorageBuffer64 寻址模型下编译。
// dxil-spirv 由 `options.physical_storage_buffer` 决定，而 vkd3d 只在"有 RAW_VA 绑定或
// 根描述符"时才开。我们的根签名两者都没有，所以实验期用 `XBGTASIX_FORCE_BDA=1`
// （见 vkd3d-patch/libs/vkd3d-shader/dxil.c 的 VKD3D_FORCE_BDA_EXPERIMENT）。
// ---------------------------------------------------------------------------
bool emit_global_load(Converter::Impl &impl, const llvm::CallInst &inst, unsigned count)
{
	// 选择子在这族里**不是**每符号恒定的常量：cs_03 是 0x164、cs_04 是 0x166。
	// 两者都是"读一个 dword"，所以在 Vulkan 侧是同一条 OpLoad；差别（缓存策略/变体）
	// 由内存模型承担。这里用一张已知表 + 首次出现时打日志，表外的值明确失败。
	static Vector<uint64_t> seen_selectors;
	uint64_t selector = 0;
	{
		const auto *sel = inst.getNumArgOperands() >= 1
		                      ? llvm::dyn_cast<llvm::ConstantInt>(inst.getArgOperand(0))
		                      : nullptr;
		if (!sel)
		{
			LOGE_INNER("[xbox-intrinsic] 内存访问族的选择子不是常量 -> 失败\n");
			return false;
		}
		selector = sel->getUniqueInteger().getZExtValue();
		// 观测到的值：0x164 / 0x166 / 0x167 / 0x16c —— 都在 0x16x 这一档。
		// 已确认**同一符号**会给出不同选择子（cs_03 是 0x164、cs_04 是 0x166），
		// 而语义由符号名（读几个 dword）决定，所以按"0x160..0x16f 这一档"接受；
		// 每个新值首次出现时打日志留痕，超出这一档明确失败。
		if ((selector & ~static_cast<uint64_t>(0xF)) != 0x160)
		{
			LOGE_INNER("[xbox-intrinsic] 内存访问族选择子 0x%llx 不在已知档位 -> 失败\n",
			           static_cast<unsigned long long>(selector));
			return false;
		}
		bool logged = false;
		for (auto v : seen_selectors)
			if (v == selector)
				logged = true;
		if (!logged)
		{
			seen_selectors.push_back(selector);
			LOGW_INNER("[xbox-intrinsic] 内存访问族首次见到选择子 0x%llx（按普通 load 处理）\n",
			           static_cast<unsigned long long>(selector));
		}
	}
	if (inst.getNumArgOperands() < 2)
		return false;

	const spv::Id addr = impl.get_id_for_value(inst.getArgOperand(1));
	if (!addr)
		return false;

	auto &builder = impl.builder();
	const spv::Id u32_type = builder.makeUintType(32);
	const spv::Id u64_type = builder.makeUintType(64);
	const spv::Id ptr_type = builder.makePointer(spv::StorageClassPhysicalStorageBuffer, u32_type);

	spv::Id components[4] = {};
	for (unsigned i = 0; i < count; i++)
	{
		spv::Id element_addr = addr;
		if (i != 0)
		{
			auto *add = impl.allocate(spv::OpIAdd, u64_type);
			add->add_id(addr);
			add->add_id(builder.makeUintConstant(4ull * i));
			impl.add(add);
			element_addr = add->id;
		}

		auto *cast = impl.allocate(spv::OpConvertUToPtr, ptr_type);
		cast->add_id(element_addr);
		impl.add(cast);

		auto *load = impl.allocate(spv::OpLoad, u32_type);
		load->add_id(cast->id);
		impl.add(load);
		components[i] = load->id;
	}

	if (count == 1)
	{
		impl.rewrite_value(&inst, components[0]);
		return true;
	}

	auto *construct = impl.allocate(spv::OpCompositeConstruct, &inst);
	for (unsigned i = 0; i < count; i++)
		construct->add_id(components[i]);
	impl.add(construct);
	impl.rewrite_value(&inst, construct->id);
	return true;
}


// ---------------------------------------------------------------------------
// 三数统计族的其余变体：Min3 / Max3 / Med3 的 f32 与 u32
// PC 上就是 min/max 的组合，逐位/逐值等价，无近似（f32 的 NaN 规则同 Med3_F32 的说明）。
// ---------------------------------------------------------------------------
bool emit_minmax3(Converter::Impl &impl, const llvm::CallInst &inst, bool is_min, bool is_max,
                  bool is_float)
{
	if (inst.getNumArgOperands() != 4)
		return false;
	const spv::Id x = impl.get_id_for_value(inst.getArgOperand(1));
	const spv::Id y = impl.get_id_for_value(inst.getArgOperand(2));
	const spv::Id z = impl.get_id_for_value(inst.getArgOperand(3));
	if (!x || !y || !z)
		return false;

	auto bin = [&](spv::Id a, spv::Id b, bool want_max)
	{
		if (is_float)
			return emit_fminmax(impl, want_max, a, b, inst);
	{
		auto *op = impl.allocate(spv::OpExtInst, &inst);
		op->add_id(impl.glsl_std450_ext);
		op->add_literal(want_max ? GLSLstd450UMax : GLSLstd450UMin);
		op->add_id(a);
		op->add_id(b);
		impl.add(op);
		return op->id;
	}
	};

	spv::Id r;
	if (is_min)
	{
		const spv::Id t = bin(x, y, false);
		r = bin(t, z, false);
	}
	else if (is_max)
	{
		const spv::Id t = bin(x, y, true);
		r = bin(t, z, true);
	}
	else
	{
		const spv::Id lo = bin(x, y, false);
		const spv::Id hi = bin(x, y, true);
		const spv::Id t = bin(hi, z, false);
		r = bin(lo, t, true);
	}

	impl.rewrite_value(&inst, r);
	return true;
}

// ---------------------------------------------------------------------------
// xbox.global_store_dwordx3_with_cache(sel, i64 addr, {i32,i32,i32}, i32, i32) -> void
// PC 上等价于：往 64 位地址连续写 3 个 dword（PhysicalStorageBuffer，与 load 族同一前提）。
// ---------------------------------------------------------------------------
bool emit_global_store3(Converter::Impl &impl, const llvm::CallInst &inst)
{
	if (inst.getNumArgOperands() < 3)
		return false;
	const spv::Id addr = impl.get_id_for_value(inst.getArgOperand(1));
	const spv::Id value = impl.get_id_for_value(inst.getArgOperand(2));
	if (!addr || !value)
		return false;

	auto &builder = impl.builder();
	const spv::Id u32_type = builder.makeUintType(32);
	const spv::Id u64_type = builder.makeUintType(64);
	const spv::Id ptr_type = builder.makePointer(spv::StorageClassPhysicalStorageBuffer, u32_type);

	for (unsigned i = 0; i < 3; i++)
	{
		spv::Id element_addr = addr;
		if (i != 0)
		{
			auto *add = impl.allocate(spv::OpIAdd, u64_type);
			add->add_id(addr);
			add->add_id(builder.makeUintConstant(4ull * i));
			impl.add(add);
			element_addr = add->id;
		}

		auto *cast = impl.allocate(spv::OpConvertUToPtr, ptr_type);
		cast->add_id(element_addr);
		impl.add(cast);

		auto *extract = impl.allocate(spv::OpCompositeExtract, u32_type);
		extract->add_id(value);
		extract->add_literal(i);
		impl.add(extract);

		auto *store = impl.allocate(spv::OpStore);
		store->add_id(cast->id);
		store->add_id(extract->id);
		impl.add(store);
	}
	return true;
}

// ---------------------------------------------------------------------------
// xbox.global_atomic_add(sel, i64 addr, i32 value) -> i32
// PC 上等价于：对 64 位地址上的 i32 做 OpAtomicIAdd，返回旧值。
// scope 用 Device、语义 AcquireRelease —— 与 vkd3d 对 UAV 原子的一贯取法一致；
// 主机侧的缓存策略位在这里没有额外语义（由 barrier 承担）。
// ---------------------------------------------------------------------------
bool emit_global_atomic(Converter::Impl &impl, const llvm::CallInst &inst, spv::Op opcode,
                        bool is_float_minmax)
{
	if (inst.getNumArgOperands() < 3)
		return false;
	const spv::Id addr = impl.get_id_for_value(inst.getArgOperand(1));
	const spv::Id value = impl.get_id_for_value(inst.getArgOperand(2));
	if (!addr || !value)
		return false;

	auto &builder = impl.builder();
	const spv::Id value_type = impl.get_type_id(inst.getType());
	const spv::Id ptr_type = builder.makePointer(spv::StorageClassPhysicalStorageBuffer, value_type);

	if (is_float_minmax)
	{
		// SPV_EXT_shader_atomic_float_min_max（OpAtomicFMinEXT/FMaxEXT = 5614/5615，
		// 这份 spv::Op 枚举里没有，按规范编号强转）
		builder.addCapability(static_cast<spv::Capability>(5612));
		builder.addExtension("SPV_EXT_shader_atomic_float_min_max");
	}

	auto *cast = impl.allocate(spv::OpConvertUToPtr, ptr_type);
	cast->add_id(addr);
	impl.add(cast);

	auto *op = impl.allocate(opcode, &inst);
	op->add_id(cast->id);
	op->add_id(builder.makeUintConstant(spv::ScopeDevice));
	op->add_id(builder.makeUintConstant(spv::MemorySemanticsUniformMemoryMask |
	                                    spv::MemorySemanticsAcquireReleaseMask));
	op->add_id(value);
	impl.add(op);
	impl.rewrite_value(&inst, op->id);
	return true;
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

	// B 族其余变体
	if (name == "xbox.Min3_F32")
		return emit_minmax3(impl, inst, true, false, true);
	if (name == "xbox.Max3_F32")
		return emit_minmax3(impl, inst, false, true, true);
	if (name == "xbox.Min3_U32")
		return emit_minmax3(impl, inst, true, false, false);
	if (name == "xbox.Max3_U32")
		return emit_minmax3(impl, inst, false, true, false);
	if (name == "xbox.Med3_U32")
		return emit_minmax3(impl, inst, false, false, false);

	// D 族 / C 族 / TypedLoad（第二批）
	if (name == "xbox.TypedLoad.i32")
		return emit_typed_load(impl, inst);
	if (name == "xbox.V_CNDMASK_B32")
		return emit_cndmask(impl, inst);
	if (name == "xbox.V_ALIGNBYTE_B32")
		return emit_alignbyte(impl, inst);
	if (name == "xbox.GetLaneID")
		return emit_get_lane_id(impl, inst);
	if (name == "xbox.ReadLane.i32")
		return emit_read_lane(impl, inst);
	if (name == "xbox.LaneSwizzle.i32")
		return emit_lane_swizzle(impl, inst);
	if (name == "xbox.MakeUniform.i32" || name == "xbox.MakeUniform.i64")
		return emit_make_uniform(impl, inst);
	if (name == "xbox.Ballot64")
		return emit_ballot64(impl, inst);

	// A 族其余几条
	if (name == "xbox.global_store_dwordx3_with_cache")
		return emit_global_store3(impl, inst);
	if (name == "xbox.global_atomic_add")
		return emit_global_atomic(impl, inst, spv::OpAtomicIAdd, false);
	if (name == "xbox.global_atomic_sub")
		return emit_global_atomic(impl, inst, spv::OpAtomicISub, false);
	if (name == "xbox.global_atomic_fmin")
		return emit_global_atomic(impl, inst, static_cast<spv::Op>(5614), true);
	if (name == "xbox.global_atomic_fmax")
		return emit_global_atomic(impl, inst, static_cast<spv::Op>(5615), true);

	// A 族：裸地址读（选择子取自实测，未见过的值一律失败，不猜）
	if (name == "xbox.global_load_dword_with_cache")
		return emit_global_load(impl, inst, 1);
	if (name == "xbox.global_load_dwordx2_with_cache")
		return emit_global_load(impl, inst, 2);
	if (name == "xbox.global_load_dwordx3_with_cache")
		return emit_global_load(impl, inst, 3);
	if (name == "xbox.global_load_dwordx4_with_cache")
		return emit_global_load(impl, inst, 4);

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
