#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

namespace amp
{
struct ProcessSpec
{
    double sampleRate = 48000.0;
    int maxBlock = 512;
};

// prepare/release run off the audio thread; process runs on the audio thread (no alloc, no locks).
class ProcessorNode
{
public:
    virtual ~ProcessorNode() = default;

    virtual void prepare(const ProcessSpec&) = 0;
    // Clears the node's signal history (filter / convolution / model state) so it starts afresh. Called off the audio
    // thread, only while the node is not being processed (a fully bypassed node about to be re-engaged); may allocate.
    virtual void reset() {}
    // io has 2 channels; mono nodes read/write channel 0.
    virtual void process(juce::AudioBuffer<float>& io, int numSamples) = 0;
    virtual bool isStereo() const { return false; }
    // RT-safe (implementations store into atomics). Built-ins clamp into dsp/ParamRanges.h and ignore non-finite values.
    virtual void setParam(const juce::String& /*name*/, float /*value*/) {}
    virtual juce::NamedValueSet getParams() const { return {}; }
    virtual juce::MemoryBlock getState() const { return {}; }
    virtual void setState(const juce::MemoryBlock&) {}
};
} // namespace amp
