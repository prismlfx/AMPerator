#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "dsp/Node.h"

namespace amp
{
struct PlanNode
{
    juce::String id;
    std::shared_ptr<ProcessorNode> node;
    // Requested bypass state. Set it directly only before the plan is built; afterwards use requestBypass().
    std::atomic<bool> bypass { false };

    /** Message thread (any single non-audio thread). Re-engaging a node that the audio thread has fully faded out
        first reset()s it, so it does not replay the signal it held when it was bypassed; the node is not being
        processed at that point (the audio thread skips it until it sees bypass == false). */
    void requestBypass(bool b);

    // Audio-thread state of the bypass ramp: 1 = fully processed, 0 = fully bypassed (not processed).
    float wet = 1.0f;
    // True while the node is fully bypassed and therefore untouched by the audio thread (published by it).
    std::atomic<bool> idle { false };
};

// A compiled, immutable chain: node order and membership never change after construction; only the per-node
// bypass flags may be flipped (PlanNode::requestBypass). A bypass change crossfades over bypassRampSeconds,
// sample-accurately: the node keeps running while its output is mixed towards (or from) its dry input, and once fully
// bypassed it is no longer processed. All nodes must already be prepared with `spec` before the plan is handed to the
// audio thread; the plan never calls prepare() itself.
class ProcessingPlan
{
public:
    static constexpr double bypassRampSeconds = 0.010;

    ProcessingPlan(std::vector<std::shared_ptr<PlanNode>> ordered, ProcessSpec);

    // Audio thread. io has >= 2 channels and >= n samples, n <= spec().maxBlock. Input is mono in channel 0.
    // The signal stays mono (channel 0) until the first non-bypassed isStereo() node, which receives channel 0
    // copied to channel 1. If no stereo node ran, channel 0 is copied to channel 1 at the end. A node that is
    // fading out still counts as running. Allocation-free.
    void process(juce::AudioBuffer<float>& io, int n);

    PlanNode* find(const juce::String& id); // nullptr when absent
    const ProcessSpec& spec() const { return processSpec; }
    const std::vector<std::shared_ptr<PlanNode>>& nodes() const { return ordered; }

private:
    const std::vector<std::shared_ptr<PlanNode>> ordered;
    const ProcessSpec processSpec;
    const float rampStep; // wet change per sample
    juce::AudioBuffer<float> dry; // the input of a node that is ramping, 2 x maxBlock
};
} // namespace amp
