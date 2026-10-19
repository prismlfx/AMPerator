#pragma once

#include <memory>

#include "dsp/Node.h"

namespace amp
{
// Neural Amp Modeler capture (.nam). Mono, channel 0. Runs the model at its own sample rate and
// resamples host -> model -> host when the device rate differs.
// Params: inputGain dB (0), outputGain dB (0), normalize bool (1): when the model has "loudness"
// metadata, normalize adds (-18 - loudness) dB at the output. No model loaded = passthrough.
class NamAmpNode : public ProcessorNode
{
public:
    // Opaque loaded model (+ metadata). Complete here only so callers can own it via unique_ptr.
    struct Loaded
    {
        struct Data; // defined in NamAmpNode.cpp
        explicit Loaded(std::unique_ptr<Data>);
        ~Loaded();
        std::unique_ptr<Data> data;
    };

    NamAmpNode();
    ~NamAmpNode() override;

    // Off the audio thread. Never throws; on failure returns nullptr and fills `error`.
    static std::unique_ptr<Loaded> loadModel(const juce::File&, juce::String& error);

    // Not concurrently with process(). Before prepare(), or later off the audio thread (re-prepares
    // the model and resamplers for the current spec).
    void setModel(std::unique_ptr<Loaded>);
    double modelSampleRate() const; // from model metadata, default 48000
    // Test support: host blocks the resampler could not fill completely (padded with silence) since the last
    // prepare()/setModel(). Should always stay 0; counted in all build types.
    int underrunCount() const;

    void prepare(const ProcessSpec&) override;
    void process(juce::AudioBuffer<float>& io, int numSamples) override;
    void setParam(const juce::String& name, float value) override;
    juce::NamedValueSet getParams() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

std::unique_ptr<NamAmpNode> makeNamAmp();
} // namespace amp
