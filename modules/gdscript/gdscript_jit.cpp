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
struct PrimitiveUnaryOperators {
    using RetT = L;
    static constexpr bool is_float = (std::is_same_v<L, float> || std::is_same_v<L, double>);

    static bjit::Value neg(bjit::Proc& proc, bjit::Value lhs) {
        return is_float ? proc.dneg(lhs) : proc.ineg(lhs);
    }
    static bjit::Value bit_neg(bjit::Proc& proc, bjit::Value lhs) {
        return proc.inot(lhs);
    }
    static bjit::Value not(bjit::Proc& proc, bjit::Value lhs) {
        return is_float ? proc.deq(lhs, proc.lcf(0)) : proc.ieq(lhs, proc.lci(0));
    }
};

template<typename L, typename R>
struct PrimitiveBinaryOperators {
    using RetT = L;
    static constexpr bool is_float = std::is_same_v<L, double>;

    _FORCE_INLINE_ static bjit::Value cast_r(bjit::Proc& proc, bjit::Value rhs) {
        return cast_to<L, R>(proc, rhs);
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
        static_assert(is_float == false);
        return proc.imod(lhs, cast_r(proc, rhs));
    }
    static bjit::Value pow(bjit::Proc& proc, bjit::Value lhs, bjit::Value rhs) {
        proc.env.push_back(cast_to<double, L>(proc, lhs)); proc.env.push_back(cast_to<double, R>(proc, rhs));
        bjit::Value ret = proc.dcallp(proc.lcu((uintptr_t)powf64), 2);
        proc.env.resize(proc.env.size() - 2);
        return cast_to<L, double>(proc, ret);
    }
    static bjit::Value shift_left(bjit::Proc& proc, bjit::Value lhs, bjit::Value rhs) {

    }
    static bjit::Value shift_right(bjit::Proc& proc, bjit::Value lhs, bjit::Value rhs) {}
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

BUILTIN_TYPE(void, Variant::NIL, ());

BUILTIN_TYPE(bool,    Variant::BOOL,  ());
BUILTIN_TYPE(int32_t, Variant::INT,   ());
BUILTIN_TYPE(int64_t, Variant::INT,   ());
BUILTIN_TYPE(float,   Variant::FLOAT, ());
BUILTIN_TYPE(double,  Variant::FLOAT, ());

BUILTIN_TYPE(Variant, Variant::VARIANT_MAX, (
    BUILTIN_FIELD(GDScriptJit::VariantLayout, type),
    BUILTIN_FIELD(GDScriptJit::VariantLayout, _data)
));
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

GDScriptJit::UnaryOperatorCodeGenFunc
GDScriptJit::unary_operators_table[Variant::VARIANT_MAX][Variant::OP_MAX] = {
};

GDScriptJit::BinaryOperatorCodeGenFunc
GDScriptJit::binary_operators_table[Variant::VARIANT_MAX][Variant::VARIANT_MAX][Variant::OP_MAX] = {
};