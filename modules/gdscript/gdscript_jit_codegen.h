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

	static bool is_primitive_type(const Variant::Type variant_type);

	struct ValueReference {
		enum LazyState : uint8_t {
			UNCHANGED = 0,
			VALUE_CHANGED,
			TYPE_CHANGED,
		};

		bjit::Value ptr{0};
		bjit::Value cached{0};

		Variant::Type type = Variant::VARIANT_MAX;
	
		int index = -1;

		union {
			// For constants
			const Variant* constant;

			// For locals
			LazyState is_changed;
		};

		ValueReference() = default;
		ValueReference(const Variant::Type p_type, bool is_preloaded = false) :
				type(p_type), is_changed(is_preloaded ? UNCHANGED : TYPE_CHANGED) {}
		ValueReference(const Variant& p_constant) :
				type(p_constant.get_type()), constant(&p_constant) {}

		_FORCE_INLINE_ bool is_cached() const {
			return cached.index != 0;
		}

		_FORCE_INLINE_ bool is_ptr_cached() const {
			return ptr.index != 0;
		}

		_FORCE_INLINE_ bool has_type() const {
			return type != Variant::VARIANT_MAX;
		}

		_FORCE_INLINE_ bool is_allocated() const {
			return index >= 0;
		}

		_FORCE_INLINE_ void drop_cache() {
			is_changed = (is_changed == TYPE_CHANGED) ? TYPE_CHANGED : UNCHANGED;
			cached.index = 0;
		}

		_FORCE_INLINE_ void reuse(const Variant::Type new_type) {
			if (is_changed != TYPE_CHANGED) {
				is_changed = (new_type != type) ? TYPE_CHANGED : UNCHANGED;
			}

			type = new_type;
			cached.index = 0;
		}

		// The value is assumed to be constant of primitive type.
		bjit::Value _emit_load_constant(bjit::Proc& proc) {
			DEV_ASSERT(is_primitive_type(type));
			const Variant* variant = constant;

			switch(variant->get_type()) {
				case Variant::BOOL:
					cached = proc.lcu(variant->operator bool());
					break;
				case Variant::INT:
					cached = proc.lci(variant->operator int64_t());
					break;
				case Variant::FLOAT:
					cached = proc.lcd(variant->operator double());
					break;
				default:
					cached = proc.lci(0);
					break;
			}

			return cached;
		}

		void _load_primitive_from_mem(bjit::Proc& proc, bjit::Value jit_ptr, unsigned offset) {
			switch(type) {
				case Variant::BOOL:
					cached = proc.li8(jit_ptr, offset);
					break;
				case Variant::FLOAT:
					cached = proc.lf64(jit_ptr, offset);
					break;
				default:
					cached = proc.li64(jit_ptr,  offset);
					break;
			}
		}

		// The value is assumed to be argument of primitive type.
		bjit::Value _emit_load_argument(bjit::Proc& proc, int index) {
			DEV_ASSERT(is_primitive_type(type));
			if (!is_ptr_cached()) {
				ptr = proc.li64(proc.env[ENV_FUNC_ARGS], index * sizeof(Variant*));
			}

			_load_primitive_from_mem(proc, ptr, _variant_data_field_offset);
			return cached;
		}

		// The value is assumed to be local of primitive type.
		bjit::Value _emit_load_local(bjit::Proc& proc) {
			DEV_ASSERT(is_primitive_type(type));
			const unsigned data_offset = (index * sizeof(Variant)) + _variant_data_field_offset;

			_load_primitive_from_mem(proc, jit_sp, data_offset);

			is_changed = UNCHANGED;
			return cached;
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

	Vector<ValueReference> locals;
	HashSet<int> dirty_locals;

	Vector<int> temporaries;
	Vector<int> temporaries_pool;

	Vector<ValueReference> constants;
	Vector<ValueReference> arguments;

	HashMap<Variant, int, VariantHasher, VariantComparator> constant_map;
	RBMap<StringName, int> name_map;
	RBMap<Variant::ValidatedOperatorEvaluator, int> operator_func_map;
	RBMap<Variant::ValidatedSetter, int> setters_map;
	RBMap<Variant::ValidatedGetter, int> getters_map;
	RBMap<Variant::ValidatedKeyedSetter, int> keyed_setters_map;
	RBMap<Variant::ValidatedKeyedGetter, int> keyed_getters_map;
	RBMap<Variant::ValidatedIndexedSetter, int> indexed_setters_map;
	RBMap<Variant::ValidatedIndexedGetter, int> indexed_getters_map;
	RBMap<Variant::ValidatedBuiltInMethod, int> builtin_method_map;
	RBMap<Variant::ValidatedConstructor, int> constructors_map;
	RBMap<Variant::ValidatedUtilityFunction, int> utilities_map;
	RBMap<GDScriptUtilityFunctions::FunctionPtr, int> gds_utilities_map;
	RBMap<MethodBind *, int> method_bind_map;
	RBMap<GDScriptFunction *, int> lambdas_map;

	int max_locals = 0;

	int stack_top = 0;
	int max_stack_size = 0;
	int constants_top = 0;

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

	int constant_alloc_idx() {
		return constants_top++;
	}

	void stack_free_idx(int idx) {
		if (idx == stack_top - 1) {
			stack_top--;
		} else {
			stack_free_list.push_back(idx);
		}
	}

	ValueReference& get_value_ref(const Address& p_address) {
		switch (p_address.mode) {
			case Address::FUNCTION_PARAMETER:
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

	// The value is assumed to be of primitive type.
	bjit::Value emit_data_load(ValueReference& p_value, const Address& p_address) {
		DEV_ASSERT(is_primitive_type(p_value.type));

		// Check cached immediate or loaded from memory value.
		if (p_value.is_cached()) return p_value.cached;

		if (p_address.mode == Address::CONSTANT) {
			// Load immediate value.
			print_line("load imm -> constant(", p_address.address, "):", Variant::get_type_name(p_value.type));
			return p_value._emit_load_constant(proc);
		} else if (p_address.mode == Address::FUNCTION_PARAMETER) {
			print_line("load mem -> argument(", p_address.address, "):", Variant::get_type_name(p_value.type));
			return p_value._emit_load_argument(proc, p_address.address);
		} else if (p_value.is_allocated()) {
			print_line("load mem -> local(", p_address.address, "):", Variant::get_type_name(p_value.type));
			return p_value._emit_load_local(proc);
		}

		// Invalid case
		ERR_FAIL_V_MSG(proc.lci(0), "Trying to load data from memory of non-allocated local.");
	}

	// The value is assumed to be a local of primitive type.
	void emit_data_store(const Address& dst_addr, const Variant::Type src_type, const bjit::Value jit_value) {
		DEV_ASSERT(dst_addr.mode == Address::TEMPORARY || dst_addr.mode == Address::LOCAL_VARIABLE);
		DEV_ASSERT(is_primitive_type(src_type));

		ValueReference& dst = locals.write[dst_addr.address];

		// Lazy: storing will be emitted only if a pointer to the value is accessed.
		if (dst.is_changed != ValueReference::TYPE_CHANGED) {
			dst.is_changed = (dst.type != src_type) ? ValueReference::TYPE_CHANGED : ValueReference::VALUE_CHANGED;
		}
		print_line("lazy data store: local(", dst_addr.address, ")", Variant::get_type_name(dst.type), "->", Variant::get_type_name(src_type));

		dst.type = src_type;
		dst.cached = jit_value;
	}

	bjit::Value emit_ptr_access(const Address& address) {
		if (address.mode == Address::CONSTANT) {
			ValueReference& value = constants.write[address.address];
			print_line("get pointer -> constant(", address.address, "):", Variant::get_type_name(value.type));

			if (value.is_ptr_cached()) return value.ptr;
			DEV_ASSERT(value.is_allocated() == false);

			value.index = constant_alloc_idx();
			value.ptr = proc.iadd(proc.env[ENV_CONSTANTS], proc.lcu(value.index * sizeof(Variant)));

			return value.ptr;
		} else if (address.mode == Address::FUNCTION_PARAMETER) {
			ValueReference& value = arguments.write[address.address];
			print_line("get pointer -> argument(", address.address, "):", Variant::get_type_name(value.type));

			if (value.is_ptr_cached()) return value.ptr;
			value.ptr = proc.li64(proc.env[ENV_FUNC_ARGS], address.address * sizeof(Variant*));

			return value.ptr;
		}

		ValueReference& value = locals.write[address.address];
		print_line("get pointer -> local(", address.address, "):", Variant::get_type_name(value.type));

		if (!value.is_ptr_cached()) {
			DEV_ASSERT(value.is_allocated() == false);
			value.index = stack_alloc_idx();
			value.ptr = proc.iadd(jit_sp, proc.lcu(value.index * sizeof(Variant)));
		}

		const unsigned data_offset = (value.index * sizeof(Variant)) + _variant_data_field_offset;

		// Lazy storing if needed.
		switch (value.is_changed) {
			// Emit type field assign with falling to value assign.
			case ValueReference::TYPE_CHANGED:
				print_line("storing type:", Variant::get_type_name(value.type), "offset:", (value.index * sizeof(Variant)));
				proc.si32(proc.lcu(value.type), jit_sp, (value.index * sizeof(Variant)));
			// Emit value assign.
			case ValueReference::VALUE_CHANGED:
				if (value.is_cached()) {
					print_line("storing value:", Variant::get_type_name(value.type), "(cached) offset:", data_offset);
					switch (value.type) {
						case Variant::BOOL:
							proc.si8(value.cached, jit_sp, data_offset);
							break;
						case Variant::FLOAT:
							proc.sf64(value.cached, jit_sp, data_offset);
							break;
						default:
							proc.si64(value.cached, jit_sp, data_offset);
							break;
					}
				}
				value.is_changed = ValueReference::UNCHANGED;
				break;
			case ValueReference::UNCHANGED:
				break;
		}

		return value.ptr;
	}

	bjit::Value emit_load_ptr_args(const Vector<Address>& p_args) {
		if (p_args.size() == 0) return proc.lci(0);

		Vector<bjit::Value> jit_ptrs;
		jit_ptrs.resize(p_args.size());

		for (int i = 0; i < p_args.size(); ++i) {
			jit_ptrs.write[i] = emit_ptr_access(p_args[i]);
		}

		bjit::Value jit_ptr_args = proc.iadd(jit_sp, proc.lcu(stack_top * sizeof(Variant)));
		for (int i = 0; i < p_args.size(); ++i) {
			proc.si64(jit_ptrs[i], jit_ptr_args, i * sizeof(uintptr_t));
		}

		int _stack_slots = ((p_args.size() + 1) * sizeof(Variant*)) / sizeof(Variant);
		if (stack_top + _stack_slots > max_stack_size) {
			max_stack_size = stack_top + _stack_slots;
		}
	
		return jit_ptr_args;
	}

	void add_stack_identifier(const StringName &p_id, int p_stackpos) {
		if (locals.size() > max_locals) {
			max_locals = locals.size();
		}
		stack_identifiers[p_id] = p_stackpos;
	}

	void push_stack_identifiers() {
		stack_identifiers_counts.push_back(locals.size());
		stack_id_stack.push_back(stack_identifiers);
	}

	void pop_stack_identifiers() {
		int current_locals = stack_identifiers_counts.back()->get();
		stack_identifiers_counts.pop_back();
		stack_identifiers = stack_id_stack.back()->get();
		stack_id_stack.pop_back();
		for (int i = current_locals; i < locals.size(); i++) {
			dirty_locals.insert(i + GDScriptFunction::FIXED_ADDRESSES_MAX);
		}
		locals.resize(current_locals);
	}

	const StringName* get_name_ptr(const StringName& p_identifier) {
		int pos;
		if (!name_map.has(p_identifier)) {
			pos = name_map.size();
			name_map[p_identifier] = pos;

			function->global_names.append(p_identifier);
		} else {
			pos = name_map[p_identifier];
		}

		return &function->global_names[pos];
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

	int get_constant_pos(const Variant &p_constant) {
		if (constant_map.has(p_constant)) {
			return constant_map[p_constant];
		}
		int idx = constants.size();
		constants.push_back(ValueReference(p_constant));
		constant_map[p_constant] = idx;
		return idx;
	}

	int get_operation_pos(const Variant::ValidatedOperatorEvaluator p_operation) {
		if (operator_func_map.has(p_operation)) {
			return operator_func_map[p_operation];
		}
		int pos = operator_func_map.size();
		operator_func_map[p_operation] = pos;
		return pos;
	}

	int get_setter_pos(const Variant::ValidatedSetter p_setter) {
		if (setters_map.has(p_setter)) {
			return setters_map[p_setter];
		}
		int pos = setters_map.size();
		setters_map[p_setter] = pos;
		return pos;
	}

	int get_getter_pos(const Variant::ValidatedGetter p_getter) {
		if (getters_map.has(p_getter)) {
			return getters_map[p_getter];
		}
		int pos = getters_map.size();
		getters_map[p_getter] = pos;
		return pos;
	}

	int get_keyed_setter_pos(const Variant::ValidatedKeyedSetter p_keyed_setter) {
		if (keyed_setters_map.has(p_keyed_setter)) {
			return keyed_setters_map[p_keyed_setter];
		}
		int pos = keyed_setters_map.size();
		keyed_setters_map[p_keyed_setter] = pos;
		return pos;
	}

	int get_keyed_getter_pos(const Variant::ValidatedKeyedGetter p_keyed_getter) {
		if (keyed_getters_map.has(p_keyed_getter)) {
			return keyed_getters_map[p_keyed_getter];
		}
		int pos = keyed_getters_map.size();
		keyed_getters_map[p_keyed_getter] = pos;
		return pos;
	}

	int get_indexed_setter_pos(const Variant::ValidatedIndexedSetter p_indexed_setter) {
		if (indexed_setters_map.has(p_indexed_setter)) {
			return indexed_setters_map[p_indexed_setter];
		}
		int pos = indexed_setters_map.size();
		indexed_setters_map[p_indexed_setter] = pos;
		return pos;
	}

	int get_indexed_getter_pos(const Variant::ValidatedIndexedGetter p_indexed_getter) {
		if (indexed_getters_map.has(p_indexed_getter)) {
			return indexed_getters_map[p_indexed_getter];
		}
		int pos = indexed_getters_map.size();
		indexed_getters_map[p_indexed_getter] = pos;
		return pos;
	}

	int get_builtin_method_pos(const Variant::ValidatedBuiltInMethod p_method) {
		if (builtin_method_map.has(p_method)) {
			return builtin_method_map[p_method];
		}
		int pos = builtin_method_map.size();
		builtin_method_map[p_method] = pos;
		return pos;
	}

	int get_constructor_pos(const Variant::ValidatedConstructor p_constructor) {
		if (constructors_map.has(p_constructor)) {
			return constructors_map[p_constructor];
		}
		int pos = constructors_map.size();
		constructors_map[p_constructor] = pos;
		return pos;
	}

	int get_utility_pos(const Variant::ValidatedUtilityFunction p_utility) {
		if (utilities_map.has(p_utility)) {
			return utilities_map[p_utility];
		}
		int pos = utilities_map.size();
		utilities_map[p_utility] = pos;
		return pos;
	}

	int get_gds_utility_pos(const GDScriptUtilityFunctions::FunctionPtr p_gds_utility) {
		if (gds_utilities_map.has(p_gds_utility)) {
			return gds_utilities_map[p_gds_utility];
		}
		int pos = gds_utilities_map.size();
		gds_utilities_map[p_gds_utility] = pos;
		return pos;
	}

	int get_method_bind_pos(MethodBind *p_method) {
		if (method_bind_map.has(p_method)) {
			return method_bind_map[p_method];
		}
		int pos = method_bind_map.size();
		method_bind_map[p_method] = pos;
		return pos;
	}

	int get_lambda_function_pos(GDScriptFunction *p_lambda_function) {
		if (lambdas_map.has(p_lambda_function)) {
			return lambdas_map[p_lambda_function];
		}
		int pos = lambdas_map.size();
		lambdas_map[p_lambda_function] = pos;
		return pos;
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
