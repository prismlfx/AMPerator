#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <optional>
#include <string_view>

namespace amp
{
/** Documented range of every built-in node parameter (the UI's knobs, ui/src/views/edit.ts PARAMS, use the same).
    Values from rig files and the bridge are clamped into these before they reach the DSP. */
struct ParamRange
{
    std::string_view type, name;
    float min, max;
};

inline constexpr ParamRange kParamRanges[] = {
    { "Gate", "threshold", -90.0f, 0.0f },     { "Gate", "release", 5.0f, 1000.0f },
    { "EQ", "low", -12.0f, 12.0f },            { "EQ", "mid", -12.0f, 12.0f },
    { "EQ", "high", -12.0f, 12.0f },           { "EQ", "midFreq", 200.0f, 5000.0f },
    { "NamAmp", "inputGain", -24.0f, 24.0f },  { "NamAmp", "outputGain", -24.0f, 24.0f },
    { "NamAmp", "normalize", 0.0f, 1.0f },     { "CabIR", "lowCut", 20.0f, 500.0f },
    { "CabIR", "highCut", 1000.0f, 20000.0f }, { "CabIR", "level", -24.0f, 12.0f },
    { "Output", "level", -60.0f, 12.0f },
};

/** `value` clamped into the range of (type, name); nullopt for a non-finite value or an unknown parameter. RT-safe. */
inline std::optional<float> clampParam(std::string_view type, const juce::String& name, float value)
{
    if (! std::isfinite(value))
        return std::nullopt;
    for (const auto& r : kParamRanges)
        if (r.type == type && name == juce::String(r.name.data(), r.name.size()))
            return juce::jlimit(r.min, r.max, value);
    return std::nullopt;
}
} // namespace amp
