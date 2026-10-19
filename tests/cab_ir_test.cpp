#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <juce_audio_formats/juce_audio_formats.h>

#include "dsp/CabIRNode.h"

namespace
{
juce::File fixtureIR() { return juce::File(AMP_FIXTURES_DIR).getChildFile("assets/ir_48k.wav"); }

// JUCE's Convolution swaps a newly loaded IR in asynchronously during process(). Process silence in
// small blocks until the node reports the IR live (bounded: ~5 s of wall time) so measurements are
// deterministic. Must be called after prepare().
bool waitForIR(amp::CabIRNode& node, double sr, int block)
{
    juce::AudioBuffer<float> work(2, block);
    const auto deadline = juce::Time::getMillisecondCounter() + 5000;
    while (! node.irActive())
    {
        if (juce::Time::getMillisecondCounter() > deadline)
            return false;
        work.clear();
        node.process(work, block);
        juce::Thread::sleep(2);
    }
    // The swap-in crossfades old->new engine over a short ramp: run 0.5 s of silence so it completes.
    for (int done = 0; done < sr / 2; done += block)
    {
        work.clear();
        node.process(work, block);
    }
    return true;
}

// prepare + (optional) wait for IR + render `in` in blocks. Unlike renderOffline, prepares exactly once
// so a pre-prepare loadIR is not re-swapped after the wait.
juce::AudioBuffer<float> render(amp::CabIRNode& node, const juce::AudioBuffer<float>& in, double sr,
                                bool waitIR = true, int block = 128)
{
    node.prepare({ sr, block });
    if (waitIR)
        REQUIRE(waitForIR(node, sr, block));
    juce::AudioBuffer<float> out(1, in.getNumSamples());
    juce::AudioBuffer<float> work(2, block);
    for (int pos = 0; pos < in.getNumSamples(); pos += block)
    {
        const int n = juce::jmin(block, in.getNumSamples() - pos);
        work.clear();
        work.copyFrom(0, 0, in, 0, pos, n);
        node.process(work, n);
        out.copyFrom(0, pos, work, 0, 0, n);
    }
    return out;
}

juce::AudioBuffer<float> impulse(int n)
{
    juce::AudioBuffer<float> b(1, n);
    b.clear();
    b.setSample(0, 0, 1.0f);
    return b;
}

float peakAbs(const juce::AudioBuffer<float>& b) { return b.getMagnitude(0, b.getNumSamples()); }

double rms(const juce::AudioBuffer<float>& b, int from)
{
    double s = 0;
    for (int i = from; i < b.getNumSamples(); ++i)
        s += (double) b.getSample(0, i) * b.getSample(0, i);
    return std::sqrt(s / (b.getNumSamples() - from));
}

juce::AudioBuffer<float> sine(float freq, double sr, double seconds)
{
    const int n = (int) (seconds * sr);
    juce::AudioBuffer<float> b(1, n);
    for (int i = 0; i < n; ++i)
        b.setSample(0, i, 0.5f * (float) std::sin(2.0 * juce::MathConstants<double>::pi * freq * i / sr));
    return b;
}

juce::AudioBuffer<float> readFixture()
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(fixtureIR()));
    REQUIRE(r != nullptr);
    juce::AudioBuffer<float> b(1, (int) r->lengthInSamples);
    r->read(&b, 0, b.getNumSamples(), 0, true, false);
    return b;
}

// Writes a 48 kHz mono 16-bit wav holding a near-unit impulse: 0.9 at index 0 plus a -46 dB tap at
// index 1. The second tap keeps the trimmed IR 2 samples long so irActive() can see the swap-in
// (a 1-sample IR looks like the convolver's idle state); it perturbs a 12 kHz tone by < 1%.
juce::File writeUnitImpulseWav(const juce::TemporaryFile& tmp)
{
    juce::AudioBuffer<float> b(1, 64);
    b.clear();
    b.setSample(0, 0, 0.9f);
    b.setSample(0, 1, 0.0045f);
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os = tmp.getFile().createOutputStream();
    REQUIRE(os != nullptr);
    auto w = wav.createWriterFor(os, juce::AudioFormatWriterOptions{}
                                         .withSampleRate(48000.0)
                                         .withNumChannels(1)
                                         .withBitsPerSample(16));
    REQUIRE(w != nullptr);
    REQUIRE(w->writeFromAudioSampleBuffer(b, 0, b.getNumSamples()));
    w.reset();
    return tmp.getFile();
}
} // namespace

TEST_CASE("cab_ir: impulse in yields IR out")
{
    auto node = amp::makeCabIR();
    juce::String err;
    REQUIRE(node->loadIR(fixtureIR(), err));
    node->setParam("lowCut", 20.0f);
    node->setParam("highCut", 20000.0f);

    const auto out = render(*node, impulse(8192), 48000.0);
    const auto ref = readFixture();

    // Trim::yes may drop leading silence, so correlate over a small lag window (scale-invariant).
    double best = -1.0;
    for (int lag = -300; lag <= 300; ++lag)
    {
        double dot = 0, a = 0, b = 0;
        for (int i = 0; i < ref.getNumSamples(); ++i)
        {
            const int j = i + lag;
            if (j < 0 || j >= out.getNumSamples()) continue;
            const double x = ref.getSample(0, i), y = out.getSample(0, j);
            dot += x * y; a += x * x; b += y * y;
        }
        if (a > 0 && b > 0)
            best = std::max(best, dot / std::sqrt(a * b));
    }
    INFO("best correlation " << best);
    REQUIRE(best > 0.99);
}

TEST_CASE("cab_ir: IR at 48k works at 44.1k host")
{
    auto node = amp::makeCabIR();
    juce::String err;
    REQUIRE(node->loadIR(fixtureIR(), err));
    REQUIRE(err.isEmpty());

    const auto out = render(*node, impulse(8192), 44100.0);
    REQUIRE(peakAbs(out) > 0.01f);
}

TEST_CASE("cab_ir: high cut attenuates 12 kHz by at least 12 dB at 6 kHz setting")
{
    // Near-unit-impulse IR (flat response), so the only spectral shaping is the cuts. Compare the same node's
    // 12 kHz output with highCut at 6 kHz vs. at the maximum (~20 kHz, effectively open).
    juce::TemporaryFile tmp(".wav");
    const auto wav = writeUnitImpulseWav(tmp);
    constexpr double sr = 48000.0;

    auto measure = [&](float highCut)
    {
        auto node = amp::makeCabIR();
        juce::String err;
        REQUIRE(node->loadIR(wav, err));
        node->setParam("lowCut", 20.0f);
        node->setParam("highCut", highCut);
        return rms(render(*node, sine(12000.0f, sr, 0.5), sr), 12000);
    };
    const double open = measure(20000.0f);
    const double cut = measure(6000.0f);
    REQUIRE(open > 0.0);
    const double attenDb = 20.0 * std::log10(open / cut);
    INFO("attenuation dB " << attenDb);
    REQUIRE(attenDb >= 12.0);
}

TEST_CASE("cab_ir: missing file returns error")
{
    auto node = amp::makeCabIR();
    juce::String err;
    REQUIRE_FALSE(node->loadIR(juce::File("/nonexistent/dir/nope.wav"), err));
    REQUIRE(err.isNotEmpty());

    // A file that exists but is not audio also fails cleanly.
    juce::TemporaryFile junk(".wav");
    junk.getFile().replaceWithText("not a wav");
    err.clear();
    REQUIRE_FALSE(node->loadIR(junk.getFile(), err));
    REQUIRE(err.isNotEmpty());
}

TEST_CASE("cab_ir: no IR = passthrough")
{
    auto node = amp::makeCabIR();
    node->setParam("level", -12.0f); // even with non-default params: nothing loaded, nothing touched

    juce::AudioBuffer<float> in(1, 4096);
    juce::Random rng(7);
    for (int i = 0; i < in.getNumSamples(); ++i)
        in.setSample(0, i, rng.nextFloat() * 2.0f - 1.0f);

    const auto out = render(*node, in, 48000.0, false);
    for (int i = 0; i < in.getNumSamples(); ++i)
        REQUIRE(out.getSample(0, i) == in.getSample(0, i));
}

TEST_CASE("cab_ir: non-finite params are ignored")
{
    auto node = amp::makeCabIR();
    node->setParam("lowCut", std::nanf(""));
    node->setParam("highCut", INFINITY);
    node->setParam("level", -INFINITY);
    const auto p = node->getParams();
    REQUIRE((float) p["lowCut"] == 80.0f);
    REQUIRE((float) p["highCut"] == 8000.0f);
    REQUIRE((float) p["level"] == 0.0f);
}
