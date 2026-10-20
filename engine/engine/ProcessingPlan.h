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
    std::atomic<bool> bypass { false };
};

// A compiled, immutable chain: node order and membership never change after construction; only the per-node
// bypass flags may be flipped (atomically, from any thread). All nodes must already be prepared with `spec`
// before the plan is handed to the audio thread; the plan never calls prepare() itself.
class ProcessingPlan
{
public:
    ProcessingPlan(std::vector<std::shared_ptr<PlanNode>> ordered, ProcessSpec);

    // Audio thread. io has >= 2 channels and >= n samples, n <= spec().maxBlock. Input is mono in channel 0.
    // The signal stays mono (channel 0) until the first non-bypassed isStereo() node, which receives channel 0
    // copied to channel 1. If no stereo node ran, channel 0 is copied to channel 1 at the end.
    void process(juce::AudioBuffer<float>& io, int n);

    PlanNode* find(const juce::String& id); // nullptr when absent
    const ProcessSpec& spec() const { return processSpec; }
    const std::vector<std::shared_ptr<PlanNode>>& nodes() const { return ordered; }

private:
    const std::vector<std::shared_ptr<PlanNode>> ordered;
    const ProcessSpec processSpec;
};
} // namespace amp
