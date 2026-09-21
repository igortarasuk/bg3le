#pragma once

#include <GameDefinitions/Base/Base.h>

BEGIN_NS(material)

struct ResourceParameter
{
    FixedString ParameterName;
    bool Enabled{ true };
};

struct ScalarResourceParameter : public ResourceParameter
{
    float Value{ .0f };
    float BaseValue{ .0f };
};

struct Vector2ResourceParameter : public ResourceParameter
{
    glm::fvec2 Value{ .0f };
    glm::fvec2 BaseValue{ .0f };
};

struct Vector3ResourceParameter : public ResourceParameter
{
    glm::fvec3 Value{ .0f };
    glm::fvec3 BaseValue{ .0f };
    bool IsColor{ false };
};

struct Vector4ResourceParameter : public ResourceParameter
{
    glm::aligned_vec4 Value{ .0f };
    glm::aligned_vec4 BaseValue{ .0f };
    bool IsColor{ false };
};

struct Texture2DResourceParameter : public ResourceParameter
{
    FixedString ID;
};

struct VirtualTextureResourceParameter : public ResourceParameter
{
    int32_t Index{ 0 };
    FixedString ID;
};

struct ScalarParameterPreset
{
    FixedString Parameter;
    float Value;
    bool Enabled;
    bool Color;
    bool Custom;
};

struct Vector2ParameterPreset
{
    FixedString Parameter;
    glm::fvec2 Value;
    bool Enabled;
    bool Color;
    bool Custom;
};

struct Vector3ParameterPreset
{
    FixedString Parameter;
    Vector3 Value;
    bool Enabled;
    bool Color;
    bool Custom;
};

struct VectorParameterPreset
{
    FixedString Parameter;
    glm::aligned_vec4 Value;
    bool Enabled;
    bool Color;
    bool Custom;
};

struct Texture2DParameterPreset
{
    FixedString Parameter;
    FixedString Value;
    bool Enabled;
    bool Color;
    bool Custom;
};

struct VirtualTextureParameterPreset
{
    FixedString Parameter;
    FixedString Value;
    bool Enabled;
    bool Color;
    bool Custom;
};

END_NS()
