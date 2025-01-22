/**************************************************************************/
/*  gdscript_jit_codegen.cpp                                              */
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

static void _method_bind_call_wrapper(
	const MethodBind* method, Object* p_object, const Variant** p_args, int p_arg_count
) {
	Callable::CallError error;
	method->call(p_object, p_args, p_arg_count, error);

	(void)error;
}

static void _method_bind_validated_call_wrapper(
	const MethodBind* method, Object* p_object, const Variant** p_args, Variant* r_ret
) {
	//print_line("validated call ret:", Variant::get_type_name(r_ret->get_type()), r_ret->stringify());

	method->validated_call(p_object, p_args, r_ret);
}

static void _variant_get_named_wrapper(const Variant* variant, const StringName& p_member, Variant* dst) {
	bool valid;
	*dst = variant->get_named(p_member, valid);

	(void)valid;
}

static void _variant_set_named_wrapper(Variant* variant, const StringName& p_member, const Variant& p_value) {
	bool valid;
	variant->set_named(p_member, p_value, valid);

	//print_line("getted member:", Variant::get_type_name(dst->get_type()), dst->stringify());

	(void)valid;
}

static void _variant_get_member_wrapper(Object* instance, const StringName& p_member, Variant& p_target) {
	p_target = instance->get(p_member);
}

static void _variant_set_member_wrapper(Object* instance, const StringName& p_member, const Variant& p_value) {
	instance->set(p_member, p_value);
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

HashMap<GDScriptFunction*, bjit::Module> GDScriptJitCodeGenerator::jit_modules_map = {};

void GDScriptJitCodeGenerator::emit_assign(ValueRef& p_target, ValueRef& p_source) {
	if (p_target.type->is_dynamic() && !p_target.type->is_nil()) goto wrapper_call;

	if (p_source.type->is_native() && (p_target.type->is_native() || p_target.type->is_nil())) {
		print_line("\tnative assign");

		if (p_target.type->is_native()) {
			// Cast.
			p_target.update_value(emit_cast_native(emit_get_native(p_source), p_source.type, p_target.type));
		} else if (p_target.type->is_nil() && p_target.can_be_cached()) {
			// Assign with type change.
			bjit::Value jit_value = emit_get_native(p_source);
			emit_set_native(p_target, p_source.type, jit_value);
		} else {
			goto wrapper_call;
		}
		return;
	} else if (p_source.type->is_builtin() && (p_source.type == p_target.type || p_target.type->is_nil())) {
		print_line("\tbuiltin assign");
		p_target.drop_cached();

		try_alloc_variant_object(p_source);
		try_alloc_variant_object(p_target);

		GDScriptJit::memcpy_aligned(
			proc, p_target.ptr,
			p_target.offset + _variant_data_field_offset,
			p_source.ptr,
			p_source.offset + _variant_data_field_offset,
			p_source.type->size
		);

		p_target.type = p_source.type;
		return;
	}

wrapper_call:
	print_line("\tdynamic assign");
	emit_function_call(
		_variant_assign_wrapper,
		emit_ptr_to_object(p_target),
		emit_ptr_to_object(p_source)
	);

	p_target.drop_cached();
	p_target.type = p_source.type;
}

uint32_t GDScriptJitCodeGenerator::add_parameter(const StringName &p_name, bool p_is_optional, const GDScriptDataType &p_type) {
	function->_argument_count++;
	function->argument_types.push_back(p_type);
	if (p_is_optional) {
		function->_default_arg_count++;
	}

	int index = arguments.size();
	arguments.append(ValueRef(Address::FUNCTION_PARAMETER, p_type));
	arguments.write[index].offset = sizeof(Variant*) * index;

	return index;
}

uint32_t GDScriptJitCodeGenerator::add_local(const StringName &p_name, const GDScriptDataType &p_type) {
	int index = locals.size();

	locals.push_back(ValueRef(Address::LOCAL_VARIABLE, p_type));
	add_stack_identifier(p_name, index);

	return index;
}

uint32_t GDScriptJitCodeGenerator::add_local_constant(const StringName &p_name, const Variant &p_constant) {
	int index = add_or_get_constant(p_constant);
	local_constants[p_name] = index;
	return index;
}

uint32_t GDScriptJitCodeGenerator::add_or_get_constant(const Variant &p_constant) {
	if (constant_map.has(p_constant)) {
		return constant_map[p_constant];
	}

	int idx = constants.size();
	constants.push_back(ValueRef(p_constant));
	constant_values.insert(idx, p_constant);
	constant_map[p_constant] = idx;

	return idx;
}

uint32_t GDScriptJitCodeGenerator::add_or_get_name(const StringName &p_name) {
	return get_name_map_pos(p_name);
}

uint32_t GDScriptJitCodeGenerator::add_temporary(const GDScriptDataType &p_type) {
	const Variant::Type temp_type = p_type.has_type ? p_type.builtin_type : Variant::NIL;
	int index = -1;

	if (temporaries_pool.is_empty()) {
		index = locals.size();
		locals.push_back(ValueRef(Address::TEMPORARY, p_type));
	} else {
		int nil_i = -1;
		int native_i = -1;

		for (int i = 0; i < temporaries_pool.size(); ++i) {
			int temp_idx = temporaries_pool[i];
			const ValueRef& candidate = locals[temp_idx];

			if (candidate.type->variant_type == temp_type) {
				index = temp_idx;
				temporaries_pool.remove_at(i);
				break;
			} else if (candidate.type->variant_type == Variant::NIL) {
				nil_i = i;
			} else if (candidate.type->is_native()) {
				native_i = i;
			}
		}

		if (index >= 0) {
			locals.write[index].drop_cached();
		}
		// In case if there is no suitable candidate of the same type.
		else if (nil_i >= 0) {
			index = temporaries_pool[nil_i];
			temporaries_pool.remove_at(nil_i);
			locals.write[index].update_type(temp_type);
		} else if (native_i >= 0) {
			index = temporaries_pool[native_i];
			temporaries_pool.remove_at(native_i);
			locals.write[index].update_type(temp_type);
		} else {
			index = locals.size();
			locals.push_back(ValueRef(Address::TEMPORARY, p_type));
		}
	}

	temporaries.push_back(index);
	return index;
}

void GDScriptJitCodeGenerator::pop_temporary() {
	ERR_FAIL_COND(temporaries.is_empty());
	int value_idx = temporaries[temporaries.size() - 1];

	ValueRef& value = locals.write[value_idx];
	if (!value.type->is_builtin() && value.type->is_native()) {
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

	jit_module = &jit_modules_map[function];

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
	// Setup stack size.
	proc.getOps()[0].imm32 = max_stack_size * sizeof(Variant);

	if (jit_module->isLoaded()) jit_module->unload();

	// Compile and debug.
	int proc_index = 0;
	{
		proc.iret(proc.lcu(0));
		proc_index = jit_module->compile(proc);

		// Hexdump of machine code.
		print_line("machine code:");
		String text;
		for (auto byte : jit_module->getBytes()) {
			text += vformat("%02x ", byte);
		}
	 	print_line(text); print_line("");
	}

	jit_module->load();

	function->_jit_function = jit_module->getPointer<ProcFunction>(proc_index);
	function->_stack_size = max_stack_size;
	function->_constants_ptr = function->constants.ptrw();
	function->_constant_count = function->constants.size();

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

void GDScriptJitCodeGenerator::write_type_adjust(const Address &p_target, Variant::Type p_new_type) {
	// 1. TODO: Destruct previouse value if needed;
	// 2. TODO: Construct new value if needed;
	// 3. Change `type` field.

	ValueRef& target = get_value_mut_ref(p_target);
	print_line("type adjust:", target.stringify(), "->", Variant::get_type_name(p_new_type));

	if (target.type->is_native() || target.type->is_builtin()) {
		target.drop_cached();
		target.update_type(p_new_type);
	} else {
		// TODO: Destruct ?
		target.state = ValueRef::UNCHANGED;
		emit_function_call(
			VariantInternal::initialize,
			emit_ptr_to_object(target),
			proc.lci(p_new_type)
		);
		target.evaluate(p_new_type);

		return;
	}
}

void GDScriptJitCodeGenerator::write_unary_operator(const Address &p_target, Variant::Operator p_operator, const Address &p_left_operand) {
	ValueRef& operand = get_value_ref(p_left_operand);
	ValueRef& target = get_value_mut_ref(p_target);

	print_line("unary operator:", Variant::get_operator_name(p_operator), operand.stringify());
	GDScriptJit::UnaryOperatorCodeGenFunc op_codegen = GDScriptJit::get_unary_operator(p_operator, operand.type->variant_type);

	if (op_codegen) {
		Variant::Type ret_type = Variant::get_operator_return_type(p_operator, operand.type->variant_type, Variant::NIL);

		print_line("\tresult type:", Variant::get_type_name(ret_type));
		bjit::Value jit_value = operand.type->is_native() ? emit_get_native_abi_compat(operand) : emit_ptr_to_data(operand);
		bjit::Value jit_result = op_codegen(proc, jit_value);

		ValueRef source(ValueRef::EXTERNAL, ret_type, jit_result);
		emit_assign(target, source);

		return;
	}

	print_line("\tdynamic evaluation");
	bjit::Value jit_operand_ptr = emit_ptr_to_object(operand);
	bjit::Value jit_target_ptr = emit_ptr_to_object(target);

	if (operand.type->variant_type != Variant::NIL) {
		Variant::Type ret_type = Variant::get_operator_return_type(p_operator, operand.type->variant_type, Variant::NIL);
		print_line("\tresult type:", Variant::get_type_name(ret_type));
		target.evaluate(ret_type);
	} else {
		print_line("\tresult type: unknown (nil)");
		target.evaluate(Variant::NIL);
	}

	emit_function_call(
		_variant_evaluate_wrapper,
		proc.lci((int64_t)p_operator),
		jit_operand_ptr,
		proc.lci(0),
		jit_target_ptr
	);
}

void GDScriptJitCodeGenerator::write_binary_operator(const Address &p_target, Variant::Operator p_operator, const Address &p_left_operand, const Address &p_right_operand) {
	ValueRef& lhs = get_value_ref(p_left_operand);
	ValueRef& rhs = get_value_ref(p_right_operand);
	ValueRef& target = get_value_mut_ref(p_target);

	print_line(
		"binary operator:",
		target.stringify(), "=",
		lhs.stringify(),
		Variant::get_operator_name(p_operator),
		rhs.stringify()
	);

	const GDScriptJit::BinaryOperatorCodeGenFunc op_codegen =
		GDScriptJit::get_binary_operator(p_operator, lhs.type->variant_type, rhs.type->variant_type);

	if (op_codegen) {
		Variant::Type ret_type = Variant::get_operator_return_type(p_operator, lhs.type->variant_type, rhs.type->variant_type);

		print_line("\tnative - result type:", Variant::get_type_name(ret_type));
		bjit::Value jit_lhs = lhs.type->is_native() ? emit_get_native_abi_compat(lhs) : emit_ptr_to_data(lhs);
		bjit::Value jit_rhs = rhs.type->is_native() ? emit_get_native_abi_compat(rhs) : emit_ptr_to_data(rhs);
		bjit::Value jit_result = op_codegen(proc, jit_lhs, jit_rhs);

		ValueRef source(ValueRef::EXTERNAL, ret_type, jit_result);
		emit_assign(target, source);
		return;
	}

	{
		print_line("\tdynamic evaluation");
		bjit::Value jit_lhs_ptr = emit_ptr_to_object(lhs);
		bjit::Value jit_rhs_ptr = emit_ptr_to_object(rhs);
		bjit::Value jit_target_ptr = emit_ptr_to_object(target);

		if (lhs.type->variant_type != Variant::NIL && rhs.type->variant_type != Variant::NIL) {
			Variant::Type ret_type = Variant::get_operator_return_type(p_operator, lhs.type->variant_type, rhs.type->variant_type);
			print_line("\tresult type:", Variant::get_type_name(ret_type));
			target.evaluate(ret_type);
		} else {
			print_line("\tresult type: unknown (nil)");
			target.evaluate(Variant::NIL);
		}

		emit_function_call(
			_variant_evaluate_wrapper,
			proc.lci((int64_t)p_operator),
			jit_lhs_ptr,
			jit_rhs_ptr,
			jit_target_ptr
		);
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
	ValueRef& source = get_value_ref(p_source);
	ValueRef& target = get_value_mut_ref(p_target);

	print_line("set named:", target.stringify() + "." + p_name, ":=", source.stringify());

	if (target.type->is_builtin()) {
		if (!target.type->fields.has(p_name)) goto wrapper_call;
		const GDScriptJit::TypeInfo::FieldInfo& field_info = target.type->fields.get(p_name);

		if (
			(field_info.type->is_dynamic() && !field_info.type->is_nil()) ||
			(field_info.type->is_builtin() && source.type != field_info.type)
		) goto wrapper_call;

		try_alloc_variant_object(target);

		ValueRef field(ValueRef::EXTERNAL, field_info.type, target.ptr, target.offset + field_info.offset);
		emit_assign(field, source);
		emit_sync_value_cache(field, false);
		return;
	}

wrapper_call:
	print_line("\twrapper");

	bjit::Value jit_source_ptr = emit_ptr_to_object(source);
	bjit::Value jit_target_ptr = emit_ptr_to_object(target);

	emit_function_call(
		_variant_set_named_wrapper,
		jit_target_ptr,
		emit_name_ptr(p_name),
		jit_source_ptr
	);
}

void GDScriptJitCodeGenerator::write_get_named(const Address &p_target, const StringName &p_name, const Address &p_source) {
	ValueRef& source = get_value_ref(p_source);
	ValueRef& target = get_value_mut_ref(p_target);

	print_line("get named:", target.stringify(), ":=", source.stringify() + "." + p_name);

	if (source.type->is_builtin()) {
		if (!source.type->fields.has(p_name)) goto wrapper_call;
		const GDScriptJit::TypeInfo::FieldInfo& field_info = source.type->fields.get(p_name);

		if (
			field_info.type->is_dynamic() ||
			(target.type->is_dynamic() && !target.type->is_nil()) ||
			(field_info.type->is_builtin() && source.type != field_info.type)
		) goto wrapper_call;

		try_alloc_variant_object(source);

		ValueRef field(ValueRef::EXTERNAL, field_info.type, source.ptr, source.offset + field_info.offset);
		emit_assign(target, field);
		return;
	}

wrapper_call:
	print_line("\twrapper");
	bjit::Value jit_source_ptr = emit_ptr_to_object(source);
	bjit::Value jit_target_ptr = emit_ptr_to_object(target);

	target.update_type(Variant::get_member_type(source.type->variant_type, p_name));

	emit_function_call(
		_variant_get_named_wrapper,
		jit_source_ptr,
		emit_name_ptr(p_name),
		jit_target_ptr
	);
}

void GDScriptJitCodeGenerator::write_set_member(const Address &p_value, const StringName &p_name) {
	ValueRef& value = get_value_ref(p_value);
	print_line("set member:", p_name, "<-", value.stringify());

	const MemberInfo member_info = get_member_info(p_name);

	if (member_info.index >= 0) {
		ERR_FAIL_MSG("Set member for script members is not implemented");	
	} else if (member_info.setter) {
		print_line("\tmethod bind setter");
		emit_function_call(
			_method_bind_validated_call_wrapper,
			proc.lcu((uintptr_t)member_info.setter),
			proc.env[ENV_INSTANCE],
			emit_load_ptr_args(p_value),
			proc.lci(0)
		);
	} else {
		print_line("\tsetter wrapper");
		emit_function_call(
			_variant_set_member_wrapper,
			proc.env[ENV_INSTANCE],
			emit_name_ptr(p_name),
			emit_ptr_to_object(value)
		);
	}
}

void GDScriptJitCodeGenerator::write_get_member(const Address &p_target, const StringName &p_name) {
	ValueRef& target = get_value_mut_ref(p_target);
	print_line("get member:", p_name, "->", target.stringify());

	const MemberInfo member_info = get_member_info(p_name);
	const GDScriptJit::TypeInfo* type_info = GDScriptJit::TypeInfo::from_variant(member_info.type);

	bjit::Value jit_target_ptr = emit_ptr_to_object(target);

	if (member_info.index >= 0) {
		ERR_FAIL_MSG("Get member for script members is not implemented");
	} else if (member_info.getter) {
		print_line("\tmethod bind getter");
		emit_function_call(
			_method_bind_validated_call_wrapper,
			proc.lcu((uintptr_t)member_info.getter),
			proc.env[ENV_INSTANCE],
			proc.lci(0),
			jit_target_ptr
		);
	} else {
		print_line("\tgetter wrapper");
		emit_function_call(
			_variant_get_member_wrapper,
			proc.env[ENV_INSTANCE],
			emit_name_ptr(p_name),
			jit_target_ptr
		);
	}

	if (type_info) {
		target.drop_cached();
		target.type = type_info;
	} else {
		target.evaluate(Variant::NIL);
	}
}

void GDScriptJitCodeGenerator::write_set_static_variable(const Address &p_value, const Address &p_class, int p_index) {
	ValueRef& value = get_value_ref(p_value);

	// FIXME: Test it!
	Variant* target_ptr = &function->_script->static_variables.ptrw()[p_index];

	bjit::Value jit_value_ptr = emit_ptr_to_object(value);

	emit_function_call(
		_variant_assign_wrapper,
		proc.lcu((uintptr_t)target_ptr),
		jit_value_ptr
	);
}

void GDScriptJitCodeGenerator::write_get_static_variable(const Address &p_target, const Address &p_class, int p_index) {
	ValueRef& target = get_value_mut_ref(p_target);

	// FIXME: Test it!
	Variant* value_ptr = &function->_script->static_variables.ptrw()[p_index];

	bjit::Value jit_target_ptr = emit_ptr_to_object(target);

	emit_function_call(
		_variant_assign_wrapper,
		jit_target_ptr,
		proc.lcu((uintptr_t)value_ptr)
	);
}

void GDScriptJitCodeGenerator::write_assign_with_conversion(const Address &p_target, const Address &p_source) {
}

void GDScriptJitCodeGenerator::write_assign(const Address &p_target, const Address &p_source) {
	ValueRef& source = get_value_ref(p_source);
	ValueRef& target = get_value_mut_ref(p_target);

	print_line("assign", target.stringify(), ":=", source.stringify());

	emit_assign(target, source);
}

void GDScriptJitCodeGenerator::write_assign_null(const Address &p_target) {
	ValueRef& target = get_value_mut_ref(p_target);

	target.drop_cached();
	target.update_type(Variant::NIL);
}

void GDScriptJitCodeGenerator::write_assign_true(const Address &p_target) {
	ValueRef& target = get_value_mut_ref(p_target);
	emit_set_native(target, GDScriptJit::TypeInfo::from<bool>(), proc.lci(1));
}

void GDScriptJitCodeGenerator::write_assign_false(const Address &p_target) {
	ValueRef& target = get_value_mut_ref(p_target);
	emit_set_native(target, GDScriptJit::TypeInfo::from<bool>(), proc.lci(0));
}

void GDScriptJitCodeGenerator::write_assign_default_parameter(const Address &p_dst, const Address &p_src, bool p_use_conversion) {
	ValueRef& source = get_value_ref(p_src);
	ValueRef& destination = get_value_mut_ref(p_dst);

	emit_assign(destination, source);
}

void GDScriptJitCodeGenerator::write_store_global(const Address &p_dst, int p_global_index) {
	constexpr unsigned _globals_ptr_offset = offsetof(GDScriptLanguage, _global_array);
	GDScriptLanguage* language_ptr = GDScriptLanguage::get_singleton();

	bjit::Value jit_globals_ptr = proc.li64(proc.lcu((uintptr_t)language_ptr), _globals_ptr_offset);
	Variant::Type global_type = language_ptr->get_global_array()[p_global_index].get_type();

	ValueRef& destination = get_value_mut_ref(p_dst);
	ValueRef global(ValueRef::EXTERNAL, global_type, jit_globals_ptr, p_global_index * sizeof(Variant));

	emit_assign(destination, global);
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
}

void GDScriptJitCodeGenerator::write_call_utility(const Address &p_target, const StringName &p_function, const Vector<Address> &p_arguments) {
	print_line("utility call:", p_function);

	bool is_return = Variant::has_utility_function_return_value(p_function);
	Variant::Type ret_type = is_return ? Variant::get_utility_function_return_type(p_function) : Variant::NIL;
	ValueRef* target = is_return ? &get_value_mut_ref(p_target) : nullptr;

	print_line("\treturns:", is_return, "target:", target ? target->stringify() : "none");

	if (Variant::get_utility_function_argument_count(p_function) == 1 &&
		Variant::get_utility_function_argument_type(p_function, 0) == Variant::NIL
	) {
		Variant::ValidatedUtilityFunction utility_func = Variant::get_validated_utility_function(p_function);

		bjit::Value jit_args = emit_load_ptr_args(p_arguments);
		bjit::Value jit_target_ptr = is_return ? emit_ptr_to_object(*target) : proc.lci(0);

		if (is_return) {
			target->update_type(Variant::get_utility_function_return_type(p_function));
		}

		print_line("\tvalidated");
		emit_function_call(
			utility_func,
			jit_target_ptr,
			jit_args,
			proc.lcu(p_arguments.size())
		);
		return;
	}

	print_line("\tnative");
	int jit_utility_index = get_utility_function_index(p_function);
	emit_load_args(p_arguments, p_function);

	bjit::Value jit_result {0};

	if (ret_type == Variant::FLOAT) {
		jit_result = proc.dcalln(jit_utility_index, p_arguments.size());
	} else {
		jit_result = proc.icalln(jit_utility_index, p_arguments.size());
	}

	proc.env.resize(proc.env.size() - p_arguments.size());
	if (!is_return) return;

	jit_result = emit_cast_native(jit_result, GDScriptJit::TypeInfo::from_variant(ret_type), target->type);
	emit_set_native(*target, target->type, jit_result);
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
	print_line("method bind:", get_value_ref(p_base).stringify() + "." + (String)p_method->get_name() + "(...)", "->", get_value_ref(p_target).stringify());

	//emit_function_call(
	//	_method_bind_call_wrapper,
	//	proc.lcu((uintptr_t)p_method),
	//);
}

void GDScriptJitCodeGenerator::write_call_method_bind_validated(const Address &p_target, const Address &p_base, MethodBind *p_method, const Vector<Address> &p_arguments) {
	print_line("method bind validated:", get_value_ref(p_base).stringify() + "." + (String)p_method->get_name() + "(...)", "->", get_value_ref(p_target).stringify());

	ValueRef& base = get_value_ref(p_base);
	ValueRef& target = get_value_mut_ref(p_target);

	bjit::Value jit_object_ptr;
	bjit::Value jit_ptr_args = emit_load_ptr_args(p_arguments);
	bjit::Value jit_target = emit_ptr_to_object(target);

	if (base.mode == ValueRef::CONSTANT) {
		const Variant& object = get_constant_value(base);
		jit_object_ptr = proc.lcu((uintptr_t)object.get_validated_object());
	} else {
		ERR_FAIL_MSG("Not implemented");
		return;
	}

	emit_function_call(
		_method_bind_validated_call_wrapper,
		proc.lcu((uintptr_t)p_method),
		jit_object_ptr,
		jit_ptr_args,
		jit_target
	);
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
	ValueRef& condition = get_value_ref(p_condition);

	DEV_ASSERT(condition.type->variant_type == Variant::BOOL);

	int lable_idx = jit_labels.size();
	jit_labels.resize(jit_labels.size() + 3);

	jit_labels.write[lable_idx]     = proc.newLabel(); // true case
	jit_labels.write[lable_idx + 1] = proc.newLabel(); // false case
	jit_labels.write[lable_idx + 2] = proc.newLabel(); // after

	proc.jnz(emit_get_native(condition), jit_labels[lable_idx], jit_labels[lable_idx + 1]);
	proc.emitLabel(jit_labels[lable_idx]);
}

void GDScriptJitCodeGenerator::write_else() {
	proc.jmp(jit_labels[jit_labels.size() - 1]);
	proc.emitLabel(jit_labels[jit_labels.size() - 2]);
}

void GDScriptJitCodeGenerator::write_endif() {
	proc.jmp(jit_labels[jit_labels.size() - 1]);
	proc.emitLabel(jit_labels[jit_labels.size() - 1]);

	jit_labels.resize(jit_labels.size() - 3);
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
	// TODO
}

// FIXME: This code copied from `gdscript_byte_codegen.cpp`
// Returns `true` if the local has been reused and not cleaned up with `clear_address()`.
bool GDScriptJitCodeGenerator::is_local_dirty(const Address &p_address) const {
	ERR_FAIL_COND_V(p_address.mode != Address::LOCAL_VARIABLE, false);
	return dirty_locals.has(p_address.address);
}

GDScriptJitCodeGenerator::~GDScriptJitCodeGenerator() {
}
