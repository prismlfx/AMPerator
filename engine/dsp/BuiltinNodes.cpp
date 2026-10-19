#include "dsp/BuiltinNodes.h"

#include <atomic>
#include <cmath>

#include <juce_dsp/juce_dsp.h>

namespace amp
{
namespace
{
class PlaceholderNode final : public ProcessorNode
{
public:
    void prepare(const ProcessSpec&) override {}
    void process(juce::AudioBuffer<float>&, int) override {}
};

// Gate: juce::dsp::NoiseGate (ratio 10, attack 1 ms). Mono, channel 0.
class GateNode final : public ProcessorNode
{
public:
    void prepare(const ProcessSpec& s) override
    {
        gate.prepare({ s.sampleRate, (juce::uint32) s.maxBlock, 1 });
        gate.setRatio(10.0f);
        gate.setAttack(1.0f);
        appliedThreshold = threshold.load();
        appliedRelease = release.load();
        gate.setThreshold(appliedThreshold);
        gate.setRelease(appliedRelease);
        gate.reset();
    }

    void process(juce::AudioBuffer<float>& io, int numSamples) override
    {
        const float t = threshold.load(), r = release.load();
        if (! juce::exactlyEqual(t, appliedThreshold)) { appliedThreshold = t; gate.setThreshold(t); }
        if (! juce::exactlyEqual(r, appliedRelease)) { appliedRelease = r; gate.setRelease(r); }

        float* ch = io.getWritePointer(0);
        juce::dsp::AudioBlock<float> block(&ch, 1, (size_t) numSamples);
        gate.process(juce::dsp::ProcessContextReplacing<float>(block));
    }

    void setParam(const juce::String& name, float v) override
    {
        if (name == "threshold") threshold.store(v);
        else if (name == "release") release.store(v);
    }

    juce::NamedValueSet getParams() const override
    {
        juce::NamedValueSet p;
        p.set("threshold", threshold.load());
        p.set("release", release.load());
        return p;
    }

private:
    juce::dsp::NoiseGate<float> gate;
    std::atomic<float> threshold { -55.0f }, release { 100.0f };
    float appliedThreshold = -55.0f, appliedRelease = 100.0f;
};

// EQ: low shelf 100 Hz, peak (midFreq, Q 1), high shelf 6 kHz (Q 0.707). Mono, channel 0.
class EQNode final : public ProcessorNode
{
public:
    EQNode()
    {
        for (auto* c : { &lowC, &midC, &highC })
            *c = new juce::dsp::IIR::Coefficients<float>(1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
        low.coefficients = lowC;
        mid.coefficients = midC;
        high.coefficients = highC;
    }

    void prepare(const ProcessSpec& s) override
    {
        sampleRate = s.sampleRate;
        low.reset(); mid.reset(); high.reset();
        applied[0] = std::nanf(""); // NaN never compares equal: forces a recompute

        update();
    }

    void process(juce::AudioBuffer<float>& io, int numSamples) override
    {
        update();
        float* ch = io.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i)
            ch[i] = high.processSample(mid.processSample(low.processSample(ch[i])));
    }

    void setParam(const juce::String& name, float v) override
    {
        if (name == "low") p[0].store(v);
        else if (name == "mid") p[1].store(v);
        else if (name == "high") p[2].store(v);
        else if (name == "midFreq") p[3].store(v);
    }

    juce::NamedValueSet getParams() const override
    {
        juce::NamedValueSet n;
        n.set("low", p[0].load());
        n.set("mid", p[1].load());
        n.set("high", p[2].load());
        n.set("midFreq", p[3].load());
        return n;
    }

private:
    // Recompute only when a value changed; writes into preallocated coefficient storage (no alloc).
    void update()
    {
        const float cur[4] = { p[0].load(), p[1].load(), p[2].load(), p[3].load() };
        const bool changed = ! juce::exactlyEqual(cur[0], applied[0]) || ! juce::exactlyEqual(cur[1], applied[1])
                             || ! juce::exactlyEqual(cur[2], applied[2]) || ! juce::exactlyEqual(cur[3], applied[3]);
        if (! changed)
            return;
        for (int i = 0; i < 4; ++i) applied[i] = cur[i];

        using AC = juce::dsp::IIR::ArrayCoefficients<float>;
        const float freq = juce::jlimit(20.0f, (float) (sampleRate * 0.45), cur[3]);
        store(*lowC, AC::makeLowShelf(sampleRate, 100.0f, 0.707f, dbToGain(cur[0])));
        store(*midC, AC::makePeakFilter(sampleRate, freq, 1.0f, dbToGain(cur[1])));
        store(*highC, AC::makeHighShelf(sampleRate, 6000.0f, 0.707f, dbToGain(cur[2])));
    }

    static float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

    static void store(juce::dsp::IIR::Coefficients<float>& c, const std::array<float, 6>& a)
    {
        float* d = c.coefficients.getRawDataPointer(); // b0 b1 b2 a1 a2, normalised by a0
        const float inv = 1.0f / a[3];
        d[0] = a[0] * inv; d[1] = a[1] * inv; d[2] = a[2] * inv;
        d[3] = a[4] * inv; d[4] = a[5] * inv;
    }

    juce::dsp::IIR::Coefficients<float>::Ptr lowC, midC, highC;
    juce::dsp::IIR::Filter<float> low, mid, high;
    std::atomic<float> p[4] { 0.0f, 0.0f, 0.0f, 800.0f };
    float applied[4] {};
    double sampleRate = 48000.0;
};

class OutputGainNode final : public ProcessorNode
{
public:
    void prepare(const ProcessSpec& s) override
    {
        gain.reset(s.sampleRate, 0.02);
        gain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(level.load()));
    }

    void process(juce::AudioBuffer<float>& io, int numSamples) override
    {
        gain.setTargetValue(juce::Decibels::decibelsToGain(level.load()));
        float* ch = io.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i)
            ch[i] *= gain.getNextValue();
    }

    void setParam(const juce::String& name, float v) override
    {
        if (name == "level") level.store(v);
    }

    juce::NamedValueSet getParams() const override
    {
        juce::NamedValueSet p;
        p.set("level", level.load());
        return p;
    }

private:
    std::atomic<float> level { 0.0f };
    juce::SmoothedValue<float> gain { 1.0f };
};
} // namespace

std::unique_ptr<ProcessorNode> makeGate() { return std::make_unique<GateNode>(); }
std::unique_ptr<ProcessorNode> makeEQ() { return std::make_unique<EQNode>(); }
std::unique_ptr<ProcessorNode> makeOutputGain() { return std::make_unique<OutputGainNode>(); }
std::unique_ptr<ProcessorNode> makePlaceholder() { return std::make_unique<PlaceholderNode>(); }
} // namespace amp
