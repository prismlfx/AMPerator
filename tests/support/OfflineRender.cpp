#include "OfflineRender.h"

juce::AudioBuffer<float> renderOffline(amp::ProcessorNode& node, const juce::AudioBuffer<float>& in,
                                       double sr, int block)
{
    node.prepare({ sr, block });

    const int total = in.getNumSamples();
    const int inCh = in.getNumChannels();
    juce::AudioBuffer<float> out(inCh, total);
    juce::AudioBuffer<float> work(juce::jmax(2, inCh), block);

    for (int pos = 0; pos < total; pos += block)
    {
        const int n = juce::jmin(block, total - pos);
        work.clear();
        for (int c = 0; c < inCh; ++c)
            work.copyFrom(c, 0, in, c, pos, n);
        node.process(work, n);
        for (int c = 0; c < inCh; ++c)
            out.copyFrom(c, pos, work, c, 0, n);
    }
    return out;
}
