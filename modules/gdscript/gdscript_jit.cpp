/**************************************************************************/
/*  gdscript_jit.cpp                                                      */
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

#include "gdscript_jit.h"

#include "core/templates/hash_map.h"

template<typename L>
struct NativeUnaryOperators {
    using RetT = L;
    static constexpr bool is_float = (std::is_same_v<L, float> || std::is_same_v<L, double>);

    static bjit::Value neg(bjit::Proc& proc, bjit::Value lhs) {
        return is_float ? proc.dneg(lhs) : proc.ineg(lhs);
    }
    static bjit::Value bit_neg(bjit::Proc& proc, bjit::Value lhs) {
        return proc.inot(lhs);
    }
    static bjit::Value bool_not(bjit::Proc& proc, bjit::Value lhs) {
        return is_float ? proc.deq(lhs, proc.lcf(0)) : proc.ieq(lhs, proc.lci(0));
    }
};

template<typename L, typename R>
struct NativeBinaryOperators {
    using RetT = L;
    static constexpr bool is_float = std::is_same_v<L, double>;

    _FORCE_INLINE_ static bjit::Value cast_r(bjit::Proc& proc, bjit::Value rhs) {
        return GDScriptJit::cast_to<L, R>(proc, rhs);
    }

    static bjit::Value add(bjit::Proc& proc, bjit::Value lhs, bjit::Value rhs) {
        const bjit::Value casted_rhs = cast_r(proc, rhs);
        return is_float ? proc.dadd(lhs, casted_rhs) : proc.iadd(lhs, casted_rhs);
    }
    static bjit::Value sub(bjit::Proc& proc, bjit::Value lhs, bjit::Value rhs) {
        const bjit::Value casted_rhs = cast_r(proc, rhs);
        return is_float ? proc.dsub(lhs, casted_rhs) : proc.isub(lhs, casted_rhs);
    }
    static bjit::Value mul(bjit::Proc& proc, bjit::Value lhs, bjit::Value rhs) {
        const bjit::Value casted_rhs = cast_r(proc, rhs);
        return is_float ? proc.dmul(lhs, casted_rhs) : proc.imul(lhs, casted_rhs);
    }
    static bjit::Value div(bjit::Proc& proc, bjit::Value lhs, bjit::Value rhs) {
        const bjit::Value casted_rhs = cast_r(proc, rhs);
        return is_float ? proc.ddiv(lhs, casted_rhs) : proc.idiv(lhs, casted_rhs);
    }
    static bjit::Value mod(bjit::Proc& proc, bjit::Value lhs, bjit::Value rhs) {
        const bjit::Value result = proc.imod(GDScriptJit::cast_to<int64_t,L>(proc, lhs), cast_r(proc, rhs));
        return GDScriptJit::cast_to<RetT,int64_t>(proc, result);
    }
    static bjit::Value pow(bjit::Proc& proc, bjit::Value lhs, bjit::Value rhs) {
        proc.env.push_back(GDScriptJit::cast_to<double, L>(proc, lhs)); proc.env.push_back(GDScriptJit::cast_to<double, R>(proc, rhs));
        bjit::Value ret = proc.dcallp(proc.lcu((uintptr_t)powf64), 2);
        proc.env.resize(proc.env.size() - 2);
        return GDScriptJit::cast_to<L, double>(proc, ret);
    }
    static bjit::Value shift_left(bjit::Proc& proc, bjit::Value lhs, bjit::Value rhs) {
        return GDScriptJit::cast_to<RetT,int64_t>(
            proc,
            proc.ishl(
                GDScriptJit::cast_to<int64_t,L>(proc, lhs),
                GDScriptJit::cast_to<int64_t,R>(proc, lhs)
            )
        );
    }
    static bjit::Value shift_right(bjit::Proc& proc, bjit::Value lhs, bjit::Value rhs) {
        return GDScriptJit::cast_to<RetT,int64_t>(
            proc,
            proc.ishr(
                GDScriptJit::cast_to<int64_t,L>(proc, lhs),
                GDScriptJit::cast_to<int64_t,R>(proc, lhs)
            )
        );
    }
};

template<typename NativeT>
static const GDScriptJit::TypeInfo* _builtin_type_info_from() {
    static_assert(false && "Unsupported native type");
    return nullptr;
}

#define BUILTIN_FIELD(m_base_type, m_name) \
    { #m_name, { offsetof(m_base_type,m_name), _builtin_type_info_from<decltype(m_base_type::m_name)>() } }

#define _UNPACK(...) __VA_ARGS__ 
#define BUILTIN_TYPE(m_native,m_v_type,m_fields)                       \
    template<>                                                         \
    const GDScriptJit::TypeInfo* _builtin_type_info_from<m_native>() { \
        static GDScriptJit::TypeInfo type_info {                       \
            typeid(m_native).hash_code(),                              \
            m_v_type,                                                  \
            { _UNPACK m_fields }                                       \
        };                                                             \
        return &type_info;                                             \
    }

BUILTIN_TYPE(std::nullptr_t, Variant::NIL, ());

BUILTIN_TYPE(bool,    Variant::BOOL,  ());
BUILTIN_TYPE(int32_t, Variant::INT,   ());
BUILTIN_TYPE(int64_t, Variant::INT,   ());
BUILTIN_TYPE(float,   Variant::FLOAT, ());
BUILTIN_TYPE(double,  Variant::FLOAT, ());

BUILTIN_TYPE(Vector2, Variant::VECTOR2, (
    BUILTIN_FIELD(Vector2, x),
    BUILTIN_FIELD(Vector2, y)
));
BUILTIN_TYPE(Vector2i, Variant::VECTOR2I, (
    BUILTIN_FIELD(Vector2, x),
    BUILTIN_FIELD(Vector2, y)
));
BUILTIN_TYPE(Vector3, Variant::VECTOR3, (
    BUILTIN_FIELD(Vector3, x),
    BUILTIN_FIELD(Vector3, y),
    BUILTIN_FIELD(Vector3, z)
));
BUILTIN_TYPE(Vector3i, Variant::VECTOR3I, (
    BUILTIN_FIELD(Vector3i, x),
    BUILTIN_FIELD(Vector3i, y),
    BUILTIN_FIELD(Vector3i, z)
));
BUILTIN_TYPE(Vector4, Variant::VECTOR4, (
    BUILTIN_FIELD(Vector4, x),
    BUILTIN_FIELD(Vector4, y),
    BUILTIN_FIELD(Vector4, z),
    BUILTIN_FIELD(Vector4, w)
));
BUILTIN_TYPE(Vector4i, Variant::VECTOR4I, (
    BUILTIN_FIELD(Vector4i, x),
    BUILTIN_FIELD(Vector4i, y),
    BUILTIN_FIELD(Vector4i, z),
    BUILTIN_FIELD(Vector4i, w)
));
BUILTIN_TYPE(Color, Variant::COLOR, (
    BUILTIN_FIELD(Color, r),
    BUILTIN_FIELD(Color, g),
    BUILTIN_FIELD(Color, b),
    BUILTIN_FIELD(Color, a)
));

HashMap<Variant::Type, const GDScriptJit::TypeInfo*> GDScriptJit::TypeInfo::variant_map = {};
HashMap<size_t, const GDScriptJit::TypeInfo*> GDScriptJit::TypeInfo::typeid_map = {};

GDScriptJit::UnaryOperatorCodeGenFunc
GDScriptJit::unary_operators_table[Variant::VARIANT_MAX][GDScriptJit::UNARY_OP_MAX] = {};

GDScriptJit::BinaryOperatorCodeGenFunc
GDScriptJit::binary_operators_table[Variant::VARIANT_MAX][Variant::VARIANT_MAX][Variant::OP_MAX] = {};

#define REGISTER_NATIVE_UNARY_OPERATORS(m_native,m_v_type)                                                           \
    GDScriptJit::unary_operators_table[m_v_type][GDScriptJit::NEGATE]     = NativeUnaryOperators<m_native>::neg;     \
    GDScriptJit::unary_operators_table[m_v_type][GDScriptJit::BIT_NEGATE] = NativeUnaryOperators<m_native>::bit_neg; \
    GDScriptJit::unary_operators_table[m_v_type][GDScriptJit::BOOL_NOT]   = NativeUnaryOperators<m_native>::bool_not;

#define REGISTER_NATIVE_BINARY_OPERATORS(m_l_n,m_l_v_type,m_r_n,m_r_v_type)                                                      \
    GDScriptJit::binary_operators_table[m_l_v_type][m_r_v_type][Variant::OP_ADD]      = NativeBinaryOperators<m_l_n,m_r_n>::add; \
    GDScriptJit::binary_operators_table[m_l_v_type][m_r_v_type][Variant::OP_SUBTRACT] = NativeBinaryOperators<m_l_n,m_r_n>::sub; \
    GDScriptJit::binary_operators_table[m_l_v_type][m_r_v_type][Variant::OP_MULTIPLY] = NativeBinaryOperators<m_l_n,m_r_n>::mul; \
    GDScriptJit::binary_operators_table[m_l_v_type][m_r_v_type][Variant::OP_DIVIDE]   = NativeBinaryOperators<m_l_n,m_r_n>::div; \
    GDScriptJit::binary_operators_table[m_l_v_type][m_r_v_type][Variant::OP_MODULE]   = NativeBinaryOperators<m_l_n,m_r_n>::mod; \
    GDScriptJit::binary_operators_table[m_l_v_type][m_r_v_type][Variant::OP_POWER]    = NativeBinaryOperators<m_l_n,m_r_n>::pow; \
    \
    GDScriptJit::binary_operators_table[m_l_v_type][m_r_v_type][Variant::OP_SHIFT_LEFT]  = NativeBinaryOperators<m_l_n,m_r_n>::shift_left; \
    GDScriptJit::binary_operators_table[m_l_v_type][m_r_v_type][Variant::OP_SHIFT_RIGHT] = NativeBinaryOperators<m_l_n,m_r_n>::shift_right;

void GDScriptJit::TypeInfo::register_native_operators() {
    REGISTER_NATIVE_UNARY_OPERATORS(bool,    Variant::BOOL);
    REGISTER_NATIVE_UNARY_OPERATORS(int64_t, Variant::INT);
    REGISTER_NATIVE_UNARY_OPERATORS(double,  Variant::FLOAT);

    REGISTER_NATIVE_BINARY_OPERATORS(bool,    Variant::BOOL,  bool,    Variant::BOOL);

    REGISTER_NATIVE_BINARY_OPERATORS(int64_t, Variant::INT,   int64_t, Variant::INT);
    REGISTER_NATIVE_BINARY_OPERATORS(int64_t, Variant::INT,   bool,    Variant::BOOL);
    REGISTER_NATIVE_BINARY_OPERATORS(int64_t, Variant::INT,   double,  Variant::FLOAT);

    REGISTER_NATIVE_BINARY_OPERATORS(double,  Variant::FLOAT, double,  Variant::FLOAT);
    REGISTER_NATIVE_BINARY_OPERATORS(double,  Variant::FLOAT, bool,    Variant::BOOL);
    REGISTER_NATIVE_BINARY_OPERATORS(double,  Variant::FLOAT, int64_t, Variant::INT);
}

template<typename T>
void GDScriptJit::TypeInfo::register_type() {
    const TypeInfo* type_info = _builtin_type_info_from<T>();

    typeid_map[typeid(T).hash_code()] = type_info;
    variant_map[type_info->variant_type] = type_info;
}

bool GDScriptJit::TypeInfo::register_builtin_types() {
    register_type<std::nullptr_t>();
    register_type<bool>();
    register_type<int32_t>();
    register_type<int64_t>();
    register_type<float>();
    register_type<double>();
    register_type<Vector2>();
    register_type<Vector2i>();
    register_type<Vector3>();
    register_type<Vector3i>();
    register_type<Vector4>();
    register_type<Vector4i>();
    register_type<Color>();

    register_native_operators();

    return true;
}

bool GDScriptJit::TypeInfo::_static_init = GDScriptJit::TypeInfo::register_builtin_types();