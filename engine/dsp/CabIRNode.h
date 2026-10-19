#pragma once

#include <memory>

#include "dsp/Node.h"

namespace amp
{
// Speaker-cabinet impulse response (.wav). Mono, channel 0. Zero-added-latency convolution, then
// lowCut (2nd-order high-pass, Q 0.707) and highCut (2nd-order low-pass, Q 0.707), then level.
// Params: lowCut Hz (80, clamped [20, 500]), highCut Hz (8000, clamped [1000, 0.45*sr]), level dB (0).
// lowCut <= 20 Hz and highCut >= 20 kHz switch the respective filter off entirely.
// Non-finite param values are ignored. No IR ever loaded = bit-exact passthrough
// (between loadIR and the swap-in the convolver idles as a unit impulse, so audio is not interrupted).
class CabIRNode : public ProcessorNode
{
public:
    CabIRNode();
    ~CabIRNode() override;

    // Off the audio thread; may be called before or after prepare(). Never throws. Decodes the file
    // synchronously (false + `error` when missing/unreadable/empty), then hands it to the convolver,
    // which swaps it in asynchronously during later process() calls (see irActive()).
    bool loadIR(const juce::File&, juce::String& error);
    // True once a loaded IR is live in the convolver (RT-safe). Test/UI support. Reports true only for
    // IRs that are >= 2 samples long after trimming (a 1-sample IR is indistinguishable from the idle state).
    bool irActive() const;

    void prepare(const ProcessSpec&) override;
    void process(juce::AudioBuffer<float>& io, int numSamples) override;
    void setParam(const juce::String& name, float value) override;
    juce::NamedValueSet getParams() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

std::unique_ptr<CabIRNode> makeCabIR();
} // namespace amp
