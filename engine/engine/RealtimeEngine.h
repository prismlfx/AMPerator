#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include <juce_audio_devices/juce_audio_devices.h>

#include "engine/ProcessingPlan.h"

namespace amp
{
// Runs the active ProcessingPlan from the audio device callback.
//
// Threading: the audio callback never allocates, locks, or frees. Plans reach the audio thread through a
// single atomic slot (newer requests replace older ones that the audio thread has not picked up yet) and
// leave it through a lock-free retire queue that the message thread drains (a juce::Timer when a
// MessageManager exists, or drainRetired() directly). The audio thread therefore never drops the last
// reference to a plan or node.
//
// Plan swaps crossfade over 30 ms with equal-power (sin/cos) gains, sample-accurately across block
// boundaries. A swap requested while a crossfade is running waits until it ends; only the most recent
// request is then faded in (intermediate ones are discarded unused), so the engine always settles on the last
// requested plan.
//
// Channel indices refer to the channel arrays passed to the device callback (JUCE packs active channels).
class RealtimeEngine : public juce::AudioIODeviceCallback
{
public:
    static constexpr double crossfadeSeconds = 0.030;
    static constexpr float silenceDb = -100.0f;

    RealtimeEngine();
    // The device must already be stopped (or never started) when the engine is destroyed.
    ~RealtimeEngine() override;

    // Any thread; take effect at the next callback. Defaults: input 0, outputs 0+1.
    void setInputChannel(int ch);  // device input that feeds channel 0 of the chain; missing input = silence
    void setOutputPair(int firstCh); // writes firstCh and firstCh+1 (L only if firstCh+1 is missing); others zeroed

    // Called by audioDeviceAboutToStart (public for tests, which drive the callback without a device). Not
    // concurrent with the callback. Allocates the work buffers. An active or pending plan whose sample rate
    // differs from `sampleRate` is discarded (the engine is silent until a matching plan is swapped in).
    void prepareToPlay(double sampleRate, int maxBlock);

    // Any thread except the audio thread. Every node in `plan` must already be prepared with plan->spec();
    // swapPlan never calls prepare(). While the device runs, a plan whose spec().sampleRate differs from the
    // device rate is refused (jassert, returns false) instead of being played at the wrong rate. Before the
    // device starts any plan is accepted; prepareToPlay() later discards it if it does not match.
    // Plans must not share node objects with the previously requested plan: during a crossfade both plans run
    // in the same callback, and a shared node would be processed twice (debug builds jassert this). Build
    // fresh nodes for every plan. nullptr fades to silence.
    bool swapPlan(std::shared_ptr<ProcessingPlan>);

    // Message thread (or any single non-audio thread). Act on the most recently requested plan.
    bool setBypass(const juce::String& nodeId, bool bypassed); // false if the node is not in that plan
    std::optional<bool> getBypass(const juce::String& nodeId) const;

    // Peak level since the previous read, in dBFS, floored at silenceDb. Input = selected input channel,
    // output = max of the written output pair. Reading resets the peak.
    float inputPeakDb() const;
    float outputPeakDb() const;

    // Set before the device starts; called on the audio thread with the raw selected input channel each
    // callback (not called when that input is missing). Must be RT-safe.
    std::function<void(const float*, int)> analyzerTap;

    // Message thread: destroy plans the audio thread has retired. Runs from a timer when a MessageManager
    // exists; tests call it directly.
    void drainRetired();
    int retiredPending() const;

    // juce::AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                          float* const* outputChannelData, int numOutputChannels, int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    // Heap-allocated owner of one plan reference. The audio thread only ever passes these around as raw
    // pointers; they are deleted off the audio thread.
    struct PlanRef
    {
        std::shared_ptr<ProcessingPlan> plan;
    };

    void startPendingFade();
    void finishFade();
    void processChunk(const float* input, int offset, int n);

    static constexpr int retireCapacity = 64;

    std::unique_ptr<juce::TimedCallback> drainTimer; // only when a MessageManager exists

    // Message-thread side
    mutable juce::CriticalSection requestLock; // never taken on the audio thread
    std::shared_ptr<ProcessingPlan> requested;

    // Handoff
    std::atomic<PlanRef*> pending { nullptr };
    juce::AbstractFifo retireFifo { retireCapacity };
    std::array<PlanRef*, retireCapacity> retireSlots {};

    // Audio-thread state
    PlanRef* active = nullptr;
    PlanRef* incoming = nullptr;
    int fadePos = 0, fadeLength = 0;
    juce::AudioBuffer<float> mainBuffer, fadeBuffer;
    int chunkCapacity = 0;
    double preparedRate = 0.0;

    std::atomic<double> runningRate { 0.0 }; // 0 = device not running
    std::atomic<int> inputChannel { 0 }, outputFirst { 0 };

    mutable std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RealtimeEngine)
};
} // namespace amp
