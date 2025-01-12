/**************************************************************************/
/*  gdscript_byte_codegen.cpp                                             */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "gdscript_jit_codegen.h"

#include "gdscript.h"

#include "core/debugger/engine_debugger.h"

bool GDScriptJitCodeGenerator::is_primitive_type(const Variant::Type variant_type) {
	return (
		variant_type == Variant::Type::BOOL ||
		variant_type == Variant::Type::INT ||
		variant_type == Variant::Type::FLOAT
	);
}

static void _method_bind_validated_call_wrapper(
	const MethodBind* method, Object* p_object, const Variant** p_args, Variant* r_ret
) {
	method->validated_call(p_object, p_args, r_ret);

	//print_line("validated call ret:", Variant::get_type_name(r_ret->get_type()), r_ret->stringify());
}

static void _variant_get_named_wrapper(const Variant* variant, Variant* dst, const StringName& p_member) {
	bool valid;
	*dst = variant->get_named(p_member, valid);

	//print_line("getted member:", Variant::get_type_name(dst->get_type()), dst->stringify());

	(void)valid;
}

static void _variant_set_named_wrapper(Variant* variant, const Variant& p_value, const StringName& p_member) {
	bool valid;
	variant->set_named(p_member, p_value, valid);

	//print_line("getted member:", Variant::get_type_name(dst->get_type()), dst->stringify());

	(void)valid;
}

using VariantConstructWrapper = void (*)(Variant&, const Variant**, int);

template<Variant::Type t>
static void _variant_construct_wrapper_impl(Variant& base, const Variant** argptrs, int args_count) {
	Callable::CallError error;
	Variant::construct(t, base, argptrs, args_count, error);

	//print_line("constructed:", Variant::get_type_name(base.get_type()), base.stringify());

	(void)error;
}

static void _variant_call_utility_function_wrapper(
	const StringName& p_name, Variant* r_ret, const Variant** p_args, int args_count
) {
	Callable::CallError error;
	Variant::call_utility_function(p_name, r_ret, p_args, args_count, error);

	(void)error;
}

static VariantConstructWrapper _get_variant_construct_wrapper(const Variant::Type t) {
	static constexpr VariantConstructWrapper wrappers_table[Variant::Type::VARIANT_MAX] = {
		_variant_construct_wrapper_impl<Variant::Type::NIL>,

		_variant_construct_wrapper_impl<Variant::Type::BOOL>,
		_variant_construct_wrapper_impl<Variant::Type::INT>,
		_variant_construct_wrapper_impl<Variant::Type::FLOAT>,
		_variant_construct_wrapper_impl<Variant::Type::STRING>,

		_variant_construct_wrapper_impl<Variant::Type::VECTOR2>,
		_variant_construct_wrapper_impl<Variant::Type::VECTOR2I>,
		_variant_construct_wrapper_impl<Variant::Type::RECT2>,
		_variant_construct_wrapper_impl<Variant::Type::RECT2I>,
		_variant_construct_wrapper_impl<Variant::Type::VECTOR3>,
		_variant_construct_wrapper_impl<Variant::Type::VECTOR3I>,
		_variant_construct_wrapper_impl<Variant::Type::TRANSFORM2D>,
		_variant_construct_wrapper_impl<Variant::Type::VECTOR4>,
		_variant_construct_wrapper_impl<Variant::Type::VECTOR4I>,
		_variant_construct_wrapper_impl<Variant::Type::PLANE>,
		_variant_construct_wrapper_impl<Variant::Type::QUATERNION>,
		_variant_construct_wrapper_impl<Variant::Type::AABB>,
		_variant_construct_wrapper_impl<Variant::Type::BASIS>,
		_variant_construct_wrapper_impl<Variant::Type::TRANSFORM3D>,
		_variant_construct_wrapper_impl<Variant::Type::PROJECTION>,

		_variant_construct_wrapper_impl<Variant::Type::COLOR>,
		_variant_construct_wrapper_impl<Variant::Type::STRING_NAME>,
		_variant_construct_wrapper_impl<Variant::Type::NODE_PATH>,
		_variant_construct_wrapper_impl<Variant::Type::RID>,
		_variant_construct_wrapper_impl<Variant::Type::OBJECT>,
		_variant_construct_wrapper_impl<Variant::Type::CALLABLE>,
		_variant_construct_wrapper_impl<Variant::Type::SIGNAL>,
		_variant_construct_wrapper_impl<Variant::Type::DICTIONARY>,
		_variant_construct_wrapper_impl<Variant::Type::ARRAY>,

		_variant_construct_wrapper_impl<Variant::Type::PACKED_BYTE_ARRAY>,
		_variant_construct_wrapper_impl<Variant::Type::PACKED_INT32_ARRAY>,
		_variant_construct_wrapper_impl<Variant::Type::PACKED_INT64_ARRAY>,
		_variant_construct_wrapper_impl<Variant::Type::PACKED_FLOAT32_ARRAY>,
		_variant_construct_wrapper_impl<Variant::Type::PACKED_FLOAT64_ARRAY>,
		_variant_construct_wrapper_impl<Variant::Type::PACKED_STRING_ARRAY>,
		_variant_construct_wrapper_impl<Variant::Type::PACKED_VECTOR2_ARRAY>,
		_variant_construct_wrapper_impl<Variant::Type::PACKED_VECTOR3_ARRAY>,
		_variant_construct_wrapper_impl<Variant::Type::PACKED_COLOR_ARRAY>,
		_variant_construct_wrapper_impl<Variant::Type::PACKED_VECTOR4_ARRAY>
	};

	return wrappers_table[(unsigned)t];
}

static void _variant_evaluate_wrapper(const Variant::Operator op, const Variant& left, const Variant& right, Variant& result) {
	bool valid;
	Variant::evaluate(op, left, right, result, valid);

	(void)valid;
}

static void _variant_assign_wrapper(Variant* destination, const Variant* source) {
	*destination = *source;
}

uint32_t GDScriptJitCodeGenerator::add_parameter(const StringName &p_name, bool p_is_optional, const GDScriptDataType &p_type) {
	function->_argument_count++;
	function->argument_types.push_back(p_type);
	if (p_is_optional) {
		function->_default_arg_count++;
	}

	return add_local(p_name, p_type);
}

uint32_t GDScriptJitCodeGenerator::add_local(const StringName &p_name, const GDScriptDataType &p_type) {
	int index = locals.size();

	locals.push_back(ValueReference(p_type.has_type ? p_type.builtin_type : Variant::VARIANT_MAX));
	add_stack_identifier(p_name, index);

	return index;
}

uint32_t GDScriptJitCodeGenerator::add_local_constant(const StringName &p_name, const Variant &p_constant) {
	int index = add_or_get_constant(p_constant);
	local_constants[p_name] = index;
	return index;
}

uint32_t GDScriptJitCodeGenerator::add_or_get_constant(const Variant &p_constant) {
	return get_constant_pos(p_constant);
}

uint32_t GDScriptJitCodeGenerator::add_or_get_name(const StringName &p_name) {
	return get_name_map_pos(p_name);
}

uint32_t GDScriptJitCodeGenerator::add_temporary(const GDScriptDataType &p_type) {
	const Variant::Type temp_type = p_type.has_type ? p_type.builtin_type : Variant::VARIANT_MAX;
	int index = -1;

	if (temporaries_pool.is_empty()) {
		index = locals.size();
		locals.push_back(ValueReference(temp_type));
	} else {
		int nil_i = -1;
		int primitive_i = -1;

		for (int i = 0; i < temporaries_pool.size(); ++i) {
			int temp_idx = temporaries_pool[i];
			const ValueReference& candidate = locals[temp_idx];

			if (candidate.type == temp_type) {
				index = temp_idx;
				temporaries_pool.remove_at(i);
				break;
			} else if (candidate.type == Variant::NIL) {
				nil_i = i;
			} else if (is_primitive_type(candidate.type)) {
				primitive_i = i;
			}
		}

		if (index >= 0) {
			locals.write[index].drop_cache();
		}
		// In case if there is no suitable candidate of the same type.
		else if (nil_i >= 0) {
			index = temporaries_pool[nil_i];
			temporaries_pool.remove_at(nil_i);
			locals.write[index].reuse(temp_type);
		} else if (primitive_i >= 0) {
			index = temporaries_pool[primitive_i];
			temporaries_pool.remove_at(primitive_i);
			locals.write[index].reuse(temp_type);
		} else {
			index = locals.size();
			locals.push_back(ValueReference(temp_type));
		}
	}

	temporaries.push_back(index);
	return index;
}

void GDScriptJitCodeGenerator::pop_temporary() {
	ERR_FAIL_COND(temporaries.is_empty());
	int value_idx = temporaries[temporaries.size() - 1];

	ValueReference& value = locals.write[value_idx];
	if (value.has_type()) {
		// TODO: Free temporaties that can contains object.
	}

	if (value.is_allocated()) {
		temporaries_pool.push_back(value_idx);
	} else if (value_idx == locals.size() - 1) {
		locals.remove_at(value_idx);
	}

	temporaries.remove_at(temporaries.size() - 1);
}

void GDScriptJitCodeGenerator::start_parameters() {
}

void GDScriptJitCodeGenerator::end_parameters() {
	function->default_arguments.reverse();
}

void GDScriptJitCodeGenerator::write_start(GDScript *p_script, const StringName &p_function_name, bool p_static, Variant p_rpc_config, const GDScriptDataType &p_return_type) {
	print_line("Compiling: ", p_script->get_script_path(), ":", p_function_name);

	function = memnew(GDScriptFunction);

	function->name = p_function_name;
	function->_script = p_script;
	function->source = p_script->get_script_path();

#ifdef DEBUG_ENABLED
	function->func_cname = (String(function->source) + " - " + String(p_function_name)).utf8();
	function->_func_cname = function->func_cname.get_data();
#endif

	function->_static = p_static;
	function->return_type = p_return_type;
	function->rpc_config = p_rpc_config;
	function->_argument_count = 0;
}

GDScriptFunction *GDScriptJitCodeGenerator::write_end() {
	static HashMap<GDScriptFunction*, bjit::Module> jit_modules_map;

	bjit::Module& jit_module = jit_modules_map[function];
	if (jit_module.isLoaded()) jit_module.unload();

	// Setup stacks size.
	proc.getOps()[0].imm32 = max_stack_size * sizeof(Variant);

	// Compile and debug.
	int proc_index = 0;
	{
		proc.iret(proc.lcu(0));
		proc_index = jit_module.compile(proc);

		String text;
		for (auto byte : jit_module.getBytes()) {
			text += vformat("%02x ", byte);
		}
		print_line(""); print_line(text);
	}

	jit_module.load();

	function->_jit_function = jit_module.getPointer<ProcFunction>(proc_index);
	function->_stack_size = max_stack_size;

	// Fill constants.
	function->constants.resize(constants_top);
	for (auto& constant : constants) {
		if (!constant.is_allocated()) continue;

		function->constants.write[constant.index] = *constant.constant;
	}
	function->_constants_ptr = function->constants.ptrw();
	function->_constant_count = constants_top;

	return function;
}

#ifdef DEBUG_ENABLED
void GDScriptJitCodeGenerator::set_signature(const String &p_signature) {
	function->profile.signature = p_signature;
}
#endif

void GDScriptJitCodeGenerator::set_initial_line(int p_line) {
	function->_initial_line = p_line;
}

#define HAS_BUILTIN_TYPE(m_var) \
	(m_var.type.has_type && m_var.type.kind == GDScriptDataType::BUILTIN)

#define IS_BUILTIN_TYPE(m_var, m_type) \
	(m_var.type.has_type && m_var.type.kind == GDScriptDataType::BUILTIN && m_var.type.builtin_type == m_type && m_type != Variant::NIL)

void GDScriptJitCodeGenerator::write_type_adjust(const Address &p_target, Variant::Type p_new_type) {
	// 1. TODO: Destruct previouse value if needed;
	// 2. TODO: Construct new value if needed;
	// 3. Change `type` field.

	ValueReference& value = get_value_ref(p_target);
	bjit::Value jit_ptr = emit_ptr_access(p_target);

	if (value.has_type()) {
		// Change `type` field.
		proc.si32(proc.lci(p_new_type), jit_ptr, 0);
	} else {
		emit_function_call(
			VariantInternal::initialize,
			jit_ptr,
			proc.lci(p_new_type)
		);
	}
}

void GDScriptJitCodeGenerator::write_unary_operator(const Address &p_target, Variant::Operator p_operator, const Address &p_left_operand) {
	ValueReference& operand = get_value_ref(p_left_operand);

	print_line("unary operator:", Variant::get_operator_name(p_operator), "for type:", Variant::get_type_name(operand.type));

	if (is_primitive_type(operand.type)) {
		Variant::Type ret_type = Variant::get_operator_return_type(p_operator, operand.type, Variant::NIL);
		bjit::Value jit_value = emit_data_load(operand, p_left_operand);

		print_line("\tprimitive evaluation - ret_type:", Variant::get_type_name(ret_type));

		switch (p_operator) {
			case Variant::OP_BIT_NEGATE:
				jit_value = proc.inot(jit_value);
				break;
			case Variant::OP_NEGATE:
				if (operand.type == Variant::FLOAT)
					jit_value = proc.fneg(jit_value);
				else
					jit_value = proc.ineg(jit_value);
				break;
			case Variant::OP_POSITIVE:
				// Do nothing here?
				break;
			case Variant::OP_NOT:
				// Cast to boolean?
				if (operand.type == Variant::FLOAT)
					jit_value = proc.feq(jit_value, proc.lcf(0));
				else
					jit_value = proc.ieq(jit_value, proc.lci(0));
				break;
			default:
				break;
		}

		emit_data_store(p_target, ret_type, jit_value);
		return;
	}

	bjit::Value jit_operand_ptr = emit_ptr_access(p_left_operand);
	bjit::Value jit_target_ptr = emit_ptr_access(p_target);

	if (operand.has_type()) {
		Variant::ValidatedOperatorEvaluator op_func = Variant::get_validated_operator_evaluator(p_operator, operand.type, Variant::NIL);
		if (op_func) {
			print_line("\tvalidated operator");
			emit_function_call(
				op_func,
				jit_operand_ptr,
				proc.lci(0),
				jit_target_ptr
			);
			return;
		}

		Variant::PTROperatorEvaluator ptr_op_func = Variant::get_ptr_operator_evaluator(p_operator, operand.type, Variant::NIL);
		if (ptr_op_func) {
			print_line("\tpointer operator");
			emit_function_call(
				op_func,
				jit_operand_ptr,
				proc.lci(0),
				jit_target_ptr
			);
			return;
		}
	}

	print_line("\tdynamic evaluation");
	emit_function_call(
		_variant_evaluate_wrapper,
		proc.lci((int64_t)p_operator),
		jit_operand_ptr,
		proc.lci(0),
		jit_target_ptr
	);
}

void GDScriptJitCodeGenerator::write_binary_operator(const Address &p_target, Variant::Operator p_operator, const Address &p_left_operand, const Address &p_right_operand) {
	ValueReference& lhs = get_value_ref(p_left_operand);
	ValueReference& rhs = get_value_ref(p_right_operand);

	print_line("binary operator:", Variant::get_type_name(lhs.type), Variant::get_operator_name(p_operator), Variant::get_type_name(rhs.type));

	if (!lhs.has_type() || !rhs.has_type()) {
		bjit::Value jit_lhs_ptr = emit_ptr_access(p_left_operand);
		bjit::Value jit_rhs_ptr = emit_ptr_access(p_right_operand);
		bjit::Value jit_target_ptr = emit_ptr_access(p_target);

		print_line("\tdynamic evaluation");
		emit_function_call(
			_variant_evaluate_wrapper,
			proc.lci((int64_t)p_operator),
			jit_lhs_ptr,
			jit_rhs_ptr,
			jit_target_ptr
		);

		return;
	}

	if (is_primitive_type(lhs.type) && is_primitive_type(rhs.type)) {
		Variant::Type ret_type = Variant::get_operator_return_type(p_operator, lhs.type, rhs.type);

		print_line("\tprimitive - return type:", Variant::get_type_name(ret_type));

		bjit::Value jit_lhs = emit_data_load(lhs, p_left_operand);
		bjit::Value jit_rhs = emit_data_load(rhs, p_right_operand);
		bjit::Value jit_result;

		const bool is_float = (lhs.type == Variant::FLOAT || rhs.type == Variant::FLOAT);

		// Cast to float.
		if (is_float) {
			if (lhs.type != Variant::FLOAT) {
				jit_lhs = proc.ci2d(jit_lhs);
			} else if (rhs.type != Variant::FLOAT) {
				jit_rhs = proc.ci2d(jit_rhs);
			}

			switch (p_operator) {
				case Variant::OP_ADD:
					jit_result = proc.dadd(jit_lhs, jit_rhs); break;
				case Variant::OP_SUBTRACT:
					jit_result = proc.dsub(jit_lhs, jit_rhs); break;
				case Variant::OP_MULTIPLY:
					jit_result = proc.dmul(jit_lhs, jit_rhs); break;
				case Variant::OP_DIVIDE:
					jit_result = proc.ddiv(jit_lhs, jit_rhs); break;	
				default:
					break;
			}
		} else switch (p_operator) {
			case Variant::OP_ADD:
				jit_result = proc.iadd(jit_lhs, jit_rhs); break;
			case Variant::OP_SUBTRACT:
				jit_result = proc.isub(jit_lhs, jit_rhs); break;
			case Variant::OP_MULTIPLY:
				jit_result = proc.imul(jit_lhs, jit_rhs); break;
			case Variant::OP_DIVIDE:
				jit_result = proc.idiv(jit_lhs, jit_rhs); break;	
			default:
				break;
		}

		emit_data_store(p_target, ret_type, jit_result);
	}
}

void GDScriptJitCodeGenerator::write_type_test(const Address &p_target, const Address &p_source, const GDScriptDataType &p_type) {
}

void GDScriptJitCodeGenerator::write_and_left_operand(const Address &p_left_operand) {
}

void GDScriptJitCodeGenerator::write_and_right_operand(const Address &p_right_operand) {
}

void GDScriptJitCodeGenerator::write_end_and(const Address &p_target) {
}

void GDScriptJitCodeGenerator::write_or_left_operand(const Address &p_left_operand) {
}

void GDScriptJitCodeGenerator::write_or_right_operand(const Address &p_right_operand) {
}

void GDScriptJitCodeGenerator::write_end_or(const Address &p_target) {
}

void GDScriptJitCodeGenerator::write_start_ternary(const Address &p_target) {
}

void GDScriptJitCodeGenerator::write_ternary_condition(const Address &p_condition) {
}

void GDScriptJitCodeGenerator::write_ternary_true_expr(const Address &p_expr) {
}

void GDScriptJitCodeGenerator::write_ternary_false_expr(const Address &p_expr) {
}

void GDScriptJitCodeGenerator::write_end_ternary() {
}

void GDScriptJitCodeGenerator::write_set(const Address &p_target, const Address &p_index, const Address &p_source) {
}

void GDScriptJitCodeGenerator::write_get(const Address &p_target, const Address &p_index, const Address &p_source) {
}

void GDScriptJitCodeGenerator::write_set_named(const Address &p_target, const StringName &p_name, const Address &p_source) {
}

void GDScriptJitCodeGenerator::write_get_named(const Address &p_target, const StringName &p_name, const Address &p_source) {
}

void GDScriptJitCodeGenerator::write_set_member(const Address &p_value, const StringName &p_name) {
}

void GDScriptJitCodeGenerator::write_get_member(const Address &p_target, const StringName &p_name) {
}

void GDScriptJitCodeGenerator::write_set_static_variable(const Address &p_value, const Address &p_class, int p_index) {
}

void GDScriptJitCodeGenerator::write_get_static_variable(const Address &p_target, const Address &p_class, int p_index) {
}

void GDScriptJitCodeGenerator::write_assign_with_conversion(const Address &p_target, const Address &p_source) {
}

void GDScriptJitCodeGenerator::write_assign(const Address &p_target, const Address &p_source) {
	ValueReference& source = get_value_ref(p_source);

	print_line("assign for: ", Variant::get_type_name(source.type));

	if (is_primitive_type(source.type)) {
		print_line("\tprimitive!");
		bjit::Value jit_value = emit_data_load(source, p_source);
		emit_data_store(p_target, source.type, jit_value);
		return;
	}

	print_line("\twrapper call");	
	emit_function_call(
		_variant_assign_wrapper,
		emit_ptr_access(p_target),
		emit_ptr_access(p_source)
	);
}

void GDScriptJitCodeGenerator::write_assign_null(const Address &p_target) {
}

void GDScriptJitCodeGenerator::write_assign_true(const Address &p_target) {
}

void GDScriptJitCodeGenerator::write_assign_false(const Address &p_target) {
}

void GDScriptJitCodeGenerator::write_assign_default_parameter(const Address &p_dst, const Address &p_src, bool p_use_conversion) {
}

void GDScriptJitCodeGenerator::write_store_global(const Address &p_dst, int p_global_index) {
}

void GDScriptJitCodeGenerator::write_store_named_global(const Address &p_dst, const StringName &p_global) {
}

void GDScriptJitCodeGenerator::write_cast(const Address &p_target, const Address &p_source, const GDScriptDataType &p_type) {
}

void GDScriptJitCodeGenerator::write_call(const Address &p_target, const Address &p_base, const StringName &p_function_name, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_super_call(const Address &p_target, const StringName &p_function_name, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_call_async(const Address &p_target, const Address &p_base, const StringName &p_function_name, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_call_gdscript_utility(const Address &p_target, const StringName &p_function, const Vector<Address> &p_arguments) {
	print_line("gdscript utility call");
}

void GDScriptJitCodeGenerator::write_call_utility(const Address &p_target, const StringName &p_function, const Vector<Address> &p_arguments) {
	print_line("utility call");

	const StringName* name_ptr = get_name_ptr(p_function);

	emit_function_call(
		_variant_call_utility_function_wrapper,
		proc.lcu((uintptr_t)name_ptr),
		emit_ptr_access(p_target),
		emit_load_ptr_args(p_arguments),
		proc.lcu(p_arguments.size())
	);
}

void GDScriptJitCodeGenerator::write_call_builtin_type(const Address &p_target, const Address &p_base, Variant::Type p_type, const StringName &p_method, bool p_is_static, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_call_builtin_type(const Address &p_target, const Address &p_base, Variant::Type p_type, const StringName &p_method, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_call_builtin_type_static(const Address &p_target, Variant::Type p_type, const StringName &p_method, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_call_native_static(const Address &p_target, const StringName &p_class, const StringName &p_method, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_call_native_static_validated(const GDScriptCodeGenerator::Address &p_target, MethodBind *p_method, const Vector<GDScriptCodeGenerator::Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_call_method_bind(const Address &p_target, const Address &p_base, MethodBind *p_method, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_call_method_bind_validated(const Address &p_target, const Address &p_base, MethodBind *p_method, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_call_self(const Address &p_target, const StringName &p_function_name, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_call_self_async(const Address &p_target, const StringName &p_function_name, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_call_script_function(const Address &p_target, const Address &p_base, const StringName &p_function_name, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_lambda(const Address &p_target, GDScriptFunction *p_function, const Vector<Address> &p_captures, bool p_use_self) {
}

void GDScriptJitCodeGenerator::write_construct(const Address &p_target, Variant::Type p_type, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_construct_array(const Address &p_target, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_construct_typed_array(const Address &p_target, const GDScriptDataType &p_element_type, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_construct_dictionary(const Address &p_target, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_construct_typed_dictionary(const Address &p_target, const GDScriptDataType &p_key_type, const GDScriptDataType &p_value_type, const Vector<Address> &p_arguments) {
}

void GDScriptJitCodeGenerator::write_await(const Address &p_target, const Address &p_operand) {
}

void GDScriptJitCodeGenerator::write_if(const Address &p_condition) {
}

void GDScriptJitCodeGenerator::write_else() {
}

void GDScriptJitCodeGenerator::write_endif() {
}

void GDScriptJitCodeGenerator::write_jump_if_shared(const Address &p_value) {
}

void GDScriptJitCodeGenerator::write_end_jump_if_shared() {
}

void GDScriptJitCodeGenerator::start_for(const GDScriptDataType &p_iterator_type, const GDScriptDataType &p_list_type) {
}

void GDScriptJitCodeGenerator::write_for_assignment(const Address &p_list) {
}

void GDScriptJitCodeGenerator::write_for(const Address &p_variable, bool p_use_conversion) {
}

void GDScriptJitCodeGenerator::write_endfor() {
}

void GDScriptJitCodeGenerator::start_while_condition() {
}

void GDScriptJitCodeGenerator::write_while(const Address &p_condition) {
}

void GDScriptJitCodeGenerator::write_endwhile() {
}

void GDScriptJitCodeGenerator::write_break() {
}

void GDScriptJitCodeGenerator::write_continue() {
}

void GDScriptJitCodeGenerator::write_breakpoint() {
}

void GDScriptJitCodeGenerator::write_newline(int p_line) {
}

void GDScriptJitCodeGenerator::write_return(const Address &p_return_value) {
	proc.iret(proc.lci(0));
}

void GDScriptJitCodeGenerator::write_assert(const Address &p_test, const Address &p_message) {
}

void GDScriptJitCodeGenerator::start_block() {
}

void GDScriptJitCodeGenerator::end_block() {
}

void GDScriptJitCodeGenerator::clear_temporaries() {
	// TODO
}

void GDScriptJitCodeGenerator::clear_address(const Address &p_address) {
	// Do not check `is_local_dirty()` here! Always clear the address since the codegen doesn't track the compiler.
	// Also, this method is used to initialize local variables of built-in types, since they cannot be `null`.

	if (p_address.type.has_type && p_address.type.kind == GDScriptDataType::BUILTIN) {
		switch (p_address.type.builtin_type) {
			case Variant::BOOL:
				write_assign_false(p_address);
				break;
			case Variant::DICTIONARY:
				if (p_address.type.has_container_element_types()) {
					write_construct_typed_dictionary(p_address, p_address.type.get_container_element_type_or_variant(0), p_address.type.get_container_element_type_or_variant(1), Vector<GDScriptCodeGenerator::Address>());
				} else {
					write_construct(p_address, p_address.type.builtin_type, Vector<GDScriptCodeGenerator::Address>());
				}
				break;
			case Variant::ARRAY:
				if (p_address.type.has_container_element_type(0)) {
					write_construct_typed_array(p_address, p_address.type.get_container_element_type(0), Vector<GDScriptCodeGenerator::Address>());
				} else {
					write_construct(p_address, p_address.type.builtin_type, Vector<GDScriptCodeGenerator::Address>());
				}
				break;
			case Variant::NIL:
			case Variant::OBJECT:
				write_assign_null(p_address);
				break;
			default:
				write_construct(p_address, p_address.type.builtin_type, Vector<GDScriptCodeGenerator::Address>());
				break;
		}
	} else {
		write_assign_null(p_address);
	}

	if (p_address.mode == Address::LOCAL_VARIABLE) {
		dirty_locals.erase(p_address.address);
	}
}

// Returns `true` if the local has been reused and not cleaned up with `clear_address()`.
bool GDScriptJitCodeGenerator::is_local_dirty(const Address &p_address) const {
	ERR_FAIL_COND_V(p_address.mode != Address::LOCAL_VARIABLE, false);
	return dirty_locals.has(p_address.address);
}

GDScriptJitCodeGenerator::~GDScriptJitCodeGenerator() {
}
