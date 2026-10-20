#include "engine/ProcessingPlan.h"

namespace amp
{
void PlanNode::requestBypass(bool b)
{
    if (! b && bypass.load(std::memory_order_relaxed) && ownership.beginReset())
    {
        node->reset(); // the audio thread cannot acquire the node while we hold it
        ownership.endReset();
    }
    bypass.store(b, std::memory_order_release);
}

ProcessingPlan::ProcessingPlan(std::vector<std::shared_ptr<PlanNode>> nodesInOrder, ProcessSpec spec)
    : ordered(std::move(nodesInOrder)),
      processSpec(spec),
      rampStep(1.0f / (float) juce::jmax(1, juce::roundToInt(bypassRampSeconds * spec.sampleRate))),
      dry(2, juce::jmax(1, spec.maxBlock))
{
    for ([[maybe_unused]] const auto& n : ordered)
        jassert(n != nullptr && n->node != nullptr);
    for (const auto& n : ordered)
    {
        const bool off = n->bypass.load();
        n->wet = off ? 0.0f : 1.0f;
        n->ownership.initialise(off ? NodeOwnership::Idle : NodeOwnership::Active);
    }
}

void ProcessingPlan::process(juce::AudioBuffer<float>& io, int n)
{
    jassert(io.getNumChannels() >= 2 && n <= io.getNumSamples() && n <= processSpec.maxBlock);
    bool stereo = false;
    for (const auto& pn : ordered) // by reference: no shared_ptr refcount traffic on the audio thread
    {
        const float target = pn->bypass.load(std::memory_order_acquire) ? 0.0f : 1.0f;
        if (pn->wet <= 0.0f && (target <= 0.0f || ! pn->ownership.audioAcquire()))
            continue; // fully bypassed, or being reset off the audio thread: the input passes dry

        ProcessorNode& node = *pn->node;
        if (! stereo && node.isStereo())
        {
            io.copyFrom(1, 0, io, 0, 0, n);
            stereo = true;
        }
        if (juce::exactlyEqual(pn->wet, target))
        {
            node.process(io, n);
            continue;
        }

        // Ramping: run the node and crossfade from its input (dry) to its output (wet), or back.
        for (int ch = 0; ch < 2; ++ch)
            dry.copyFrom(ch, 0, io, ch, 0, n);
        node.process(io, n);
        const float step = target > pn->wet ? rampStep : -rampStep;
        float w = pn->wet;
        float* l = io.getWritePointer(0);
        float* r = io.getWritePointer(1);
        const float* dl = dry.getReadPointer(0);
        const float* dr = dry.getReadPointer(1);
        for (int i = 0; i < n; ++i)
        {
            w = juce::jlimit(0.0f, 1.0f, w + step);
            l[i] = dl[i] + (l[i] - dl[i]) * w;
            r[i] = dr[i] + (r[i] - dr[i]) * w;
        }
        pn->wet = w;
        if (w <= 0.0f)
            pn->ownership.audioRelease(); // no longer touched: requestBypass may reset it
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
