/**************************************************************************/
/*  gdscript_jit.h                                                        */
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

#ifndef GDSCRIPT_JIT_BUILTIN_H
#define GDSCRIPT_JIT_BUILTIN_H

#include "core/variant/variant.h"

#include <bjit.h>

class GDScriptJit {
public:
	// Used for storing information about builtin types.
	struct TypeInfo {
		// Using to generate a code for a field access.
		struct FieldInfo {
			unsigned offset = 0;
			const TypeInfo* type = nullptr;
		};

		size_t typeid_hash = 0;
		Variant::Type variant_type = Variant::VARIANT_MAX;
		unsigned size = 0;

		HashMap<StringName, FieldInfo> fields;
	private:
		static HashMap<Variant::Type, const TypeInfo*> variant_map;
		static HashMap<size_t, const TypeInfo*> typeid_map;

		static bool _static_init;

		template<typename T>
		static void register_type();
		static void register_native_operators();
		static bool register_builtin_types();
	public:
		static const TypeInfo* from_variant(const Variant::Type p_type) {
			if (!variant_map.has(p_type)) [[unlikely]] return nullptr;
			return variant_map.get(p_type);
		}

		template<typename T>
		static const TypeInfo* from() {
			const size_t type_hash = typeid(T).hash_code();
			if (!typeid_map.has(type_hash)) [[unlikely]] return nullptr;
			return typeid_map.get(type_hash);
		}

		_FORCE_INLINE_ bool is_native() const {
			return variant_type > Variant::NIL && variant_type <= Variant::FLOAT;
		}
		_FORCE_INLINE_ bool is_builtin() const {
			return (variant_type >= Variant::VECTOR2 && variant_type <= Variant::VECTOR4I) ||
				variant_type == Variant::COLOR;
		}
		_FORCE_INLINE_ bool is_dynamic() const {
			return !is_native() && !is_builtin();
		}
		_FORCE_INLINE_ bool is_variant() const {
			return variant_type != Variant::VARIANT_MAX;
		}
		template<typename T>
		_FORCE_INLINE_ bool is_same() const {
			return typeid(T).hash_code() == typeid_hash;
		}
	};

	using UnaryOperatorCodeGenFunc = bjit::Value(*)(bjit::Proc& proc, bjit::Value lhs);
	using BinaryOperatorCodeGenFunc = bjit::Value(*)(bjit::Proc& proc, bjit::Value lhs, bjit::Value rhs);

	// Layout structure used to provide meta information about godot's internal `Variant` class.
	struct VariantLayout {
		static constexpr unsigned _data_alignment = 8;
		static constexpr unsigned _data_field_offset = _data_alignment;
		static constexpr unsigned _data_field_size = sizeof(Variant) - _data_field_offset;
	};
private:
	friend struct TypeInfo;

	enum UnaryOperator {
		NEGATE = 0,
		BIT_NEGATE,
		BOOL_NOT,

		UNARY_OP_MAX
	};

	static UnaryOperatorCodeGenFunc unary_operators_table[Variant::VARIANT_MAX][UNARY_OP_MAX];
	static BinaryOperatorCodeGenFunc binary_operators_table[Variant::VARIANT_MAX][Variant::VARIANT_MAX][Variant::OP_MAX];
public:
	static UnaryOperatorCodeGenFunc get_unary_operator(Variant::Operator p_operator, Variant::Type p_type) {
		return unary_operators_table[p_type][p_operator];
	}

	static BinaryOperatorCodeGenFunc get_binary_operator(Variant::Operator p_operator, Variant::Type p_lhs_type, Variant::Type p_rhs_type) {
		return binary_operators_table[p_lhs_type][p_rhs_type][p_operator];
	}

	template<typename To, typename From>
	static _FORCE_INLINE_ bjit::Value cast_to(bjit::Proc& proc, const bjit::Value val) {
	    if constexpr (std::is_same_v<To, From>) {
	        return val;
	    }
	    else if constexpr (std::is_same_v<To, double>) {
	        if constexpr (std::is_integral_v<From>) return proc.ci2d(val);
	        else if constexpr (std::is_same_v<From, float>) return proc.cf2d(val);
	    }
	    else if constexpr (std::is_same_v<To, float>) {
	        if constexpr (std::is_integral_v<From>) return proc.ci2f(val);
	        else if constexpr (std::is_same_v<From, double>) return proc.cd2f(val);
	    }
	    else if constexpr (std::is_integral_v<To>) {
	        if constexpr (std::is_same_v<From, double>) return proc.cd2i(val);
	        else if constexpr (std::is_same_v<From, float>) return proc.cf2i(val);
	        else if constexpr (std::is_integral_v<From>) return val;
	    } else {
	    	static_assert("Bad cast: invalid source type" && false);
		}
	}

	static void memcpy_aligned(bjit::Proc& proc, bjit::Value dst_ptr, unsigned dst_offset, bjit::Value src_ptr, unsigned src_offset, unsigned size) {
		// 8-bytes.
		unsigned num = size / sizeof(int64_t);
		unsigned mod = size % sizeof(int64_t);
		for (int i = 0; i < num; i++) {
			bjit::Value value = proc.li64(src_ptr, src_offset + (i * sizeof(int64_t)));
			proc.si64(value, dst_ptr, dst_offset + (i * sizeof(int64_t)));
		}
		if (mod == 0) return;
		src_offset += num * sizeof(int64_t);
		dst_offset += num * sizeof(int64_t);

		// 4-bytes.
		if (mod >= sizeof(int32_t)) {
			bjit::Value value = proc.li32(src_ptr, src_offset);
			proc.si32(value, dst_ptr, dst_offset);
			src_offset += sizeof(int32_t);
			dst_offset += sizeof(int32_t);
		}
		mod = mod % sizeof(int32_t);
		if (mod == 0) return;

		// 2-bytes.
		if (mod >= sizeof(int16_t)) {
			bjit::Value value = proc.li16(src_ptr, src_offset);
			proc.si16(value, dst_ptr, dst_offset);
			src_offset += sizeof(int16_t);
			dst_offset += sizeof(int16_t);
		}
		mod = mod % sizeof(int16_t);
		if (mod == 0) return;

		// 1-byte.
		bjit::Value value = proc.li8(src_ptr, src_offset);
		proc.si8(value, dst_ptr, dst_offset);
	}

	template<typename T>
	static void store_to_memory(bjit::Proc& proc, const bjit::Value value, const bjit::Value ptr, const unsigned offset) {
		static_assert("Memory store is not supported for this C++ type" && false);
	}

	template<typename T>
	static bjit::Value load_from_memory(bjit::Proc& proc, const bjit::Value ptr, const unsigned offset) {
		static_assert("Memory load is not supported for this C++ type" && false);
	}

	template<>
	_FORCE_INLINE_ bjit::Value load_from_memory<bool>(bjit::Proc& proc, const bjit::Value ptr, const unsigned offset) {
		return proc.li8(ptr, offset);
	}

	template<>
	_FORCE_INLINE_ bjit::Value load_from_memory<uint16_t>(bjit::Proc& proc, const bjit::Value ptr, const unsigned offset) {
		return proc.lu16(ptr, offset);
	}

	template<>
	_FORCE_INLINE_ bjit::Value load_from_memory<uint32_t>(bjit::Proc& proc, const bjit::Value ptr, const unsigned offset) {
		return proc.lu32(ptr, offset);
	}

	template<>
	_FORCE_INLINE_ bjit::Value load_from_memory<uint64_t>(bjit::Proc& proc, const bjit::Value ptr, const unsigned offset) {
		return proc.li64(ptr, offset);
	}

	template<>
	_FORCE_INLINE_ bjit::Value load_from_memory<int16_t>(bjit::Proc& proc, const bjit::Value ptr, const unsigned offset) {
		return proc.li16(ptr, offset);
	}

	template<>
	_FORCE_INLINE_ bjit::Value load_from_memory<int32_t>(bjit::Proc& proc, const bjit::Value ptr, const unsigned offset) {
		return proc.li32(ptr, offset);
	}

	template<>
	_FORCE_INLINE_ bjit::Value load_from_memory<int64_t>(bjit::Proc& proc, const bjit::Value ptr, const unsigned offset) {
		return proc.li64(ptr, offset);
	}

	template<>
	_FORCE_INLINE_ bjit::Value load_from_memory<float>(bjit::Proc& proc, const bjit::Value ptr, const unsigned offset) {
		return proc.lf32(ptr, offset);
	}

	template<>
	_FORCE_INLINE_ bjit::Value load_from_memory<double>(bjit::Proc& proc, const bjit::Value ptr, const unsigned offset) {
		return proc.lf64(ptr, offset);
	}

	template<>
	_FORCE_INLINE_ void store_to_memory<bool>(bjit::Proc& proc, const bjit::Value value, const bjit::Value ptr, const unsigned offset) {
		proc.si8(value, ptr, offset);
	}

	template<>
	_FORCE_INLINE_ void store_to_memory<uint16_t>(bjit::Proc& proc, const bjit::Value value, const bjit::Value ptr, const unsigned offset) {
		proc.si16(value, ptr, offset);
	}

	template<>
	_FORCE_INLINE_ void store_to_memory<uint32_t>(bjit::Proc& proc, const bjit::Value value, const bjit::Value ptr, const unsigned offset) {
		proc.si32(value, ptr, offset);
	}

	template<>
	_FORCE_INLINE_ void store_to_memory<uint64_t>(bjit::Proc& proc, const bjit::Value value, const bjit::Value ptr, const unsigned offset) {
		proc.si64(value, ptr, offset);
	}

	template<>
	_FORCE_INLINE_ void store_to_memory<int16_t>(bjit::Proc& proc, const bjit::Value value, const bjit::Value ptr, const unsigned offset) {
		proc.si16(value, ptr, offset);
	}

	template<>
	_FORCE_INLINE_ void store_to_memory<int32_t>(bjit::Proc& proc, const bjit::Value value, const bjit::Value ptr, const unsigned offset) {
		proc.si32(value, ptr, offset);
	}

	template<>
	_FORCE_INLINE_ void store_to_memory<int64_t>(bjit::Proc& proc, const bjit::Value value, const bjit::Value ptr, const unsigned offset) {
		proc.si64(value, ptr, offset);
	}

	template<>
	_FORCE_INLINE_ void store_to_memory<float>(bjit::Proc& proc, const bjit::Value value, const bjit::Value ptr, const unsigned offset) {
		proc.sf32(value, ptr, offset);
	}

	template<>
	_FORCE_INLINE_ void store_to_memory<double>(bjit::Proc& proc, const bjit::Value value, const bjit::Value ptr, const unsigned offset) {
		proc.sf64(value, ptr, offset);
	}
};

#endif // GDSCRIPT_JIT_BUILTIN_H