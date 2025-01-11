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

static _FORCE_INLINE_ bool is_primitive_type(const Variant::Type variant_type) {
	return (
		variant_type == Variant::Type::BOOL ||
		variant_type == Variant::Type::INT ||
		variant_type == Variant::Type::FLOAT
	);
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
	StackSlot slot(p_type.builtin_type, p_type.can_contain_object());
	int index = locals.size();

	locals.push_back(slot);
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
	Variant::Type temp_type = Variant::NIL;
	if (p_type.has_type && p_type.kind == GDScriptDataType::BUILTIN) {
		switch (p_type.builtin_type) {
			case Variant::NIL:
			case Variant::BOOL:
			case Variant::INT:
			case Variant::FLOAT:
			case Variant::STRING:
			case Variant::VECTOR2:
			case Variant::VECTOR2I:
			case Variant::RECT2:
			case Variant::RECT2I:
			case Variant::VECTOR3:
			case Variant::VECTOR3I:
			case Variant::TRANSFORM2D:
			case Variant::VECTOR4:
			case Variant::VECTOR4I:
			case Variant::PLANE:
			case Variant::QUATERNION:
			case Variant::AABB:
			case Variant::BASIS:
			case Variant::TRANSFORM3D:
			case Variant::PROJECTION:
			case Variant::COLOR:
			case Variant::STRING_NAME:
			case Variant::NODE_PATH:
			case Variant::RID:
			case Variant::CALLABLE:
			case Variant::SIGNAL:
				temp_type = p_type.builtin_type;
				break;
			case Variant::OBJECT:
			case Variant::DICTIONARY:
			case Variant::ARRAY:
			case Variant::PACKED_BYTE_ARRAY:
			case Variant::PACKED_INT32_ARRAY:
			case Variant::PACKED_INT64_ARRAY:
			case Variant::PACKED_FLOAT32_ARRAY:
			case Variant::PACKED_FLOAT64_ARRAY:
			case Variant::PACKED_STRING_ARRAY:
			case Variant::PACKED_VECTOR2_ARRAY:
			case Variant::PACKED_VECTOR3_ARRAY:
			case Variant::PACKED_COLOR_ARRAY:
			case Variant::PACKED_VECTOR4_ARRAY:
			case Variant::VARIANT_MAX:
				// Arrays, dictionaries, and objects are reference counted, so we don't use the pool for them.
				temp_type = Variant::NIL;
				break;
		}
	}

	if (!temporaries_pool.has(temp_type)) {
		temporaries_pool[temp_type] = List<int>();
	}

	List<int> &pool = temporaries_pool[temp_type];
	int slot;

	if (pool.is_empty()) {
		StackSlot new_temp(temp_type, p_type.can_contain_object());
		slot = temporaries.size();

		pool.push_back(slot);
		temporaries.push_back(new_temp);
	} else {
		slot = pool.front()->get();
		pool.pop_front();
	}

	used_temporaries.push_back(slot);
	return slot;
}

void GDScriptJitCodeGenerator::pop_temporary() {
	ERR_FAIL_COND(used_temporaries.is_empty());
	int slot_idx = used_temporaries.back()->get();

	if (temporaries[slot_idx].can_contain_object) {
		// Avoid keeping in the stack long-lived references to objects,
		// which may prevent `RefCounted` objects from being freed.
		// However, the cleanup will be performed an the end of the
		// statement, to allow object references to survive chaining.
		temporaries_pending_clear.insert(slot_idx);
	}
	temporaries_pool[temporaries[slot_idx].type].push_back(slot_idx);
	used_temporaries.pop_back();
}

void GDScriptJitCodeGenerator::start_parameters() {
}

void GDScriptJitCodeGenerator::end_parameters() {
	function->default_arguments.reverse();
}

void GDScriptJitCodeGenerator::write_start(GDScript *p_script, const StringName &p_function_name, bool p_static, Variant p_rpc_config, const GDScriptDataType &p_return_type) {
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
}

void GDScriptJitCodeGenerator::write_unary_operator(const Address &p_target, Variant::Operator p_operator, const Address &p_left_operand) {
	if (HAS_BUILTIN_TYPE(p_left_operand)) {
		// Gather specific operator.
		Variant::Type operand_type = p_left_operand.type.builtin_type;
		Variant::Type ret_type = Variant::get_operator_return_type(p_operator, operand_type, Variant::NIL);

		if (is_primitive_type(operand_type)) {
			bjit::Value jit_value = get_jit_data(p_left_operand);
			

			switch (operand_type) {
				case Variant::OP_BIT_NEGATE:
					 proc->ineg(jit_value);
					break;
				case Variant::OP_NEGATE:
					break;
				case Variant::OP_POSITIVE:
					break;
				case Variant::OP_NOT:
					//proc->bc
					break;
				default:
					break;
			}
		}

		//append_opcode(GDScriptFunction::OPCODE_OPERATOR_VALIDATED);
		//append(p_left_operand);
		//append(Address());
		//append(p_target);
		//append(op_func);
		return;
	}
}

void GDScriptJitCodeGenerator::write_binary_operator(const Address &p_target, Variant::Operator p_operator, const Address &p_left_operand, const Address &p_right_operand) {
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
}

void GDScriptJitCodeGenerator::write_call_utility(const Address &p_target, const StringName &p_function, const Vector<Address> &p_arguments) {
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
}

void GDScriptJitCodeGenerator::write_assert(const Address &p_test, const Address &p_message) {
}

void GDScriptJitCodeGenerator::start_block() {
}

void GDScriptJitCodeGenerator::end_block() {
}

void GDScriptJitCodeGenerator::clear_temporaries() {
	for (int slot_idx : temporaries_pending_clear) {
		// The temporary may have been reused as something else since it was added to the list.
		// In that case, there's **no** need to clear it.
		if (temporaries[slot_idx].can_contain_object) {
			clear_address(Address(Address::TEMPORARY, slot_idx)); // Can contain `RefCounted`, so clear it.
		}
	}
	temporaries_pending_clear.clear();
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
