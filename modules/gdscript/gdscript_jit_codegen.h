/**************************************************************************/
/*  gdscript_jit_codegen.h                                                */
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

#ifndef GDSCRIPT_JIT_CODEGEN_H
#define GDSCRIPT_JIT_CODEGEN_H

#include "gdscript_codegen.h"
#include "gdscript_function.h"
#include "gdscript_jit.h"
#include "gdscript_utility_functions.h"

#include <bjit.h>

class GDScriptJitCodeGenerator : public GDScriptCodeGenerator {
	enum Environment {
		ENV_INSTANCE = 0,
		ENV_MEMBERS = 1,
		ENV_CONSTANTS = 2,
		ENV_FUNC_ARGS = 3
	};

	static constexpr bjit::Value jit_sp { 0 };

	static constexpr unsigned _variant_data_field_offset = sizeof(uint64_t);
	static constexpr unsigned _object_data_ptr_field_offset = sizeof(ObjectID);

	struct ValueRef {
		enum LazyState : uint8_t {
			UNCHANGED = 0,
			VALUE_CHANGED,
			TYPE_CHANGED,
		};
		enum AddressMode : uint8_t {
			LOCAL = 0,
			TEMPORARY,
			ARGUMENT,
			CONSTANT,
			EXTERNAL
		};

		bjit::Value ptr{0};
		bjit::Value cached{0};

		LazyState state = UNCHANGED;
		AddressMode mode = LOCAL;

		const GDScriptJit::TypeInfo* type = nullptr;

		int offset = -1;

		ValueRef() = default;
		ValueRef(const Address::AddressMode p_mode, const GDScriptDataType& p_type) {
			switch (p_mode) {
				case Address::CONSTANT:
					mode = CONSTANT;
					break;
				case Address::LOCAL_VARIABLE:
					state = TYPE_CHANGED;
					mode = LOCAL;
					break;
				case Address::FUNCTION_PARAMETER:
					mode = ARGUMENT;
					break;
				case Address::TEMPORARY:
					state = TYPE_CHANGED;
					mode = TEMPORARY;
					break;
				case Address::NIL:
					mode = EXTERNAL;
					break;
				default:
					mode = EXTERNAL;
					ERR_FAIL_MSG("Invalid addressing mode");
					print_line("Addressing mode:", (unsigned)p_mode);
					break;
			}

			type = p_type.has_type ?
				GDScriptJit::TypeInfo::from_variant(p_type.builtin_type) :
				GDScriptJit::TypeInfo::from<std::nullptr_t>();
		}

		ValueRef(const Variant &p_constant) {
			mode = CONSTANT;
			type = GDScriptJit::TypeInfo::from_variant(p_constant.get_type());
		}

		_FORCE_INLINE_ bool is_allocated() const {
			return (mode > CONSTANT) || (offset >= 0);
		}

		_FORCE_INLINE_ bool is_cached() const {
			return cached.index != 0;
		}

		_FORCE_INLINE_ bool is_nil() const {
			return type->variant_type == Variant::NIL;
		}

		_FORCE_INLINE_ bool can_be_cached() const {
			return mode < CONSTANT;
		}

		_FORCE_INLINE_ void drop_cached() {
			cached.index = 0;
		}

		_FORCE_INLINE_ void evaluate(const Variant::Type p_type) {
			type = GDScriptJit::TypeInfo::from_variant(p_type);
			state = UNCHANGED;
		}

		_FORCE_INLINE_ void update_type(const Variant::Type p_type) {
			if (p_type == type->variant_type) return;

			type = GDScriptJit::TypeInfo::from_variant(p_type);
			state = TYPE_CHANGED;
		}

		_FORCE_INLINE_ void update_value(const bjit::Value p_value) {
			cached = p_value;
			if (state != TYPE_CHANGED) {
				state = VALUE_CHANGED;
			}
		}

		String stringify() const {
			String result = Variant::get_type_name(type->variant_type);
			result += '(';
			switch (mode) {
				case LOCAL: result += "local:"; break;		
				case TEMPORARY: result += "temp:"; break;
				case ARGUMENT: result += "arg:"; break;
				case CONSTANT: result += "const:"; break;
				case EXTERNAL: result += "extern:"; break;
			}

			if (offset >= 0) {
				result += itos(offset / sizeof(Variant));
			} else {
				result += "cache";	
			}

			result += ')';
			return result;
		}
	};

	struct MemberInfo {
		int index = -1;
		Variant::Type type = Variant::VARIANT_MAX;
		StringName class_name;

		MethodBind* getter = nullptr;
		MethodBind* setter = nullptr;

		_FORCE_INLINE_ bool is_valid() {
			return index >= 0;
		}
	};

	using ProcFunction = void (Object*,Variant*,const Variant*,const Variant**);

	GDScriptFunction *function;

	bjit::Module jit_module;
	bjit::Proc proc = bjit::Proc(0, "iiii");

	List<RBMap<StringName, int>> stack_id_stack;
	RBMap<StringName, int> stack_identifiers;
	List<int> stack_identifiers_counts;
	RBMap<StringName, int> local_constants;

	Vector<ValueRef> locals;
	HashSet<int> dirty_locals;

	Vector<int> temporaries;
	Vector<int> temporaries_pool;

	Vector<ValueRef> constants;
	Vector<ValueRef> arguments;

	RBMap<int, Variant> constant_values;
	HashMap<Variant, int, VariantHasher, VariantComparator> constant_map;
	RBMap<StringName, int> name_map;

	int max_locals = 0;

	int stack_top = 0;
	int max_stack_size = 0;

	List<int> stack_free_list;

	int stack_alloc_idx() {
		if (stack_free_list.is_empty()) {
			int idx = stack_top++;
			if (stack_top > max_stack_size) {
				max_stack_size = stack_top;
			}
			return idx;
		}

		int idx = stack_free_list.back()->get();
		stack_free_list.pop_back();

		return idx;
	}

	void stack_free_idx(int idx) {
		if (idx == stack_top - 1) {
			stack_top--;
		} else {
			stack_free_list.push_back(idx);
		}
	}

	MemberInfo get_member_info(const StringName& p_name) {
		MemberInfo ret;
		GDScript* script = function->get_script();

		for (GDScript* base = script->get_base().ptr(); base != nullptr; script = base) {
			if (script->debug_get_member_indices().has(p_name)) break;
		}

		if (script->debug_get_member_indices().has(p_name)) {
			const auto& info = script->debug_get_member_indices().get(p_name);

			ret.index = info.index;
			ret.type = info.property_info.type;
			ret.class_name = info.property_info.class_name;
		} else if (script->get_native().ptr()) {
			PropertyInfo info;
			const StringName& class_name = script->get_native().ptr()->get_name();

			if (ClassDB::get_property_info(class_name, p_name, &info)) {
				ret.index = ClassDB::get_property_index(class_name, p_name);
				ret.type = info.type;
				ret.class_name = info.class_name;
				ret.getter = ClassDB::get_method(class_name, ClassDB::get_property_getter(class_name, p_name));
				ret.setter = ClassDB::get_method(class_name, ClassDB::get_property_setter(class_name, p_name));
			}
		}

		return ret;
	}

	ValueRef& get_value_ref(const Address& p_address) {
		switch (p_address.mode) {
			case Address::FUNCTION_PARAMETER:
				return arguments.write[p_address.address];
				break;
			case Address::LOCAL_VARIABLE:
			case Address::TEMPORARY:
				return locals.write[p_address.address];
				break;
			case Address::CONSTANT:
				return constants.write[p_address.address];
				break;
			default:
				ERR_PRINT("Unsupported address mode while code generating");
				break;
		}
	}

	ValueRef& get_value_mut_ref(const Address& p_address) {
		return get_value_ref(p_address);
	}

	void store_native(ValueRef& p_value) {
		DEV_ASSERT(p_value.type->is_native());
		const unsigned offset = p_value.offset + _variant_data_field_offset;

		switch (p_value.type->variant_type) {
			case Variant::BOOL:
				GDScriptJit::store_to_memory<bool>(proc, p_value.cached, p_value.ptr, offset);
				break;
			case Variant::INT:
				GDScriptJit::store_to_memory<int64_t>(proc, p_value.cached, p_value.ptr, offset);
				break;
			case Variant::FLOAT:
				GDScriptJit::store_to_memory<double>(proc, p_value.cached, p_value.ptr, offset);
				break;
			default:
				ERR_FAIL_MSG("Native value expected");
				break;
		}
	}

	bjit::Value load_native(const ValueRef& p_value) {
		DEV_ASSERT(p_value.type->is_native());
		const unsigned offset = p_value.offset + _variant_data_field_offset;

		switch (p_value.type->variant_type) {
			case Variant::BOOL:
				return GDScriptJit::load_from_memory<bool>(proc, p_value.ptr, offset);
				break;
			case Variant::INT:
				return GDScriptJit::load_from_memory<int64_t>(proc, p_value.ptr, offset);
				break;
			case Variant::FLOAT:
				return GDScriptJit::load_from_memory<double>(proc, p_value.ptr, offset);
				break;
			default:
				ERR_FAIL_V_MSG({0}, "Native value expected");
				break;
		}
	}

	const Variant& get_constant_value(const ValueRef& p_value) {
		const unsigned index = ((uintptr_t)&p_value - (uintptr_t)constants.ptr()) / sizeof(ValueRef);
		return constant_values.find(index)->get();
	}

	bjit::Value load_native_constant(const ValueRef& p_value) {
		DEV_ASSERT(p_value.type->is_native());
		const Variant& variant = get_constant_value(p_value);
		
		switch (p_value.type->variant_type) {
			case Variant::BOOL:
			case Variant::INT:
				return proc.lci(variant.operator int64_t());
				break;
			case Variant::FLOAT:
				return proc.lcd(variant.operator double());
				break;
			default:
				ERR_FAIL_V_MSG({0}, "Native value expected");
				break;
		}
	}

	bjit::Value emit_cast_native(ValueRef& p_value, const Variant::Type p_target_type) {
		const bjit::Value jit_value = emit_get_native(p_value);

		// Specific cast from `float` and `int` (32-bit verisons).
		if (p_value.type->is_same<float>()) {
			switch (p_target_type) {
				case Variant::BOOL:
					return GDScriptJit::cast_to<bool,float>(proc, jit_value); break;
				case Variant::INT:
					return GDScriptJit::cast_to<int64_t,float>(proc, jit_value); break;
				case Variant::FLOAT:
					return GDScriptJit::cast_to<double,float>(proc, jit_value); break;
				default: goto inval_type;
			}
		} else if (p_value.type->is_same<int>()) {
			switch (p_target_type) {
				case Variant::BOOL:
					return GDScriptJit::cast_to<bool,int>(proc, jit_value); break;
				case Variant::INT:
					return GDScriptJit::cast_to<int64_t,int>(proc, jit_value); break;
				case Variant::FLOAT:
					return GDScriptJit::cast_to<double,int>(proc, jit_value); break;
				default: goto inval_type;
			}
		} else {
			if (p_value.type->variant_type == p_target_type) return jit_value;

			switch (p_value.type->variant_type) {
				case Variant::BOOL:  goto cast_bool;
				case Variant::INT:   goto cast_int;
				case Variant::FLOAT: goto cast_double;
				default: goto inval_type;
			}
		}

		cast_bool: {
			switch (p_target_type) {
				case Variant::INT:   return GDScriptJit::cast_to<int64_t, bool>(proc, jit_value); break;
				case Variant::FLOAT: return GDScriptJit::cast_to<double,  bool>(proc, jit_value); break;
				default: goto inval_type;
			}
		}
		cast_int: {
			switch (p_target_type) {
				case Variant::BOOL:  return GDScriptJit::cast_to<bool,    int64_t>(proc, jit_value); break;
				case Variant::FLOAT: return GDScriptJit::cast_to<double,  int64_t>(proc, jit_value); break;
				default: goto inval_type;
			}
		}
		cast_double: {
			switch (p_target_type) {
				case Variant::BOOL:  return GDScriptJit::cast_to<bool,    double>(proc, jit_value); break;
				case Variant::INT:   return GDScriptJit::cast_to<int64_t, double>(proc, jit_value); break;
				default: goto inval_type;
			}
		}

		inval_type:
		ERR_FAIL_V_MSG({0}, "Invalid native type");
	}

	void try_alloc_variant_object(ValueRef& p_value) {
		if (p_value.mode == ValueRef::ARGUMENT && p_value.ptr.index == 0) {
			p_value.ptr = proc.li64(proc.env[ENV_FUNC_ARGS], p_value.offset);
			p_value.offset = 0;
		}

		if (p_value.is_allocated()) return;

		switch (p_value.mode) {
			case ValueRef::LOCAL:
			case ValueRef::TEMPORARY: {
				int index = stack_alloc_idx();
				p_value.ptr = jit_sp;
				p_value.offset = sizeof(Variant) * index;
			} break;
			case ValueRef::CONSTANT: {
				int index = function->constants.size();
				function->constants.push_back(get_constant_value(p_value));
				p_value.ptr = proc.env[ENV_CONSTANTS];
				p_value.offset = sizeof(Variant) * index;
			} break;
			default:
				ERR_FAIL_MSG("Allocatable expected");
				break;
		}
	}

	void emit_sync_value_cache(ValueRef& p_value, bool sync_type = true) {
		DEV_ASSERT(p_value.is_allocated());
		if (p_value.mode == ValueRef::EXTERNAL) return;

		switch (p_value.state) {
			case ValueRef::TYPE_CHANGED:
				if (sync_type) {
					GDScriptJit::store_to_memory<int32_t>(
						proc,
						proc.lci(p_value.type->variant_type),
						p_value.ptr,
						p_value.offset
					);
				}
			case ValueRef::VALUE_CHANGED: {
				if (p_value.is_cached()) store_native(p_value);
				p_value.state = sync_type ?
					ValueRef::UNCHANGED :
					(p_value.state == ValueRef::TYPE_CHANGED ? ValueRef::TYPE_CHANGED : ValueRef::UNCHANGED);
			} break;
			default:
				break;
		}
	}

	bjit::Value emit_get_native(ValueRef& p_value) {
		if (!p_value.is_cached()) {
			if (p_value.mode == ValueRef::CONSTANT) {
				p_value.cached = load_native_constant(p_value);
			} else {
				if (p_value.mode == ValueRef::ARGUMENT && p_value.ptr.index == 0) {
					p_value.ptr = proc.li64(proc.env[ENV_FUNC_ARGS], p_value.offset);
					p_value.offset = 0;
				}
				p_value.cached = load_native(p_value);
			}
		}
		return p_value.cached;
	}

	void emit_set_native(ValueRef& p_destination, const Variant::Type p_src_type, const bjit::Value p_jit_value) {
		DEV_ASSERT(
			p_destination.mode == ValueRef::TEMPORARY ||
			p_destination.mode == ValueRef::LOCAL ||
			p_destination.mode == ValueRef::ARGUMENT
		);

		p_destination.update_type(p_src_type);
		p_destination.update_value(p_jit_value);
	}

	bjit::Value emit_ptr_to_object(ValueRef& p_value) {
		try_alloc_variant_object(p_value);
		emit_sync_value_cache(p_value);
		return p_value.offset > 0 ? proc.iadd(p_value.ptr, proc.lcu(p_value.offset)) : p_value.ptr;
	}

	bjit::Value emit_ptr_to_data(ValueRef& p_value) {
		try_alloc_variant_object(p_value);
		emit_sync_value_cache(p_value, false);
		return proc.iadd(p_value.ptr, proc.lcu((p_value.offset > 0 ? p_value.offset : 0) + _variant_data_field_offset));
	}

	bjit::Value emit_load_ptr_args(const Address& p_arg)  {
		const unsigned argptrs_offset = stack_top * sizeof(Variant);
		proc.si64(
			emit_ptr_to_object(get_value_ref(p_arg)),
			jit_sp, argptrs_offset
		);
		return proc.iadd(jit_sp, proc.lcu(argptrs_offset));
	}

	bjit::Value emit_load_ptr_args(const Vector<Address>& p_args) {
		if (p_args.size() == 0) return proc.lci(0);

		Vector<bjit::Value> jit_ptrs;
		jit_ptrs.resize(p_args.size());

		for (int i = 0; i < p_args.size(); ++i) {
			jit_ptrs.write[i] = emit_ptr_to_object(get_value_ref(p_args[i]));
		}

		const unsigned argptrs_offset = stack_top * sizeof(Variant);
		for (int i = 0; i < p_args.size(); ++i) {
			proc.si64(jit_ptrs[i], jit_sp, argptrs_offset + (i * sizeof(uintptr_t)));
		}

		int _stack_slots = ((p_args.size() + 1) * sizeof(Variant*)) / sizeof(Variant);
		if (stack_top + _stack_slots > max_stack_size) {
			max_stack_size = stack_top + _stack_slots;
		}
	
		return proc.iadd(jit_sp, proc.lcu(argptrs_offset));
	}

	template<typename R, typename... FuncArgs, typename... JitArgs>
	void emit_function_call(R (*func_ptr)(FuncArgs...), JitArgs... jit_args) {
		static_assert(sizeof...(FuncArgs) == sizeof...(JitArgs));
		constexpr unsigned args_count = sizeof...(FuncArgs);

		if constexpr (args_count > 0) {
			proc.env.reserve(proc.env.size() + args_count);
			(proc.env.push_back(jit_args),...);
		}

		proc.icallp(proc.lcu((uintptr_t)func_ptr), args_count);

		if constexpr (args_count > 0) {
			proc.env.resize(proc.env.size() - args_count);
		}
	}

	void add_stack_identifier(const StringName &p_id, int p_stackpos) {
		if (locals.size() > max_locals) {
			max_locals = locals.size();
		}
		stack_identifiers[p_id] = p_stackpos;
	}

	const StringName* get_name_ptr(const StringName& p_identifier) {
		static Vector<StringName> names;

		int pos;
		if (!name_map.has(p_identifier)) {
			pos = names.size();
			name_map[p_identifier] = pos;

			names.append(p_identifier);
		} else {
			pos = name_map[p_identifier];
		}

		return &names[pos];
	}

	bjit::Value emit_name_ptr(const StringName& p_identifier) {
		int pos;
		if (!name_map.has(p_identifier)) {
			pos = function->constants.size();
			function->constants.append(p_identifier);
		} else {
			pos = name_map.find(p_identifier)->get();
		}

		int offset = (sizeof(Variant) * pos) + _variant_data_field_offset;
		return proc.iadd(proc.env[ENV_CONSTANTS], proc.lci(offset));
	}

	int get_name_map_pos(const StringName &p_identifier) {
		int ret;
		if (!name_map.has(p_identifier)) {
			ret = name_map.size();
			name_map[p_identifier] = ret;
		} else {
			ret = name_map[p_identifier];
		}
		return ret;
	}
public:
	virtual uint32_t add_parameter(const StringName &p_name, bool p_is_optional, const GDScriptDataType &p_type) override;
	virtual uint32_t add_local(const StringName &p_name, const GDScriptDataType &p_type) override;
	virtual uint32_t add_local_constant(const StringName &p_name, const Variant &p_constant) override;
	virtual uint32_t add_or_get_constant(const Variant &p_constant) override;
	virtual uint32_t add_or_get_name(const StringName &p_name) override;
	virtual uint32_t add_temporary(const GDScriptDataType &p_type) override;
	virtual void pop_temporary() override;
	virtual void clear_temporaries() override;
	virtual void clear_address(const Address &p_address) override;
	virtual bool is_local_dirty(const Address &p_address) const override;

	virtual void start_parameters() override;
	virtual void end_parameters() override;

	virtual void start_block() override;
	virtual void end_block() override;

	virtual void write_start(GDScript *p_script, const StringName &p_function_name, bool p_static, Variant p_rpc_config, const GDScriptDataType &p_return_type) override;
	virtual GDScriptFunction *write_end() override;

#ifdef DEBUG_ENABLED
	virtual void set_signature(const String &p_signature) override;
#endif
	virtual void set_initial_line(int p_line) override;

	virtual void write_type_adjust(const Address &p_target, Variant::Type p_new_type) override;
	virtual void write_unary_operator(const Address &p_target, Variant::Operator p_operator, const Address &p_left_operand) override;
	virtual void write_binary_operator(const Address &p_target, Variant::Operator p_operator, const Address &p_left_operand, const Address &p_right_operand) override;
	virtual void write_type_test(const Address &p_target, const Address &p_source, const GDScriptDataType &p_type) override;
	virtual void write_and_left_operand(const Address &p_left_operand) override;
	virtual void write_and_right_operand(const Address &p_right_operand) override;
	virtual void write_end_and(const Address &p_target) override;
	virtual void write_or_left_operand(const Address &p_left_operand) override;
	virtual void write_or_right_operand(const Address &p_right_operand) override;
	virtual void write_end_or(const Address &p_target) override;
	virtual void write_start_ternary(const Address &p_target) override;
	virtual void write_ternary_condition(const Address &p_condition) override;
	virtual void write_ternary_true_expr(const Address &p_expr) override;
	virtual void write_ternary_false_expr(const Address &p_expr) override;
	virtual void write_end_ternary() override;
	virtual void write_set(const Address &p_target, const Address &p_index, const Address &p_source) override;
	virtual void write_get(const Address &p_target, const Address &p_index, const Address &p_source) override;
	virtual void write_set_named(const Address &p_target, const StringName &p_name, const Address &p_source) override;
	virtual void write_get_named(const Address &p_target, const StringName &p_name, const Address &p_source) override;
	virtual void write_set_member(const Address &p_value, const StringName &p_name) override;
	virtual void write_get_member(const Address &p_target, const StringName &p_name) override;
	virtual void write_set_static_variable(const Address &p_value, const Address &p_class, int p_index) override;
	virtual void write_get_static_variable(const Address &p_target, const Address &p_class, int p_index) override;
	virtual void write_assign(const Address &p_target, const Address &p_source) override;
	virtual void write_assign_with_conversion(const Address &p_target, const Address &p_source) override;
	virtual void write_assign_null(const Address &p_target) override;
	virtual void write_assign_true(const Address &p_target) override;
	virtual void write_assign_false(const Address &p_target) override;
	virtual void write_assign_default_parameter(const Address &p_dst, const Address &p_src, bool p_use_conversion) override;
	virtual void write_store_global(const Address &p_dst, int p_global_index) override;
	virtual void write_store_named_global(const Address &p_dst, const StringName &p_global) override;
	virtual void write_cast(const Address &p_target, const Address &p_source, const GDScriptDataType &p_type) override;
	virtual void write_call(const Address &p_target, const Address &p_base, const StringName &p_function_name, const Vector<Address> &p_arguments) override;
	virtual void write_super_call(const Address &p_target, const StringName &p_function_name, const Vector<Address> &p_arguments) override;
	virtual void write_call_async(const Address &p_target, const Address &p_base, const StringName &p_function_name, const Vector<Address> &p_arguments) override;
	virtual void write_call_utility(const Address &p_target, const StringName &p_function, const Vector<Address> &p_arguments) override;
	void write_call_builtin_type(const Address &p_target, const Address &p_base, Variant::Type p_type, const StringName &p_method, bool p_is_static, const Vector<Address> &p_arguments);
	virtual void write_call_gdscript_utility(const Address &p_target, const StringName &p_function, const Vector<Address> &p_arguments) override;
	virtual void write_call_builtin_type(const Address &p_target, const Address &p_base, Variant::Type p_type, const StringName &p_method, const Vector<Address> &p_arguments) override;
	virtual void write_call_builtin_type_static(const Address &p_target, Variant::Type p_type, const StringName &p_method, const Vector<Address> &p_arguments) override;
	virtual void write_call_native_static(const Address &p_target, const StringName &p_class, const StringName &p_method, const Vector<Address> &p_arguments) override;
	virtual void write_call_native_static_validated(const Address &p_target, MethodBind *p_method, const Vector<Address> &p_arguments) override;
	virtual void write_call_method_bind(const Address &p_target, const Address &p_base, MethodBind *p_method, const Vector<Address> &p_arguments) override;
	virtual void write_call_method_bind_validated(const Address &p_target, const Address &p_base, MethodBind *p_method, const Vector<Address> &p_arguments) override;
	virtual void write_call_self(const Address &p_target, const StringName &p_function_name, const Vector<Address> &p_arguments) override;
	virtual void write_call_self_async(const Address &p_target, const StringName &p_function_name, const Vector<Address> &p_arguments) override;
	virtual void write_call_script_function(const Address &p_target, const Address &p_base, const StringName &p_function_name, const Vector<Address> &p_arguments) override;
	virtual void write_lambda(const Address &p_target, GDScriptFunction *p_function, const Vector<Address> &p_captures, bool p_use_self) override;
	virtual void write_construct(const Address &p_target, Variant::Type p_type, const Vector<Address> &p_arguments) override;
	virtual void write_construct_array(const Address &p_target, const Vector<Address> &p_arguments) override;
	virtual void write_construct_typed_array(const Address &p_target, const GDScriptDataType &p_element_type, const Vector<Address> &p_arguments) override;
	virtual void write_construct_dictionary(const Address &p_target, const Vector<Address> &p_arguments) override;
	virtual void write_construct_typed_dictionary(const Address &p_target, const GDScriptDataType &p_key_type, const GDScriptDataType &p_value_type, const Vector<Address> &p_arguments) override;
	virtual void write_await(const Address &p_target, const Address &p_operand) override;
	virtual void write_if(const Address &p_condition) override;
	virtual void write_else() override;
	virtual void write_endif() override;
	virtual void write_jump_if_shared(const Address &p_value) override;
	virtual void write_end_jump_if_shared() override;
	virtual void start_for(const GDScriptDataType &p_iterator_type, const GDScriptDataType &p_list_type) override;
	virtual void write_for_assignment(const Address &p_list) override;
	virtual void write_for(const Address &p_variable, bool p_use_conversion) override;
	virtual void write_endfor() override;
	virtual void start_while_condition() override;
	virtual void write_while(const Address &p_condition) override;
	virtual void write_endwhile() override;
	virtual void write_break() override;
	virtual void write_continue() override;
	virtual void write_breakpoint() override;
	virtual void write_newline(int p_line) override;
	virtual void write_return(const Address &p_return_value) override;
	virtual void write_assert(const Address &p_test, const Address &p_message) override;

	virtual ~GDScriptJitCodeGenerator();
};

#endif // GDSCRIPT_JIT_CODEGEN_H
