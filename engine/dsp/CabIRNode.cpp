#include "dsp/CabIRNode.h"

#include <array>
#include <atomic>
#include <cmath>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

namespace amp
{
struct CabIRNode::Impl
{
    Impl()
    {
        for (auto* c : { &hpC, &lpC })
            *c = new juce::dsp::IIR::Coefficients<float>(1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
        hp.coefficients = hpC;
        lp.coefficients = lpC;
        formats.registerBasicFormats();
    }

    // Recompute only when a value changed; writes into preallocated coefficient storage (no alloc).
    void updateFilters()
    {
        const float lo = lowCut.load(), hi = highCut.load();
        if (juce::exactlyEqual(lo, appliedLow) && juce::exactlyEqual(hi, appliedHigh))
            return;
        appliedLow = lo;
        appliedHigh = hi;
        hpOn = lo > kLowCutOff; // at the range extremes a cut is switched off entirely (not just very mild)
        lpOn = hi < kHighCutOff;

        using AC = juce::dsp::IIR::ArrayCoefficients<float>;
        store(*hpC, AC::makeHighPass(sampleRate, juce::jlimit(20.0f, 500.0f, lo), 0.707f));
        store(*lpC, AC::makeLowPass(sampleRate, juce::jlimit(1000.0f, (float) (sampleRate * 0.45), hi), 0.707f));
    }

    static void store(juce::dsp::IIR::Coefficients<float>& c, const std::array<float, 6>& a)
    {
        float* d = c.coefficients.getRawDataPointer(); // b0 b1 b2 a1 a2, normalised by a0
        const float inv = 1.0f / a[3];
        d[0] = a[0] * inv; d[1] = a[1] * inv; d[2] = a[2] * inv;
        d[3] = a[4] * inv; d[4] = a[5] * inv;
    }

    juce::dsp::Convolution conv; // default Latency{0}: zero added latency
    juce::AudioFormatManager formats;
    juce::dsp::IIR::Coefficients<float>::Ptr hpC, lpC;
    juce::dsp::IIR::Filter<float> hp, lp;
    juce::SmoothedValue<float> gain { 1.0f };
    std::atomic<float> lowCut { 80.0f }, highCut { 8000.0f }, level { 0.0f };
    static constexpr float kLowCutOff = 20.0f, kHighCutOff = 20000.0f;
    bool hpOn = true, lpOn = true;
    float appliedLow = std::nanf(""), appliedHigh = std::nanf("");
    std::atomic<bool> loaded { false };
    double sampleRate = 48000.0;
};

CabIRNode::CabIRNode() : impl(std::make_unique<Impl>()) {}
CabIRNode::~CabIRNode() = default;

bool CabIRNode::loadIR(const juce::File& file, juce::String& error)
{
    error.clear();
    if (! file.existsAsFile())
    {
        error = "IR file not found: " + file.getFullPathName();
        return false;
    }
    std::unique_ptr<juce::AudioFormatReader> reader(impl->formats.createReaderFor(file));
    if (reader == nullptr)
    {
        error = "Unreadable IR file: " + file.getFullPathName();
        return false;
    }
    if (reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0 || reader->numChannels == 0
        || reader->lengthInSamples > (juce::int64) 16 * 1024 * 1024)
    {
        error = "IR file is empty or too large: " + file.getFullPathName();
        return false;
    }

    juce::AudioBuffer<float> buf((int) reader->numChannels, (int) reader->lengthInSamples);
    if (! reader->read(&buf, 0, buf.getNumSamples(), 0, true, true))
    {
        error = "Failed to decode IR file: " + file.getFullPathName();
        return false;
    }

    impl->conv.loadImpulseResponse(std::move(buf), reader->sampleRate, juce::dsp::Convolution::Stereo::no,
                                   juce::dsp::Convolution::Trim::yes, juce::dsp::Convolution::Normalise::yes);
    impl->loaded.store(true);
    return true;
}

bool CabIRNode::irActive() const
{
    // The convolver idles on a 1-sample unit impulse until the loaded IR is swapped in.
    return impl->loaded.load() && impl->conv.getCurrentIRSize() > 1;
}

void CabIRNode::prepare(const ProcessSpec& s)
{
    impl->sampleRate = s.sampleRate;
    impl->conv.prepare({ s.sampleRate, (juce::uint32) s.maxBlock, 1 });
    impl->hp.reset();
    impl->lp.reset();
    impl->appliedLow = impl->appliedHigh = std::nanf(""); // NaN never compares equal: forces a recompute
    impl->updateFilters();
    impl->gain.reset(s.sampleRate, 0.02);
    impl->gain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(impl->level.load()));
}

void CabIRNode::process(juce::AudioBuffer<float>& io, int numSamples)
{
    if (! impl->loaded.load())
        return; // never loaded an IR: bit-exact passthrough (until the swap-in the convolver is a unit impulse)

    impl->updateFilters();
    impl->gain.setTargetValue(juce::Decibels::decibelsToGain(impl->level.load()));

    float* ch = io.getWritePointer(0);
    float* chans[1] { ch };
    juce::dsp::AudioBlock<float> block(chans, 1, (size_t) numSamples);
    impl->conv.process(juce::dsp::ProcessContextReplacing<float>(block));

    for (int i = 0; i < numSamples; ++i)
    {
        float x = ch[i];
        if (impl->hpOn) x = impl->hp.processSample(x);
        if (impl->lpOn) x = impl->lp.processSample(x);
        ch[i] = x * impl->gain.getNextValue();
    }
}

void CabIRNode::setParam(const juce::String& name, float v)
{
    if (! std::isfinite(v))
        return;
    if (name == "lowCut") impl->lowCut.store(v);
    else if (name == "highCut") impl->highCut.store(v);
    else if (name == "level") impl->level.store(v);
}

juce::NamedValueSet CabIRNode::getParams() const
{
    juce::NamedValueSet p;
    p.set("lowCut", impl->lowCut.load());
    p.set("highCut", impl->highCut.load());
    p.set("level", impl->level.load());
    return p;
}

std::unique_ptr<CabIRNode> makeCabIR() { return std::make_unique<CabIRNode>(); }
} // namespace amp
