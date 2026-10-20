#include "engine/ProcessingPlan.h"

namespace amp
{
ProcessingPlan::ProcessingPlan(std::vector<std::shared_ptr<PlanNode>> nodesInOrder, ProcessSpec spec)
    : ordered(std::move(nodesInOrder)), processSpec(spec)
{
    for ([[maybe_unused]] const auto& n : ordered)
        jassert(n != nullptr && n->node != nullptr);
}

void ProcessingPlan::process(juce::AudioBuffer<float>& io, int n)
{
    jassert(io.getNumChannels() >= 2 && n <= io.getNumSamples() && n <= processSpec.maxBlock);
    bool stereo = false;
    for (const auto& pn : ordered) // by reference: no shared_ptr refcount traffic on the audio thread
    {
        if (pn->bypass.load(std::memory_order_relaxed))
            continue;
        ProcessorNode& node = *pn->node;
        if (! stereo && node.isStereo())
        {
            io.copyFrom(1, 0, io, 0, 0, n);
            stereo = true;
        }
        node.process(io, n);
    }
    if (! stereo)
        io.copyFrom(1, 0, io, 0, 0, n);
}

PlanNode* ProcessingPlan::find(const juce::String& id)
{
    for (const auto& pn : ordered)
        if (pn->id == id)
            return pn.get();
    return nullptr;
}
} // namespace amp
