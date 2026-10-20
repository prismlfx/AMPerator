#include "engine/RealtimeEngine.h"

#include <cmath>

#include "engine/RtSafety.h"

namespace amp
{
namespace
{
// Raise `peak` to `value` if larger. The reader resets with exchange(0); a lost race only drops one block's peak.
void raisePeak(std::atomic<float>& peak, float value)
{
    float prev = peak.load(std::memory_order_relaxed);
    while (value > prev && ! peak.compare_exchange_weak(prev, value, std::memory_order_relaxed))
    {
    }
}

float peakOf(const float* data, int n)
{
    const auto r = juce::FloatVectorOperations::findMinAndMax(data, n);
    return juce::jmax(-r.getStart(), r.getEnd());
}

float readPeakDb(std::atomic<float>& peak)
{
    return juce::Decibels::gainToDecibels(peak.exchange(0.0f, std::memory_order_relaxed), RealtimeEngine::silenceDb);
}
} // namespace

RealtimeEngine::RealtimeEngine()
{
    ensureDrainTimer();
}

void RealtimeEngine::ensureDrainTimer()
{
    // Timers need a MessageManager (even constructing one starts JUCE's timer thread); headless tests have
    // none and call drainRetired() themselves. If the engine was built before the MessageManager existed,
    // the first swapPlan() on the message thread creates the timer.
    if (drainTimer != nullptr || ! juce::MessageManager::existsAndIsCurrentThread())
        return;
    drainTimer = std::make_unique<juce::TimedCallback>([this] { drainRetired(); });
    drainTimer->startTimer(100);
}

RealtimeEngine::~RealtimeEngine()
{
    drainTimer.reset();
    delete pending.exchange(nullptr);
    delete incoming;
    delete active;
    drainRetired();
}

void RealtimeEngine::setInputChannel(int ch) { inputChannel.store(ch); }
void RealtimeEngine::setOutputPair(int firstCh) { outputFirst.store(firstCh); }

void RealtimeEngine::prepareToPlay(double sampleRate, int maxBlock)
{
    jassert(sampleRate > 0.0 && maxBlock > 0);
    mainBuffer.setSize(2, maxBlock);
    fadeBuffer.setSize(2, maxBlock);
    chunkCapacity = maxBlock;
    preparedRate = sampleRate;
    fadeLength = juce::jmax(1, juce::roundToInt(crossfadeSeconds * sampleRate));

    // The callback is not running, so plans can be settled and freed directly here.
    if (incoming != nullptr)
    {
        delete active;
        active = incoming;
        incoming = nullptr;
    }
    fadePos = 0;
    const auto matches = [sampleRate](PlanRef* r) {
        return r->plan == nullptr || juce::exactlyEqual(r->plan->spec().sampleRate, sampleRate);
    };
    if (active != nullptr && ! matches(active))
    {
        delete active;
        active = nullptr;
    }
    {
        const juce::ScopedLock sl(requestLock);
        if (auto* p = pending.load(); p != nullptr && ! matches(p))
            delete pending.exchange(nullptr);
        if (requested != nullptr && ! juce::exactlyEqual(requested->spec().sampleRate, sampleRate))
            requested = nullptr;
        runningRate.store(sampleRate);
    }
    inputPeak.store(0.0f);
    outputPeak.store(0.0f);
}

bool RealtimeEngine::swapPlan(std::shared_ptr<ProcessingPlan> plan)
{
    ensureDrainTimer();
    const juce::ScopedLock sl(requestLock);
    if (plan != nullptr)
    {
        const double rate = runningRate.load();
        if (rate > 0.0 && ! juce::exactlyEqual(plan->spec().sampleRate, rate))
        {
            jassertfalse; // plan prepared for another sample rate than the running device: refused
            return false;
        }
#if JUCE_DEBUG
        // Best effort: checks against the previously requested plan (normally the one this swap fades from).
        if (requested != nullptr)
            for (const auto& a : plan->nodes())
                for (const auto& b : requested->nodes())
                    jassert(a->node != b->node); // nodes must not be shared between plans (see header)
#endif
    }
    requested = plan;
    // A request the audio thread never picked up is replaced; it was never played, so free it right here.
    delete pending.exchange(new PlanRef { std::move(plan) }, std::memory_order_acq_rel);
    return true;
}

bool RealtimeEngine::setBypass(const juce::String& nodeId, bool bypassed)
{
    const juce::ScopedLock sl(requestLock);
    if (requested == nullptr)
        return false;
    if (auto* n = requested->find(nodeId))
    {
        n->bypass.store(bypassed);
        return true;
    }
    return false;
}

std::optional<bool> RealtimeEngine::getBypass(const juce::String& nodeId) const
{
    const juce::ScopedLock sl(requestLock);
    if (requested != nullptr)
        if (auto* n = requested->find(nodeId))
            return n->bypass.load();
    return std::nullopt;
}

float RealtimeEngine::inputPeakDb() const { return readPeakDb(inputPeak); }
float RealtimeEngine::outputPeakDb() const { return readPeakDb(outputPeak); }

void RealtimeEngine::drainRetired()
{
    retireFifo.read(retireFifo.getNumReady()).forEach([this](int i) {
        delete retireSlots[(size_t) i];
        retireSlots[(size_t) i] = nullptr;
    });
}

int RealtimeEngine::retiredPending() const { return retireFifo.getNumReady(); }

void RealtimeEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    prepareToPlay(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
}

void RealtimeEngine::audioDeviceStopped() { runningRate.store(0.0); }

//==============================================================================
// Audio thread from here on.

void RealtimeEngine::startPendingFade()
{
    if (incoming != nullptr || pending.load(std::memory_order_relaxed) == nullptr)
        return;
    // Finishing a fade retires one plan; only start when that retirement is guaranteed a queue slot.
    if (retireFifo.getFreeSpace() < 2)
    {
        // Nobody drains the retire queue (no drain timer, no drainRetired() calls): swaps are stuck.
        jassert(warnedRetireFull);
        warnedRetireFull = true;
        return;
    }
    PlanRef* next = pending.exchange(nullptr, std::memory_order_acq_rel);
    if (next == nullptr)
        return;
    if (next->plan != nullptr && ! juce::exactlyEqual(next->plan->spec().sampleRate, preparedRate))
    {
        jassertfalse; // slipped past the swapPlan check while the device was (re)starting: never play it
        retireFifo.write(1).forEach([this, next](int i) { retireSlots[(size_t) i] = next; });
        return;
    }
    incoming = next;
    fadePos = 0;
}

void RealtimeEngine::finishFade()
{
    if (active != nullptr)
    {
        PlanRef* old = active;
        retireFifo.write(1).forEach([this, old](int i) { retireSlots[(size_t) i] = old; });
    }
    active = incoming;
    incoming = nullptr;
    fadePos = 0;
}

void RealtimeEngine::processChunk(const float* input, int offset, int n)
{
    const auto run = [input, offset, n](PlanRef* ref, juce::AudioBuffer<float>& buf) {
        buf.clear(1, 0, n);
        if (ref == nullptr || ref->plan == nullptr)
        {
            buf.clear(0, 0, n);
            return;
        }
        if (input != nullptr)
            buf.copyFrom(0, 0, input + offset, n);
        else
            buf.clear(0, 0, n);
        ref->plan->process(buf, n);
    };

    run(active, mainBuffer);
    if (incoming == nullptr)
        return;

    run(incoming, fadeBuffer);
    const float halfPi = juce::MathConstants<float>::halfPi;
    for (int ch = 0; ch < 2; ++ch)
    {
        float* out = mainBuffer.getWritePointer(ch);
        const float* in = fadeBuffer.getReadPointer(ch);
        for (int i = 0; i < n; ++i)
        {
            const int t = fadePos + i;
            if (t >= fadeLength)
            {
                out[i] = in[i];
                continue;
            }
            // Equal gain: cos^2 + sin^2 = 1, so correlated plan outputs never sum above either plan's level.
            const float s = std::sin(halfPi * (float) t / (float) fadeLength);
            const float gIn = s * s;
            out[i] = out[i] * (1.0f - gIn) + in[i] * gIn;
        }
    }
    fadePos += n;
    if (fadePos >= fadeLength)
        finishFade();
}

void RealtimeEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                                      float* const* outputChannelData, int numOutputChannels,
                                                      int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    const RtSafety::ScopedNoAlloc noAlloc;
    const juce::ScopedNoDenormals noDenormals;

    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
    if (numSamples <= 0 || chunkCapacity == 0)
        return;

    const int inCh = inputChannel.load(std::memory_order_relaxed);
    const float* input = (inCh >= 0 && inCh < numInputChannels) ? inputChannelData[inCh] : nullptr;
    if (input != nullptr)
    {
        raisePeak(inputPeak, peakOf(input, numSamples));
        if (analyzerTap)
            analyzerTap(input, numSamples);
    }

    const int first = outputFirst.load(std::memory_order_relaxed);
    float* outL = (first >= 0 && first < numOutputChannels) ? outputChannelData[first] : nullptr;
    float* outR = (first >= 0 && first + 1 < numOutputChannels) ? outputChannelData[first + 1] : nullptr;

    for (int pos = 0; pos < numSamples;)
    {
        startPendingFade();
        int chunk = chunkCapacity;
        for (PlanRef* r : { active, incoming })
            if (r != nullptr && r->plan != nullptr)
                chunk = juce::jmin(chunk, r->plan->spec().maxBlock);
        const int n = juce::jmin(juce::jmax(1, chunk), numSamples - pos);

        processChunk(input, pos, n);
        if (outL != nullptr)
            juce::FloatVectorOperations::copy(outL + pos, mainBuffer.getReadPointer(0), n);
        if (outR != nullptr)
            juce::FloatVectorOperations::copy(outR + pos, mainBuffer.getReadPointer(1), n);
        pos += n;
    }

    float outPeak = 0.0f;
    if (outL != nullptr)
        outPeak = peakOf(outL, numSamples);
    if (outR != nullptr)
        outPeak = juce::jmax(outPeak, peakOf(outR, numSamples));
    raisePeak(outputPeak, outPeak);
}
} // namespace amp
