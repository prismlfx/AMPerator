#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "dsp/Node.h"

namespace amp
{
// Which thread may touch a node: the audio thread (Active), nobody (Idle: fully bypassed), or the message thread
// (Resetting). Every hand-over is a single compare-and-swap, so a reset can never overlap process(). Lock-free.
class NodeOwnership
{
public:
    enum State : int { Active, Idle, Resetting };

    explicit NodeOwnership(State initial) : state_(initial) {}

    // Audio thread, before processing a node it does not own yet (wet == 0): Idle -> Active. False while the message
    // thread is resetting it; the audio thread then skips the node for this block (its input passes dry).
    bool audioAcquire() { return transition(Idle, Active); }
    // Audio thread, once a fade-out has completed and it will not touch the node again: Active -> Idle.
    void audioRelease() { state_.store(Idle, std::memory_order_release); }
    // Message thread: Idle -> Resetting. False when the audio thread owns the node (still processing or fading).
    bool beginReset() { return transition(Idle, Resetting); }
    // Message thread, after reset() returned: Resetting -> Idle (publishes the reset's writes to the audio thread).
    void endReset() { state_.store(Idle, std::memory_order_release); }

    // Before the node is handed to the audio thread only.
    void initialise(State s) { state_.store(s, std::memory_order_release); }

    State state() const { return state_.load(std::memory_order_acquire); }

private:
    bool transition(State from, State to)
    {
        return state_.compare_exchange_strong(from, to, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    std::atomic<State> state_;
    static_assert(std::atomic<State>::is_always_lock_free);
};

struct PlanNode
{
    juce::String id;
    std::shared_ptr<ProcessorNode> node;
    // Requested bypass state. Set it directly only before the plan is built; afterwards use requestBypass().
    std::atomic<bool> bypass { false };

    /** Message thread (any single non-audio thread). Re-engaging a node that the audio thread has fully faded out
        first reset()s it, so it does not replay the signal it held when it was bypassed. The reset runs only after
        claiming the node (ownership Idle -> Resetting); while it runs the audio thread skips the node. If the audio
        thread still owns the node (it is mid fade-out, or already re-acquired it in an on -> off -> on burst), the
        reset is skipped: the node never went silent, so its state is still continuous with the signal. */
    void requestBypass(bool b);

    // Audio-thread state of the bypass ramp: 1 = fully processed, 0 = fully bypassed (not processed).
    float wet = 1.0f;
    // Hand-over between the audio thread (processing) and the message thread (reset on re-engage).
    NodeOwnership ownership { NodeOwnership::Active };
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
