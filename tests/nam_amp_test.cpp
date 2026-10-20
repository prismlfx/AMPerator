#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/NamAmpNode.h"
#include "engine/RtSafety.h"
#include "support/OfflineRender.h"

namespace
{
juce::File fixturesDir() { return juce::File(AMP_FIXTURES_DIR); }
juce::File tinyModel() { return fixturesDir().getChildFile("assets").getChildFile("tiny.nam"); }

std::unique_ptr<amp::NamAmpNode> ampWithFixture()
{
    juce::String err;
    auto loaded = amp::NamAmpNode::loadModel(tinyModel(), err);
    INFO(err);
    REQUIRE(loaded != nullptr);
    auto node = amp::makeNamAmp();
    node->setModel(std::move(loaded));
    return node;
}

juce::AudioBuffer<float> readWav(const juce::File& f)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(f.createInputStream().release(), true));
    REQUIRE(reader != nullptr);
    juce::AudioBuffer<float> b((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read(&b, 0, b.getNumSamples(), 0, true, true);
    return b;
}

juce::AudioBuffer<float> sine(float freq, float amp, double sr, double seconds)
{
    const int n = (int) (seconds * sr);
    juce::AudioBuffer<float> b(1, n);
    for (int i = 0; i < n; ++i)
        b.setSample(0, i, amp * (float) std::sin(2.0 * juce::MathConstants<double>::pi * freq * i / sr));
    return b;
}

float rmsDb(const juce::AudioBuffer<float>& b, int start)
{
    double sum = 0;
    for (int i = start; i < b.getNumSamples(); ++i)
        sum += (double) b.getSample(0, i) * b.getSample(0, i);
    const double rms = std::sqrt(sum / (b.getNumSamples() - start));
    return (float) (20.0 * std::log10(juce::jmax(rms, 1e-12)));
}

bool allFinite(const juce::AudioBuffer<float>& b)
{
    for (int i = 0; i < b.getNumSamples(); ++i)
        if (! std::isfinite(b.getSample(0, i)))
            return false;
    return true;
}

// Frequency of the strongest FFT bin (Hann window, parabolic peak interpolation) over 2^15 samples from `start`.
double dominantFrequency(const juce::AudioBuffer<float>& b, int start, double sr)
{
    constexpr int order = 15, n = 1 << order;
    REQUIRE(b.getNumSamples() - start >= n);
    std::vector<float> data(2 * n, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * i / (n - 1));
        data[(size_t) i] = (float) (b.getSample(0, start + i) * w);
    }
    juce::dsp::FFT fft(order);
    fft.performFrequencyOnlyForwardTransform(data.data());
    int peak = 1;
    for (int k = 2; k < n / 2 - 1; ++k)
        if (data[(size_t) k] > data[(size_t) peak])
            peak = k;
    const double a = std::log(data[(size_t) peak - 1] + 1e-20), m = std::log(data[(size_t) peak] + 1e-20),
                 c = std::log(data[(size_t) peak + 1] + 1e-20);
    const double offset = 0.5 * (a - c) / (a - 2.0 * m + c);
    return (peak + offset) * sr / n;
}
} // namespace

TEST_CASE("nam_amp: loads fixture model")
{
    juce::String err;
    auto loaded = amp::NamAmpNode::loadModel(tinyModel(), err);
    INFO(err);
    REQUIRE(loaded != nullptr);
    REQUIRE(err.isEmpty());

    auto node = amp::makeNamAmp();
    node->setModel(std::move(loaded));
    REQUIRE(node->modelSampleRate() == 48000.0);
}

TEST_CASE("nam_amp: bad file returns error without throwing")
{
    juce::TemporaryFile tmp(".nam");
    REQUIRE(tmp.getFile().replaceWithText("{ this is not a model"));

    juce::String err;
    std::unique_ptr<amp::NamAmpNode::Loaded> loaded;
    REQUIRE_NOTHROW(loaded = amp::NamAmpNode::loadModel(tmp.getFile(), err));
    REQUIRE(loaded == nullptr);
    REQUIRE(err.isNotEmpty());

    juce::String err2;
    REQUIRE_NOTHROW(loaded = amp::NamAmpNode::loadModel(fixturesDir().getChildFile("missing.nam"), err2));
    REQUIRE(loaded == nullptr);
    REQUIRE(err2.isNotEmpty());
}

TEST_CASE("nam_amp: processes DI at native rate")
{
    auto node = ampWithFixture();
    // di_48k.wav is generated at NAM's loudness reference level (peak -3 dBFS), so normalize should land near -18.
    const auto di = readWav(fixturesDir().getChildFile("di").getChildFile("di_48k.wav"));
    REQUIRE(di.getNumSamples() == 5 * 48000);

    const auto out = renderOffline(*node, di, 48000.0);
    REQUIRE(allFinite(out));
    const float rms = rmsDb(out, 4800);
    INFO("output RMS dBFS: " << rms);
    REQUIRE(rms > -60.0f);
    REQUIRE(std::abs(rms - (-18.0f)) <= 6.0f);
}

TEST_CASE("nam_amp: normalize applies (-18 - loudness) dB")
{
    const auto di = readWav(fixturesDir().getChildFile("di").getChildFile("di_48k.wav"));
    auto on = ampWithFixture();
    auto off = ampWithFixture();
    off->setParam("normalize", 0.0f);
    REQUIRE(static_cast<float>(off->getParams()["normalize"]) == 0.0f);

    const float diff = rmsDb(renderOffline(*on, di, 48000.0), 4800) - rmsDb(renderOffline(*off, di, 48000.0), 4800);
    // tiny.nam metadata: loudness -37.8406867980957 dB
    REQUIRE(std::abs(diff - (-18.0f + 37.8406868f)) < 0.1f);
}

TEST_CASE("nam_amp: NamAmp resamples to host rate")
{
    constexpr double hostSr = 44100.0;
    auto node = ampWithFixture();
    const auto in = sine(440.0f, 0.1f, hostSr, 1.0);

    const auto out = renderOffline(*node, in, hostSr);
    REQUIRE(allFinite(out));
    REQUIRE(rmsDb(out, 4410) > -60.0f);
    const double f = dominantFrequency(out, 4410, hostSr);
    INFO("dominant frequency: " << f);
    REQUIRE(std::abs(f - 440.0) <= 2.0);
}

TEST_CASE("nam_amp: setModel after prepare re-prepares for current spec")
{
    constexpr double hostSr = 44100.0;
    constexpr int block = 256;
    auto node = amp::makeNamAmp();
    node->prepare({ hostSr, block });

    juce::String err;
    node->setModel(amp::NamAmpNode::loadModel(tinyModel(), err));
    REQUIRE(err.isEmpty());

    const auto in = sine(440.0f, 0.1f, hostSr, 1.0);
    juce::AudioBuffer<float> out(1, in.getNumSamples()), work(2, block);
    for (int pos = 0; pos < in.getNumSamples(); pos += block)
    {
        const int n = juce::jmin(block, in.getNumSamples() - pos);
        work.clear();
        work.copyFrom(0, 0, in, 0, pos, n);
        node->process(work, n);
        out.copyFrom(0, pos, work, 0, 0, n);
    }
    REQUIRE(allFinite(out));
    REQUIRE(std::abs(dominantFrequency(out, 4410, hostSr) - 440.0) <= 2.0);
}

TEST_CASE("nam_amp: resampler has no underruns across rates and block sizes")
{
    const double hostSr = GENERATE(44100.0, 88200.0, 96000.0);
    const int block = GENERATE(1, 7, 64, 333);
    CAPTURE(hostSr, block);

    auto node = ampWithFixture();
    const auto in = sine(440.0f, 0.1f, hostSr, 0.5);
    const auto out = renderOffline(*node, in, hostSr, block);
    REQUIRE(allFinite(out));
    REQUIRE(node->underrunCount() == 0);
}

TEST_CASE("nam_amp: processing allocates nothing")
{
    constexpr double hostSr = 44100.0; // resampling path (model runs at 48 kHz)
    const int block = GENERATE(64, 333);
    CAPTURE(block);
    auto node = ampWithFixture();
    node->prepare({ hostSr, block });
    const auto in = sine(440.0f, 0.1f, hostSr, 2.0);
    juce::AudioBuffer<float> work(2, block);

    amp::RtSafety::reset();
    for (int pos = 0; pos + block <= in.getNumSamples(); pos += block)
    {
        work.copyFrom(0, 0, in, 0, pos, block);
        const amp::RtSafety::ScopedNoAlloc guard;
        node->process(work, block);
    }
    REQUIRE(amp::RtSafety::violations() == 0);
    REQUIRE(node->underrunCount() == 0);
}

TEST_CASE("nam_amp: no model = passthrough")
{
    auto node = amp::makeNamAmp();
    REQUIRE(node->modelSampleRate() == 48000.0);
    const auto in = sine(440.0f, 0.3f, 44100.0, 0.5);
    const auto out = renderOffline(*node, in, 44100.0);
    for (int i = 0; i < in.getNumSamples(); ++i)
        REQUIRE(out.getSample(0, i) == in.getSample(0, i));
}
