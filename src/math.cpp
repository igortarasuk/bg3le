// Ext.Math, against the same glm the real extender uses.
//
// Every function here mirrors the body of its counterpart in bg3se's
// Lua/Libs/Math.inl and calls the same glm function. That matters more than
// it might look: a hand-rolled slerp or matrix decomposition would agree to
// a few digits and disagree in the last, and this is arithmetic mods use to
// place things in the world. glm is already vendored for the component
// definitions, so using it costs nothing.
//
// The Lua representation is bg3se's, from LuaPush.inl: a vector is a flat
// array of 2, 3 or 4 numbers, a quaternion an array of 4 as x, y, z, w, and
// a matrix a flat array in column-major order -- 9 for a 3x3, 16 for a 4x4.
//
// Upstream's calling convention, from CallFunc: one argument past the
// operands means "write the result into that instead of returning it", and
// the call then returns nothing.

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/matrix_interpolation.hpp>
#include <glm/gtx/perpendicular.hpp>
#include <glm/gtx/projection.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtx/vector_angle.hpp>

#include <cmath>
#include <cstdint>
#include <random>

// Norbyte's Lua fork is compiled as C++, as bg3se compiles it, so these
// must not be wrapped in extern "C" or the symbols will not match --
// lua_host.cpp says the same. Wrapping them here cost a crash at the first
// Ext.Math call, with "undefined symbol: lua_type" and nothing at link
// time, because the build warns on unresolved symbols rather than failing.
#include "lauxlib.h"
#include "lua.h"

namespace bg3le {

namespace {

// What a Lua value at an index is, by its length: bg3se distinguishes these
// the same way, since none of them are tagged.
enum class Shape { Scalar, Vec2, Vec3, Vec4, Mat3, Mat4, None };

struct Value {
    Shape Kind{Shape::None};
    float S{0.0f};
    glm::vec4 V{0.0f};
    glm::mat3 M3{1.0f};
    glm::mat4 M4{1.0f};
};

int arity_of(lua_State* L, int index) {
    if (lua_type(L, index) == LUA_TNUMBER) return 1;
    if (lua_type(L, index) != LUA_TTABLE) return 0;
    return (int)lua_rawlen(L, index);
}

float number_at(lua_State* L, int index, int i) {
    lua_rawgeti(L, index, i + 1);
    const auto v = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

Value read_value(lua_State* L, int index) {
    Value out;
    const int arity = arity_of(L, index);
    switch (arity) {
    case 1:
        out.Kind = Shape::Scalar;
        out.S = (float)luaL_checknumber(L, index);
        break;

    case 2:
    case 3:
    case 4:
        out.Kind = arity == 2   ? Shape::Vec2
                   : arity == 3 ? Shape::Vec3
                                : Shape::Vec4;
        for (int i = 0; i < arity; ++i) out.V[i] = number_at(L, index, i);
        break;

    case 9:
        out.Kind = Shape::Mat3;
        for (int c = 0; c < 3; ++c) {
            for (int r = 0; r < 3; ++r) {
                out.M3[c][r] = number_at(L, index, c * 3 + r);
            }
        }
        break;

    case 16:
        out.Kind = Shape::Mat4;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                out.M4[c][r] = number_at(L, index, c * 4 + r);
            }
        }
        break;

    default:
        luaL_error(L, "expected a number, a 2-4 element vector, or a 9 or "
                      "16 element matrix; got %d elements", arity);
        break;
    }
    return out;
}

glm::vec3 read_vec3(lua_State* L, int index) {
    if (arity_of(L, index) != 3) {
        luaL_error(L, "expected a 3 element vector");
    }
    return glm::vec3(number_at(L, index, 0), number_at(L, index, 1),
                     number_at(L, index, 2));
}

// A quaternion is an array of x, y, z, w.
glm::quat read_quat(lua_State* L, int index) {
    if (arity_of(L, index) != 4) {
        luaL_error(L, "expected a 4 element quaternion");
    }
    return glm::quat(number_at(L, index, 3), number_at(L, index, 0),
                     number_at(L, index, 1), number_at(L, index, 2));
}

glm::mat4 as_mat4(lua_State* L, Value const& v) {
    if (v.Kind == Shape::Mat4) return v.M4;
    if (v.Kind == Shape::Mat3) {
        // Upstream widens the same way, setting the corner explicitly.
        glm::mat4 m{v.M3};
        m[3][3] = 1.0f;
        return m;
    }
    luaL_error(L, "expected a 3x3 or 4x4 matrix");
    return glm::mat4{1.0f};
}

void push_floats(lua_State* L, float const* values, int count) {
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i) {
        lua_pushnumber(L, values[i]);
        lua_rawseti(L, -2, i + 1);
    }
}

void push_vec(lua_State* L, glm::vec4 const& v, int count) {
    float raw[4] = {v.x, v.y, v.z, v.w};
    push_floats(L, raw, count);
}

void push_quat(lua_State* L, glm::quat const& q) {
    float raw[4] = {q.x, q.y, q.z, q.w};
    push_floats(L, raw, 4);
}

void push_mat3(lua_State* L, glm::mat3 const& m) {
    float raw[9];
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) raw[c * 3 + r] = m[c][r];
    }
    push_floats(L, raw, 9);
}

void push_mat4(lua_State* L, glm::mat4 const& m) {
    float raw[16];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) raw[c * 4 + r] = m[c][r];
    }
    push_floats(L, raw, 16);
}

void push_value(lua_State* L, Value const& v) {
    switch (v.Kind) {
    case Shape::Scalar: lua_pushnumber(L, v.S); break;
    case Shape::Vec2: push_vec(L, v.V, 2); break;
    case Shape::Vec3: push_vec(L, v.V, 3); break;
    case Shape::Vec4: push_vec(L, v.V, 4); break;
    case Shape::Mat3: push_mat3(L, v.M3); break;
    case Shape::Mat4: push_mat4(L, v.M4); break;
    case Shape::None: lua_pushnil(L); break;
    }
}

// Writes a result into a table the caller supplied, which is what upstream
// does when there is an argument past the operands.
void assign_floats(lua_State* L, int index, float const* values, int count) {
    luaL_checktype(L, index, LUA_TTABLE);
    for (int i = 0; i < count; ++i) {
        lua_pushnumber(L, values[i]);
        lua_rawseti(L, index, i + 1);
    }
}

void assign_vec3(lua_State* L, int index, glm::vec3 const& v) {
    float raw[3] = {v.x, v.y, v.z};
    assign_floats(L, index, raw, 3);
}

void assign_value(lua_State* L, int index, Value const& v) {
    switch (v.Kind) {
    case Shape::Scalar: {
        // A scalar result cannot be written into a table element-wise the
        // way a vector can; upstream's assign refuses it too.
        luaL_error(L, "cannot write a scalar result into a table");
        break;
    }
    case Shape::Vec2: case Shape::Vec3: case Shape::Vec4: {
        float raw[4] = {v.V.x, v.V.y, v.V.z, v.V.w};
        assign_floats(L, index, raw,
                      v.Kind == Shape::Vec2 ? 2 : v.Kind == Shape::Vec3 ? 3 : 4);
        break;
    }
    case Shape::Mat3: {
        float raw[9];
        for (int c = 0; c < 3; ++c) {
            for (int r = 0; r < 3; ++r) raw[c * 3 + r] = v.M3[c][r];
        }
        assign_floats(L, index, raw, 9);
        break;
    }
    case Shape::Mat4: {
        float raw[16];
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) raw[c * 4 + r] = v.M4[c][r];
        }
        assign_floats(L, index, raw, 16);
        break;
    }
    case Shape::None: break;
    }
}

// Returns the result, or writes it into the extra argument and returns
// nothing, as upstream's CallFunc does.
int deliver(lua_State* L, Value const& result, int operands) {
    if (lua_gettop(L) > operands) {
        assign_value(L, operands + 1, result);
        return 0;
    }
    push_value(L, result);
    return 1;
}

Value scalar(float v) {
    Value out;
    out.Kind = Shape::Scalar;
    out.S = v;
    return out;
}

Value vector(glm::vec4 const& v, Shape kind) {
    Value out;
    out.Kind = kind;
    out.V = v;
    return out;
}

// The component-wise operators, applied to whatever shapes the arguments
// are. glm defines these for every combination bg3se accepts, including a
// scalar broadcast against a vector or a matrix.
template <class Op>
int componentwise(lua_State* L, Op op) {
    const Value a = read_value(L, 1);
    const Value b = read_value(L, 2);

    if (a.Kind == Shape::Mat4 || b.Kind == Shape::Mat4
        || a.Kind == Shape::Mat3 || b.Kind == Shape::Mat3) {
        luaL_error(L, "matrices are not accepted by this operation");
    }

    if (a.Kind == Shape::Scalar && b.Kind == Shape::Scalar) {
        return deliver(L, scalar(op(a.S, b.S)), 2);
    }

    const Shape kind = a.Kind == Shape::Scalar ? b.Kind : a.Kind;
    const int n = kind == Shape::Vec2 ? 2 : kind == Shape::Vec3 ? 3 : 4;
    glm::vec4 out{0.0f};
    for (int i = 0; i < n; ++i) {
        const float l = a.Kind == Shape::Scalar ? a.S : a.V[i];
        const float r = b.Kind == Shape::Scalar ? b.S : b.V[i];
        out[i] = op(l, r);
    }
    return deliver(L, vector(out, kind), 2);
}

}  // namespace

// ---- arithmetic -----------------------------------------------------------

extern "C" int bg3le_math_add(lua_State* L) {
    const Value a = read_value(L, 1);
    const Value b = read_value(L, 2);
    if (a.Kind == Shape::Mat4 && b.Kind == Shape::Mat4) {
        Value out;
        out.Kind = Shape::Mat4;
        out.M4 = a.M4 + b.M4;
        return deliver(L, out, 2);
    }
    if (a.Kind == Shape::Mat3 && b.Kind == Shape::Mat3) {
        Value out;
        out.Kind = Shape::Mat3;
        out.M3 = a.M3 + b.M3;
        return deliver(L, out, 2);
    }
    return componentwise(L, [](float x, float y) { return x + y; });
}

extern "C" int bg3le_math_sub(lua_State* L) {
    const Value a = read_value(L, 1);
    const Value b = read_value(L, 2);
    if (a.Kind == Shape::Mat4 && b.Kind == Shape::Mat4) {
        Value out;
        out.Kind = Shape::Mat4;
        out.M4 = a.M4 - b.M4;
        return deliver(L, out, 2);
    }
    if (a.Kind == Shape::Mat3 && b.Kind == Shape::Mat3) {
        Value out;
        out.Kind = Shape::Mat3;
        out.M3 = a.M3 - b.M3;
        return deliver(L, out, 2);
    }
    return componentwise(L, [](float x, float y) { return x - y; });
}

// glm's operator*, which is a matrix product for two matrices and a
// transform for a matrix and a vector -- not component-wise like the rest.
extern "C" int bg3le_math_mul(lua_State* L) {
    const Value a = read_value(L, 1);
    const Value b = read_value(L, 2);

    Value out;
    if (a.Kind == Shape::Mat4 && b.Kind == Shape::Mat4) {
        out.Kind = Shape::Mat4;
        out.M4 = a.M4 * b.M4;
        return deliver(L, out, 2);
    }
    if (a.Kind == Shape::Mat3 && b.Kind == Shape::Mat3) {
        out.Kind = Shape::Mat3;
        out.M3 = a.M3 * b.M3;
        return deliver(L, out, 2);
    }
    if (a.Kind == Shape::Mat4 && b.Kind == Shape::Vec4) {
        return deliver(L, vector(a.M4 * b.V, Shape::Vec4), 2);
    }
    if (a.Kind == Shape::Mat3 && b.Kind == Shape::Vec3) {
        const glm::vec3 r = a.M3 * glm::vec3(b.V);
        return deliver(L, vector(glm::vec4(r, 0.0f), Shape::Vec3), 2);
    }
    if (a.Kind == Shape::Vec4 && b.Kind == Shape::Mat4) {
        return deliver(L, vector(b.V * a.M4, Shape::Vec4), 2);
    }
    if (a.Kind == Shape::Vec3 && b.Kind == Shape::Mat3) {
        const glm::vec3 r = glm::vec3(a.V) * b.M3;
        return deliver(L, vector(glm::vec4(r, 0.0f), Shape::Vec3), 2);
    }
    if (a.Kind == Shape::Mat4 && b.Kind == Shape::Scalar) {
        out.Kind = Shape::Mat4;
        out.M4 = a.M4 * b.S;
        return deliver(L, out, 2);
    }
    if (a.Kind == Shape::Scalar && b.Kind == Shape::Mat4) {
        out.Kind = Shape::Mat4;
        out.M4 = a.S * b.M4;
        return deliver(L, out, 2);
    }
    if (a.Kind == Shape::Mat3 && b.Kind == Shape::Scalar) {
        out.Kind = Shape::Mat3;
        out.M3 = a.M3 * b.S;
        return deliver(L, out, 2);
    }
    if (a.Kind == Shape::Scalar && b.Kind == Shape::Mat3) {
        out.Kind = Shape::Mat3;
        out.M3 = a.S * b.M3;
        return deliver(L, out, 2);
    }
    return componentwise(L, [](float x, float y) { return x * y; });
}

// glm divides one matrix by another as a multiplication by the inverse,
// which is not what component-wise division would give.
extern "C" int bg3le_math_div(lua_State* L) {
    const Value a = read_value(L, 1);
    const Value b = read_value(L, 2);

    Value out;
    if (a.Kind == Shape::Mat4 && b.Kind == Shape::Mat4) {
        out.Kind = Shape::Mat4;
        out.M4 = a.M4 / b.M4;
        return deliver(L, out, 2);
    }
    if (a.Kind == Shape::Mat3 && b.Kind == Shape::Mat3) {
        out.Kind = Shape::Mat3;
        out.M3 = a.M3 / b.M3;
        return deliver(L, out, 2);
    }
    if (a.Kind == Shape::Mat4 && b.Kind == Shape::Scalar) {
        out.Kind = Shape::Mat4;
        out.M4 = a.M4 / b.S;
        return deliver(L, out, 2);
    }
    if (a.Kind == Shape::Mat3 && b.Kind == Shape::Scalar) {
        out.Kind = Shape::Mat3;
        out.M3 = a.M3 / b.S;
        return deliver(L, out, 2);
    }
    return componentwise(L, [](float x, float y) { return x / y; });
}

// ---- vector geometry ------------------------------------------------------

extern "C" int bg3le_math_reflect(lua_State* L) {
    const Value i = read_value(L, 1);
    const Value n = read_value(L, 2);
    const int arity = i.Kind == Shape::Vec2 ? 2 : i.Kind == Shape::Vec3 ? 3 : 4;
    glm::vec4 out{0.0f};
    switch (arity) {
    case 2: {
        const glm::vec2 r = glm::reflect(glm::vec2(i.V), glm::vec2(n.V));
        out = glm::vec4(r, 0.0f, 0.0f);
        break;
    }
    case 3: {
        const glm::vec3 r = glm::reflect(glm::vec3(i.V), glm::vec3(n.V));
        out = glm::vec4(r, 0.0f);
        break;
    }
    default: out = glm::reflect(i.V, n.V); break;
    }
    return deliver(L, vector(out, i.Kind), 2);
}

extern "C" int bg3le_math_angle(lua_State* L) {
    const Value a = read_value(L, 1);
    const Value b = read_value(L, 2);
    float angle = 0.0f;
    switch (a.Kind) {
    case Shape::Vec2:
        angle = glm::angle(glm::vec2(a.V), glm::vec2(b.V));
        break;
    case Shape::Vec3:
        angle = glm::angle(glm::vec3(a.V), glm::vec3(b.V));
        break;
    case Shape::Vec4: angle = glm::angle(a.V, b.V); break;
    default: luaL_error(L, "expected two vectors"); break;
    }
    lua_pushnumber(L, angle);
    return 1;
}

extern "C" int bg3le_math_cross(lua_State* L) {
    const glm::vec3 r = glm::cross(read_vec3(L, 1), read_vec3(L, 2));
    if (lua_gettop(L) > 2) {
        assign_vec3(L, 3, r);
        return 0;
    }
    push_vec(L, glm::vec4(r, 0.0f), 3);
    return 1;
}

extern "C" int bg3le_math_distance(lua_State* L) {
    lua_pushnumber(L, glm::distance(read_vec3(L, 1), read_vec3(L, 2)));
    return 1;
}

extern "C" int bg3le_math_dot(lua_State* L) {
    lua_pushnumber(L, glm::dot(read_vec3(L, 1), read_vec3(L, 2)));
    return 1;
}

extern "C" int bg3le_math_length(lua_State* L) {
    const Value a = read_value(L, 1);
    float length = 0.0f;
    switch (a.Kind) {
    case Shape::Scalar: length = std::fabs(a.S); break;
    case Shape::Vec2: length = glm::length(glm::vec2(a.V)); break;
    case Shape::Vec3: length = glm::length(glm::vec3(a.V)); break;
    case Shape::Vec4: length = glm::length(a.V); break;
    default: luaL_error(L, "expected a scalar or a vector"); break;
    }
    lua_pushnumber(L, length);
    return 1;
}

extern "C" int bg3le_math_normalize(lua_State* L) {
    const Value a = read_value(L, 1);
    glm::vec4 out{0.0f};
    switch (a.Kind) {
    case Shape::Vec2:
        out = glm::vec4(glm::normalize(glm::vec2(a.V)), 0.0f, 0.0f);
        break;
    case Shape::Vec3:
        out = glm::vec4(glm::normalize(glm::vec3(a.V)), 0.0f);
        break;
    case Shape::Vec4: out = glm::normalize(a.V); break;
    default: luaL_error(L, "expected a vector"); break;
    }
    return deliver(L, vector(out, a.Kind), 1);
}

extern "C" int bg3le_math_perpendicular(lua_State* L) {
    const Value x = read_value(L, 1);
    const Value n = read_value(L, 2);
    glm::vec4 out{0.0f};
    switch (x.Kind) {
    case Shape::Vec2:
        out = glm::vec4(glm::perp(glm::vec2(x.V), glm::vec2(n.V)), 0.0f, 0.0f);
        break;
    case Shape::Vec3:
        out = glm::vec4(glm::perp(glm::vec3(x.V), glm::vec3(n.V)), 0.0f);
        break;
    case Shape::Vec4: out = glm::perp(x.V, n.V); break;
    default: luaL_error(L, "expected a vector"); break;
    }
    return deliver(L, vector(out, x.Kind), 2);
}

extern "C" int bg3le_math_project(lua_State* L) {
    const Value x = read_value(L, 1);
    const Value n = read_value(L, 2);
    glm::vec4 out{0.0f};
    switch (x.Kind) {
    case Shape::Vec2:
        out = glm::vec4(glm::proj(glm::vec2(x.V), glm::vec2(n.V)), 0.0f, 0.0f);
        break;
    case Shape::Vec3:
        out = glm::vec4(glm::proj(glm::vec3(x.V), glm::vec3(n.V)), 0.0f);
        break;
    case Shape::Vec4: out = glm::proj(x.V, n.V); break;
    default: luaL_error(L, "expected a vector"); break;
    }
    return deliver(L, vector(out, x.Kind), 2);
}

// ---- matrices -------------------------------------------------------------

extern "C" int bg3le_math_determinant(lua_State* L) {
    const Value a = read_value(L, 1);
    if (a.Kind == Shape::Mat4) {
        lua_pushnumber(L, glm::determinant(a.M4));
    } else if (a.Kind == Shape::Mat3) {
        lua_pushnumber(L, glm::determinant(a.M3));
    } else {
        return luaL_error(L, "expected a 3x3 or 4x4 matrix");
    }
    return 1;
}

extern "C" int bg3le_math_inverse(lua_State* L) {
    const Value a = read_value(L, 1);
    Value out;
    if (a.Kind == Shape::Mat4) {
        out.Kind = Shape::Mat4;
        out.M4 = glm::inverse(a.M4);
    } else if (a.Kind == Shape::Mat3) {
        out.Kind = Shape::Mat3;
        out.M3 = glm::inverse(a.M3);
    } else {
        return luaL_error(L, "expected a 3x3 or 4x4 matrix");
    }
    return deliver(L, out, 1);
}

extern "C" int bg3le_math_transpose(lua_State* L) {
    const Value a = read_value(L, 1);
    Value out;
    if (a.Kind == Shape::Mat4) {
        out.Kind = Shape::Mat4;
        out.M4 = glm::transpose(a.M4);
    } else if (a.Kind == Shape::Mat3) {
        out.Kind = Shape::Mat3;
        out.M3 = glm::transpose(a.M3);
    } else {
        return luaL_error(L, "expected a 3x3 or 4x4 matrix");
    }
    return deliver(L, out, 1);
}

extern "C" int bg3le_math_outer_product(lua_State* L) {
    const Value c = read_value(L, 1);
    const Value r = read_value(L, 2);
    Value out;
    if (c.Kind == Shape::Vec3 && r.Kind == Shape::Vec3) {
        out.Kind = Shape::Mat3;
        out.M3 = glm::outerProduct(glm::vec3(c.V), glm::vec3(r.V));
    } else if (c.Kind == Shape::Vec4 && r.Kind == Shape::Vec4) {
        out.Kind = Shape::Mat4;
        out.M4 = glm::outerProduct(c.V, r.V);
    } else {
        return luaL_error(L, "expected two vectors of 3 or 4 elements");
    }
    return deliver(L, out, 2);
}

// Rotate, Translate and Scale transform the matrix in place, as upstream
// does -- they assign into argument one rather than returning.
extern "C" int bg3le_math_rotate(lua_State* L) {
    const Value m = read_value(L, 1);
    const auto angle = (float)luaL_checknumber(L, 2);
    const glm::vec3 axis = read_vec3(L, 3);

    Value out;
    if (m.Kind == Shape::Mat4) {
        out.Kind = Shape::Mat4;
        out.M4 = glm::rotate(m.M4, angle, axis);
    } else if (m.Kind == Shape::Mat3) {
        glm::mat4 wide{m.M3};
        wide[3][3] = 1.0f;
        out.Kind = Shape::Mat3;
        out.M3 = glm::mat3(glm::rotate(wide, angle, axis));
    } else {
        return luaL_error(L, "Expected a 3x3 or 4x4 matrix");
    }
    assign_value(L, 1, out);
    return 0;
}

extern "C" int bg3le_math_translate(lua_State* L) {
    const Value m = read_value(L, 1);
    Value out;
    out.Kind = Shape::Mat4;
    out.M4 = glm::translate(as_mat4(L, m), read_vec3(L, 2));
    assign_value(L, 1, out);
    return 0;
}

extern "C" int bg3le_math_scale(lua_State* L) {
    const Value m = read_value(L, 1);
    Value out;
    out.Kind = Shape::Mat4;
    out.M4 = glm::scale(as_mat4(L, m), read_vec3(L, 2));
    assign_value(L, 1, out);
    return 0;
}

extern "C" int bg3le_math_extract_euler_angles(lua_State* L) {
    const Value m = read_value(L, 1);
    glm::vec3 angle;
    glm::extractEulerAngleXYZ(as_mat4(L, m), angle.x, angle.y, angle.z);
    push_vec(L, glm::vec4(angle, 0.0f), 3);
    return 1;
}

extern "C" int bg3le_math_build_from_euler_angles3(lua_State* L) {
    const glm::vec3 a = read_vec3(L, 1);
    push_mat3(L, glm::mat3(glm::eulerAngleYXZ(a.x, a.y, a.z)));
    return 1;
}

extern "C" int bg3le_math_build_from_euler_angles4(lua_State* L) {
    const glm::vec3 a = read_vec3(L, 1);
    push_mat4(L, glm::eulerAngleYXZ(a.x, a.y, a.z));
    return 1;
}

extern "C" int bg3le_math_decompose(lua_State* L) {
    const Value m = read_value(L, 1);

    glm::quat orientation;
    glm::vec3 scale;
    glm::vec3 translation;
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::decompose(as_mat4(L, m), scale, orientation, translation, skew,
                   perspective);

    const glm::vec3 rotation{glm::yaw(orientation), glm::pitch(orientation),
                             glm::roll(orientation)};
    assign_vec3(L, 2, scale);
    assign_vec3(L, 3, rotation);
    assign_vec3(L, 4, translation);
    return 0;
}

extern "C" int bg3le_math_extract_axis_angle(lua_State* L) {
    const Value m = read_value(L, 1);
    glm::vec3 axis;
    float angle = 0.0f;
    glm::axisAngle(as_mat4(L, m), axis, angle);
    assign_vec3(L, 2, axis);
    lua_pushnumber(L, angle);
    return 1;
}

extern "C" int bg3le_math_build_from_axis_angle3(lua_State* L) {
    const glm::vec3 axis = read_vec3(L, 1);
    push_mat3(L, glm::mat3(
        glm::axisAngleMatrix(axis, (float)luaL_checknumber(L, 2))));
    return 1;
}

extern "C" int bg3le_math_build_from_axis_angle4(lua_State* L) {
    const glm::vec3 axis = read_vec3(L, 1);
    push_mat4(L, glm::axisAngleMatrix(axis, (float)luaL_checknumber(L, 2)));
    return 1;
}

extern "C" int bg3le_math_build_rotation3(lua_State* L) {
    const glm::vec3 v = read_vec3(L, 1);
    push_mat3(L, glm::mat3(glm::rotate((float)luaL_checknumber(L, 2), v)));
    return 1;
}

extern "C" int bg3le_math_build_rotation4(lua_State* L) {
    const glm::vec3 v = read_vec3(L, 1);
    push_mat4(L, glm::rotate((float)luaL_checknumber(L, 2), v));
    return 1;
}

extern "C" int bg3le_math_build_translation(lua_State* L) {
    push_mat4(L, glm::translate(read_vec3(L, 1)));
    return 1;
}

extern "C" int bg3le_math_build_scale(lua_State* L) {
    push_mat4(L, glm::scale(read_vec3(L, 1)));
    return 1;
}

// ---- quaternions ----------------------------------------------------------

extern "C" int bg3le_math_quat_from_euler(lua_State* L) {
    push_quat(L, glm::quat(read_vec3(L, 1)));
    return 1;
}

extern "C" int bg3le_math_quat_from_to_rotation(lua_State* L) {
    push_quat(L, glm::quat(read_vec3(L, 1), read_vec3(L, 2)));
    return 1;
}

extern "C" int bg3le_math_quat_dot(lua_State* L) {
    lua_pushnumber(L, glm::dot(read_quat(L, 1), read_quat(L, 2)));
    return 1;
}

extern "C" int bg3le_math_quat_slerp(lua_State* L) {
    push_quat(L, glm::slerp(read_quat(L, 1), read_quat(L, 2),
                            (float)luaL_checknumber(L, 3)));
    return 1;
}

extern "C" int bg3le_math_quat_to_mat3(lua_State* L) {
    push_mat3(L, glm::mat3_cast(read_quat(L, 1)));
    return 1;
}

extern "C" int bg3le_math_quat_to_mat4(lua_State* L) {
    push_mat4(L, glm::mat4_cast(read_quat(L, 1)));
    return 1;
}

extern "C" int bg3le_math_mat3_to_quat(lua_State* L) {
    const Value m = read_value(L, 1);
    if (m.Kind != Shape::Mat3) return luaL_error(L, "expected a 3x3 matrix");
    push_quat(L, glm::quat_cast(m.M3));
    return 1;
}

extern "C" int bg3le_math_mat4_to_quat(lua_State* L) {
    const Value m = read_value(L, 1);
    if (m.Kind != Shape::Mat4) return luaL_error(L, "expected a 4x4 matrix");
    push_quat(L, glm::quat_cast(m.M4));
    return 1;
}

extern "C" int bg3le_math_quat_normalize(lua_State* L) {
    push_quat(L, glm::normalize(read_quat(L, 1)));
    return 1;
}

extern "C" int bg3le_math_quat_inverse(lua_State* L) {
    push_quat(L, glm::inverse(read_quat(L, 1)));
    return 1;
}

extern "C" int bg3le_math_quat_length(lua_State* L) {
    lua_pushnumber(L, glm::length(read_quat(L, 1)));
    return 1;
}

extern "C" int bg3le_math_quat_rotate(lua_State* L) {
    const glm::quat q = read_quat(L, 1);
    const int arity = arity_of(L, 2);
    if (arity == 4) {
        const Value b = read_value(L, 2);
        push_vec(L, glm::rotate(q, b.V), 4);
        return 1;
    }
    if (arity == 3) {
        push_vec(L, glm::vec4(glm::rotate(q, read_vec3(L, 2)), 0.0f), 3);
        return 1;
    }
    return luaL_error(L, "Expected a vec3 or vec4 value");
}

extern "C" int bg3le_math_quat_rotate_axis_angle(lua_State* L) {
    const glm::quat q = read_quat(L, 1);
    const glm::vec3 axis = read_vec3(L, 2);
    push_quat(L, glm::rotate(q, (float)luaL_checknumber(L, 3), axis));
    return 1;
}

extern "C" int bg3le_math_quat_mul(lua_State* L) {
    const int a = arity_of(L, 1);
    const int b = arity_of(L, 2);

    if (a == 3 && b == 4) {
        push_vec(L, glm::vec4(read_vec3(L, 1) * read_quat(L, 2), 0.0f), 3);
        return 1;
    }
    if (a == 4 && b == 4) {
        push_quat(L, read_quat(L, 1) * read_quat(L, 2));
        return 1;
    }
    if (a == 4 && b == 3) {
        push_vec(L, glm::vec4(read_quat(L, 1) * read_vec3(L, 2), 0.0f), 3);
        return 1;
    }
    return luaL_error(L, "Expected quaternion and vec3 arguments");
}

// ---- scalars --------------------------------------------------------------

extern "C" int bg3le_math_fract(lua_State* L) {
    lua_pushnumber(L, glm::fract((float)luaL_checknumber(L, 1)));
    return 1;
}

extern "C" int bg3le_math_trunc(lua_State* L) {
    lua_pushnumber(L, glm::trunc((float)luaL_checknumber(L, 1)));
    return 1;
}

extern "C" int bg3le_math_sign(lua_State* L) {
    lua_pushnumber(L, glm::sign((float)luaL_checknumber(L, 1)));
    return 1;
}

extern "C" int bg3le_math_clamp(lua_State* L) {
    lua_pushnumber(L, glm::clamp((float)luaL_checknumber(L, 1),
                                 (float)luaL_checknumber(L, 2),
                                 (float)luaL_checknumber(L, 3)));
    return 1;
}

extern "C" int bg3le_math_smoothstep(lua_State* L) {
    lua_pushnumber(L, glm::smoothstep((float)luaL_checknumber(L, 1),
                                      (float)luaL_checknumber(L, 2),
                                      (float)luaL_checknumber(L, 3)));
    return 1;
}

extern "C" int bg3le_math_lerp(lua_State* L) {
    lua_pushnumber(L, glm::lerp((float)luaL_checknumber(L, 1),
                                (float)luaL_checknumber(L, 2),
                                (float)luaL_checknumber(L, 3)));
    return 1;
}

extern "C" int bg3le_math_acos(lua_State* L) {
    lua_pushnumber(L, glm::acos((float)luaL_checknumber(L, 1)));
    return 1;
}

extern "C" int bg3le_math_asin(lua_State* L) {
    lua_pushnumber(L, glm::asin((float)luaL_checknumber(L, 1)));
    return 1;
}

extern "C" int bg3le_math_atan(lua_State* L) {
    lua_pushnumber(L, glm::atan((float)luaL_checknumber(L, 1)));
    return 1;
}

extern "C" int bg3le_math_atan2(lua_State* L) {
    lua_pushnumber(L, glm::atan((float)luaL_checknumber(L, 1),
                                (float)luaL_checknumber(L, 2)));
    return 1;
}

extern "C" int bg3le_math_is_nan(lua_State* L) {
    lua_pushboolean(L, glm::isnan(luaL_checknumber(L, 1)) ? 1 : 0);
    return 1;
}

extern "C" int bg3le_math_is_inf(lua_State* L) {
    lua_pushboolean(L, glm::isinf(luaL_checknumber(L, 1)) ? 1 : 0);
    return 1;
}

extern "C" int bg3le_math_round(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)std::llround(luaL_checknumber(L, 1)));
    return 1;
}

// Upstream draws from the extension state's own generator rather than
// Lua's, so that a mod's randomness is reproducible with the save. bg3le
// has no such state yet, so it keeps one here with the same distributions.
extern "C" int bg3le_math_random(lua_State* L) {
    static std::mt19937_64 rng{std::random_device{}()};

    switch (lua_gettop(L)) {
    case 0: {
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        lua_pushnumber(L, dist(rng));
        return 1;
    }
    case 1: {
        const lua_Integer up = luaL_checkinteger(L, 1);
        luaL_argcheck(L, 1 <= up, 1, "interval is empty");
        std::uniform_int_distribution<std::int64_t> dist(1, up);
        lua_pushinteger(L, (lua_Integer)dist(rng));
        return 1;
    }
    case 2: {
        const lua_Integer low = luaL_checkinteger(L, 1);
        const lua_Integer up = luaL_checkinteger(L, 2);
        luaL_argcheck(L, low <= up, 1, "interval is empty");
        std::uniform_int_distribution<std::int64_t> dist(low, up);
        lua_pushinteger(L, (lua_Integer)dist(rng));
        return 1;
    }
    default:
        return luaL_error(L, "wrong number of arguments");
    }
}

}  // namespace bg3le
