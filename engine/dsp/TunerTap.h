#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <optional>
#include <vector>

namespace amp
{
struct TunerReading
{
    float hz = 0.0f;
    juce::String note; // sharps: "C", "C#", ... "B"
    int octave = 0;    // scientific pitch notation (A4 = 440 Hz, middle C = C4)
    float cents = 0.0f; // deviation from the nearest note, in [-50, 50)
};

// YIN pitch detector fed from the raw guitar input.
//  - push(): audio thread only. Wait-free single-producer ring (juce::AbstractFifo over a preallocated buffer);
//    no allocation, no locks. When the ring is full the NEWEST samples are dropped (the producer never touches
//    consumer state). The consumer drains on every poll, so overflow only happens if poll() stalls > ~170 ms.
//  - poll(): message thread only. Drains the ring into a rolling history of the most recent window, then runs YIN
//    on the latest 2048 samples. At sample rates > 48 kHz the window is decimated by an integer factor
//    (round(sr / 48000), by averaging) so it still spans ~40 ms and the lag search stays cheap.
//  - prepare() must not run concurrently with push()/poll().
class TunerTap
{
public:
    static constexpr int kWindow = 2048;
    static constexpr float kMinHz = 60.0f;
    static constexpr float kMaxHz = 1200.0f;
    static constexpr float kThreshold = 0.15f;
    static constexpr float kSilenceDb = -50.0f;

    void prepare(double sampleRate);
    void push(const float* samples, int numSamples);
    std::optional<TunerReading> poll();

private:
    juce::AbstractFifo fifo { 1 };
    std::vector<float> ring;     // fifo storage
    std::vector<float> history;  // latest kWindow*decim raw samples, circular
    std::vector<float> window;   // decimated, linearised analysis window (kWindow)
    std::vector<float> diff;     // d'(tau) scratch
    int decim = 1;
    int historyPos = 0;
    long long consumed = 0;
    double effectiveRate = 48000.0;
};
} // namespace amp
