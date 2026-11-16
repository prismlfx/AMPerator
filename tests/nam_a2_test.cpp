#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "dsp/NamAmpNode.h"
#include "support/OfflineRender.h"

namespace
{
juce::File examplesDir() { return juce::File(AMP_NAM_EXAMPLES_DIR); }

void checkModelLoadsAndRenders(const char* fileName)
{
    juce::String err;
    auto loaded = amp::NamAmpNode::loadModel(examplesDir().getChildFile(fileName), err);
    INFO(fileName << ": " << err);
    REQUIRE(loaded != nullptr);
    auto node = amp::makeNamAmp();
    node->setModel(std::move(loaded));

    constexpr double sr = 48000.0;
    const int n = (int) (0.5 * sr);
    const float amp = std::pow(10.0f, -20.0f / 20.0f);
    juce::AudioBuffer<float> in(1, n);
    for (int i = 0; i < n; ++i)
        in.setSample(0, i, amp * (float) std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / sr));

    const auto out = renderOffline(*node, in, sr, 128);

    double sum = 0;
    for (int i = 0; i < out.getNumSamples(); ++i)
        REQUIRE(std::isfinite(out.getSample(0, i)));
    const int half = out.getNumSamples() / 2;
    for (int i = half; i < out.getNumSamples(); ++i)
        sum += (double) out.getSample(0, i) * out.getSample(0, i);
    const double rmsDb = 20.0 * std::log10(std::max(std::sqrt(sum / (out.getNumSamples() - half)), 1e-12));
    INFO("second-half RMS dBFS: " << rmsDb);
    REQUIRE(rmsDb > -60.0);
}
} // namespace

TEST_CASE("nam: A2 slimmable container model loads and renders") { checkModelLoadsAndRenders("A2.nam"); }

TEST_CASE("nam: A2 feature-test wavenet loads") { checkModelLoadsAndRenders("wavenet_a2_feature_test.nam"); }
