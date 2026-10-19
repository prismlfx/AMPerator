#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include "dsp/BuiltinNodes.h"
#include "support/OfflineRender.h"

namespace
{
constexpr double kSr = 48000.0;

juce::AudioBuffer<float> sine(float freq, float amp, double seconds)
{
    const int n = (int) (seconds * kSr);
    juce::AudioBuffer<float> b(1, n);
    for (int i = 0; i < n; ++i)
        b.setSample(0, i, amp * std::sin(2.0 * juce::MathConstants<double>::pi * freq * i / kSr));
    return b;
}

float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

// RMS in dBFS over samples after `skipSeconds`.
float rmsDb(const juce::AudioBuffer<float>& b, double skipSeconds = 0.2)
{
    const int start = (int) (skipSeconds * kSr);
    double sum = 0;
    for (int i = start; i < b.getNumSamples(); ++i)
        sum += (double) b.getSample(0, i) * b.getSample(0, i);
    const double rms = std::sqrt(sum / (b.getNumSamples() - start));
    return (float) (20.0 * std::log10(juce::jmax(rms, 1e-12)));
}

float peakAfter(const juce::AudioBuffer<float>& b, double skipSeconds = 0.2)
{
    const int start = (int) (skipSeconds * kSr);
    float p = 0;
    for (int i = start; i < b.getNumSamples(); ++i)
        p = juce::jmax(p, std::abs(b.getSample(0, i)));
    return p;
}
} // namespace

TEST_CASE("builtin_nodes: gate silences signal below threshold")
{
    auto gate = amp::makeGate();
    auto in = sine(440.0f, dbToGain(-70.0f) * 1.41421356f, 1.0); // RMS -70 dBFS
    auto out = renderOffline(*gate, in, kSr);
    REQUIRE(rmsDb(out) < -90.0f);
}

TEST_CASE("builtin_nodes: gate passes signal above threshold")
{
    auto gate = amp::makeGate();
    auto in = sine(440.0f, dbToGain(-20.0f) * 1.41421356f, 1.0);
    auto out = renderOffline(*gate, in, kSr);
    REQUIRE(std::abs(rmsDb(out) - rmsDb(in)) < 0.5f);
}

TEST_CASE("builtin_nodes: EQ flat is identity")
{
    auto eq = amp::makeEQ();
    auto in = sine(1000.0f, 0.5f, 0.5);
    auto out = renderOffline(*eq, in, kSr);
    float maxDiff = 0;
    for (int i = 0; i < in.getNumSamples(); ++i)
        maxDiff = juce::jmax(maxDiff, std::abs(in.getSample(0, i) - out.getSample(0, i)));
    REQUIRE(maxDiff < 1e-5f);
}

TEST_CASE("builtin_nodes: EQ +6 dB high shelf boosts highs by ~6 dB")
{
    // The 6 kHz / Q 0.707 shelf only reaches 4.7 dB at 8 kHz, so measure at 12 kHz (~5.8 dB).
    auto eq = amp::makeEQ();
    eq->setParam("high", 6.0f);
    auto in = sine(12000.0f, 0.1f, 1.0);
    auto out = renderOffline(*eq, in, kSr);
    REQUIRE(std::abs((rmsDb(out) - rmsDb(in)) - 6.0f) < 1.0f);
}

TEST_CASE("builtin_nodes: output gain -6 dB halves amplitude")
{
    auto g = amp::makeOutputGain();
    g->setParam("level", -6.0f);
    auto in = sine(440.0f, 0.5f, 1.0);
    auto out = renderOffline(*g, in, kSr);
    REQUIRE(peakAfter(out) == Catch::Approx(0.5f * 0.5012f).margin(0.01));
}

TEST_CASE("builtin_nodes: placeholder is passthrough")
{
    auto p = amp::makePlaceholder();
    auto in = sine(440.0f, 0.5f, 0.1);
    auto out = renderOffline(*p, in, kSr);
    for (int i = 0; i < in.getNumSamples(); ++i)
        REQUIRE(out.getSample(0, i) == in.getSample(0, i));
}

TEST_CASE("builtin_nodes: params round trip and unknown names are ignored")
{
    auto eq = amp::makeEQ();
    REQUIRE((float) eq->getParams()["midFreq"] == 800.0f);
    eq->setParam("mid", 3.0f);
    eq->setParam("bogus", 1.0f);
    REQUIRE((float) eq->getParams()["mid"] == 3.0f);
    REQUIRE(! eq->getParams().contains("bogus"));

    auto gate = amp::makeGate();
    REQUIRE((float) gate->getParams()["threshold"] == -55.0f);
    REQUIRE((float) gate->getParams()["release"] == 100.0f);
    REQUIRE((float) amp::makeOutputGain()->getParams()["level"] == 0.0f);
}

TEST_CASE("builtin_nodes: params are block-size independent")
{
    auto in = sine(2000.0f, 0.3f, 0.5);
    auto make = [] {
        auto eq = amp::makeEQ();
        eq->setParam("low", 4.0f);
        eq->setParam("mid", -5.0f);
        eq->setParam("high", 6.0f);
        return eq;
    };
    auto a = make();
    auto b = make();
    auto outA = renderOffline(*a, in, kSr, 64);
    auto outB = renderOffline(*b, in, kSr, 512);
    float maxDiff = 0;
    for (int i = 0; i < in.getNumSamples(); ++i)
        maxDiff = juce::jmax(maxDiff, std::abs(outA.getSample(0, i) - outB.getSample(0, i)));
    REQUIRE(maxDiff < 1e-4f);

    auto g1 = amp::makeOutputGain();
    auto g2 = amp::makeOutputGain();
    g1->setParam("level", -9.0f);
    g2->setParam("level", -9.0f);
    auto o1 = renderOffline(*g1, in, kSr, 64);
    auto o2 = renderOffline(*g2, in, kSr, 512);
    maxDiff = 0;
    for (int i = 0; i < in.getNumSamples(); ++i)
        maxDiff = juce::jmax(maxDiff, std::abs(o1.getSample(0, i) - o2.getSample(0, i)));
    REQUIRE(maxDiff < 1e-4f);
}
