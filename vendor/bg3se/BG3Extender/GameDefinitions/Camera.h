#pragma once

#include <GameDefinitions/Base/Base.h>

BEGIN_SE()

struct DOFSettings
{
    bool DOF;
    bool NearOnly;
    bool FarOnly;
    float FocalDistance;
    float NearSharpDistance;
    float FarSharpDistance;
    float Aperture;
};

struct DeltaExposureSettings
{
    float DeltaCompensation;
    float DeltaMin;
    float DeltaMax;
};

struct DeltaExposure
{
    bool Enabled;
    DeltaExposureSettings Settings;
};

struct ExposureSettings
{
    float ExposureMin;
    float ExposureMax;
    float ExposureCompensation;
    float ExposureMinPercentile;
    float ExposureMaxPercentile;
    int ExposureMeteringMode;
    bool Exposure;
    DeltaExposure Delta;
};


struct PostProcessCameraSetting
{
    DOFSettings DOF;
    DOFSettings AlternateDOF;
    std::array<ExposureSettings, 2> Exposure;
    float LensFlareTreshold;
    float LensFlareIntensity;
    float LensFlareGhostDispersal;
    float LensFlareHaloWidth;
    float LensFlareChromaticDistortion;
    bool LensFlareEnabled;
    bool DOFForcedSettingsEnabled;
};

struct ColorCorrectionRange
{
    glm::vec2 XYOffset;
    float Saturation;
    float ExposureShift;
};

struct ColorCorrectionSettings
{
    ColorCorrectionRange GlobalRange;
    ColorCorrectionRange DarkRange;
    ColorCorrectionRange ShadowRange;
    ColorCorrectionRange MidtonesRange;
    ColorCorrectionRange HighlightRange;
    ColorCorrectionRange SpecularRange;
    glm::vec2 LutBlendFactors;
    int LutApplyMode;
    float Hue;
    float Saturation;
    float Contrast;
    float Pivot;
    float Shadows;
    float Highlights;
};

struct PostProcessSetting
{
    glm::mat4 WhiteBalanceMatrix;
    ColorCorrectionSettings ColorCorrection;
    [[bg3::hidden]] void* GradingLutTexture2;
    ColorCorrectionSettings BlendedColorCorrection;
    [[bg3::hidden]] void* BlendedLutTexture;
    float ColorCorrectionInterpolationFactor;
    float WhiteTemperature;
    float WhiteBalanceTint;
    float Brightness;
    bool GradingLutEnabled;
    FixedString GradingLut;
    [[bg3::hidden]] void* GradingLutTexture;
    bool UseViewModeHDRSettings;
    PostProcessCameraSetting Camera;
    float BloomThreshold;
    float BloomAmount;
    float GodRaysRayIntensity;
    float GodRaysPower;
    float GodRaysThreshold;
    float ToneMapHighlightsFixAmount;
    float FadeOut;
    glm::vec3 VignetteColor;
    float VignetteIntensity;
    float VignettePower;
    bool Bloom;
    bool GodRaysEnabled;
    bool Vignette;
};

END_SE()
